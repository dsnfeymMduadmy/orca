#include "orca_shell.h"
#include "orca_cmdfs.h"
#include "orca_appmgr.h"
#include "orca_arena.h"
#include "orca_memmap.h"
#include "orca_fbstream.h"
#include "orca_link_service.h"
#include "orca_fbtest.h"
#include "orca_lvgl.h"
#include "orca_ui.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32h7xx_hal.h"
#include "ff.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define SHELL_MAX_ARGS   12

static const orca_shell_io_t* s_io;
static char s_out[SHELL_OUT_MAX];
static uint32_t s_out_pos;

/*
 * Задача, которая сейчас выполняет команду и, значит, владеет s_out.
 * Буфер один на весь shell, а console->write доступен любому модулю —
 * включая долгоживущее приложение из `run`, которое живёт в своей задаче
 * и пишет когда захочет. Без владельца такой модуль затирал вывод команды,
 * набранной в консоли, и мог порвать s_out_pos посреди vsnprintf.
 */
static TaskHandle_t s_out_owner;

static void out_reset(void)
{
    s_out_pos = 0;
    s_out[0] = '\0';
}

static void out_vprintf(const char* fmt, va_list ap)
{
    if (s_out_pos >= SHELL_OUT_MAX - 1) {
        return;
    }
    int n = vsnprintf(&s_out[s_out_pos], SHELL_OUT_MAX - s_out_pos, fmt, ap);
    if (n > 0) {
        /*
         * vsnprintf возвращает длину, которая была бы записана без обрезки,
         * поэтому s_out_pos надо ограничивать явно — иначе указатель уезжает
         * за буфер и следующий вызов пишет мимо.
         */
        s_out_pos += (uint32_t)n;
        if (s_out_pos > SHELL_OUT_MAX - 1) {
            s_out_pos = SHELL_OUT_MAX - 1;
        }
    }
}

static void out_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_vprintf(fmt, ap);
    va_end(ap);
}

/*
 * Публичные обёртки — через них пишут команды с SD-карты (orca_console_api_t).
 *
 * Пишет только задача, которая сейчас внутри orca_shell_exec. Модуль,
 * запущенный через `run`, консоли не принадлежит: его вывод уходит в
 * системный лог (api->log), а не в чужой ответ на команду.
 */
static bool out_owned_by_caller(void)
{
    return s_out_owner != NULL && s_out_owner == xTaskGetCurrentTaskHandle();
}

void orca_shell_out_write(const char* s)
{
    if (s == NULL || !out_owned_by_caller()) {
        return;
    }
    out_printf("%s", s);
}

void orca_shell_out_printf(const char* fmt, ...)
{
    if (fmt == NULL || !out_owned_by_caller()) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    out_vprintf(fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Встроенные команды                                                 */
/*                                                                    */
/* В ядре остаётся только то, что нельзя выгрузить на карту: работа с */
/* самим менеджером модулей (run/ps/kill), транспортом (fb/link) и    */
/* железом (reboot). Всё остальное — файлы в 0:/commands.             */
/* ------------------------------------------------------------------ */

static void help_list_cb(const char* name, uint32_t size, void* ctx)
{
    uint32_t* col = (uint32_t*)ctx;
    (void)size;
    out_printf("%s%-12s", (*col == 0) ? "  " : "", name);
    if (++(*col) == 4) {
        out_printf("\r\n");
        *col = 0;
    }
}

static int cmd_help(int argc, char** argv)
{
    (void)argc; (void)argv;
    out_printf(
        "Orca shell — встроенные команды:\r\n"
        "  run <path> [stack]  загрузить и запустить .orca\r\n"
        "  ps                  задачи ядра и запущенные модули\r\n"
        "  kill <id>           остановить инстанс\r\n"
        "  fb <on|off|status|src>  трансляция экрана на ПК\r\n"
        "  link                статистика связи с ПК\r\n"
        "  gui [reload|стр.]   страницы интерфейса с карты\r\n"
        "  date [set|unix|tz]  часы (локальное время, пояс)\r\n"
        "  reboot              перезагрузка\r\n"
        "  help                эта справка\r\n"
        "\r\nКоманды с карты (" ORCA_CMDFS_DIR "):\r\n");

    uint32_t col = 0;
    uint32_t n = orca_cmdfs_list(help_list_cb, &col);
    if (col != 0) {
        out_printf("\r\n");
    }
    if (n == 0) {
        out_printf("  (нет — каталог пуст или карта не смонтирована)\r\n");
    }
    return 0;
}

static int cmd_run(int argc, char** argv)
{
    if (argc < 2) {
        out_printf("usage: run <path.orca> [stack_bytes]\r\n");
        return -1;
    }
    uint32_t stack = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0;

    int32_t id = orca_appmgr_start(argv[1], stack);
    if (id < 0) {
        out_printf("run: не удалось запустить '%s'\r\n", argv[1]);
        return -1;
    }
    out_printf("запущено, instance id = %ld\r\n", (long)id);
    return 0;
}

static const char* task_state_name(eTaskState st)
{
    switch (st) {
        case eRunning:   return "run";
        case eReady:     return "ready";
        case eBlocked:   return "block";
        case eSuspended: return "susp";
        case eDeleted:   return "del";
        default:         return "?";
    }
}

/*
 * Задачи ядра, а не только загруженные модули.
 *
 * Пока `ps` показывал один appmgr, «интерфейс не рисует» было невозможно
 * отличить от «задача gui не создалась», «она вечно в block» и «её вытесняет
 * кто-то с приоритетом выше». Состояние и свободный стек отвечают на это
 * сразу. Стек — в словах, как в xTaskCreate, чтобы сравнивать с заказанным.
 */
static void ps_kernel_tasks(void)
{
    static TaskStatus_t tasks[16];
    UBaseType_t n = uxTaskGetSystemState(tasks, 16, NULL);

    out_printf("задачи ядра (%lu), куча %lu КБ свободно (минимум %lu КБ):\r\n",
               (unsigned long)n,
               (unsigned long)(xPortGetFreeHeapSize() / 1024u),
               (unsigned long)(xPortGetMinimumEverFreeHeapSize() / 1024u));
    out_printf("  %-16s %-6s %-4s %-8s\r\n", "TASK", "STATE", "PRI", "STACK");
    for (UBaseType_t i = 0; i < n; i++) {
        out_printf("  %-16s %-6s %-4lu %-8lu\r\n",
                   tasks[i].pcTaskName,
                   task_state_name(tasks[i].eCurrentState),
                   (unsigned long)tasks[i].uxCurrentPriority,
                   (unsigned long)tasks[i].usStackHighWaterMark);
    }
}

static int cmd_ps(int argc, char** argv)
{
    (void)argc; (void)argv;

    ps_kernel_tasks();

    static orca_app_instance_t list[ORCA_APPMGR_MAX_RUNNING];
    uint32_t n = orca_appmgr_list(list, ORCA_APPMGR_MAX_RUNNING);

    if (n == 0) {
        out_printf("\r\nнет запущенных инстансов\r\n");
        return 0;
    }

    out_printf("\r\n");

    out_printf("%-4s %-20s %-8s %-10s\r\n", "ID", "NAME", "SIZE", "UPTIME_MS");
    uint32_t now = (uint32_t)xTaskGetTickCount();
    for (uint32_t i = 0; i < n; i++) {
        /*
         * Печатаем list[i].id, а не i: список сжатый, и после kill первого
         * инстанса индекс в нём перестаёт совпадать с номером слота, который
         * ждёт orca_appmgr_stop(). Раньше `ps` показывал 0, а `kill 0` бил мимо.
         */
        out_printf("%-4ld %-20s %-8lu %-10lu\r\n",
                   (long)list[i].id,
                   list[i].name,
                   (unsigned long)list[i].module.image_size,
                   (unsigned long)(now - list[i].start_time_ms));
    }
    return 0;
}

static int cmd_kill(int argc, char** argv)
{
    if (argc < 2) {
        out_printf("usage: kill <id>\r\n");
        return -1;
    }
    int32_t id = (int32_t)strtol(argv[1], NULL, 0);
    if (!orca_appmgr_stop(id)) {
        out_printf("kill: инстанс %ld не найден или уже остановлен\r\n", (long)id);
        return -1;
    }
    out_printf("инстанс %ld остановлен\r\n", (long)id);
    return 0;
}

static const char* fb_source_name(uint8_t src)
{
    return (src == (uint8_t)ORCA_FB_SRC_TEST) ? "test" : "gui";
}

static int cmd_fb(int argc, char** argv)
{
    if (argc < 2) {
        out_printf("usage: fb <on|off|status|src [gui|test]>\r\n");
        return -1;
    }

    if (strcmp(argv[1], "on") == 0) {
        orca_fbstream_enable(true);
        out_printf("fbstream: включён, источник %s\r\n",
                   fb_source_name((uint8_t)orca_fbstream_get_source()));
    } else if (strcmp(argv[1], "off") == 0) {
        orca_fbstream_enable(false);
        out_printf("fbstream: выключен\r\n");
    } else if (strcmp(argv[1], "src") == 0) {
        /*
         * Переключатель источника кадров. Транслировать можно либо реальный
         * интерфейс на LVGL, либо тестовую картинку — одновременно нельзя,
         * иначе на ПК летят кадры от двух продюсеров вперемешку.
         */
        if (argc < 3) {
            out_printf("источник: %s (usage: fb src <gui|test>)\r\n",
                       fb_source_name((uint8_t)orca_fbstream_get_source()));
            return 0;
        }
        if (strcmp(argv[2], "gui") == 0) {
            orca_fbstream_set_source(ORCA_FB_SRC_GUI);
        } else if (strcmp(argv[2], "test") == 0) {
            orca_fbstream_set_source(ORCA_FB_SRC_TEST);
        } else {
            out_printf("fb src: ожидается gui или test\r\n");
            return -1;
        }
        out_printf("источник кадров: %s\r\n",
                   fb_source_name((uint8_t)orca_fbstream_get_source()));
    } else if (strcmp(argv[1], "status") == 0) {
        orca_fbstream_stats_t st;
        orca_fbstream_get_stats(&st);
        out_printf("fbstream:   %s, источник %s\r\n",
                   st.enabled ? "on" : "off", fb_source_name(st.source));
        out_printf("  формат:   pixfmt=0x%02X scale=%u max_fps=%u\r\n",
                   st.pixfmt, st.scale, st.max_fps);
        out_printf("  отправлено: %lu кадров, %lu байт\r\n",
                   (unsigned long)st.frames_sent,
                   (unsigned long)st.bytes_sent);
        out_printf("  дропнуто:   %lu\r\n", (unsigned long)st.frames_dropped);
        out_printf("  отрисовано: %lu (gui), %lu (test)\r\n",
                   (unsigned long)orca_lvgl_frame_count(),
                   (unsigned long)orca_fbtest_frame_count());
        out_printf("  витков:    %lu (gui), %lu (test)\r\n",
                   (unsigned long)orca_lvgl_loop_count(),
                   (unsigned long)orca_fbtest_loop_count());
        out_printf("  куча LVGL:  %lu КБ занято\r\n",
                   (unsigned long)(orca_lvgl_heap_used() / 1024u));
    } else {
        out_printf("fb: неизвестный аргумент '%s'\r\n", argv[1]);
        return -1;
    }
    return 0;
}

static int cmd_link(int argc, char** argv)
{
    (void)argc; (void)argv;
    orca_link_service_stats_t st;
    orca_link_service_get_stats(&st);

    out_printf("Orca Link (USART1 921600 8N1)\r\n");
    out_printf("  клиент:     %s\r\n",
               st.peer_seen ? "был на связи" : "не подключался");
    out_printf("  кадров rx:  %lu\r\n", (unsigned long)st.frames_rx);
    out_printf("  кадров tx:  %lu\r\n", (unsigned long)st.frames_tx);
    out_printf("  CRC ошибок: %lu\r\n", (unsigned long)st.crc_errors);
    out_printf("  ресинхрон:  %lu\r\n", (unsigned long)st.resyncs);
    out_printf("  байт в shell: %lu\r\n", (unsigned long)st.shell_bytes);
    if (st.peer_seen) {
        uint32_t now = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        out_printf("  последний кадр: %lu мс назад\r\n",
                   (unsigned long)(now - st.last_rx_ms));
    }
    return 0;
}

/*
 * Часы. В консоли всё локальное (UTC + смещение из backup-регистра):
 * человек читает время, а не unix-секунды. Выставлять тоже разрешаем в
 * локальном — заставлять пересчитывать UTC в голове значит гарантированно
 * получить часы, сдвинутые на величину пояса.
 */
static const char* const s_weekday_ru[7] = {
    "вс", "пн", "вт", "ср", "чт", "пт", "сб"
};

static bool date_parse_ymd(const char* s, orca_datetime_t* dt)
{
    char* end;
    unsigned long y = strtoul(s, &end, 10);
    if (*end != '-') {
        return false;
    }
    unsigned long m = strtoul(end + 1, &end, 10);
    if (*end != '-') {
        return false;
    }
    unsigned long d = strtoul(end + 1, &end, 10);
    if (*end != '\0') {
        return false;
    }
    dt->year  = (uint16_t)y;
    dt->month = (uint8_t)m;
    dt->day   = (uint8_t)d;
    return true;
}

static bool date_parse_hms(const char* s, orca_datetime_t* dt)
{
    char* end;
    unsigned long h = strtoul(s, &end, 10);
    if (*end != ':') {
        return false;
    }
    unsigned long mi = strtoul(end + 1, &end, 10);
    unsigned long sec = 0;
    /* Секунды необязательны: обычно ставят время с точностью до минуты. */
    if (*end == ':') {
        sec = strtoul(end + 1, &end, 10);
    }
    if (*end != '\0') {
        return false;
    }
    dt->hour   = (uint8_t)h;
    dt->minute = (uint8_t)mi;
    dt->second = (uint8_t)sec;
    return true;
}

static void date_print(void)
{
    uint64_t utc = board_rtc_unix();
    int32_t  tz  = board_rtc_tz_offset();
    /* Через int64: при отрицательном поясе и дате около эпохи uint64 уходит в минус. */
    uint64_t local = (uint64_t)((int64_t)utc + (int64_t)tz * 60);

    orca_datetime_t lt, ut;
    board_rtc_split(local, &lt);
    board_rtc_split(utc, &ut);

    int32_t tza = (tz < 0) ? -tz : tz;
    out_printf("%04u-%02u-%02u %02u:%02u:%02u %s (UTC%c%02ld:%02ld)\r\n",
               lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second,
               s_weekday_ru[lt.weekday % 7],
               (tz < 0) ? '-' : '+', (long)(tza / 60), (long)(tza % 60));
    out_printf("  UTC:  %04u-%02u-%02u %02u:%02u:%02u\r\n",
               ut.year, ut.month, ut.day, ut.hour, ut.minute, ut.second);
    out_printf("  unix: %lu\r\n", (unsigned long)utc);
    if (!board_rtc_time_valid()) {
        out_printf("  время не выставлено (идёт от заводской даты)\r\n");
    }
}

static int cmd_date(int argc, char** argv)
{
    if (!board_rtc_ready()) {
        out_printf("date: RTC не поднялся\r\n");
        return -1;
    }

    if (argc == 1) {
        date_print();
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        orca_datetime_t dt = {0};
        if (argc < 3 || !date_parse_ymd(argv[2], &dt) ||
            (argc > 3 && !date_parse_hms(argv[3], &dt))) {
            out_printf("usage: date set <ГГГГ-ММ-ДД> [ЧЧ:ММ[:СС]]\r\n");
            return -1;
        }
        uint64_t local = board_rtc_make_unix(&dt);
        if (local == 0) {
            out_printf("date: некорректная дата/время\r\n");
            return -1;
        }
        /* Введено локальное время — в RTC уходит UTC. */
        int64_t utc = (int64_t)local - (int64_t)board_rtc_tz_offset() * 60;
        if (utc < 0 || !board_rtc_set_unix((uint64_t)utc)) {
            out_printf("date: не удалось выставить время\r\n");
            return -1;
        }
        date_print();
        return 0;
    }

    if (strcmp(argv[1], "unix") == 0) {
        /* Для скриптов с ПК: секунды UTC без разбора часовых поясов. */
        if (argc < 3) {
            out_printf("usage: date unix <секунды UTC>\r\n");
            return -1;
        }
        if (!board_rtc_set_unix(strtoull(argv[2], NULL, 10))) {
            out_printf("date: не удалось выставить время\r\n");
            return -1;
        }
        date_print();
        return 0;
    }

    if (strcmp(argv[1], "tz") == 0) {
        if (argc < 3) {
            out_printf("usage: date tz <смещение в минутах, напр. 180>\r\n");
            return -1;
        }
        if (!board_rtc_set_tz_offset((int32_t)strtol(argv[2], NULL, 10))) {
            out_printf("date: смещение вне диапазона -1440..1440\r\n");
            return -1;
        }
        date_print();
        return 0;
    }

    out_printf("usage: date [set <ГГГГ-ММ-ДД> [ЧЧ:ММ[:СС]] | unix <сек> | tz <мин>]\r\n");
    return -1;
}

/*
 * Интерфейс лежит на карте страницами (0:/gui), поэтому им нужна не «настройка»,
 * а перечитывание: поправил файл — `gui reload`, и экран пересобрался без
 * перезагрузки. Без аргументов команда отвечает, что показано сейчас и откуда
 * это взято, — «правлю файл, а ничего не меняется» чаще всего означает, что
 * страница взята из встроенной копии, потому что на карте её нет.
 */
static int cmd_gui(int argc, char** argv)
{
    if (argc < 2) {
        out_printf("страница:   %s (%s)\r\n", orca_ui_current(),
                   orca_ui_from_card() ? ORCA_UI_DIR : "встроенная копия");
        out_printf("usage: gui <reload|страница>\r\n");
        return 0;
    }

    if (strcmp(argv[1], "reload") == 0) {
        if (!orca_ui_reload()) {
            out_printf("gui: интерфейс не запущен\r\n");
            return -1;
        }
        out_printf("страница '%s' перечитывается\r\n", orca_ui_current());
        return 0;
    }

    if (!orca_ui_open(argv[1])) {
        out_printf("gui: страницы '%s' нет ни в " ORCA_UI_DIR
                   ", ни в прошивке\r\n", argv[1]);
        return -1;
    }
    out_printf("переход на '%s'\r\n", argv[1]);
    return 0;
}

static int cmd_reboot(int argc, char** argv)
{
    (void)argc; (void)argv;
    out_printf("перезагрузка...\r\n");
    if (s_io != NULL) {
        s_io->write((const uint8_t*)s_out, s_out_pos);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    NVIC_SystemReset();
    return 0;
}

/*
 * ls <каталог> — список файлов и подкаталогов. Android-клиент использует
 * `ls 0:/apps` для построения списка приложений; формат вывода — по одному
 * имени в строке, каталоги помечаются суффиксом '/'.
 */
static int cmd_ls(int argc, char** argv)
{
    const char* dir = (argc >= 2) ? argv[1] : "0:/";

    DIR d;
    if (f_opendir(&d, dir) != FR_OK) {
        out_printf("ls: не открыть '%s'\r\n", dir);
        return -1;
    }

    FILINFO fno;
    uint32_t n = 0;
    for (;;) {
        if (f_readdir(&d, &fno) != FR_OK || fno.fname[0] == '\0') {
            break;
        }
        if (fno.fattrib & AM_DIR) {
            out_printf("%s/\r\n", fno.fname);
        } else {
            out_printf("%s\r\n", fno.fname);
        }
        n++;
    }
    f_closedir(&d);

    if (n == 0) {
        out_printf("(пусто)\r\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Диспетчер                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* name;
    int (*fn)(int argc, char** argv);
} shell_cmd_t;

static const shell_cmd_t s_cmds[] = {
    { "help",   cmd_help   },
    { "run",    cmd_run    },
    { "ps",     cmd_ps     },
    { "kill",   cmd_kill   },
    { "ls",     cmd_ls     },
    { "fb",     cmd_fb     },
    { "link",   cmd_link   },
    { "gui",    cmd_gui    },
    { "date",   cmd_date   },
    { "reboot", cmd_reboot },
};
#define SHELL_CMD_COUNT (sizeof(s_cmds) / sizeof(s_cmds[0]))

static int tokenize(char* line, char** argv, int max_args)
{
    int argc = 0;
    char* p = line;
    while (*p != '\0' && argc < max_args) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    return argc;
}

int orca_shell_exec(const char* line, char* out_buf, uint32_t out_len)
{
    char work[SHELL_LINE_MAX];
    char* argv[SHELL_MAX_ARGS];

    strncpy(work, line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    out_reset();
    s_out_owner = xTaskGetCurrentTaskHandle();

    int argc = tokenize(work, argv, SHELL_MAX_ARGS);
    if (argc == 0) {
        s_out_owner = NULL;
        if (out_buf != NULL && out_len > 0) {
            out_buf[0] = '\0';
        }
        return 0;
    }

    int rc = -1;
    bool found = false;
    for (uint32_t i = 0; i < SHELL_CMD_COUNT; i++) {
        if (strcmp(s_cmds[i].name, argv[0]) == 0) {
            rc = s_cmds[i].fn(argc, argv);
            found = true;
            break;
        }
    }

    if (!found) {
        /* Не встроенная — ищем на карте: 0:/commands/<argv[0]>.orca */
        orca_cmdfs_status_t st = orca_cmdfs_exec(argc, argv, &rc);
        if (st == ORCA_CMDFS_OK) {
            found = true;
        } else if (st != ORCA_CMDFS_NOT_FOUND) {
            out_printf("%s: %s\r\n", argv[0], orca_cmdfs_status_str(st));
            found = true;
            rc = -1;
        }
    }

    if (!found) {
        out_printf("неизвестная команда '%s' (help — список)\r\n", argv[0]);
    }

    if (out_buf != NULL && out_len > 0) {
        strncpy(out_buf, s_out, out_len - 1);
        out_buf[out_len - 1] = '\0';
    }

    s_out_owner = NULL;
    return rc;
}

void orca_shell_init(const orca_shell_io_t* io)
{
    s_io = io;
    out_reset();
}

static const char s_banner[] =
    "\r\n"
    "  ___                 ___  ___\r\n"
    " / _ \\ _ __ __ _ __ _/ _ \\/ __|\r\n"
    "| (_) | '__/ _` / _` | (_) \\__ \\\r\n"
    " \\___/|_|  \\__,_\\__,_|\\___/|___/\r\n"
    "\r\nOrca OS shell. 'help' — список команд.\r\n";

void orca_shell_greet(void)
{
    if (s_io == NULL) {
        return;
    }
    s_io->write((const uint8_t*)s_banner, (uint32_t)strlen(s_banner));
    s_io->write((const uint8_t*)"orca> ", 6);
}

/*
 * Построчный ввод с эхом. Состояние статическое: вызывающий поток всегда
 * один (владелец RX — см. orca_link_service.c), конкуренции нет.
 */
void orca_shell_feed_byte(uint8_t ch)
{
    static char     line[SHELL_LINE_MAX];
    static uint32_t pos;

    if (s_io == NULL) {
        return;
    }

    if (ch == '\r' || ch == '\n') {
        s_io->write((const uint8_t*)"\r\n", 2);
        line[pos] = '\0';
        if (pos > 0) {
            orca_shell_exec(line, NULL, 0);
            s_io->write((const uint8_t*)s_out, s_out_pos);
        }
        pos = 0;
        s_io->write((const uint8_t*)"orca> ", 6);
    } else if (ch == 0x08 || ch == 0x7F) {
        if (pos > 0) {
            pos--;
            s_io->write((const uint8_t*)"\b \b", 3);
        }
    } else if (ch >= 0x20 && ch < 0x7F && pos < SHELL_LINE_MAX - 1) {
        line[pos++] = (char)ch;
        s_io->write(&ch, 1);
    }
}

void orca_shell_task(void* arg)
{
    (void)arg;

    orca_shell_greet();

    for (;;) {
        uint8_t ch;
        if (s_io->read(&ch, 1, portMAX_DELAY) == 1) {
            orca_shell_feed_byte(ch);
        }
    }
}
