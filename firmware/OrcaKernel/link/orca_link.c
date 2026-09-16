#include "orca_link.h"
#include <string.h>

/* CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF, без реверса и xorout. */
uint16_t orca_link_crc16(const uint8_t* data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint32_t orca_link_encode(uint8_t* out, uint32_t out_cap,
                          uint8_t type, uint16_t seq,
                          const void* payload, uint16_t payload_len)
{
    if (out == NULL || payload_len > ORCA_LINK_MAX_PAYLOAD) {
        return 0;
    }
    uint32_t total = ORCA_LINK_HEADER_SIZE + payload_len + ORCA_LINK_CRC_SIZE;
    if (out_cap < total) {
        return 0;
    }
    if (payload_len > 0 && payload == NULL) {
        return 0;
    }

    out[0] = ORCA_LINK_MAGIC0;
    out[1] = ORCA_LINK_MAGIC1;
    out[2] = ORCA_LINK_VERSION;
    out[3] = type;
    out[4] = (uint8_t)(seq & 0xFFu);
    out[5] = (uint8_t)(seq >> 8);
    out[6] = (uint8_t)(payload_len & 0xFFu);
    out[7] = (uint8_t)(payload_len >> 8);

    if (payload_len > 0) {
        memcpy(&out[ORCA_LINK_HEADER_SIZE], payload, payload_len);
    }

    /* CRC считается от ver (индекс 2) до конца payload. */
    uint16_t crc = orca_link_crc16(&out[2], 6u + payload_len);
    out[ORCA_LINK_HEADER_SIZE + payload_len]     = (uint8_t)(crc & 0xFFu);
    out[ORCA_LINK_HEADER_SIZE + payload_len + 1] = (uint8_t)(crc >> 8);

    return total;
}

void orca_link_rx_init(orca_link_rx_t* rx)
{
    memset(rx, 0, sizeof(*rx));
    rx->state = ORCA_LINK_RX_WAIT_MAGIC0;
}

void orca_link_rx_reset(orca_link_rx_t* rx)
{
    rx->state       = ORCA_LINK_RX_WAIT_MAGIC0;
    rx->pos         = 0;
    rx->payload_len = 0;
    rx->resyncs++;
}

bool orca_link_rx_byte(orca_link_rx_t* rx, uint8_t byte, orca_link_frame_t* out)
{
    switch (rx->state) {
        case ORCA_LINK_RX_WAIT_MAGIC0:
            if (byte == ORCA_LINK_MAGIC0) {
                rx->buf[0] = byte;
                rx->pos = 1;
                rx->state = ORCA_LINK_RX_WAIT_MAGIC1;
            }
            break;

        case ORCA_LINK_RX_WAIT_MAGIC1:
            if (byte == ORCA_LINK_MAGIC1) {
                rx->buf[1] = byte;
                rx->pos = 2;
                rx->state = ORCA_LINK_RX_HEADER;
            } else if (byte == ORCA_LINK_MAGIC0) {
                /* 0xAA 0xAA — остаёмся в ожидании второй магии */
                rx->pos = 1;
            } else {
                rx->resyncs++;
                rx->state = ORCA_LINK_RX_WAIT_MAGIC0;
            }
            break;

        case ORCA_LINK_RX_HEADER:
            rx->buf[rx->pos++] = byte;
            if (rx->pos == ORCA_LINK_HEADER_SIZE) {
                if (rx->buf[2] != ORCA_LINK_VERSION) {
                    rx->resyncs++;
                    rx->state = ORCA_LINK_RX_WAIT_MAGIC0;
                    break;
                }
                rx->payload_len = (uint16_t)rx->buf[6] | ((uint16_t)rx->buf[7] << 8);
                if (rx->payload_len > ORCA_LINK_MAX_PAYLOAD) {
                    rx->resyncs++;
                    rx->state = ORCA_LINK_RX_WAIT_MAGIC0;
                    break;
                }
                rx->state = (rx->payload_len > 0) ? ORCA_LINK_RX_PAYLOAD
                                                  : ORCA_LINK_RX_CRC;
            }
            break;

        case ORCA_LINK_RX_PAYLOAD:
            rx->buf[rx->pos++] = byte;
            if (rx->pos == ORCA_LINK_HEADER_SIZE + rx->payload_len) {
                rx->state = ORCA_LINK_RX_CRC;
            }
            break;

        case ORCA_LINK_RX_CRC:
            rx->buf[rx->pos++] = byte;
            if (rx->pos == ORCA_LINK_HEADER_SIZE + rx->payload_len + ORCA_LINK_CRC_SIZE) {
                uint16_t got = (uint16_t)rx->buf[rx->pos - 2] |
                               ((uint16_t)rx->buf[rx->pos - 1] << 8);
                uint16_t want = orca_link_crc16(&rx->buf[2], 6u + rx->payload_len);

                rx->state = ORCA_LINK_RX_WAIT_MAGIC0;

                if (got != want) {
                    rx->crc_errors++;
                    return false;
                }

                rx->frames_ok++;
                if (out != NULL) {
                    out->type    = rx->buf[3];
                    out->seq     = (uint16_t)rx->buf[4] | ((uint16_t)rx->buf[5] << 8);
                    out->len     = rx->payload_len;
                    out->payload = (rx->payload_len > 0) ? &rx->buf[ORCA_LINK_HEADER_SIZE]
                                                         : NULL;
                }
                return true;
            }
            break;

        default:
            rx->state = ORCA_LINK_RX_WAIT_MAGIC0;
            break;
    }

    return false;
}
