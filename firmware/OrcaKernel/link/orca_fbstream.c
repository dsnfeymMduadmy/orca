#include "orca_fbstream.h"
#include "orca_link_service.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define FB_CHUNK_PAYLOAD  (ORCA_LINK_MAX_PAYLOAD - sizeof(orca_link_fb_chunk_t))

/* Источник всегда RGB565 (формат фреймбуфера LTDC) — два байта на пиксель. */
#define SRC_BPP           2u

static const orca_fbstream_transport_t* s_tr;
static orca_fbstream_stats_t s_stats;
static uint32_t s_frame_id;
static uint32_t s_last_send_ms;
static bool     s_geom_reported;

/*
 * Кадры идут десятками в секунду, поэтому о битой геометрии сообщаем один
 * раз за время работы: иначе консоль забьётся, и настоящая первая строка
 * уедет из истории терминала.
 */
static void report_bad_geometry(uint32_t fb_bytes, uint16_t w, uint16_t h,
                                uint32_t stride)
{
    s_stats.frames_dropped++;
    if (s_geom_reported) {
        return;
    }
    s_geom_reported = true;
    printf("[ERROR] fbstream: геометрия кадра не влезает в буфер: "
           "%ux%u stride=%lu buf=%lu Б — кадры дропаются\r\n",
           (unsigned)w, (unsigned)h, (unsigned long)stride,
           (unsigned long)fb_bytes);
}

/*
 * TX идёт через orca_link_service_send: там общий мьютекс и единая
 * нумерация seq. Свой буфер со своим счётчиком давал на одном UART два
 * независимых потока кадров — на ПК seq скакал назад.
 */
static bool send_frame(uint8_t type, const void* payload, uint16_t len)
{
    if (!orca_link_service_send(type, payload, len)) {
        return false;
    }
    s_stats.bytes_sent += ORCA_LINK_HEADER_SIZE + len + ORCA_LINK_CRC_SIZE;
    return true;
}

void orca_fbstream_init(const orca_fbstream_transport_t* transport)
{
    s_tr = transport;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.display_id = 0;
    s_stats.pixfmt     = ORCA_LINK_PIXFMT_RGB565;
    s_stats.scale      = 2;   /* по умолчанию половина разрешения — щадим UART */
    s_stats.max_fps    = 10;
    s_stats.source     = ORCA_FB_SRC_GUI;
    s_frame_id = 0;
    s_geom_reported = false;
}

/*
 * s_frame_id при смене источника продолжает расти, а не начинается заново:
 * клиент собирает чанки по frame_id, и повторный номер склеил бы хвост кадра
 * старого источника с началом кадра нового.
 */
void orca_fbstream_set_source(orca_fb_source_t src)
{
    s_stats.source = (uint8_t)src;
}

orca_fb_source_t orca_fbstream_get_source(void)
{
    return (orca_fb_source_t)s_stats.source;
}

void orca_fbstream_enable(bool enable)
{
    s_stats.enabled = enable;
}

bool orca_fbstream_is_enabled(void)
{
    return s_stats.enabled;
}

void orca_fbstream_configure(const orca_link_fb_req_t* req)
{
    if (req == NULL) {
        return;
    }
    s_stats.enabled    = (req->enable != 0);
    s_stats.display_id = req->display_id;
    if (req->pixfmt != 0) {
        s_stats.pixfmt = req->pixfmt;
    }
    s_stats.scale   = (req->scale == 0) ? 1 : req->scale;
    s_stats.max_fps = (req->max_fps == 0) ? 10 : req->max_fps;
}

void orca_fbstream_get_stats(orca_fbstream_stats_t* out)
{
    if (out != NULL) {
        *out = s_stats;
    }
}

/* Конвертация одного пикселя источника (RGB565) в целевой формат. */
static uint32_t convert_pixel(uint16_t src, uint8_t dst_fmt, uint8_t* out)
{
    switch (dst_fmt) {
        case ORCA_LINK_PIXFMT_RGB565:
            out[0] = (uint8_t)(src & 0xFFu);
            out[1] = (uint8_t)(src >> 8);
            return 2;

        case ORCA_LINK_PIXFMT_RGB888: {
            uint8_t r = (uint8_t)((src >> 11) & 0x1Fu);
            uint8_t g = (uint8_t)((src >> 5) & 0x3Fu);
            uint8_t b = (uint8_t)(src & 0x1Fu);
            out[0] = (uint8_t)((r << 3) | (r >> 2));
            out[1] = (uint8_t)((g << 2) | (g >> 4));
            out[2] = (uint8_t)((b << 3) | (b >> 2));
            return 3;
        }

        case ORCA_LINK_PIXFMT_GRAY8: {
            uint8_t r = (uint8_t)(((src >> 11) & 0x1Fu) << 3);
            uint8_t g = (uint8_t)(((src >> 5) & 0x3Fu) << 2);
            uint8_t b = (uint8_t)((src & 0x1Fu) << 3);
            out[0] = (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
            return 1;
        }

        default:
            out[0] = (uint8_t)(src & 0xFFu);
            out[1] = (uint8_t)(src >> 8);
            return 2;
    }
}

/* ------------------------------------------------------------------ */
/* RLE_RGB565                                                          */
/* ------------------------------------------------------------------ */
/*
 * Формат (объявлен в orca_link.h, декодер уже есть в Android —
 * FrameAssembler.kt): подряд идущие пары [count:u8][pixel:u16 LE],
 * count = 1..255.
 *
 * Зачем это здесь. UART 921600 отдаёт 92 КБ/с, кадр 240x160 RGB565 — это
 * 76.8 КБ, то есть 1.2 кадра в секунду; полный 480x320 — 300 КБ и три
 * секунды на кадр. Интерфейс LVGL при этом состоит из заливок, где соседние
 * пиксели равны, и сжимается такой картинкой в разы. Без сжатия трансляция
 * не может быть живой в принципе — это ограничение линии, а не клиента.
 *
 * Проходов два: FB_INFO обязан назвать payload_bytes и chunk_count ДО того,
 * как пойдут данные, а размер после RLE заранее неизвестен. Первый проход
 * считает, второй отправляет. Кадр между проходами не меняется: GUI
 * транслирует снимок из ORCA_FB_SLOT_GUI_TX (см. orca_lvgl.h), а не
 * буфер, в который рисует.
 *
 * Проходы обязаны совпасть байт в байт, иначе клиент получит кадр не того
 * размера, который ему обещали. Поэтому это ОДНА функция с флагом «считать
 * или отправлять», а не два похожих цикла: два цикла разъезжаются на первой
 * же правке.
 *
 * Прогон не переходит на следующую строку: строки читаются с прореживанием
 * по scale, и склейка через границу строки сэкономила бы одну пару на строку,
 * зато сделала бы оба прохода зависимыми от порядка обхода. При count <= 255
 * экономия всё равно в пределах процента.
 */
#define RLE_PAIR_SIZE  3u
#define RLE_MAX_RUN    255u

typedef struct {
    uint32_t frame_id;
    uint8_t* buf;        /* chunk_buf: заголовок FB_CHUNK + данные */
    uint8_t* data;
    uint32_t cap;        /* сколько данных влезает в один чанк */
    uint32_t pos;
    uint16_t index;
    bool     failed;     /* транспорт отказал — дальше отправлять нечего */
} rle_tx_t;

static bool rle_flush(rle_tx_t* tx)
{
    if (tx->pos == 0) {
        return true;
    }
    orca_link_fb_chunk_t* ch = (orca_link_fb_chunk_t*)tx->buf;
    ch->frame_id    = tx->frame_id;
    ch->chunk_index = tx->index++;
    ch->data_len    = (uint16_t)tx->pos;
    bool ok = send_frame(ORCA_LINK_FB_CHUNK, tx->buf,
                         (uint16_t)(sizeof(orca_link_fb_chunk_t) + tx->pos));
    tx->pos = 0;
    if (!ok) {
        tx->failed = true;
    }
    return ok;
}

static void rle_emit(rle_tx_t* tx, uint8_t count, uint16_t px)
{
    if (tx == NULL || tx->failed) {
        return;           /* проход подсчёта или уже сорвавшаяся отправка */
    }
    /* Пара из трёх байт не режется между чанками: клиент читает её целиком. */
    if (tx->pos + RLE_PAIR_SIZE > tx->cap && !rle_flush(tx)) {
        return;
    }
    tx->data[tx->pos++] = count;
    tx->data[tx->pos++] = (uint8_t)(px & 0xFFu);
    tx->data[tx->pos++] = (uint8_t)(px >> 8);
}

/* Возвращает размер кадра после RLE. tx == NULL — только посчитать. */
static uint32_t rle_walk(const uint8_t* base, uint32_t stride, uint8_t scale,
                         uint16_t out_w, uint16_t out_h, rle_tx_t* tx)
{
    uint32_t bytes = 0;

    for (uint16_t y = 0; y < out_h; y++) {
        const uint16_t* row = (const uint16_t*)(base + (uint32_t)y * scale * stride);
        uint16_t run_px  = 0;
        uint32_t run_len = 0;

        for (uint16_t x = 0; x < out_w; x++) {
            uint16_t px = row[(uint32_t)x * scale];
            if (run_len != 0 && px == run_px && run_len < RLE_MAX_RUN) {
                run_len++;
                continue;
            }
            if (run_len != 0) {
                bytes += RLE_PAIR_SIZE;
                rle_emit(tx, (uint8_t)run_len, run_px);
            }
            run_px  = px;
            run_len = 1;
        }
        if (run_len != 0) {
            bytes += RLE_PAIR_SIZE;
            rle_emit(tx, (uint8_t)run_len, run_px);
        }
        if (tx != NULL && tx->failed) {
            return bytes;
        }
    }
    return bytes;
}

void orca_fbstream_push_frame(orca_fb_source_t src, uint8_t display_id,
                              const void* fb, uint32_t fb_bytes,
                              uint16_t width, uint16_t height,
                              uint32_t stride, uint8_t src_pixfmt)
{
    if (!s_stats.enabled || fb == NULL || s_tr == NULL) {
        return;
    }
    /*
     * Кадр не от выбранного источника — молча выходим. Это не дроп:
     * frames_dropped считает кадры, которые не влезли в линию, и мешать
     * туда «второй продюсер что-то нарисовал» значит потерять смысл счётчика.
     */
    if ((uint8_t)src != s_stats.source) {
        return;
    }
    if (display_id != s_stats.display_id) {
        return;
    }
    /* Источником пока считаем только RGB565 (формат LTDC-фреймбуфера). */
    if (src_pixfmt != ORCA_LINK_PIXFMT_RGB565) {
        return;
    }

    uint32_t now = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t min_interval = 1000u / (s_stats.max_fps ? s_stats.max_fps : 1u);
    if (now - s_last_send_ms < min_interval) {
        return;
    }

    uint8_t  scale  = s_stats.scale ? s_stats.scale : 1;
    uint16_t out_w  = (uint16_t)(width / scale);
    uint16_t out_h  = (uint16_t)(height / scale);
    if (out_w == 0 || out_h == 0) {
        return;
    }

    /*
     * width/height/stride приходят от продюсера кадров, а читаем мы по ним
     * чужой буфер в SDRAM. Проверяем, что самый дальний байт обхода лежит
     * внутри fb_bytes: иначе завышенный height — это не «плохая картинка»,
     * а чтение за границей буфера, которое отладить нечем.
     * Ошибка в размерах — вина вызывающего, поэтому громко и один раз:
     * молчаливый выход выглядел бы как «трансляция просто не работает».
     */
    if (stride < (uint32_t)width * SRC_BPP || fb_bytes == 0) {
        report_bad_geometry(fb_bytes, width, height, stride);
        return;
    }
    uint32_t last_byte = (uint32_t)(out_h - 1u) * scale * stride +
                         ((uint32_t)(out_w - 1u) * scale + 1u) * SRC_BPP;
    if (last_byte > fb_bytes) {
        report_bad_geometry(fb_bytes, width, height, stride);
        return;
    }

    const uint8_t* base = (const uint8_t*)fb;

    /*
     * Формат кадра решается здесь, а не берётся из s_stats напрямую: RLE
     * может оказаться невыгодным, и тогда этот кадр уйдёт обычным RGB565.
     * Клиенту это ничего не ломает — pixfmt стоит в FB_INFO каждого кадра.
     */
    uint8_t  pixfmt = s_stats.pixfmt;
    uint32_t chunk_cap;
    uint32_t total_bytes;

    if (pixfmt == ORCA_LINK_PIXFMT_RLE_RGB565) {
        chunk_cap   = (FB_CHUNK_PAYLOAD / RLE_PAIR_SIZE) * RLE_PAIR_SIZE;
        total_bytes = rle_walk(base, stride, scale, out_w, out_h, NULL);
        /*
         * Худший случай RLE — картинка без двух одинаковых соседей: 3 байта
         * на пиксель против 2 у RGB565, то есть кадр раздувается в 1.5 раза.
         * Фото или градиент на весь экран — ровно этот случай, и отдавать
         * его сжатым значит сделать трансляцию медленнее, чем без сжатия.
         */
        if (total_bytes >= (uint32_t)out_w * out_h * 2u) {
            pixfmt = ORCA_LINK_PIXFMT_RGB565;
        }
    }

    if (pixfmt != ORCA_LINK_PIXFMT_RLE_RGB565) {
        uint8_t  probe[4];
        uint32_t bpp = convert_pixel(0, pixfmt, probe);

        /*
         * Пиксель не режется между чанками, поэтому ёмкость чанка — целое
         * число пикселей, а не весь FB_CHUNK_PAYLOAD. При RGB888 (bpp=3) разница
         * давала chunk_count меньше фактического числа отправленных чанков.
         */
        chunk_cap   = (FB_CHUNK_PAYLOAD / bpp) * bpp;
        total_bytes = (uint32_t)out_w * out_h * bpp;
    }

    uint32_t chunk_count = (total_bytes + chunk_cap - 1) / chunk_cap;

    /*
     * Если транспорт заведомо не тянет кадр — дропаем, GUI не ждёт.
     *
     * Порог — полный размер ближайшего кадра, то есть FB_INFO целиком:
     * заголовок + payload + CRC. Проверка на одни 8 байт заголовка почти
     * всегда отвечала «место есть», нехватка обнаруживалась уже посреди
     * записи, и кадр рвался на середине вместо чистого пропуска — ровно то,
     * что этот файл обещает не делать.
     */
    const uint32_t info_frame_bytes =
        ORCA_LINK_HEADER_SIZE + (uint32_t)sizeof(orca_link_fb_info_t) + ORCA_LINK_CRC_SIZE;
    if (s_tr->writable != NULL && s_tr->writable() < info_frame_bytes) {
        s_stats.frames_dropped++;
        return;
    }

    s_frame_id++;

    orca_link_fb_info_t info = {
        .frame_id      = s_frame_id,
        .width         = out_w,
        .height        = out_h,
        .pixfmt        = pixfmt,
        .display_id    = display_id,
        .chunk_count   = (uint16_t)chunk_count,
        .payload_bytes = total_bytes
    };
    if (!send_frame(ORCA_LINK_FB_INFO, &info, sizeof(info))) {
        s_stats.frames_dropped++;
        return;
    }

    /* Разбираем framebuffer построчно с прореживанием по scale. */
    static uint8_t chunk_buf[ORCA_LINK_MAX_PAYLOAD];
    orca_link_fb_chunk_t* ch = (orca_link_fb_chunk_t*)chunk_buf;
    uint8_t* data = chunk_buf + sizeof(orca_link_fb_chunk_t);
    uint32_t data_pos = 0;
    uint16_t chunk_index = 0;

    if (pixfmt == ORCA_LINK_PIXFMT_RLE_RGB565) {
        rle_tx_t tx = {
            .frame_id = s_frame_id,
            .buf      = chunk_buf,
            .data     = data,
            .cap      = chunk_cap,
            .pos      = 0,
            .index    = 0,
            .failed   = false
        };
        uint32_t sent = rle_walk(base, stride, scale, out_w, out_h, &tx);
        if (!tx.failed) {
            rle_flush(&tx);
        }
        if (tx.failed) {
            s_stats.frames_dropped++;
            return;
        }
        /*
         * Второй проход обязан дать ровно то, что обещано в FB_INFO. Если
         * не дал, клиент уже ждёт другое число байт: кадр битый, и лучше
         * оборвать его на FB_CHUNK, чем прислать FB_END и заставить клиент
         * нарисовать мусор.
         */
        if (sent != total_bytes) {
            s_stats.frames_dropped++;
            return;
        }
        send_frame(ORCA_LINK_FB_END, &s_frame_id, sizeof(s_frame_id));
        s_stats.frames_sent++;
        s_last_send_ms = now;
        return;
    }

    for (uint16_t y = 0; y < out_h; y++) {
        const uint16_t* row = (const uint16_t*)(base + (uint32_t)y * scale * stride);
        for (uint16_t x = 0; x < out_w; x++) {
            uint8_t px[4];
            uint32_t n = convert_pixel(row[(uint32_t)x * scale], pixfmt, px);

            if (data_pos + n > chunk_cap) {
                ch->frame_id    = s_frame_id;
                ch->chunk_index = chunk_index++;
                ch->data_len    = (uint16_t)data_pos;
                if (!send_frame(ORCA_LINK_FB_CHUNK, chunk_buf,
                                (uint16_t)(sizeof(orca_link_fb_chunk_t) + data_pos))) {
                    s_stats.frames_dropped++;
                    return;
                }
                data_pos = 0;
            }

            memcpy(&data[data_pos], px, n);
            data_pos += n;
        }
    }

    if (data_pos > 0) {
        ch->frame_id    = s_frame_id;
        ch->chunk_index = chunk_index++;
        ch->data_len    = (uint16_t)data_pos;
        if (!send_frame(ORCA_LINK_FB_CHUNK, chunk_buf,
                        (uint16_t)(sizeof(orca_link_fb_chunk_t) + data_pos))) {
            s_stats.frames_dropped++;
            return;
        }
    }

    send_frame(ORCA_LINK_FB_END, &s_frame_id, sizeof(s_frame_id));

    s_stats.frames_sent++;
    s_last_send_ms = now;
}
