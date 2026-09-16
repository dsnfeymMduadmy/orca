#include "orca_fbtest.h"
#include "orca_fbstream.h"
#include "orca_memmap.h"
#include "orca_input.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"

#define FBT_W       ORCA_FBTEST_WIDTH
#define FBT_H       ORCA_FBTEST_HEIGHT
#define FBT_STRIDE  (FBT_W * 2u)
#define FBT_BYTES   (FBT_STRIDE * FBT_H)

static uint16_t* s_fb;
static uint32_t  s_frames;
static uint32_t  s_loops;   /* витков задачи — см. orca_fbtest.h */

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static void fill_rect(int32_t x0, int32_t y0, int32_t w, int32_t h, uint16_t c)
{
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > (int32_t)FBT_W) { w = (int32_t)FBT_W - x0; }
    if (y0 + h > (int32_t)FBT_H) { h = (int32_t)FBT_H - y0; }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int32_t y = y0; y < y0 + h; y++) {
        uint16_t* row = s_fb + (uint32_t)y * FBT_W;
        for (int32_t x = x0; x < x0 + w; x++) {
            row[x] = c;
        }
    }
}

/*
 * Кадр состоит из вещей, по которым сразу видно, что канал живой и
 * пиксели не перепутаны:
 *   - диагональный градиент, который ползёт (движение = кадры идут);
 *   - рамка (видно, что размеры и stride сошлись);
 *   - цветовые плашки R/G/B (видно, что байтпорядок RGB565 правильный);
 *   - бегающий квадрат (видно текущий кадр без счётчика);
 *   - метка под последним касанием (видно, что тапы с клиента дошли
 *     и координаты не перепутаны местами).
 */
static void render(uint32_t frame)
{
    uint8_t phase = (uint8_t)(frame * 3u);

    for (uint32_t y = 0; y < FBT_H; y++) {
        uint16_t* row = s_fb + y * FBT_W;
        for (uint32_t x = 0; x < FBT_W; x++) {
            uint8_t v = (uint8_t)(x + y + phase);
            row[x] = rgb565((uint8_t)(v), (uint8_t)(255u - v), (uint8_t)(x ^ y));
        }
    }

    /* Плашки основных цветов в левом верхнем углу. */
    fill_rect(8,  8,  24, 24, rgb565(255, 0, 0));
    fill_rect(36, 8,  24, 24, rgb565(0, 255, 0));
    fill_rect(64, 8,  24, 24, rgb565(0, 0, 255));
    fill_rect(92, 8,  24, 24, rgb565(255, 255, 255));
    fill_rect(120, 8, 24, 24, rgb565(0, 0, 0));

    /* Рамка в 2 px. */
    uint16_t border = rgb565(255, 255, 0);
    fill_rect(0, 0, (int32_t)FBT_W, 2, border);
    fill_rect(0, (int32_t)FBT_H - 2, (int32_t)FBT_W, 2, border);
    fill_rect(0, 0, 2, (int32_t)FBT_H, border);
    fill_rect((int32_t)FBT_W - 2, 0, 2, (int32_t)FBT_H, border);

    /* Квадрат, ходящий туда-обратно по треугольной волне. */
    uint32_t span = FBT_W - 40u;
    uint32_t t    = frame % (span * 2u);
    int32_t  bx   = (int32_t)((t < span) ? t : (span * 2u - t));
    fill_rect(bx, (int32_t)FBT_H - 60, 32, 32, rgb565(255, 0, 255));

    /*
     * Перекрестие там, куда ткнули мышью в окне клиента. Зелёное,
     * пока кнопка держится, серое после отпускания — по одному кадру
     * видно и факт доставки события, и правильность масштаба координат.
     */
    orca_input_touch_t tp;
    if (orca_input_last_touch(&tp) && tp.display_id == 0) {
        uint16_t mark = (tp.action != 0) ? rgb565(0, 255, 0) : rgb565(128, 128, 128);
        fill_rect((int32_t)tp.x - 12, (int32_t)tp.y - 1, 25, 3, mark);
        fill_rect((int32_t)tp.x - 1, (int32_t)tp.y - 12, 3, 25, mark);
    }
}

bool orca_fbtest_init(void)
{
    if (!board_sdram_ready()) {
        return false;
    }
    /*
     * Свой слот FB_POOL, а не начало пула: в слоте 0 теперь рисует LVGL.
     * Пока оба писали в ORCA_FB_POOL_BASE, одновременная работа двух
     * генераторов портила кадр прямо в памяти, а не только в канале.
     */
    s_fb = (uint16_t*)ORCA_FB_SLOT_BASE(ORCA_FB_SLOT_TEST);
    s_frames = 0;
    render(0);
    return true;
}

uint32_t orca_fbtest_frame_count(void)
{
    return s_frames;
}

uint32_t orca_fbtest_loop_count(void)
{
    return s_loops;
}

void orca_fbtest_task(void* arg)
{
    (void)arg;

    if (s_fb == NULL) {
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        s_loops++;

        orca_fbstream_stats_t st;
        orca_fbstream_get_stats(&st);

        if (!st.enabled || st.source != ORCA_FB_SRC_TEST) {
            /* Никто не смотрит или транслируется GUI — не рисуем. */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        render(s_frames++);
        orca_fbstream_push_frame(ORCA_FB_SRC_TEST, 0, s_fb, FBT_BYTES,
                                 FBT_W, FBT_H,
                                 FBT_STRIDE, ORCA_LINK_PIXFMT_RGB565);

        /*
         * Спим ровно интервал кадра. Раньше здесь было фиксированные 10 мс:
         * кадр перерисовывался 100 раз в секунду (480x320x2 = 300 КБ записи
         * в SDRAM), а push_frame отбрасывал 9 из 10 — CPU жгли впустую, и
         * анимация летела в 10 раз быстрее реальной частоты трансляции.
         */
        uint32_t period_ms = 1000u / (st.max_fps ? st.max_fps : 1u);
        vTaskDelay(pdMS_TO_TICKS(period_ms ? period_ms : 1u));
    }
}
