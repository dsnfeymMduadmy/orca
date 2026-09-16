/*
 * esp_link — пример модуля Orca: мост между системным UART и ESP8266/ESP32.
 *
 * Что показывает:
 *   - захват и обязательное освобождение ресурса (UART);
 *   - цикл жизни модуля через api->task->should_stop();
 *   - сборку построчного вывода из потока байт: лог принимает строки, а UART
 *     отдаёт куски произвольной длины, и склеивать их — задача модуля.
 *
 * Сборка:  make            (см. Makefile рядом)
 * Установка: build/esp_link.orca -> SD:/modules/esp_link/esp_link.orca
 *
 * Порт и скорость заданы под конкретную распайку платы. bsp_modem_uart_* в
 * ядре пока заглушка (возвращает false), поэтому на живом железе open честно
 * не удастся — модуль сообщит об этом в лог и выйдет с ошибкой, а не повиснет.
 */

#include "orca_api.h"

#define TAG            "esp_link"

#define ESP_UART_PORT  0u
#define ESP_UART_BAUD  115200u

#define RX_CHUNK       64u
#define RX_TIMEOUT_MS  100u
#define LINE_MAX       128u

static const orca_api_t* s_api;
static void*             s_uart;

static void flush_line(char* line, uint32_t* fill)
{
    if (*fill == 0u) {
        return;
    }
    line[*fill] = '\0';
    s_api->log->info(TAG, line);
    *fill = 0u;
}

static void close_uart(void)
{
    if (s_uart != NULL) {
        s_api->uart->close(s_uart);
        s_uart = NULL;
    }
}

int orca_app_main(const orca_api_t* api)
{
    s_api = api;

    s_uart = api->uart->open(ESP_UART_PORT, ESP_UART_BAUD);
    if (s_uart == NULL) {
        api->log->error(TAG, "UART не открылся: порт занят или не реализован");
        return -1;
    }
    api->log->info(TAG, "мост поднят: порт 0, 115200");

    /* AT\r\n — то, на что отвечает любая прошивка ESP, годится как проба связи. */
    static const char probe[] = "AT\r\n";
    api->uart->write(s_uart, probe, sizeof(probe) - 1u);

    char     line[LINE_MAX];
    uint32_t fill = 0u;
    uint8_t  chunk[RX_CHUNK];

    while (!api->task->should_stop()) {
        int32_t n = api->uart->read(s_uart, chunk, RX_CHUNK, RX_TIMEOUT_MS);
        if (n < 0) {
            api->log->error(TAG, "ошибка чтения UART, мост остановлен");
            break;
        }
        if (n == 0) {
            continue; /* таймаут — просто нет данных от ESP */
        }

        for (int32_t i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                flush_line(line, &fill);
                continue;
            }
            if (fill == LINE_MAX - 1u) {
                /* Строка длиннее буфера: отдаём кусок, чтобы не потерять данные. */
                flush_line(line, &fill);
            }
            line[fill++] = c;
        }
    }

    flush_line(line, &fill);
    close_uart();
    api->log->info(TAG, "мост остановлен");
    return 0;
}

void orca_app_stop(void)
{
    /*
     * Зовётся при остановке модуля извне. UART закрываем здесь, а не только в
     * конце main: задачу могут снять до того, как цикл дойдёт до выхода.
     */
    close_uart();
}
