#include "main.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"
#include "orca_appmgr.h"
#include "orca_api.h"
#include "orca_shell.h"
#include "orca_fbstream.h"
#include "orca_link_service.h"
#include "orca_usb_msc.h"
#include "orca_fbtest.h"
#include "orca_lvgl.h"
#include "orca_panic.h"
#include <stdio.h>

extern const orca_api_t                g_orca_api;
extern const orca_shell_io_t           g_uart_shell_io;
extern const orca_link_service_io_t    g_uart_link_io;
extern const orca_fbstream_transport_t g_uart_fbstream_transport;
extern bool bsp_uart_init(void);

/*
 * Светодиод — индикатор отказа, а не признак жизни.
 *
 * Никакого «сердцебиения» нет специально: моргание раз в 500 мс видно
 * всегда, и на его фоне невозможно заметить, что какая-то подсистема не
 * поднялась.
 *
 * LED0 горит с первой инструкции задачи инициализации и гаснет только в её
 * конце, если всё поднялось. То есть «горит» — это и «была ошибка», и
 * «инициализация не дошла до конца»: SD, QSPI и SDRAM отмеряют таймауты
 * секундами, и застрять внутри них — штатная неприятность, которую иначе
 * никак не отличить от нормальной работы. Гаснет светодиод ровно в одном
 * случае: ядро дошло до конца без единой жалобы.
 *
 * Горит именно LED0: на плате он красный, а LED1 зелёный, и зелёный «всё
 * сломалось» читается ровно наоборот. LED1 остаётся свободным — им
 * распоряжается интерфейс (переключатели в orca_ui.c).
 *
 * Панику это не трогает: orca_panic мигает тем же LED0 своим кодом
 * (см. orca_panic.h), и мигание там осмысленно — оно кодирует причину.
 */
static bool s_init_failed;

static void error_led(void)
{
    s_init_failed = true;
    board_led_set(0, true);
}

static void system_init_task(void* arg)
{
    (void)arg;

    board_led_set(0, true);

    if (!bsp_uart_init()) {
        /* Печатать некуда — только светодиод ошибки. */
        error_led();
    }

    printf("\r\n=== Orca OS starting ===\r\n");

    bool sdram_ok = board_sdram_init();
    if (!sdram_ok) {
        printf("[ERROR] SDRAM init failed\r\n");
        error_led();
    } else {
        printf("[OK] SDRAM 32MB @ 0xC0000000\r\n");
    }

    if (!board_qspi_init()) {
        const uint8_t* id = board_qspi_jedec_id();
        printf("[WARN] QSPI init failed: %s (JEDEC %02X %02X %02X)\r\n",
               board_qspi_error(), id[0], id[1], id[2]);
        error_led();
    } else {
        const uint8_t* id = board_qspi_jedec_id();
        printf("[OK] QSPI 16MB @ 0x90000000 (JEDEC %02X %02X %02X)\r\n",
               id[0], id[1], id[2]);
    }

    if (!board_sdmmc_init()) {
        printf("[WARN] SD card init failed\r\n");
        error_led();
    } else {
        printf("[OK] SD card mounted\r\n");
    }

    /*
     * RTC поднимается до сервисов, чтобы у логов и у api->system->unix_time
     * с первой секунды было время. Отказ здесь не фатален: часы покажут
     * заводскую дату, остальная система работает как обычно.
     */
    if (!board_rtc_init()) {
        printf("[WARN] RTC init failed\r\n");
        error_led();
    } else {
        orca_datetime_t dt;
        board_rtc_split((uint64_t)((int64_t)board_rtc_unix() +
                                   board_rtc_tz_offset() * 60), &dt);
        printf("[OK] RTC %04u-%02u-%02u %02u:%02u:%02u%s\r\n",
               dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
               board_rtc_time_valid() ? "" : " (время не выставлено)");
    }

    orca_appmgr_init(&g_orca_api);
    orca_fbstream_init(&g_uart_fbstream_transport);
    orca_shell_init(&g_uart_shell_io);

    /*
     * USB Mass Storage. Поднимается после носителей: хост получает три тома
     * (FLASH ядра только на чтение, QSPI, карта), и к моменту подключения они
     * должны быть размечены — подробности в orca_usb_msc.h.
     *
     * 1024 слова стека, а не 512: задача монтирует тома, а f_mount с
     * включённым LFN и f_mkfs чистого QSPI держат структуры FatFs на стеке.
     */
    if (!orca_usb_msc_init()) {
        printf("[WARN] USB MSC init failed\r\n");
        error_led();
    } else if (xTaskCreate(orca_usb_msc_task, "usb", 1024, NULL,
                           tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        printf("[ERROR] не удалось создать задачу usb\r\n");
        error_led();
    } else {
        printf("[OK] USB MSC: FLASH 2MB (ro), QSPI 16MB, SD card\r\n");
    }

    /*
     * Служба Orca Link поднимается всегда, даже если периферия не завелась:
     * это единственный канал диагностики, когда SD/SDRAM молчат.
     * Она же владеет UART RX и раздаёт байты shell'у (см. orca_link_service.c).
     */
    orca_link_service_init(&g_uart_link_io);
    /*
     * 3072 слова (12 КБ) — не с запасом "на всякий случай". Эта задача
     * синхронно выполняет команду: orca_shell_exec -> orca_cmdfs_exec ->
     * orca_loader_load -> код самого модуля. Плюс FatFs с включённым LFN
     * держит FILINFO/DIR по ~600 байт на кадр стека. На прежней 1024 первый
     * же `ls` с длинными именами клал задачу в vApplicationStackOverflowHook.
     */
    if (xTaskCreate(orca_link_service_task, "link", 3072, NULL,
                    tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        printf("[ERROR] не удалось создать задачу link\r\n");
        error_led();
    } else {
        printf("[OK] Orca Link service (UART %u 8N1)\r\n", 921600u);
    }

    /*
     * GUI. Дисплея на плате пока нет, поэтому единственный «экран» — это
     * трансляция кадров на ПК, но рисует их настоящий LVGL, а не тестовый
     * генератор: источником кадров по умолчанию стоит ORCA_FB_SRC_GUI.
     *
     * Две задачи, а не одна: gui крутит lv_timer_handler (отрисовка, анимации,
     * тач), gui_tx синхронно отдаёт готовый кадр в UART. Отправка 240x160 на
     * 921600 занимает ~0.8 с — делай это внутри lv_timer_handler, и интерфейс
     * реагировал бы на тапы раз в секунду. Подробнее в orca_lvgl.h.
     */
    bool gui_ok = false;
    if (sdram_ok && orca_lvgl_init()) {
        BaseType_t ui = xTaskCreate(orca_lvgl_task, "gui", 3072, NULL,
                                    tskIDLE_PRIORITY + 1, NULL);
        BaseType_t tx = xTaskCreate(orca_lvgl_stream_task, "gui_tx", 512, NULL,
                                    tskIDLE_PRIORITY + 1, NULL);
        gui_ok = (ui == pdPASS && tx == pdPASS);
        if (!gui_ok) {
            printf("[ERROR] не удалось создать задачи GUI\r\n");
            error_led();
        } else {
            printf("[OK] GUI %ux%u на LVGL (fb on — начать трансляцию)\r\n",
                   ORCA_GUI_WIDTH, ORCA_GUI_HEIGHT);
        }
    }

    /*
     * Тестовый генератор кадров остаётся как запасной источник: по нему видно,
     * жив ли сам тракт SDRAM -> Orca Link -> ПК, когда с GUI что-то не так
     * (`fb src test` в консоли). Если GUI не поднялся, переключаемся на него
     * сразу — иначе `fb on` покажет пустой экран.
     */
    if (sdram_ok && orca_fbtest_init()) {
        xTaskCreate(orca_fbtest_task, "fbtest", 512, NULL,
                    tskIDLE_PRIORITY + 1, NULL);
        printf("[OK] Test framebuffer %ux%u (fb src test — переключиться)\r\n",
               ORCA_FBTEST_WIDTH, ORCA_FBTEST_HEIGHT);
    }

    if (!gui_ok) {
        orca_fbstream_set_source(ORCA_FB_SRC_TEST);
        printf("[WARN] GUI недоступен — источник кадров переключён на test\r\n");
        error_led();
    }

    if (s_init_failed) {
        printf("[WARN] Orca kernel ready, но с замечаниями выше (LED0 горит)\r\n");
    } else {
        board_led_set(0, false);
        printf("[OK] Orca kernel ready\r\n");
    }

    vTaskDelete(NULL);
}

int main(void)
{
    HAL_Init();
    board_init();

    xTaskCreate(system_init_task, "sys_init", 512, NULL, tskIDLE_PRIORITY + 3, NULL);

    vTaskStartScheduler();

    while (1) {
    }
}

void Error_Handler(void)
{
    orca_panic(ORCA_PANIC_ERROR_HDLR, NULL, NULL);
}
