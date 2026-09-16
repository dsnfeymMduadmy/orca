#include "orca_gfx.h"
#include <string.h>

/*
 * Внутреннее представление — RGB565, как у панели. Все входные цвета
 * ARGB8888 разбираются здесь, чтобы вызывающий код не знал про формат.
 */

static inline uint16_t to565(uint32_t argb)
{
    uint32_t r = (argb >> 16) & 0xFFu;
    uint32_t g = (argb >> 8) & 0xFFu;
    uint32_t b = argb & 0xFFu;
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/*
 * Смешивание с уже лежащим в буфере пикселем. a — 0..255.
 *
 * Раскладываем 565 обратно в 8 бит на канал приблизительно (сдвиг +
 * дополнение старшими битами), иначе на градиентах и сглаженном тексте
 * заметно «съедаются» тёмные тона.
 */
static inline void blend_px(uint16_t* dst, uint32_t sr, uint32_t sg, uint32_t sb,
                            uint32_t a)
{
    if (a >= 255u) {
        *dst = (uint16_t)(((sr & 0xF8u) << 8) | ((sg & 0xFCu) << 3) | (sb >> 3));
        return;
    }

    uint16_t d = *dst;
    uint32_t dr = (uint32_t)((d >> 11) & 0x1Fu);
    uint32_t dg = (uint32_t)((d >> 5) & 0x3Fu);
    uint32_t db = (uint32_t)(d & 0x1Fu);
    dr = (dr << 3) | (dr >> 2);
    dg = (dg << 2) | (dg >> 4);
    db = (db << 3) | (db >> 2);

    uint32_t ia = 255u - a;
    uint32_t r = (sr * a + dr * ia + 127u) / 255u;
    uint32_t gg = (sg * a + dg * ia + 127u) / 255u;
    uint32_t b = (sb * a + db * ia + 127u) / 255u;

    *dst = (uint16_t)(((r & 0xF8u) << 8) | ((gg & 0xFCu) << 3) | (b >> 3));
}

static inline bool clipped_out(const orca_gfx_t* g)
{
    return g == NULL || g->pixels == NULL ||
           g->clip_x0 >= g->clip_x1 || g->clip_y0 >= g->clip_y1;
}

void orca_gfx_init(orca_gfx_t* g, uint16_t* pixels, int32_t w, int32_t h,
                   int32_t stride_px)
{
    if (g == NULL) {
        return;
    }
    g->pixels = pixels;
    g->w = w;
    g->h = h;
    g->stride_px = (stride_px > 0) ? stride_px : w;
    orca_gfx_clip_reset(g);
}

void orca_gfx_clip_reset(orca_gfx_t* g)
{
    if (g == NULL) {
        return;
    }
    g->clip_x0 = 0;
    g->clip_y0 = 0;
    g->clip_x1 = g->w;
    g->clip_y1 = g->h;
}

void orca_gfx_clip_set(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (g == NULL) {
        return;
    }
    int32_t x1 = x + w;
    int32_t y1 = y + h;

    /* Пересечение с текущим окном: вложенные слои не должны его расширять. */
    if (x > g->clip_x0) { g->clip_x0 = x; }
    if (y > g->clip_y0) { g->clip_y0 = y; }
    if (x1 < g->clip_x1) { g->clip_x1 = x1; }
    if (y1 < g->clip_y1) { g->clip_y1 = y1; }

    if (g->clip_x0 < 0) { g->clip_x0 = 0; }
    if (g->clip_y0 < 0) { g->clip_y0 = 0; }
    if (g->clip_x1 > g->w) { g->clip_x1 = g->w; }
    if (g->clip_y1 > g->h) { g->clip_y1 = g->h; }
}

void orca_gfx_pixel(orca_gfx_t* g, int32_t x, int32_t y, uint32_t color)
{
    if (clipped_out(g) ||
        x < g->clip_x0 || x >= g->clip_x1 || y < g->clip_y0 || y >= g->clip_y1) {
        return;
    }
    blend_px(&g->pixels[y * g->stride_px + x],
             (color >> 16) & 0xFFu, (color >> 8) & 0xFFu, color & 0xFFu,
             (color >> 24) & 0xFFu);
}

void orca_gfx_fill_rect(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h,
                        uint32_t color)
{
    if (clipped_out(g) || w <= 0 || h <= 0) {
        return;
    }

    int32_t x0 = (x > g->clip_x0) ? x : g->clip_x0;
    int32_t y0 = (y > g->clip_y0) ? y : g->clip_y0;
    int32_t x1 = (x + w < g->clip_x1) ? x + w : g->clip_x1;
    int32_t y1 = (y + h < g->clip_y1) ? y + h : g->clip_y1;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    uint32_t a = (color >> 24) & 0xFFu;
    if (a == 0) {
        return;
    }

    if (a >= 255u) {
        /* Непрозрачная заливка — пишем напрямую, без чтения из SDRAM. */
        uint16_t c = to565(color);
        for (int32_t yy = y0; yy < y1; yy++) {
            uint16_t* row = &g->pixels[yy * g->stride_px + x0];
            for (int32_t xx = x0; xx < x1; xx++) {
                *row++ = c;
            }
        }
        return;
    }

    uint32_t sr = (color >> 16) & 0xFFu;
    uint32_t sg = (color >> 8) & 0xFFu;
    uint32_t sb = color & 0xFFu;
    for (int32_t yy = y0; yy < y1; yy++) {
        uint16_t* row = &g->pixels[yy * g->stride_px + x0];
        for (int32_t xx = x0; xx < x1; xx++) {
            blend_px(row++, sr, sg, sb, a);
        }
    }
}

void orca_gfx_clear(orca_gfx_t* g, uint32_t color)
{
    if (g == NULL) {
        return;
    }
    orca_gfx_fill_rect(g, 0, 0, g->w, g->h, color | 0xFF000000u);
}

void orca_gfx_stroke_rect(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t thickness, uint32_t color)
{
    if (thickness <= 0 || w <= 0 || h <= 0) {
        return;
    }
    if (thickness * 2 >= w || thickness * 2 >= h) {
        orca_gfx_fill_rect(g, x, y, w, h, color);
        return;
    }
    orca_gfx_fill_rect(g, x, y, w, thickness, color);
    orca_gfx_fill_rect(g, x, y + h - thickness, w, thickness, color);
    orca_gfx_fill_rect(g, x, y + thickness, thickness, h - thickness * 2, color);
    orca_gfx_fill_rect(g, x + w - thickness, y + thickness, thickness,
                       h - thickness * 2, color);
}

void orca_gfx_fill_vgradient(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t top_color, uint32_t bottom_color)
{
    if (clipped_out(g) || w <= 0 || h <= 0) {
        return;
    }

    int32_t tr = (int32_t)((top_color >> 16) & 0xFFu);
    int32_t tg = (int32_t)((top_color >> 8) & 0xFFu);
    int32_t tb = (int32_t)(top_color & 0xFFu);
    int32_t ta = (int32_t)((top_color >> 24) & 0xFFu);
    int32_t br = (int32_t)((bottom_color >> 16) & 0xFFu);
    int32_t bg = (int32_t)((bottom_color >> 8) & 0xFFu);
    int32_t bb = (int32_t)(bottom_color & 0xFFu);
    int32_t ba = (int32_t)((bottom_color >> 24) & 0xFFu);

    int32_t y0 = (y > g->clip_y0) ? y : g->clip_y0;
    int32_t y1 = (y + h < g->clip_y1) ? y + h : g->clip_y1;
    int32_t denom = (h > 1) ? (h - 1) : 1;

    for (int32_t yy = y0; yy < y1; yy++) {
        int32_t t = yy - y;
        uint32_t r = (uint32_t)(tr + (br - tr) * t / denom);
        uint32_t gg = (uint32_t)(tg + (bg - tg) * t / denom);
        uint32_t b = (uint32_t)(tb + (bb - tb) * t / denom);
        uint32_t a = (uint32_t)(ta + (ba - ta) * t / denom);
        orca_gfx_fill_rect(g, x, yy, w, 1, ORCA_ARGB(a, r, gg, b));
    }
}

/*
 * Покрытие пикселя для скругления/круга. Считаем в целых: сравниваем
 * квадрат расстояния до центра дуги с (r-1)^2 и (r+1)^2, между ними —
 * линейная оценка. Идея — дать сглаженную кромку без sqrt на пиксель;
 * ступенчатые скругления сразу выдают самодельный интерфейс.
 */
static inline uint32_t corner_coverage(int32_t dx, int32_t dy, int32_t r)
{
    int32_t d2 = dx * dx + dy * dy;
    int32_t inner = (r - 1) * (r - 1);
    int32_t outer = (r + 1) * (r + 1);

    if (d2 <= inner) {
        return 255u;
    }
    if (d2 >= outer) {
        return 0u;
    }
    return (uint32_t)(255 * (outer - d2) / (outer - inner));
}

void orca_gfx_fill_round_rect(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t radius, uint32_t color)
{
    if (clipped_out(g) || w <= 0 || h <= 0) {
        return;
    }
    int32_t max_r = ((w < h) ? w : h) / 2;
    if (radius > max_r) { radius = max_r; }
    if (radius <= 0) {
        orca_gfx_fill_rect(g, x, y, w, h, color);
        return;
    }

    /* Середина — обычными прямоугольниками, дуги — только в углах. */
    orca_gfx_fill_rect(g, x + radius, y, w - radius * 2, h, color);
    orca_gfx_fill_rect(g, x, y + radius, radius, h - radius * 2, color);
    orca_gfx_fill_rect(g, x + w - radius, y + radius, radius, h - radius * 2, color);

    uint32_t sr = (color >> 16) & 0xFFu;
    uint32_t sg = (color >> 8) & 0xFFu;
    uint32_t sb = color & 0xFFu;
    uint32_t sa = (color >> 24) & 0xFFu;
    if (sa == 0) {
        return;
    }

    for (int32_t j = 0; j < radius; j++) {
        for (int32_t i = 0; i < radius; i++) {
            int32_t dx = radius - i;
            int32_t dy = radius - j;
            uint32_t cov = corner_coverage(dx, dy, radius);
            if (cov == 0) {
                continue;
            }
            uint32_t a = (cov * sa + 127u) / 255u;

            const int32_t px[4] = { x + i, x + w - 1 - i, x + i, x + w - 1 - i };
            const int32_t py[4] = { y + j, y + j, y + h - 1 - j, y + h - 1 - j };
            for (int32_t k = 0; k < 4; k++) {
                if (px[k] < g->clip_x0 || px[k] >= g->clip_x1 ||
                    py[k] < g->clip_y0 || py[k] >= g->clip_y1) {
                    continue;
                }
                blend_px(&g->pixels[py[k] * g->stride_px + px[k]], sr, sg, sb, a);
            }
        }
    }
}

void orca_gfx_stroke_round_rect(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h,
                                int32_t radius, int32_t thickness, uint32_t color)
{
    if (thickness <= 0) {
        return;
    }
    /*
     * Контур = внешняя фигура минус внутренняя. Рисовать вычитанием нельзя
     * (буфер не имеет альфы), поэтому идём по кольцу: считаем покрытие для
     * внешнего и внутреннего радиуса и берём разницу.
     */
    int32_t max_r = ((w < h) ? w : h) / 2;
    if (radius > max_r) { radius = max_r; }
    if (radius <= 0) {
        orca_gfx_stroke_rect(g, x, y, w, h, thickness, color);
        return;
    }

    /* Прямые участки. */
    orca_gfx_fill_rect(g, x + radius, y, w - radius * 2, thickness, color);
    orca_gfx_fill_rect(g, x + radius, y + h - thickness, w - radius * 2, thickness, color);
    orca_gfx_fill_rect(g, x, y + radius, thickness, h - radius * 2, color);
    orca_gfx_fill_rect(g, x + w - thickness, y + radius, thickness, h - radius * 2, color);

    uint32_t sr = (color >> 16) & 0xFFu;
    uint32_t sg = (color >> 8) & 0xFFu;
    uint32_t sb = color & 0xFFu;
    uint32_t sa = (color >> 24) & 0xFFu;
    int32_t inner_r = radius - thickness;

    for (int32_t j = 0; j < radius; j++) {
        for (int32_t i = 0; i < radius; i++) {
            int32_t dx = radius - i;
            int32_t dy = radius - j;
            int32_t cov = (int32_t)corner_coverage(dx, dy, radius);
            if (inner_r > 0) {
                cov -= (int32_t)corner_coverage(dx, dy, inner_r);
            }
            if (cov <= 0) {
                continue;
            }
            uint32_t a = ((uint32_t)cov * sa + 127u) / 255u;

            const int32_t px[4] = { x + i, x + w - 1 - i, x + i, x + w - 1 - i };
            const int32_t py[4] = { y + j, y + j, y + h - 1 - j, y + h - 1 - j };
            for (int32_t k = 0; k < 4; k++) {
                if (px[k] < g->clip_x0 || px[k] >= g->clip_x1 ||
                    py[k] < g->clip_y0 || py[k] >= g->clip_y1) {
                    continue;
                }
                blend_px(&g->pixels[py[k] * g->stride_px + px[k]], sr, sg, sb, a);
            }
        }
    }
}

void orca_gfx_fill_circle(orca_gfx_t* g, int32_t cx, int32_t cy, int32_t radius,
                          uint32_t color)
{
    if (clipped_out(g) || radius <= 0) {
        return;
    }
    uint32_t sr = (color >> 16) & 0xFFu;
    uint32_t sg = (color >> 8) & 0xFFu;
    uint32_t sb = color & 0xFFu;
    uint32_t sa = (color >> 24) & 0xFFu;
    if (sa == 0) {
        return;
    }

    for (int32_t dy = -radius; dy <= radius; dy++) {
        int32_t yy = cy + dy;
        if (yy < g->clip_y0 || yy >= g->clip_y1) {
            continue;
        }
        for (int32_t dx = -radius; dx <= radius; dx++) {
            int32_t xx = cx + dx;
            if (xx < g->clip_x0 || xx >= g->clip_x1) {
                continue;
            }
            uint32_t cov = corner_coverage(dx, dy, radius);
            if (cov == 0) {
                continue;
            }
            blend_px(&g->pixels[yy * g->stride_px + xx], sr, sg, sb,
                     (cov * sa + 127u) / 255u);
        }
    }
}

void orca_gfx_line(orca_gfx_t* g, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint32_t color)
{
    /* Брезенхэм: линии в интерфейсе — разделители, сглаживание им не нужно. */
    int32_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx - dy;

    for (;;) {
        orca_gfx_pixel(g, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int32_t e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

uint32_t orca_gfx_text_width(orca_font_id_t font, const char* s)
{
    return orca_font_text_width(orca_font_get(font), s);
}

uint32_t orca_gfx_font_height(orca_font_id_t font)
{
    return orca_font_get(font)->line_height;
}

void orca_gfx_text(orca_gfx_t* g, orca_font_id_t font, int32_t x, int32_t y,
                   const char* s, uint32_t color)
{
    if (clipped_out(g) || s == NULL) {
        return;
    }

    const orca_font_t* f = orca_font_get(font);
    uint32_t sr = (color >> 16) & 0xFFu;
    uint32_t sg = (color >> 8) & 0xFFu;
    uint32_t sb = color & 0xFFu;
    uint32_t sa = (color >> 24) & 0xFFu;
    if (sa == 0) {
        return;
    }

    int32_t pen = x;
    while (*s != '\0') {
        uint32_t cp;
        s = orca_utf8_next(s, &cp);
        const orca_glyph_t* gl = orca_font_glyph(f, cp);
        if (gl == NULL) {
            continue;
        }

        int32_t gx = pen + gl->left;
        int32_t gy = y + gl->top;
        const uint8_t* src = &f->pixels[gl->offset];

        for (int32_t j = 0; j < (int32_t)gl->h; j++) {
            int32_t yy = gy + j;
            if (yy < g->clip_y0 || yy >= g->clip_y1) {
                continue;
            }
            const uint8_t* srow = src + j * gl->w;
            uint16_t* drow = &g->pixels[yy * g->stride_px];
            for (int32_t i = 0; i < (int32_t)gl->w; i++) {
                uint32_t cov = srow[i];
                if (cov == 0) {
                    continue;
                }
                int32_t xx = gx + i;
                if (xx < g->clip_x0 || xx >= g->clip_x1) {
                    continue;
                }
                blend_px(&drow[xx], sr, sg, sb,
                         (sa == 255u) ? cov : (cov * sa + 127u) / 255u);
            }
        }
        pen += gl->advance;
    }
}

void orca_gfx_text_aligned(orca_gfx_t* g, orca_font_id_t font,
                           int32_t x, int32_t y, int32_t box_w,
                           orca_align_t align, const char* s, uint32_t color)
{
    int32_t tw = (int32_t)orca_gfx_text_width(font, s);
    int32_t tx = x;
    if (align == ORCA_ALIGN_CENTER) {
        tx = x + (box_w - tw) / 2;
    } else if (align == ORCA_ALIGN_RIGHT) {
        tx = x + box_w - tw;
    }
    orca_gfx_text(g, font, tx, y, s, color);
}
