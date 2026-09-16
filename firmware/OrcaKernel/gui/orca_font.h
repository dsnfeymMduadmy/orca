#ifndef ORCA_FONT_H
#define ORCA_FONT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Растровые шрифты интерфейса. Данные генерируются на ПК
 * (tools/fontgen/make_font.py) и лежат в orca_font_data.c: на устройстве
 * нет ни FreeType, ни свободного флеша под парсер TTF.
 *
 * Пиксель глифа — это 8 бит прозрачности, а не бит «есть/нет». Поэтому
 * текст выходит сглаженным: orca_gfx смешивает alpha с фоном. Битовый
 * шрифт стоил бы в 8 раз меньше флеша, но на 480x320 буквы выглядели бы
 * рвано, а вся затея — ради нормального вида интерфейса.
 *
 * Все глифы всех кеглей лежат в одном общем массиве пикселей, глиф
 * ссылается на него смещением: так нет ни дырок на выравнивании, ни
 * 4 отдельных массивов в карте памяти.
 */

typedef enum {
    ORCA_FONT_SMALL = 0,   /* 13 px — подписи, вторичный текст     */
    ORCA_FONT_REGULAR,     /* 17 px — основной текст интерфейса    */
    ORCA_FONT_MEDIUM,      /* 22 px — заголовки, имена приложений  */
    ORCA_FONT_LARGE,       /* 58 px — только цифры: часы           */
    ORCA_FONT_COUNT
} orca_font_id_t;

typedef struct {
    uint16_t codepoint;
    uint8_t  w;            /* размер карты прозрачности              */
    uint8_t  h;
    int8_t   left;         /* сдвиг от точки вставки по X            */
    int8_t   top;          /* сдвиг от верха строки по Y             */
    uint8_t  advance;      /* насколько сдвинуть точку вставки       */
    uint32_t offset;       /* смещение в общем массиве пикселей      */
} orca_glyph_t;

/*
 * Диапазон кодов -> непрерывный участок таблицы глифов. Кириллица в
 * Unicode далеко от ASCII, а держать таблицу на 1104 записи ради 66 букв
 * незачем — поэтому диапазоны, а не плоский индекс.
 */
typedef struct {
    uint16_t first;
    uint16_t last;
    uint16_t glyph_index;
} orca_font_range_t;

typedef struct {
    const orca_glyph_t*      glyphs;
    const orca_font_range_t* ranges;
    uint8_t                  range_count;
    const uint8_t*           pixels;
    uint8_t                  ascent;      /* от верха строки до базовой линии */
    uint8_t                  line_height;
} orca_font_t;

extern const orca_font_t g_orca_fonts[ORCA_FONT_COUNT];

/* NULL, если символа нет в этом кегле. */
const orca_glyph_t* orca_font_glyph(const orca_font_t* font, uint32_t codepoint);

/*
 * Разбор UTF-8: интерфейс подписан по-русски, а исходники модулей —
 * обычный UTF-8 без изысков. Возвращает следующую позицию в строке,
 * кодовую точку кладёт в out. Битые последовательности не разворачиваются
 * в ошибку: символ просто становится '?', чтобы приложение не падало
 * из-за кривой подписи.
 */
const char* orca_utf8_next(const char* s, uint32_t* out);

const orca_font_t* orca_font_get(orca_font_id_t id);

/* Ширина строки в пикселях (сумма advance с учётом UTF-8). */
uint32_t orca_font_text_width(const orca_font_t* font, const char* s);

#endif /* ORCA_FONT_H */
