#include "orca_font.h"
#include <stddef.h>

const orca_font_t* orca_font_get(orca_font_id_t id)
{
    if ((uint32_t)id >= ORCA_FONT_COUNT) {
        return &g_orca_fonts[ORCA_FONT_REGULAR];
    }
    return &g_orca_fonts[id];
}

const orca_glyph_t* orca_font_glyph(const orca_font_t* font, uint32_t codepoint)
{
    if (font == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < font->range_count; i++) {
        const orca_font_range_t* r = &font->ranges[i];
        if (codepoint >= r->first && codepoint <= r->last) {
            return &font->glyphs[r->glyph_index + (codepoint - r->first)];
        }
    }
    return NULL;
}

const char* orca_utf8_next(const char* s, uint32_t* out)
{
    uint32_t cp;
    uint8_t  c = (uint8_t)*s;
    uint32_t extra;

    if (c < 0x80u) {
        cp = c;
        extra = 0;
    } else if ((c & 0xE0u) == 0xC0u) {
        cp = c & 0x1Fu;
        extra = 1;
    } else if ((c & 0xF0u) == 0xE0u) {
        cp = c & 0x0Fu;
        extra = 2;
    } else if ((c & 0xF8u) == 0xF0u) {
        cp = c & 0x07u;
        extra = 3;
    } else {
        /* Продолжающий байт там, где ожидался ведущий: строка битая. */
        if (out != NULL) {
            *out = '?';
        }
        return s + 1;
    }

    s++;
    for (uint32_t i = 0; i < extra; i++) {
        if (((uint8_t)*s & 0xC0u) != 0x80u) {
            /* Обрыв последовательности — не глотаем следующий символ. */
            if (out != NULL) {
                *out = '?';
            }
            return s;
        }
        cp = (cp << 6) | ((uint8_t)*s & 0x3Fu);
        s++;
    }

    if (out != NULL) {
        *out = cp;
    }
    return s;
}

uint32_t orca_font_text_width(const orca_font_t* font, const char* s)
{
    if (font == NULL || s == NULL) {
        return 0;
    }

    uint32_t w = 0;
    while (*s != '\0') {
        uint32_t cp;
        s = orca_utf8_next(s, &cp);
        const orca_glyph_t* g = orca_font_glyph(font, cp);
        if (g != NULL) {
            w += g->advance;
        }
    }
    return w;
}
