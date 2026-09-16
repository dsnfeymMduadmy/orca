#include "orca_lvgl.h"
#include "orca_ui.h"
#include "orca_fbstream.h"
#include "orca_input.h"
#include "orca_memmap.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#define GUI_W       ORCA_GUI_WIDTH
#define GUI_H       ORCA_GUI_HEIGHT
#define GUI_STRIDE  (GUI_W * 2u)
#define GUI_BYTES   (GUI_STRIDE * GUI_H)

static lv_display_t* s_disp;
static lv_indev_t*   s_indev;

static uint16_t* s_fb;      /* кадр, в который рисует LVGL */
static uint16_t* s_tx;      /* снимок кадра, который уходит в линию */
static uint32_t  s_frames;
static uint32_t  s_loops;   /* витков lv_timer_handler — см. orca_lvgl.h */

/*
 * Кадр ждёт отправки. Ставит задача GUI (flush_cb), снимает gui_tx —
 * по одному писателю на переход, поэтому мьютекс не нужен, но volatile
 * обязателен: без него компилятор кеширует флаг в регистре.
 */
static volatile bool s_tx_pending;
static TaskHandle_t  s_tx_task;

/* Состояние указателя для LVGL: см. indev_read_cb. */
static lv_point_t       s_point;
static lv_indev_state_t s_state = LV_INDEV_STATE_RELEASED;

/* ------------------------------------------------------------------ */
/* Мосты в ядро: время, сон, логи                                     */
/* ------------------------------------------------------------------ */

static uint32_t tick_get_cb(void)
{
    /* configTICK_RATE_HZ = 1000, поэтому тик == миллисекунда. */
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void delay_cb(uint32_t ms)
{
    /* Без этого lv_delay_ms() крутит busy-wait и съедает такты у всех задач. */
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1u));
}

#if LV_USE_LOG
/*
 * Логи LVGL уходят в ту же консоль, что и остальная прошивка: клиент
 * показывает их в панели лога вместе с сообщениями ядра. Именно здесь
 * всплывает самое частое — "Couldn't allocate memory" при исчерпании
 * LV_MEM_SIZE; без вывода это выглядит как молча не нарисованный виджет.
 */
static void log_cb(lv_log_level_t level, const char* buf)
{
    static const char* const name[] = { "TRACE", "INFO", "WARN", "ERROR", "USER" };
    int idx = (int)level;
    const char* tag = (idx >= 0 && idx < (int)(sizeof(name) / sizeof(name[0])))
                      ? name[idx] : "?";
    printf("[lvgl][%s] %s", tag, buf ? buf : "");
}
#endif

/* ------------------------------------------------------------------ */
/* Дисплей                                                            */
/* ------------------------------------------------------------------ */

static void frame_ready(void)
{
    orca_fbstream_stats_t st;
    orca_fbstream_get_stats(&st);

    /* Никто не смотрит или транслируется fbtest — копировать нечего. */
    if (!st.enabled || st.source != (uint8_t)ORCA_FB_SRC_GUI) {
        return;
    }
    if (s_tx_task == NULL || s_tx_pending) {
        /*
         * Предыдущий кадр ещё уходит в линию. Пропускаем этот: догонять
         * отрисовку всё равно нельзя (UART медленнее LVGL на два порядка),
         * а ждать здесь значит остановить интерфейс.
         */
        return;
    }

    memcpy(s_tx, s_fb, GUI_BYTES);
    s_tx_pending = true;
    xTaskNotifyGive(s_tx_task);
}

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    (void)area;
    (void)px_map;

    /*
     * Режим DIRECT: px_map — это весь наш кадр, а flush_cb вызывается по
     * одному разу на каждую перерисованную область. Отправлять имеет смысл
     * только после последней, когда кадр целиком собран.
     */
    if (lv_display_flush_is_last(disp)) {
        s_frames++;
        frame_ready();
    }
    lv_display_flush_ready(disp);
}

/* ------------------------------------------------------------------ */
/* Ввод                                                               */
/* ------------------------------------------------------------------ */

/*
 * За один вызов LVGL берёт ровно одно состояние указателя, поэтому события
 * снимаются из ринга по одному, а continue_reading просит вызвать нас снова,
 * пока очередь не пуста. Иначе пришедшие пачкой down+up схлопнулись бы в
 * последнее состояние и нажатие потерялось.
 *
 * Когда событий нет, повторяем последнее известное состояние: сообщить
 * RELEASED посреди перетаскивания значит оборвать жест.
 */
static void indev_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    (void)indev;

    orca_input_touch_t tp;
    if (orca_input_pop_touch(&tp)) {
        if (tp.display_id == 0u) {
            s_point.x = (int32_t)tp.x;
            s_point.y = (int32_t)tp.y;
            s_state = (tp.action == 0u) ? LV_INDEV_STATE_RELEASED
                                        : LV_INDEV_STATE_PRESSED;
        }
        data->continue_reading = (orca_input_pending() > 0u);
    }

    data->point = s_point;
    data->state = s_state;
}

/* ------------------------------------------------------------------ */
/* Инициализация и задачи                                             */
/* ------------------------------------------------------------------ */

bool orca_lvgl_init(void)
{
    /* Куча LVGL (LV_MEM_ADR) и оба буфера лежат в SDRAM. */
    if (!board_sdram_ready()) {
        printf("[WARN] GUI: SDRAM не поднята, LVGL не запускаем\r\n");
        return false;
    }

    s_fb = (uint16_t*)ORCA_FB_SLOT_BASE(ORCA_FB_SLOT_GUI);
    s_tx = (uint16_t*)ORCA_FB_SLOT_BASE(ORCA_FB_SLOT_GUI_TX);

    /*
     * Время и сон LVGL берёт у ядра ДО lv_init: иначе первые вызовы
     * lv_tick_get() вернут ноль, и таймеры решат, что прошло 0 мс.
     */
    lv_tick_set_cb(tick_get_cb);
    lv_delay_set_cb(delay_cb);
#if LV_USE_LOG
    lv_log_register_print_cb(log_cb);
#endif

    lv_init();

    s_disp = lv_display_create(GUI_W, GUI_H);
    if (s_disp == NULL) {
        printf("[ERROR] GUI: lv_display_create не удался\r\n");
        return false;
    }
    /*
     * DIRECT + один буфер: LVGL рисует прямо в кадр SDRAM и обновляет только
     * изменившиеся области, а не перерисовывает экран целиком. Для нас это
     * важно вдвойне — 300 КБ в SDRAM на каждый кадр стоят дороже отрисовки.
     */
    lv_display_set_buffers(s_disp, s_fb, NULL, GUI_BYTES,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush_cb);

    s_indev = lv_indev_create();
    if (s_indev == NULL) {
        printf("[ERROR] GUI: lv_indev_create не удался\r\n");
        return false;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, indev_read_cb);
    lv_indev_set_display(s_indev, s_disp);

    /*
     * Экран собирает orca_ui: страницы лежат на карте (0:/gui), и в прошивке
     * от интерфейса остаётся только их разбор. Без карты берётся встроенная
     * копия — см. orca_ui.h.
     */
    if (!orca_ui_init()) {
        printf("[ERROR] GUI: интерфейс не собрался\r\n");
        return false;
    }

    printf("[OK] LVGL %s: %ux%u RGB565, кадр @0x%08lX, снимок @0x%08lX\r\n",
           LVGL_VERSION_INFO, GUI_W, GUI_H,
           (unsigned long)(uintptr_t)s_fb, (unsigned long)(uintptr_t)s_tx);
    printf("[OK] LVGL: куча %u КБ @0x%08lX, занято %u КБ\r\n",
           (unsigned)(LV_MEM_SIZE / 1024u), (unsigned long)LV_MEM_ADR,
           (unsigned)(orca_lvgl_heap_used() / 1024u));
    return true;
}

uint32_t orca_lvgl_frame_count(void)
{
    return s_frames;
}

uint32_t orca_lvgl_loop_count(void)
{
    return s_loops;
}

uint32_t orca_lvgl_heap_used(void)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    return (uint32_t)(mon.total_size - mon.free_size);
}

void orca_lvgl_task(void* arg)
{
    (void)arg;

    if (s_disp == NULL) {
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        /*
         * lv_timer_handler сам берёт lv_lock (мьютекс LVGL), поэтому дерево
         * виджетов защищено от чужих задач. Он же возвращает, через сколько
         * миллисекунд ждать следующего таймера — спим ровно столько, но не
         * больше 30 мс, чтобы не проспать свежие тапы.
         */
        s_loops++;

        uint32_t next = lv_timer_handler();
        if (next == LV_NO_TIMER_READY || next > 30u) {
            next = 30u;
        }
        vTaskDelay(pdMS_TO_TICKS(next ? next : 1u));
    }
}

void orca_lvgl_stream_task(void* arg)
{
    (void)arg;

    s_tx_task = xTaskGetCurrentTaskHandle();

    for (;;) {
        /*
         * Ждём снимок бесконечно: пока трансляция выключена, кадры сюда не
         * попадают вообще, и задача не тратит ни такта.
         */
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        orca_fbstream_push_frame(ORCA_FB_SRC_GUI, 0, s_tx, GUI_BYTES,
                                 GUI_W, GUI_H,
                                 GUI_STRIDE, ORCA_LINK_PIXFMT_RGB565);
        s_tx_pending = false;
    }
}
