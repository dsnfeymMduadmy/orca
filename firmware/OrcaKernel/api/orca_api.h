#ifndef ORCA_API_H
#define ORCA_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Стабильный ABI между ядром Orca и загружаемыми модулями/приложениями.
 * Правила:
 *   - никогда не менять порядок/сигнатуры существующих полей;
 *   - новые функции добавлять только в конец соответствующей подтаблицы;
 *   - при несовместимых изменениях — увеличивать ORCA_API_VERSION,
 *     loader откажет в запуске модулей со старым api_version из манифеста.
 */
#define ORCA_API_VERSION 3u

typedef enum {
    ORCA_LOG_INFO = 0,
    ORCA_LOG_WARN,
    ORCA_LOG_ERROR
} orca_log_level_t;

typedef struct {
    void (*write)(orca_log_level_t level, const char* tag, const char* msg);
    void (*info)(const char* tag, const char* msg);
    void (*warn)(const char* tag, const char* msg);
    void (*error)(const char* tag, const char* msg);
} orca_log_api_t;

typedef struct {
    void (*sleep_ms)(uint32_t ms);
    void (*yield)(void);
    bool (*should_stop)(void);
    uint32_t (*uptime_ms)(void);
} orca_task_api_t;

typedef enum {
    ORCA_FILE_READ = 0,
    ORCA_FILE_WRITE,
    ORCA_FILE_APPEND
} orca_file_mode_t;

/*
 * Метаданные файла/каталога. Плоские POD-структуры без указателей на
 * внутренности ядра: модуль получает копию, а не вид на FatFs.
 */
#define ORCA_NAME_MAX 64u

typedef struct {
    char     name[ORCA_NAME_MAX];
    uint32_t size;
    bool     is_dir;
    uint16_t mdate;            /* FAT-формат: год-1980<<9 | месяц<<5 | день */
    uint16_t mtime;            /* час<<11 | минута<<5 | секунда/2          */
} orca_dirent_t;

/*
 * Размеры тома — 64-битные. Карта на 8/16/32 ГБ в uint32_t не помещается:
 * кластеры * размер кластера переполняются, и модуль получал бы остаток от
 * деления на 4 ГБ вместо объёма.
 */
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t cluster_bytes;    /* кластер всегда мал, 32 бит хватает */
    char     label[16];
} orca_fsinfo_t;

typedef struct {
    void*   (*open)(const char* path, orca_file_mode_t mode);
    int32_t (*read)(void* handle, void* buf, uint32_t len);
    int32_t (*write)(void* handle, const void* buf, uint32_t len);
    int32_t (*seek)(void* handle, int32_t offset, int32_t whence);
    void    (*close)(void* handle);
    bool    (*exists)(const char* path);
    bool    (*mkdir)(const char* path);

    /* --- добавлено в API v2 (нужно командам с SD: ls/rm/cp/mv/df) --- */
    void*    (*opendir)(const char* path);
    bool     (*readdir)(void* dir, orca_dirent_t* out);
    void     (*closedir)(void* dir);
    bool     (*stat)(const char* path, orca_dirent_t* out);
    bool     (*remove)(const char* path);
    bool     (*rename)(const char* from, const char* to);
    bool     (*fsinfo)(const char* drive, orca_fsinfo_t* out);
    uint32_t (*size)(void* handle);
} orca_storage_api_t;

typedef enum {
    ORCA_GPIO_INPUT = 0,
    ORCA_GPIO_OUTPUT,
    ORCA_GPIO_INPUT_PULLUP,
    ORCA_GPIO_INPUT_PULLDOWN
} orca_gpio_mode_t;

typedef struct {
    bool (*claim)(uint32_t pin_id);
    void (*release)(uint32_t pin_id);
    void (*set_mode)(uint32_t pin_id, orca_gpio_mode_t mode);
    void (*write)(uint32_t pin_id, bool level);
    bool (*read)(uint32_t pin_id);
} orca_gpio_api_t;

typedef struct {
    void*   (*open)(uint32_t port_id, uint32_t baudrate);
    int32_t (*read)(void* handle, void* buf, uint32_t len, uint32_t timeout_ms);
    int32_t (*write)(void* handle, const void* buf, uint32_t len);
    void    (*close)(void* handle);
} orca_uart_api_t;

typedef struct {
    void*   (*open)(uint32_t bus_id);
    int32_t (*transfer)(void* handle, const uint8_t* tx, uint8_t* rx, uint32_t len);
    void    (*close)(void* handle);
} orca_spi_api_t;

typedef struct {
    void*   (*open)(uint32_t bus_id);
    int32_t (*write)(void* handle, uint16_t addr, const uint8_t* buf, uint32_t len);
    int32_t (*read)(void* handle, uint16_t addr, uint8_t* buf, uint32_t len);
    void    (*close)(void* handle);
} orca_i2c_api_t;

typedef struct {
    void (*clear)(uint32_t display_id, uint32_t color);
    void (*pixel)(uint32_t display_id, int32_t x, int32_t y, uint32_t color);
    void (*rect)(uint32_t display_id, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, bool fill);
    void (*text)(uint32_t display_id, int32_t x, int32_t y, const char* str, uint32_t color);
    void (*flush)(uint32_t display_id);
    bool (*poll_touch)(uint32_t display_id, int32_t* x, int32_t* y);
    bool (*poll_button)(uint32_t button_id);
} orca_gui_api_t;

typedef struct {
    uint32_t sdram_total;
    uint32_t arena_total;
    uint32_t arena_free;
    uint32_t arena_largest;
    uint32_t kernel_heap_free;
    uint32_t kernel_heap_min_free;
} orca_mem_info_t;

typedef struct {
    uint32_t (*battery_pct)(void);
    uint64_t (*unix_time)(void);
    void     (*get_device_info)(char* buf, uint32_t buf_len);

    /* --- добавлено в API v2 --- */
    void     (*mem_info)(orca_mem_info_t* out);
    void     (*reboot)(void);
    uint32_t (*api_version)(void);
} orca_system_api_t;

typedef struct {
    int32_t (*send)(const char* target, const void* data, uint32_t len);
    int32_t (*recv)(void* buf, uint32_t buf_len, uint32_t timeout_ms);
} orca_ipc_api_t;

/*
 * Консоль команды. Появилась в API v2 вместе с командами на SD-карте
 * (0:/commands/<имя>): команда — это обычный .orca-модуль, но её вывод
 * должен вернуться тому, кто набрал строку (UART-консоль или Orca Link),
 * а не уйти в общий лог. Поэтому write/writef пишут в буфер вызова, а не
 * в printf.
 *
 * Реализация в ядре — orca_shell_out_*(), см. orca_cmdfs.c.
 */
typedef struct {
    void (*write)(const char* s);
    void (*writef)(const char* fmt, ...);
    void (*flush)(void);
} orca_console_api_t;

typedef struct {
    uint32_t                   version;   /* == ORCA_API_VERSION */
    const orca_log_api_t*      log;
    const orca_task_api_t*     task;
    const orca_storage_api_t*  storage;
    const orca_gpio_api_t*     gpio;
    const orca_uart_api_t*     uart;
    const orca_spi_api_t*      spi;
    const orca_i2c_api_t*      i2c;
    const orca_gui_api_t*      gui;
    const orca_system_api_t*   system;
    const orca_ipc_api_t*      ipc;

    /* --- добавлено в API v2 --- */
    const orca_console_api_t*  console;
} orca_api_t;

/* Точка входа, которую обязан экспортировать каждый модуль/приложение. */
typedef int  (*orca_app_main_fn)(const orca_api_t* api);
/* Опциональная точка остановки. */
typedef void (*orca_app_stop_fn)(void);

/*
 * Точка входа команды shell'а (файл в 0:/commands/). В отличие от
 * orca_app_main команда получает argv, выполняется синхронно в задаче
 * вызывающего и обязана вернуть управление — своей задачи у неё нет.
 * Код возврата становится статусом команды ($? в Orca Link).
 */
typedef int  (*orca_cmd_main_fn)(const orca_api_t* api, int argc, char** argv);

#define ORCA_APP_MAIN_SYMBOL "orca_app_main"
#define ORCA_APP_STOP_SYMBOL "orca_app_stop"
#define ORCA_CMD_MAIN_SYMBOL "orca_cmd_main"

#ifdef __cplusplus
}
#endif

#endif /* ORCA_API_H */
