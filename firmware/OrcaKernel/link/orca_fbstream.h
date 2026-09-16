#ifndef ORCA_FBSTREAM_H
#define ORCA_FBSTREAM_H

#include <stdint.h>
#include <stdbool.h>
#include "orca_link.h"

/*
 * Трансляция содержимого framebuffer наружу кадрами Orca Link.
 * Потребители: tools/fbstream (ПК, UART/TCP) и Android-приложение
 * (через ESP32-S3, который прозрачно пересылает те же кадры в TCP).
 *
 * Стрим намеренно "best effort": если транспорт не успевает, кадр
 * дропается (frames_dropped++), а GUI не блокируется — трансляция не
 * должна влиять на отзывчивость устройства.
 */

/*
 * Кадры уходят через orca_link_service_send() — там общий на всю систему
 * мьютекс TX и единая нумерация seq. Транспорт нужен только для обратного
 * давления: спросить, тянет ли линия кадр прямо сейчас.
 */
typedef struct {
    /* сколько байт транспорт готов принять прямо сейчас; 0 = дропнуть кадр */
    uint32_t (*writable)(void);
} orca_fbstream_transport_t;

/*
 * Источников кадров в системе больше одного: GUI на LVGL и тестовый
 * генератор fbtest. На один display_id одновременно должен работать ровно
 * один — иначе на ПК приходят перемешанные кадры от двух продюсеров, и
 * картинка мерцает между интерфейсом и тестовой заливкой. Кто именно
 * транслируется, выбирается здесь (shell: `fb src gui|test`).
 */
typedef enum {
    ORCA_FB_SRC_GUI  = 0,   /* кадры LVGL (orca_lvgl) */
    ORCA_FB_SRC_TEST = 1    /* тестовая картинка (orca_fbtest) */
} orca_fb_source_t;

typedef struct {
    bool     enabled;
    uint8_t  source;        /* orca_fb_source_t */
    uint8_t  display_id;
    uint8_t  pixfmt;
    uint8_t  scale;
    uint8_t  max_fps;
    uint32_t frames_sent;
    uint32_t frames_dropped;
    uint32_t bytes_sent;
} orca_fbstream_stats_t;

void orca_fbstream_init(const orca_fbstream_transport_t* transport);

/* Включение/выключение (shell: `fb on|off`, либо кадр ORCA_LINK_FB_REQ). */
void orca_fbstream_enable(bool enable);
bool orca_fbstream_is_enabled(void);

void orca_fbstream_set_source(orca_fb_source_t src);
orca_fb_source_t orca_fbstream_get_source(void);

/* Применить запрос параметров от клиента (ПК/Android). */
void orca_fbstream_configure(const orca_link_fb_req_t* req);

/*
 * Вызывается источником кадров после отрисовки (GUI — из flush_cb LVGL).
 * fb — указатель на framebuffer, fb_bytes — его РЕАЛЬНЫЙ размер,
 * stride — байт в строке.
 * fb_bytes обязателен: по width/height/stride здесь читают чужую память, и
 * без размера буфера завышенный height даёт чтение за границей вместо
 * ошибки. Не влезающая геометрия — дроп кадра плюс строка в лог.
 * Если стрим выключен, кадр не от выбранного источника или не пришло
 * время по max_fps — no-op.
 */
void orca_fbstream_push_frame(orca_fb_source_t src, uint8_t display_id,
                              const void* fb, uint32_t fb_bytes,
                              uint16_t width, uint16_t height,
                              uint32_t stride, uint8_t src_pixfmt);

void orca_fbstream_get_stats(orca_fbstream_stats_t* out);

#endif /* ORCA_FBSTREAM_H */
