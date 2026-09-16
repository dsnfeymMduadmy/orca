#include "orca_api.h"
#include "orca_fileio.h"
#include "orca_arena.h"
#include "orca_memmap.h"
#include "orca_shell.h"
#include "orca_appmgr.h"
#include "orca_input.h"
#include "main.h"
#include "board.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/*
 * Реализация orca_api_t поверх HAL/FreeRTOS.
 * Всё, что видит загруженный модуль, проходит через эту таблицу.
 *
 * Правило: ни одна функция отсюда не должна ронять систему при мусорных
 * аргументах от модуля — модульный код считается недоверенным.
 */

/* ------------------------------------------------------------------ */
/* Права: проверка на входе в группу                                  */
/*                                                                    */
/* Права модуля берутся из его манифеста и живут в TLS его задачи     */
/* (см. orca_appmgr.c). Задачи ядра TLS не трогают, поэтому shell,     */
/* GUI и службы, вызывая ту же таблицу, проходят проверку всегда.     */
/*                                                                    */
/* Проверяем в точке захвата ресурса (open/claim) — по модели         */
/* capability: получил хендл, значит право уже проверено. GPIO и GUI  */
/* адресуются глобальным id, а не хендлом, поэтому там проверяется     */
/* каждый вызов.                                                      */
/* ------------------------------------------------------------------ */

static void perm_denied(const char* group)
{
    printf("[WARN][perm] модулю не разрешена группа '%s'\r\n", group);
}

/* Возвращает true, если группа разрешена; иначе логирует отказ. */
static bool perm_check(orca_perm_t need, const char* group)
{
    if (orca_appmgr_task_perm_allows(need)) {
        return true;
    }
    perm_denied(group);
    return false;
}

/* ------------------------------------------------------------------ */
/* log                                                                */
/* ------------------------------------------------------------------ */

static const char* level_str(orca_log_level_t lvl)
{
    switch (lvl) {
        case ORCA_LOG_WARN:  return "WARN";
        case ORCA_LOG_ERROR: return "ERROR";
        default:             return "INFO";
    }
}

static void api_log_write(orca_log_level_t level, const char* tag, const char* msg)
{
    if (tag == NULL) tag = "?";
    if (msg == NULL) msg = "";
    printf("[%s][%s] %s\r\n", level_str(level), tag, msg);
}

static void api_log_info(const char* tag, const char* msg)
{
    api_log_write(ORCA_LOG_INFO, tag, msg);
}

static void api_log_warn(const char* tag, const char* msg)
{
    api_log_write(ORCA_LOG_WARN, tag, msg);
}

static void api_log_error(const char* tag, const char* msg)
{
    api_log_write(ORCA_LOG_ERROR, tag, msg);
}

static const orca_log_api_t s_log_api = {
    .write = api_log_write,
    .info  = api_log_info,
    .warn  = api_log_warn,
    .error = api_log_error,
};

/* ------------------------------------------------------------------ */
/* task                                                               */
/* ------------------------------------------------------------------ */

static void api_task_sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void api_task_yield(void)
{
    taskYIELD();
}

/*
 * Флаг остановки хранится в notification value таска модуля:
 * orca_appmgr_stop() выставляет ORCA_STOP_NOTIFY_BIT, модуль его опрашивает.
 */
static bool api_task_should_stop(void)
{
    /*
     * xTaskNotifyWait() сбрасывает уведомление при чтении, поэтому цикл
     * видел true ровно один раз: типовой модуль спрашивает в условии while,
     * потом ещё раз перед выходом — и второй вызов уже говорил "работаем".
     * ulTaskNotifyValueClear(NULL, 0) читает значение, ничего не сбрасывая,
     * и не блокируется — флаг остаётся взведённым до конца жизни задачи.
     */
    uint32_t value = ulTaskNotifyValueClear(NULL, 0);
    return (value & ORCA_STOP_NOTIFY_BIT) != 0;
}

static uint32_t api_task_uptime_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static const orca_task_api_t s_task_api = {
    .sleep_ms    = api_task_sleep_ms,
    .yield       = api_task_yield,
    .should_stop = api_task_should_stop,
    .uptime_ms   = api_task_uptime_ms,
};

/* ------------------------------------------------------------------ */
/* storage                                                            */
/* ------------------------------------------------------------------ */

static void* api_storage_open(const char* path, orca_file_mode_t mode)
{
    if (path == NULL) {
        return NULL;
    }
    if (!perm_check(ORCA_PERM_STORAGE, "storage")) {
        return NULL;
    }
    orca_fio_mode_t m;
    switch (mode) {
        case ORCA_FILE_WRITE:  m = ORCA_FIO_WRITE;  break;
        case ORCA_FILE_APPEND: m = ORCA_FIO_APPEND; break;
        default:               m = ORCA_FIO_READ;   break;
    }
    return orca_fileio_open_ex(path, m);
}

static int32_t api_storage_read(void* handle, void* buf, uint32_t len)
{
    if (handle == NULL || buf == NULL) {
        return -1;
    }
    return orca_fileio_read((orca_file_t*)handle, buf, len);
}

static int32_t api_storage_write(void* handle, const void* buf, uint32_t len)
{
    if (handle == NULL || buf == NULL) {
        return -1;
    }
    return orca_fileio_write((orca_file_t*)handle, buf, len);
}

static int32_t api_storage_seek(void* handle, int32_t offset, int32_t whence)
{
    if (handle == NULL || whence != 0 || offset < 0) {
        return -1;  /* пока только SEEK_SET */
    }
    return orca_fileio_seek((orca_file_t*)handle, (uint32_t)offset) ? offset : -1;
}

static void api_storage_close(void* handle)
{
    if (handle != NULL) {
        orca_fileio_close((orca_file_t*)handle);
    }
}

static bool api_storage_exists(const char* path)
{
    if (path == NULL || !perm_check(ORCA_PERM_STORAGE, "storage")) {
        return false;
    }
    orca_file_t* f = orca_fileio_open(path, false);
    if (f == NULL) {
        return false;
    }
    orca_fileio_close(f);
    return true;
}

static bool api_storage_mkdir(const char* path)
{
    if (path == NULL || !perm_check(ORCA_PERM_STORAGE, "storage")) {
        return false;
    }
    return orca_fileio_mkdir(path);
}

/* --- API v2: каталоги и метаданные (нужно ls/rm/cp/mv/df с карты) --- */

/*
 * DIR у FatFs великоват для стека модуля, поэтому раздаём из статического
 * пула: модуль получает тег-хендл, а не указатель внутрь ядра.
 */
#define DIR_SLOT_COUNT   4u
#define DIR_HANDLE_BASE  0x4D170000u

static DIR  s_dir_slots[DIR_SLOT_COUNT];
static bool s_dir_used[DIR_SLOT_COUNT];

static DIR* dir_from_handle(void* handle)
{
    uintptr_t tag = (uintptr_t)handle;
    if (tag < DIR_HANDLE_BASE || tag >= DIR_HANDLE_BASE + DIR_SLOT_COUNT) {
        return NULL;
    }
    uint32_t idx = (uint32_t)(tag - DIR_HANDLE_BASE);
    return s_dir_used[idx] ? &s_dir_slots[idx] : NULL;
}

static void fill_dirent(const FILINFO* fno, orca_dirent_t* out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->name, fno->fname, ORCA_NAME_MAX - 1);
    out->size   = (uint32_t)fno->fsize;
    out->is_dir = (fno->fattrib & AM_DIR) != 0;
    out->mdate  = fno->fdate;
    out->mtime  = fno->ftime;
}

static void* api_storage_opendir(const char* path)
{
    if (path == NULL || !perm_check(ORCA_PERM_STORAGE, "storage")) {
        return NULL;
    }
    for (uint32_t i = 0; i < DIR_SLOT_COUNT; i++) {
        if (s_dir_used[i]) {
            continue;
        }
        if (f_opendir(&s_dir_slots[i], path) != FR_OK) {
            return NULL;
        }
        s_dir_used[i] = true;
        return (void*)(uintptr_t)(DIR_HANDLE_BASE + i);
    }
    return NULL;
}

static bool api_storage_readdir(void* handle, orca_dirent_t* out)
{
    DIR* dir = dir_from_handle(handle);
    if (dir == NULL || out == NULL) {
        return false;
    }
    FILINFO fno;
    if (f_readdir(dir, &fno) != FR_OK || fno.fname[0] == '\0') {
        return false;
    }
    fill_dirent(&fno, out);
    return true;
}

static void api_storage_closedir(void* handle)
{
    uintptr_t tag = (uintptr_t)handle;
    if (tag < DIR_HANDLE_BASE || tag >= DIR_HANDLE_BASE + DIR_SLOT_COUNT) {
        return;
    }
    uint32_t idx = (uint32_t)(tag - DIR_HANDLE_BASE);
    if (s_dir_used[idx]) {
        f_closedir(&s_dir_slots[idx]);
        s_dir_used[idx] = false;
    }
}

static bool api_storage_stat(const char* path, orca_dirent_t* out)
{
    if (path == NULL || out == NULL || !perm_check(ORCA_PERM_STORAGE, "storage")) {
        return false;
    }
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) {
        return false;
    }
    fill_dirent(&fno, out);
    return true;
}

static bool api_storage_remove(const char* path)
{
    if (path == NULL || !perm_check(ORCA_PERM_STORAGE, "storage")) {
        return false;
    }
    return f_unlink(path) == FR_OK;
}

static bool api_storage_rename(const char* from, const char* to)
{
    if (from == NULL || to == NULL || !perm_check(ORCA_PERM_STORAGE, "storage")) {
        return false;
    }
    return f_rename(from, to) == FR_OK;
}

static bool api_storage_fsinfo(const char* drive, orca_fsinfo_t* out)
{
    if (out == NULL || !perm_check(ORCA_PERM_STORAGE, "storage")) {
        return false;
    }
    if (drive == NULL) {
        drive = "0:";
    }

    FATFS*  fs = NULL;
    DWORD   free_clusters = 0;
    if (f_getfree(drive, &free_clusters, &fs) != FR_OK || fs == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    uint32_t sect = (uint32_t)fs->csize * FF_MAX_SS;
    out->cluster_bytes = sect;
    /*
     * Умножаем в 64 битах. На карте 8 ГБ (n_fatent порядка двух миллионов при
     * кластере 32 КБ) 32-битное произведение переполняется, и модуль вместе с
     * командой `free` получал остаток от деления объёма на 4 ГБ.
     */
    out->total_bytes = (uint64_t)(fs->n_fatent - 2u) * sect;
    out->free_bytes  = (uint64_t)free_clusters * sect;

    char label[16] = {0};
    if (f_getlabel(drive, label, NULL) == FR_OK) {
        strncpy(out->label, label, sizeof(out->label) - 1);
    }
    return true;
}

static uint32_t api_storage_size(void* handle)
{
    if (handle == NULL) {
        return 0;
    }
    return orca_fileio_size((orca_file_t*)handle);
}

static const orca_storage_api_t s_storage_api = {
    .open   = api_storage_open,
    .read   = api_storage_read,
    .write  = api_storage_write,
    .seek   = api_storage_seek,
    .close  = api_storage_close,
    .exists = api_storage_exists,
    .mkdir  = api_storage_mkdir,

    .opendir  = api_storage_opendir,
    .readdir  = api_storage_readdir,
    .closedir = api_storage_closedir,
    .stat     = api_storage_stat,
    .remove   = api_storage_remove,
    .rename   = api_storage_rename,
    .fsinfo   = api_storage_fsinfo,
    .size     = api_storage_size,
};

/* ------------------------------------------------------------------ */
/* gpio — только пины, выведенные на разъёмы P1/P2                    */
/* ------------------------------------------------------------------ */

/*
 * Модуль оперирует "логическим" pin_id, а не портом напрямую: так ядро
 * контролирует, к чему модулю вообще можно прикасаться (SDRAM/QSPI/LCD
 * пины сюда не входят). Таблица — подмножество свободных ног с P1/P2.
 */
typedef struct {
    GPIO_TypeDef* port;
    uint16_t      pin;
    bool          claimed;
} gpio_slot_t;

static gpio_slot_t s_gpio_slots[] = {
    { GPIOA, GPIO_PIN_2,  false },  /* 0  */
    { GPIOA, GPIO_PIN_3,  false },  /* 1  */
    { GPIOA, GPIO_PIN_5,  false },  /* 2  */
    { GPIOA, GPIO_PIN_7,  false },  /* 3  */
    { GPIOB, GPIO_PIN_8,  false },  /* 4  */
    { GPIOB, GPIO_PIN_9,  false },  /* 5  */
    { GPIOB, GPIO_PIN_10, false },  /* 6  */
    { GPIOB, GPIO_PIN_11, false },  /* 7  */
    { GPIOB, GPIO_PIN_12, false },  /* 8  */
    { GPIOB, GPIO_PIN_13, false },  /* 9  */
    { GPIOE, GPIO_PIN_2,  false },  /* 10 */
    { GPIOE, GPIO_PIN_3,  false },  /* 11 */
};
#define GPIO_SLOT_COUNT (sizeof(s_gpio_slots) / sizeof(s_gpio_slots[0]))

static bool api_gpio_claim(uint32_t pin_id)
{
    if (!perm_check(ORCA_PERM_GPIO, "gpio")) {
        return false;
    }
    if (pin_id >= GPIO_SLOT_COUNT || s_gpio_slots[pin_id].claimed) {
        return false;
    }
    s_gpio_slots[pin_id].claimed = true;
    return true;
}

static void api_gpio_release(uint32_t pin_id)
{
    if (!orca_appmgr_task_perm_allows(ORCA_PERM_GPIO)) {
        return;
    }
    if (pin_id < GPIO_SLOT_COUNT) {
        s_gpio_slots[pin_id].claimed = false;
    }
}

/*
 * set_mode/write/read проверяют право на каждом вызове, а не полагаются на
 * claim: пин адресуется глобальным pin_id, и модуль без права gpio мог бы
 * дотянуться до ноги, которую захватил кто-то другой, — флаг claimed один
 * на всю систему и о владельце ничего не знает.
 */
static void api_gpio_set_mode(uint32_t pin_id, orca_gpio_mode_t mode)
{
    if (!perm_check(ORCA_PERM_GPIO, "gpio")) {
        return;
    }
    if (pin_id >= GPIO_SLOT_COUNT || !s_gpio_slots[pin_id].claimed) {
        return;
    }

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin   = s_gpio_slots[pin_id].pin;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;

    switch (mode) {
        case ORCA_GPIO_OUTPUT:
            gpio.Mode = GPIO_MODE_OUTPUT_PP;
            gpio.Pull = GPIO_NOPULL;
            break;
        case ORCA_GPIO_INPUT_PULLUP:
            gpio.Mode = GPIO_MODE_INPUT;
            gpio.Pull = GPIO_PULLUP;
            break;
        case ORCA_GPIO_INPUT_PULLDOWN:
            gpio.Mode = GPIO_MODE_INPUT;
            gpio.Pull = GPIO_PULLDOWN;
            break;
        default:
            gpio.Mode = GPIO_MODE_INPUT;
            gpio.Pull = GPIO_NOPULL;
            break;
    }
    HAL_GPIO_Init(s_gpio_slots[pin_id].port, &gpio);
}

static void api_gpio_write(uint32_t pin_id, bool level)
{
    if (!perm_check(ORCA_PERM_GPIO, "gpio")) {
        return;
    }
    if (pin_id >= GPIO_SLOT_COUNT || !s_gpio_slots[pin_id].claimed) {
        return;
    }
    HAL_GPIO_WritePin(s_gpio_slots[pin_id].port, s_gpio_slots[pin_id].pin,
                      level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static bool api_gpio_read(uint32_t pin_id)
{
    if (!perm_check(ORCA_PERM_GPIO, "gpio")) {
        return false;
    }
    if (pin_id >= GPIO_SLOT_COUNT || !s_gpio_slots[pin_id].claimed) {
        return false;
    }
    return HAL_GPIO_ReadPin(s_gpio_slots[pin_id].port,
                            s_gpio_slots[pin_id].pin) == GPIO_PIN_SET;
}

static const orca_gpio_api_t s_gpio_api = {
    .claim    = api_gpio_claim,
    .release  = api_gpio_release,
    .set_mode = api_gpio_set_mode,
    .write    = api_gpio_write,
    .read     = api_gpio_read,
};

/* ------------------------------------------------------------------ */
/* uart — порт 0 отдан модулям (USART2, пины на P1)                   */
/* ------------------------------------------------------------------ */

extern bool     bsp_modem_uart_open(uint32_t port_id, uint32_t baudrate);
extern int32_t  bsp_modem_uart_read(uint32_t port_id, void* buf, uint32_t len, uint32_t timeout_ms);
extern int32_t  bsp_modem_uart_write(uint32_t port_id, const void* buf, uint32_t len);
extern void     bsp_modem_uart_close(uint32_t port_id);

#define UART_HANDLE_BASE 0x4A700000u
/*
 * Портов ровно столько, сколько реализовано в bsp_uart.c: один модемный
 * USART2 с port_id == 0. Диапазон валидных тегов обязан совпадать с этим
 * числом. Пока он был BASE+0..7, модуль мог передать хендл BASE+3 в read()
 * и получить не отказ, а вызов BSP с несуществующим портом — то есть ошибка
 * уезжала на уровень ниже, где port_id никто уже не проверяет.
 * При появлении второго порта менять здесь и в bsp_uart.c одновременно.
 */
#define UART_PORT_COUNT  1u

static void* api_uart_open(uint32_t port_id, uint32_t baudrate)
{
    if (port_id >= UART_PORT_COUNT) {
        return NULL;
    }
    if (!perm_check(ORCA_PERM_UART, "uart")) {
        return NULL;
    }
    if (!bsp_modem_uart_open(port_id, baudrate)) {
        return NULL;
    }
    /* Хендл — не указатель, а тег: модуль не должен получать адреса ядра. */
    return (void*)(uintptr_t)(UART_HANDLE_BASE + port_id);
}

static bool uart_handle_to_port(void* handle, uint32_t* port_id)
{
    uintptr_t tag = (uintptr_t)handle;
    if (tag < UART_HANDLE_BASE || tag >= UART_HANDLE_BASE + UART_PORT_COUNT) {
        return false;
    }
    *port_id = (uint32_t)(tag - UART_HANDLE_BASE);
    return true;
}

static int32_t api_uart_read(void* handle, void* buf, uint32_t len, uint32_t timeout_ms)
{
    uint32_t port;
    if (!uart_handle_to_port(handle, &port) || buf == NULL) {
        return -1;
    }
    return bsp_modem_uart_read(port, buf, len, timeout_ms);
}

static int32_t api_uart_write(void* handle, const void* buf, uint32_t len)
{
    uint32_t port;
    if (!uart_handle_to_port(handle, &port) || buf == NULL) {
        return -1;
    }
    return bsp_modem_uart_write(port, buf, len);
}

static void api_uart_close(void* handle)
{
    uint32_t port;
    if (uart_handle_to_port(handle, &port)) {
        bsp_modem_uart_close(port);
    }
}

static const orca_uart_api_t s_uart_api = {
    .open  = api_uart_open,
    .read  = api_uart_read,
    .write = api_uart_write,
    .close = api_uart_close,
};

/* ------------------------------------------------------------------ */
/* spi / i2c — заглушки до драйверов шин (Milestone 2)                */
/* ------------------------------------------------------------------ */

static void*   api_spi_open(uint32_t bus_id)
{
    (void)bus_id;
    if (!perm_check(ORCA_PERM_SPI, "spi")) {
        return NULL;
    }
    return NULL;
}
static int32_t api_spi_transfer(void* h, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
    (void)h; (void)tx; (void)rx; (void)len; return -1;
}
static void    api_spi_close(void* h) { (void)h; }

static const orca_spi_api_t s_spi_api = {
    .open     = api_spi_open,
    .transfer = api_spi_transfer,
    .close    = api_spi_close,
};

static void*   api_i2c_open(uint32_t bus_id)
{
    (void)bus_id;
    if (!perm_check(ORCA_PERM_I2C, "i2c")) {
        return NULL;
    }
    return NULL;
}
static int32_t api_i2c_write(void* h, uint16_t addr, const uint8_t* buf, uint32_t len)
{
    (void)h; (void)addr; (void)buf; (void)len; return -1;
}
static int32_t api_i2c_read(void* h, uint16_t addr, uint8_t* buf, uint32_t len)
{
    (void)h; (void)addr; (void)buf; (void)len; return -1;
}
static void    api_i2c_close(void* h) { (void)h; }

static const orca_i2c_api_t s_i2c_api = {
    .open  = api_i2c_open,
    .write = api_i2c_write,
    .read  = api_i2c_read,
    .close = api_i2c_close,
};

/* ------------------------------------------------------------------ */
/* gui — заглушки до GUI-движка (Milestone 2)                         */
/* ------------------------------------------------------------------ */

static void api_gui_clear(uint32_t d, uint32_t c)
{
    (void)d; (void)c;
    (void)perm_check(ORCA_PERM_GUI, "gui");
}
static void api_gui_pixel(uint32_t d, int32_t x, int32_t y, uint32_t c)
{
    (void)d; (void)x; (void)y; (void)c;
    (void)perm_check(ORCA_PERM_GUI, "gui");
}
static void api_gui_rect(uint32_t d, int32_t x, int32_t y, int32_t w, int32_t h,
                         uint32_t c, bool fill)
{
    (void)d; (void)x; (void)y; (void)w; (void)h; (void)c; (void)fill;
    (void)perm_check(ORCA_PERM_GUI, "gui");
}
static void api_gui_text(uint32_t d, int32_t x, int32_t y, const char* s, uint32_t c)
{
    (void)d; (void)x; (void)y; (void)s; (void)c;
    (void)perm_check(ORCA_PERM_GUI, "gui");
}
static void api_gui_flush(uint32_t d)
{
    (void)d;
    (void)perm_check(ORCA_PERM_GUI, "gui");
}
/*
 * Физического тачскрина нет, поэтому координаты приходят снаружи —
 * кадрами ORCA_LINK_TOUCH от tools/fbstream (--stream gui) или Android.
 * Для модуля это выглядит как обычный тач, так что код, написанный
 * сейчас, заработает и с реальной панелью без правок.
 */
static bool api_gui_poll_touch(uint32_t d, int32_t* x, int32_t* y)
{
    if (!perm_check(ORCA_PERM_GUI, "gui")) {
        return false;
    }
    orca_input_touch_t ev;
    if (!orca_input_pop_touch(&ev)) {
        return false;
    }
    if (ev.display_id != (uint8_t)d) {
        return false;
    }
    if (x != NULL) {
        *x = (int32_t)ev.x;
    }
    if (y != NULL) {
        *y = (int32_t)ev.y;
    }
    /* up отдаём как "нет касания": модулю нужен факт нажатия, не фронт. */
    return ev.action != 0;
}
static bool api_gui_poll_button(uint32_t button_id)
{
    if (!perm_check(ORCA_PERM_GUI, "gui")) {
        return false;
    }
    /* Локальная кнопка или удалённая с клиента — модулю без разницы. */
    return board_button_pressed((uint8_t)button_id) ||
           orca_input_button_state((uint8_t)button_id);
}

static const orca_gui_api_t s_gui_api = {
    .clear       = api_gui_clear,
    .pixel       = api_gui_pixel,
    .rect        = api_gui_rect,
    .text        = api_gui_text,
    .flush       = api_gui_flush,
    .poll_touch  = api_gui_poll_touch,
    .poll_button = api_gui_poll_button,
};

/* ------------------------------------------------------------------ */
/* system                                                             */
/* ------------------------------------------------------------------ */

static uint32_t api_system_battery_pct(void)
{
    /* Плата питается от USB 5V, батареи нет — всегда 100%. */
    return 100;
}

static uint64_t api_system_unix_time(void)
{
    /*
     * Всегда UTC, без учёта пояса: пояс — дело отображения, а модуль,
     * который меряет интервалы, не должен ловить прыжок часов при его
     * смене. Ноль означает "часов нет" (RTC не поднялся).
     */
    return board_rtc_unix();
}

static void api_system_get_device_info(char* buf, uint32_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return;
    }
    snprintf(buf, buf_len, "orca-h743 api=%u sdram=32M qspi=16M",
             (unsigned)ORCA_API_VERSION);
}

static void api_system_mem_info(orca_mem_info_t* out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->sdram_total = ORCA_SDRAM_SIZE;
    out->arena_total = ORCA_APP_ARENA_SIZE;
    /* Без SDRAM арены нет вовсе — не показываем 26 МБ, которых не существует. */
    if (orca_arena_ready()) {
        out->arena_free    = orca_arena_free_bytes();
        out->arena_largest = orca_arena_largest_free_block();
    }
    out->kernel_heap_free     = (uint32_t)xPortGetFreeHeapSize();
    out->kernel_heap_min_free = (uint32_t)xPortGetMinimumEverFreeHeapSize();
}

static void api_system_reboot(void)
{
    /*
     * Единственная функция группы system, которая что-то меняет, — поэтому
     * право спрашивается только здесь. battery/unix_time/mem_info/
     * get_device_info/api_version отдают справочные величины и никому не
     * вредят: запирать их значило бы заставлять каждый модуль просить
     * "system" ради того, чтобы узнать время.
     */
    if (!perm_check(ORCA_PERM_SYSTEM, "system")) {
        return;
    }
    NVIC_SystemReset();
}

static uint32_t api_system_api_version(void)
{
    return ORCA_API_VERSION;
}

static const orca_system_api_t s_system_api = {
    .battery_pct     = api_system_battery_pct,
    .unix_time       = api_system_unix_time,
    .get_device_info = api_system_get_device_info,

    .mem_info    = api_system_mem_info,
    .reboot      = api_system_reboot,
    .api_version = api_system_api_version,
};

/* ------------------------------------------------------------------ */
/* ipc — заглушка до реализации шины сообщений                        */
/* ------------------------------------------------------------------ */

static int32_t api_ipc_send(const char* target, const void* data, uint32_t len)
{
    (void)target; (void)data; (void)len;
    if (!perm_check(ORCA_PERM_IPC, "ipc")) {
        return -1;
    }
    return -1;
}
static int32_t api_ipc_recv(void* buf, uint32_t buf_len, uint32_t timeout_ms)
{
    (void)buf; (void)buf_len; (void)timeout_ms;
    if (!perm_check(ORCA_PERM_IPC, "ipc")) {
        return -1;
    }
    return -1;
}

static const orca_ipc_api_t s_ipc_api = {
    .send = api_ipc_send,
    .recv = api_ipc_recv,
};

/* ------------------------------------------------------------------ */
/* console — вывод команды возвращается тому, кто её набрал            */
/* ------------------------------------------------------------------ */

static void api_console_write(const char* s)
{
    orca_shell_out_write(s);
}

static void api_console_writef(const char* fmt, ...)
{
    if (fmt == NULL) {
        return;
    }
    /*
     * Форматируем здесь, а не пробрасываем va_list: у orca_shell_out_printf
     * variadic-сигнатура, и передать ей va_list напрямую нельзя.
     */
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        orca_shell_out_write(buf);
    }
}

static void api_console_flush(void)
{
    /*
     * Буфер отдаётся целиком после возврата из команды (см. orca_shell_exec),
     * поэтому промежуточный flush не нужен. Оставлен в ABI, чтобы позже
     * появился стриминг длинного вывода без смены версии API.
     */
}

static const orca_console_api_t s_console_api = {
    .write  = api_console_write,
    .writef = api_console_writef,
    .flush  = api_console_flush,
};

/* ------------------------------------------------------------------ */
/* Таблица целиком                                                    */
/* ------------------------------------------------------------------ */

const orca_api_t g_orca_api = {
    .version = ORCA_API_VERSION,
    .log     = &s_log_api,
    .task    = &s_task_api,
    .storage = &s_storage_api,
    .gpio    = &s_gpio_api,
    .uart    = &s_uart_api,
    .spi     = &s_spi_api,
    .i2c     = &s_i2c_api,
    .gui     = &s_gui_api,
    .system  = &s_system_api,
    .ipc     = &s_ipc_api,
    .console = &s_console_api,
};
