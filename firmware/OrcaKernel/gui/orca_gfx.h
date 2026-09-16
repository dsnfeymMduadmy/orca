#ifndef ORCA_GFX_H
#define ORCA_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include "orca_font.h"

/*
 * Примитивы рисования по framebuffer RGB565.
 *
 * Работает не «в текущий дисплей», а в явно переданный orca_gfx_t: ядру
 * нужно рисовать и в основной кадр, и во всплывающие слои, а глобальное
 * состояние на две задачи (десктоп + приложение) сразу же привело бы к
 * гонке за «текущим» буфером.
 *
 * Цвет наружу — ARGB8888 (0xAARRGGBB), внутри — RGB565. Модулю не нужно
 * знать формат панели: он передаёт обычный #RRGGBB, а конверсия и
 * смешивание живут здесь. Альфа реально применяется, поэтому полупрозрачные
 * панели и тени — это одна строка в приложении, а не ручной блендинг.
 */

typedef struct {
    uint16_t* pixels;
    int32_t   w;
    int32_t   h;
    int32_t   stride_px;   /* пикселей в строке (не байт) */

    /* Область отсечения: рисование за её пределы отбрасывается. */
    int32_t   clip_x0;
    int32_t   clip_y0;
    int32_t   clip_x1;     /* правая/нижняя границы не входят */
    int32_t   clip_y1;
} orca_gfx_t;

#define ORCA_ARGB(a, r, g, b) \
    ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
                ((uint32_t)(g) << 8)  |  (uint32_t)(b)))
#define ORCA_RGB(r, g, b) ORCA_ARGB(0xFF, r, g, b)

void orca_gfx_init(orca_gfx_t* g, uint16_t* pixels, int32_t w, int32_t h,
                   int32_t stride_px);

/* Отсечение. set сохраняет пересечение с текущим, reset снимает. */
void orca_gfx_clip_set(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h);
void orca_gfx_clip_reset(orca_gfx_t* g);

void orca_gfx_clear(orca_gfx_t* g, uint32_t color);
void orca_gfx_pixel(orca_gfx_t* g, int32_t x, int32_t y, uint32_t color);
void orca_gfx_fill_rect(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h,
                        uint32_t color);
void orca_gfx_stroke_rect(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t thickness, uint32_t color);

/*
 * Прямоугольник со скруглёнными углами, со сглаженной границей. Основа
 * всего интерфейса: панели, кнопки, плитки приложений.
 */
void orca_gfx_fill_round_rect(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t radius, uint32_t color);
void orca_gfx_stroke_round_rect(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h,
                                int32_t radius, int32_t thickness, uint32_t color);

void orca_gfx_fill_circle(orca_gfx_t* g, int32_t cx, int32_t cy, int32_t radius,
                          uint32_t color);

/*
 * Вертикальный градиент. Обои и панели без него выглядят как плоская
 * заливка, а это первое, по чему интерфейс читается как дешёвый.
 */
void orca_gfx_fill_vgradient(orca_gfx_t* g, int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t top_color, uint32_t bottom_color);

void orca_gfx_line(orca_gfx_t* g, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint32_t color);

typedef enum {
    ORCA_ALIGN_LEFT = 0,
    ORCA_ALIGN_CENTER,
    ORCA_ALIGN_RIGHT
} orca_align_t;

/*
 * Текст. y — верх строки, не базовая линия: так вызывающему не нужно
 * помнить ascent каждого кегля, чтобы разместить надпись в блоке.
 */
void orca_gfx_text(orca_gfx_t* g, orca_font_id_t font, int32_t x, int32_t y,
                   const char* s, uint32_t color);
void orca_gfx_text_aligned(orca_gfx_t* g, orca_font_id_t font,
                           int32_t x, int32_t y, int32_t box_w,
                           orca_align_t align, const char* s, uint32_t color);

uint32_t orca_gfx_text_width(orca_font_id_t font, const char* s);
uint32_t orca_gfx_font_height(orca_font_id_t font);

#endif /* ORCA_GFX_H */
