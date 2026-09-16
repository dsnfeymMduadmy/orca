#ifndef ORCA_LINK_H
#define ORCA_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Orca Link Protocol (OLP) v1
 * ---------------------------
 * Один кадровый протокол на все внешние каналы устройства:
 *   - STM32 <-> XIAO ESP32-C3 (UART, радио-сопроцессор Wi-Fi/BLE)
 *   - STM32 <-> ПК (UART через CH340, tools/fbstream)
 *   - ESP32 <-> ПК/Android (TCP 3333 и BLE, те же кадры)
 *
 * Формат кадра (little-endian):
 *
 *   +------+------+-----+------+--------+--------+---------+-------+
 *   | 0xAA | 0x55 | ver | type | seq:2  | len:2  | payload | crc:2 |
 *   +------+------+-----+------+--------+--------+---------+-------+
 *      0      1      2     3      4..5     6..7    8..8+len
 *
 * crc16 = CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF) по байтам [2 .. 7+len],
 * то есть ver..payload включительно. Магия в CRC не входит — она нужна только
 * для ресинхронизации потока после мусора на линии.
 *
 * Приёмник — потоковый конечный автомат (orca_link_rx_*), устойчивый к
 * обрывам: при несовпадении CRC кадр отбрасывается и поиск магии идёт дальше.
 */

#define ORCA_LINK_MAGIC0        0xAAu
#define ORCA_LINK_MAGIC1        0x55u
#define ORCA_LINK_VERSION       1u

#define ORCA_LINK_HEADER_SIZE   8u
#define ORCA_LINK_CRC_SIZE      2u
#define ORCA_LINK_MAX_PAYLOAD   1024u
#define ORCA_LINK_MAX_FRAME     (ORCA_LINK_HEADER_SIZE + ORCA_LINK_MAX_PAYLOAD + ORCA_LINK_CRC_SIZE)

/*
 * Сколько ждать продолжения начатого кадра, прежде чем считать его мусором.
 * На 921600 полный кадр (1034 байта) идёт ~11 мс, так что 200 мс — с запасом
 * даже для медленного клиента, но незаметно для человека за консолью.
 */
#define ORCA_LINK_FRAME_TIMEOUT_MS  200u

/* ---------------------------------------------------------------- */
/* Типы кадров                                                      */
/* ---------------------------------------------------------------- */
typedef enum {
    /* служебные */
    ORCA_LINK_PING        = 0x01, /* -> устройство, payload пуст            */
    ORCA_LINK_PONG        = 0x02, /* <- устройство, payload пуст            */
    ORCA_LINK_HELLO       = 0x03, /* <- orca_link_hello_t                   */

    /* логи */
    ORCA_LINK_LOG         = 0x10, /* <- orca_link_log_t + текст             */

    /* трансляция экрана */
    ORCA_LINK_FB_INFO     = 0x20, /* <- orca_link_fb_info_t (начало кадра)  */
    ORCA_LINK_FB_CHUNK    = 0x21, /* <- orca_link_fb_chunk_t + данные       */
    ORCA_LINK_FB_END      = 0x22, /* <- uint32 frame_id                     */
    ORCA_LINK_FB_REQ      = 0x23, /* -> orca_link_fb_req_t                  */

    /* ввод (удалённое управление с ПК/телефона) */
    ORCA_LINK_TOUCH       = 0x30, /* -> orca_link_touch_t                   */
    ORCA_LINK_BUTTON      = 0x31, /* -> orca_link_button_t                  */

    /* мост в системный shell */
    ORCA_LINK_SHELL       = 0x40, /* -> строка команды (без NUL)            */
    ORCA_LINK_SHELL_RESP  = 0x41, /* <- orca_link_shell_resp_t + текст      */

    /* состояние сети на ESP32-C3 */
    ORCA_LINK_NET_STATUS  = 0x50, /* <-> orca_link_net_status_t             */
    ORCA_LINK_NET_CONFIG  = 0x51, /* -> orca_link_net_config_t              */

    /*
     * Команды самому ESP32-C3 (Wi-Fi/BLE-сопроцессор): текстовая строка вида
     * "wifi scan", "ble adv on", "sys info". Текст, а не бинарный код на
     * команду, выбран сознательно — набор команд ESP будет расти, и добавление
     * новой не должно требовать синхронной правки протокола в трёх местах
     * (прошивка часов, ESP, Android-приложение).
     */
    ORCA_LINK_ESP_CMD     = 0x52, /* -> строка команды (без NUL)            */
    ORCA_LINK_ESP_RESP    = 0x53, /* <- orca_link_esp_resp_t + текст        */

    ORCA_LINK_ERROR       = 0xF0  /* <- orca_link_error_t + текст           */
} orca_link_type_t;

/* ---------------------------------------------------------------- */
/* Форматы пикселей для трансляции                                  */
/* ---------------------------------------------------------------- */
typedef enum {
    ORCA_LINK_PIXFMT_RGB565     = 0x01,
    ORCA_LINK_PIXFMT_RGB888     = 0x02,
    ORCA_LINK_PIXFMT_GRAY8      = 0x03,
    ORCA_LINK_PIXFMT_RLE_RGB565 = 0x10  /* RLE: [count:u8][pixel:u16] */
} orca_link_pixfmt_t;

/* ---------------------------------------------------------------- */
/* Payload-структуры (все packed, little-endian)                    */
/* ---------------------------------------------------------------- */
#pragma pack(push, 1)

typedef struct {
    uint8_t  api_version;      /* ORCA_API_VERSION прошивки */
    uint8_t  link_version;     /* ORCA_LINK_VERSION         */
    uint16_t display_count;
    uint32_t uptime_ms;
    char     device[24];       /* "orca-h743", NUL-terminated */
    char     fw_version[16];
} orca_link_hello_t;

typedef struct {
    uint8_t  level;            /* 0=info 1=warn 2=error */
    uint8_t  _rsv;
    uint16_t msg_len;
    char     tag[16];
    /* далее msg_len байт текста */
} orca_link_log_t;

typedef struct {
    uint32_t frame_id;
    uint16_t width;
    uint16_t height;
    uint8_t  pixfmt;           /* orca_link_pixfmt_t */
    uint8_t  display_id;
    uint16_t chunk_count;
    uint32_t payload_bytes;    /* суммарный размер данных кадра */
} orca_link_fb_info_t;

typedef struct {
    uint32_t frame_id;
    uint16_t chunk_index;
    uint16_t data_len;
    /* далее data_len байт пиксельных данных */
} orca_link_fb_chunk_t;

typedef struct {
    uint8_t  enable;
    uint8_t  display_id;
    uint8_t  pixfmt;           /* желаемый формат */
    uint8_t  scale;            /* 1 = 1:1, 2 = половина, 4 = четверть */
    uint8_t  max_fps;
    uint8_t  _rsv[3];
} orca_link_fb_req_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t  display_id;
    uint8_t  action;           /* 0=up 1=down 2=move */
    uint8_t  _rsv[2];
} orca_link_touch_t;

typedef struct {
    uint8_t button_id;
    uint8_t pressed;
    uint8_t _rsv[2];
} orca_link_button_t;

typedef struct {
    int32_t  status;           /* код возврата команды */
    uint16_t text_len;
    /*
     * 1 = за этим кадром идёт продолжение того же ответа, 0 = последний.
     * Вывод команды бывает длиннее ORCA_LINK_MAX_PAYLOAD (SHELL_OUT_MAX =
     * 2048), а в один кадр влезает text_len <= 1016. Раньше такой ответ
     * просто не отправлялся: orca_link_encode отбрасывал payload > 1024,
     * и `help`/`ls` на клиенте выглядели как зависание.
     * Клиент склеивает текст, пока more != 0; status берёт из последнего.
     */
    uint16_t more;
    /* далее text_len байт вывода */
} orca_link_shell_resp_t;

/* Сколько байт текста влезает в один кадр ответа shell'а. */
#define ORCA_LINK_SHELL_TEXT_MAX \
    (ORCA_LINK_MAX_PAYLOAD - (uint16_t)sizeof(orca_link_shell_resp_t))

typedef struct {
    uint8_t  state;            /* 0=down 1=connecting 2=connected 3=ap_mode */
    int8_t   rssi;
    uint16_t _rsv;
    char     ssid[33];
    char     ip[16];
} orca_link_net_status_t;

typedef struct {
    char ssid[33];
    char password[65];
} orca_link_net_config_t;

typedef struct {
    uint16_t code;
    uint16_t text_len;
    /* далее text_len байт описания */
} orca_link_error_t;

#pragma pack(pop)

/*
 * Ответ на ESP_CMD. Формат намеренно совпадает с ответом shell'а: тот же
 * status, та же склейка по more, — значит и на клиенте это один и тот же код
 * сборки многокадрового текста.
 */
typedef orca_link_shell_resp_t orca_link_esp_resp_t;

#define ORCA_LINK_ESP_TEXT_MAX ORCA_LINK_SHELL_TEXT_MAX

/* ---------------------------------------------------------------- */
/* CRC                                                              */
/* ---------------------------------------------------------------- */
uint16_t orca_link_crc16(const uint8_t* data, uint32_t len);

/* ---------------------------------------------------------------- */
/* Сериализация                                                     */
/* ---------------------------------------------------------------- */

/*
 * Собирает кадр в out (должен быть >= ORCA_LINK_MAX_FRAME).
 * Возвращает полную длину кадра или 0 при ошибке.
 */
uint32_t orca_link_encode(uint8_t* out, uint32_t out_cap,
                          uint8_t type, uint16_t seq,
                          const void* payload, uint16_t payload_len);

/* ---------------------------------------------------------------- */
/* Потоковый разбор                                                 */
/* ---------------------------------------------------------------- */
typedef enum {
    ORCA_LINK_RX_WAIT_MAGIC0 = 0,
    ORCA_LINK_RX_WAIT_MAGIC1,
    ORCA_LINK_RX_HEADER,
    ORCA_LINK_RX_PAYLOAD,
    ORCA_LINK_RX_CRC
} orca_link_rx_state_t;

typedef struct {
    orca_link_rx_state_t state;
    uint8_t  buf[ORCA_LINK_MAX_FRAME];
    uint32_t pos;
    uint16_t payload_len;

    /* статистика линии — полезна при отладке UART */
    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t resyncs;
} orca_link_rx_t;

typedef struct {
    uint8_t        type;
    uint16_t       seq;
    uint16_t       len;
    const uint8_t* payload;
} orca_link_frame_t;

void orca_link_rx_init(orca_link_rx_t* rx);

/*
 * Принудительно выбросить недособранный кадр и вернуться к поиску магии.
 * Нужно, когда 0xAA пришёл не из протокола (например, второй байт UTF-8
 * символа в консольном вводе) и парсер завис в ожидании остатка кадра.
 */
void orca_link_rx_reset(orca_link_rx_t* rx);

/*
 * Скармливает один байт. Возвращает true, когда собран валидный кадр —
 * тогда `out` указывает внутрь rx->buf (валиден до следующего вызова).
 */
bool orca_link_rx_byte(orca_link_rx_t* rx, uint8_t byte, orca_link_frame_t* out);

#endif /* ORCA_LINK_H */
