#ifndef ORCA_LINK_SERVICE_H
#define ORCA_LINK_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "orca_link.h"

/*
 * Служба взаимодействия с ПК. Стартует вместе с системой и живёт всё время
 * работы устройства — ПК может подключиться в любой момент.
 *
 * Одна задача владеет UART RX и демультиплексирует поток:
 *
 *   байт == 0xAA (и парсер в покое) -> бинарный кадр Orca Link
 *   иначе                           -> текст в shell (orca_shell_feed_byte)
 *
 * Так на одном проводе одновременно живут человекочитаемая консоль
 * (screen/minicom) и протокол для tools/fbstream и Android-клиента.
 * Два таска на один порт посадить нельзя — байты растащатся случайно,
 * поэтому RX ровно один, а shell получает символы через callback.
 */

typedef struct {
    int (*read)(uint8_t* buf, uint32_t len, uint32_t timeout_ms);
    int (*write)(const uint8_t* buf, uint32_t len);
} orca_link_service_io_t;

typedef struct {
    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t crc_errors;
    uint32_t resyncs;
    uint32_t shell_bytes;
    bool     peer_seen;      /* пришёл хоть один валидный кадр */
    uint32_t last_rx_ms;
} orca_link_service_stats_t;

void orca_link_service_init(const orca_link_service_io_t* io);

/* Бесконечный цикл разбора RX — запускать как FreeRTOS-таск. */
void orca_link_service_task(void* arg);

/* Отправка кадра наружу (используется fbstream и логами). */
bool orca_link_service_send(uint8_t type, const void* payload, uint16_t len);

void orca_link_service_get_stats(orca_link_service_stats_t* out);

#endif /* ORCA_LINK_SERVICE_H */
