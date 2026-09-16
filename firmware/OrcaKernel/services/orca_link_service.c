#include "orca_link_service.h"
#include "orca_fbstream.h"
#include "orca_shell.h"
#include "orca_input.h"
#include "orca_api.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>

#define ORCA_FW_VERSION "0.1.0"

static const orca_link_service_io_t* s_io;
static orca_link_rx_t              s_rx;
static orca_link_service_stats_t   s_stats;
static uint16_t                    s_tx_seq;
static SemaphoreHandle_t           s_tx_lock;
static TaskHandle_t                s_task;   /* владелец статических буферов ответа */

static uint8_t s_tx_buf[ORCA_LINK_MAX_FRAME];

static uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

bool orca_link_service_send(uint8_t type, const void* payload, uint16_t len)
{
    if (s_io == NULL || s_io->write == NULL || s_tx_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }

    uint32_t n = orca_link_encode(s_tx_buf, sizeof(s_tx_buf), type,
                                  s_tx_seq++, payload, len);
    bool ok = (n > 0) && (s_io->write(s_tx_buf, n) == (int)n);
    if (ok) {
        s_stats.frames_tx++;
    }

    xSemaphoreGive(s_tx_lock);
    return ok;
}

static void send_hello(void)
{
    orca_link_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.api_version   = (uint8_t)ORCA_API_VERSION;
    hello.link_version  = ORCA_LINK_VERSION;
    hello.display_count = 0;    /* дисплея пока нет физически */
    hello.uptime_ms     = now_ms();
    strncpy(hello.device, "orca-h743", sizeof(hello.device) - 1);
    strncpy(hello.fw_version, ORCA_FW_VERSION, sizeof(hello.fw_version) - 1);

    orca_link_service_send(ORCA_LINK_HELLO, &hello, sizeof(hello));
}

static void send_error(uint16_t code, const char* text)
{
    uint8_t buf[sizeof(orca_link_error_t) + 96];
    orca_link_error_t* err = (orca_link_error_t*)buf;
    uint16_t tlen = (uint16_t)strnlen(text, sizeof(buf) - sizeof(*err));

    err->code     = code;
    err->text_len = tlen;
    memcpy(buf + sizeof(*err), text, tlen);

    orca_link_service_send(ORCA_LINK_ERROR, buf, (uint16_t)(sizeof(*err) + tlen));
}

/* ------------------------------------------------------------------ */
/* Обработчики кадров                                                 */
/* ------------------------------------------------------------------ */

static void on_shell_cmd(const orca_link_frame_t* f)
{
    /*
     * ИНВАРИАНТ: вызывается только из orca_link_service_task. Ответ и вывод
     * shell лежат в статических буферах ниже — второй вызывающий (shell из
     * USB, второй линк) молча писал бы в те же 2 КБ и портил чужой ответ.
     * Появился второй источник команд — заводите буферы в его контексте.
     */
    configASSERT(s_task == NULL || s_task == xTaskGetCurrentTaskHandle());

    if (f->len == 0 || f->payload == NULL) {
        send_error(1, "empty shell command");
        return;
    }

    char cmd[SHELL_LINE_MAX];
    uint16_t len = (f->len < sizeof(cmd) - 1) ? f->len : (uint16_t)(sizeof(cmd) - 1);
    memcpy(cmd, f->payload, len);
    cmd[len] = '\0';

    /*
     * Ответ собираем в статическом буфере: кадр всё равно уходит под
     * мьютексом, а на стеке таска 2 КБ держать не хочется.
     */
    static char out[SHELL_OUT_MAX];
    int rc = orca_shell_exec(cmd, out, sizeof(out));

    /*
     * Режем на кадры по ORCA_LINK_SHELL_TEXT_MAX. Раньше сюда влезало
     * ровно 1024 байта текста, к которым добавлялся 8-байтный заголовок:
     * payload получался 1032 > ORCA_LINK_MAX_PAYLOAD, orca_link_encode
     * возвращал 0, и весь ответ пропадал молча. `help` и `ls` на клиенте
     * выглядели как зависшая команда.
     */
    static uint8_t resp[sizeof(orca_link_shell_resp_t) + ORCA_LINK_SHELL_TEXT_MAX];
    orca_link_shell_resp_t* hdr = (orca_link_shell_resp_t*)resp;

    uint32_t total = (uint32_t)strnlen(out, sizeof(out));
    uint32_t sent  = 0;

    do {
        uint32_t chunk = total - sent;
        if (chunk > ORCA_LINK_SHELL_TEXT_MAX) {
            chunk = ORCA_LINK_SHELL_TEXT_MAX;
        }

        hdr->status   = (int32_t)rc;
        hdr->text_len = (uint16_t)chunk;
        hdr->more     = (sent + chunk < total) ? 1u : 0u;
        memcpy(resp + sizeof(*hdr), out + sent, chunk);

        orca_link_service_send(ORCA_LINK_SHELL_RESP, resp,
                               (uint16_t)(sizeof(*hdr) + chunk));
        sent += chunk;
    } while (sent < total);   /* пустой вывод — один кадр с text_len = 0 */
}

static void on_fb_req(const orca_link_frame_t* f)
{
    if (f->len < sizeof(orca_link_fb_req_t) || f->payload == NULL) {
        send_error(2, "bad fb_req");
        return;
    }
    orca_fbstream_configure((const orca_link_fb_req_t*)f->payload);
}

static void on_touch(const orca_link_frame_t* f)
{
    if (f->len < sizeof(orca_link_touch_t) || f->payload == NULL) {
        send_error(5, "bad touch");
        return;
    }
    const orca_link_touch_t* t = (const orca_link_touch_t*)f->payload;
    orca_input_push_touch(t->display_id, t->x, t->y, t->action);
}

static void on_button(const orca_link_frame_t* f)
{
    if (f->len < sizeof(orca_link_button_t) || f->payload == NULL) {
        send_error(3, "bad button");
        return;
    }
    const orca_link_button_t* b = (const orca_link_button_t*)f->payload;
    orca_input_set_button(b->button_id, b->pressed != 0);
}

static void dispatch(const orca_link_frame_t* f)
{
    s_stats.frames_rx++;
    s_stats.last_rx_ms = now_ms();
    s_stats.peer_seen  = true;

    switch (f->type) {
        case ORCA_LINK_PING:
            orca_link_service_send(ORCA_LINK_PONG, NULL, 0);
            break;

        case ORCA_LINK_HELLO:
            send_hello();
            break;

        case ORCA_LINK_FB_REQ:
            on_fb_req(f);
            break;

        case ORCA_LINK_SHELL:
            on_shell_cmd(f);
            break;

        case ORCA_LINK_TOUCH:
            on_touch(f);
            break;

        case ORCA_LINK_BUTTON:
            on_button(f);
            break;

        default:
            send_error(4, "unknown frame type");
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Служба                                                             */
/* ------------------------------------------------------------------ */

void orca_link_service_init(const orca_link_service_io_t* io)
{
    s_io = io;
    orca_link_rx_init(&s_rx);
    memset(&s_stats, 0, sizeof(s_stats));
    s_tx_seq  = 0;
    s_tx_lock = xSemaphoreCreateMutex();
    orca_input_init();
}

void orca_link_service_get_stats(orca_link_service_stats_t* out)
{
    if (out == NULL) {
        return;
    }
    *out = s_stats;
    out->crc_errors = s_rx.crc_errors;
    out->resyncs    = s_rx.resyncs;
}

void orca_link_service_task(void* arg)
{
    (void)arg;

    /*
     * Задача запоминает себя: on_shell_cmd держит вывод shell в статических
     * буферах, и это корректно ровно до тех пор, пока обработчик вызывается
     * только отсюда. Инвариант проверяется configASSERT, а не комментарием.
     */
    s_task = xTaskGetCurrentTaskHandle();

    if (s_io == NULL || s_io->read == NULL) {
        vTaskDelete(NULL);
        return;
    }

    /* Приветствие консоли печатает эта же задача — она владеет портом. */
    orca_shell_greet();

    for (;;) {
        /*
         * Демультиплексор. Пока парсер Link в состоянии WAIT_MAGIC0, байт
         * 0xAA считаем началом кадра, всё остальное — текстом для shell.
         * Как только парсер вошёл в кадр, байты уходят только ему, пока
         * кадр не соберётся, не сломается CRC или не истечёт таймаут.
         *
         * 0xAA не входит в ASCII, но встречается внутри UTF-8 (например,
         * 'Ъ' = D0 AA), поэтому одного признака мало: без таймаута ввод
         * кириллицы в консоль уводил парсер в кадр навсегда и ел строку.
         * В покое ждём вечно — таймаут нужен только внутри кадра.
         */
        bool in_frame = (s_rx.state != ORCA_LINK_RX_WAIT_MAGIC0);
        uint32_t timeout = in_frame ? ORCA_LINK_FRAME_TIMEOUT_MS : portMAX_DELAY;

        uint8_t ch;
        if (s_io->read(&ch, 1, timeout) != 1) {
            if (in_frame) {
                orca_link_rx_reset(&s_rx);
            }
            continue;
        }

        if (in_frame || ch == ORCA_LINK_MAGIC0) {
            orca_link_frame_t frame;
            if (orca_link_rx_byte(&s_rx, ch, &frame)) {
                dispatch(&frame);
            }
            continue;
        }

        s_stats.shell_bytes++;
        orca_shell_feed_byte(ch);
    }
}
