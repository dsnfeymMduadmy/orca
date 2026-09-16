#!/usr/bin/env python3
"""
Генератор растровых шрифтов для GUI Orca.

Зачем отдельный инструмент: на устройстве нет ни FreeType, ни места под
TTF-парсер, а текст в интерфейсе должен выглядеть как текст, а не как
угловатые пиксельные буквы 5x7. Компромисс — растеризовать глифы здесь,
на ПК, и положить в прошивку карту прозрачности (8 бит на пиксель).
Ядро потом просто смешивает alpha с фоном: сглаживание получается
бесплатно, потому что вся тяжёлая работа уже сделана.

Формат alpha, а не 1 бит на пиксель, выбран сознательно: разница по флешу
восьмикратная, но 1-битный шрифт на 480x320 выглядит рвано, а весь смысл
затеи — красивый интерфейс.

Запуск (из корня репозитория):
    python3 tools/fontgen/make_font.py

Перегенерировать нужно только при смене гарнитуры/кеглей — результат
(firmware/OrcaKernel/gui/orca_font_data.c) лежит в репозитории, чтобы
сборка прошивки не требовала ни Python, ни шрифтов в системе.
"""

import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("нужен Pillow: pip install Pillow")

FONT_FILE = "/usr/share/fonts/inter/Inter[opsz,wght].ttf"

ASCII = [(0x20, 0x7E)]
CYRILLIC = [(0x401, 0x401), (0x410, 0x44F), (0x451, 0x451)]
DIGITS = [(0x20, 0x20), (0x2E, 0x2E), (0x30, 0x3A)]

# (имя, кегль в px, ось веса Inter, диапазоны символов)
FONTS = [
    ("small",   13, 450, ASCII + CYRILLIC),
    ("regular", 17, 450, ASCII + CYRILLIC),
    ("medium",  22, 600, ASCII + CYRILLIC),
    ("large",   58, 300, DIGITS),
]

OUT_C = "firmware/OrcaKernel/gui/orca_font_data.c"


def load(size, weight):
    f = ImageFont.truetype(FONT_FILE, size)
    try:
        f.set_variation_by_axes([14.0, weight])
    except Exception:
        pass
    return f


def codepoints(ranges):
    out = []
    for lo, hi in ranges:
        out.extend(range(lo, hi + 1))
    return out


def render(font, cp):
    """Растеризует один символ в 8-битную карту прозрачности."""
    ch = chr(cp)
    box = font.getbbox(ch)
    if box is None:
        box = (0, 0, 0, 0)
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    advance = int(round(font.getlength(ch)))

    if w <= 0 or h <= 0:  # пробел и прочие пустые
        return 0, 0, 0, 0, advance, b""

    pad = 2
    img = Image.new("L", (w + pad * 2, h + pad * 2), 0)
    ImageDraw.Draw(img).text((pad - x0, pad - y0), ch, font=font, fill=255)

    bbox = img.getbbox()
    if bbox is None:
        return 0, 0, 0, 0, advance, b""
    img = img.crop(bbox)
    # left/top — смещение от точки вставки и от верха строки соответственно.
    # Слагаемые x0/y0 обязательны: рисовали со сдвигом на -x0/-y0, и без
    # возврата этого сдвига все глифы прижимаются к верху строки — точка и
    # двоеточие уезжают под потолок, а цифры разной высоты теряют базовую
    # линию.
    return (img.width, img.height,
            x0 + bbox[0] - pad, y0 + bbox[1] - pad,
            advance, img.tobytes())


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if not os.path.exists(FONT_FILE):
        sys.exit("не найден %s" % FONT_FILE)

    blob = bytearray()
    tables = []

    for name, size, weight, ranges in FONTS:
        font = load(size, weight)
        ascent, descent = font.getmetrics()
        glyphs = []
        for cp in codepoints(ranges):
            w, h, left, top, adv, bits = render(font, cp)
            off = len(blob)
            blob.extend(bits)
            glyphs.append((cp, w, h, left, top, adv, off))
        tables.append((name, ascent, ascent + descent, ranges, glyphs))

    lines = []
    lines.append("/*")
    lines.append(" * СГЕНЕРИРОВАНО tools/fontgen/make_font.py — не править руками.")
    lines.append(" * Гарнитура: Inter. Формат: 8 бит прозрачности на пиксель.")
    lines.append(" */")
    lines.append('#include "orca_font.h"')
    lines.append("")

    lines.append("static const uint8_t s_pixels[] = {")
    for i in range(0, len(blob), 20):
        chunk = ",".join(str(b) for b in blob[i:i + 20])
        lines.append("    " + chunk + ",")
    lines.append("};")
    lines.append("")

    for name, ascent, line_h, ranges, glyphs in tables:
        lines.append("static const orca_glyph_t s_glyphs_%s[] = {" % name)
        for cp, w, h, left, top, adv, off in glyphs:
            lines.append("    { %5u, %3u, %3u, %4d, %4d, %3u, %7u }," %
                         (cp, w, h, left, top, adv, off))
        lines.append("};")
        lines.append("")
        lines.append("static const orca_font_range_t s_ranges_%s[] = {" % name)
        idx = 0
        for lo, hi in ranges:
            lines.append("    { 0x%04X, 0x%04X, %u }," % (lo, hi, idx))
            idx += hi - lo + 1
        lines.append("};")
        lines.append("")

    lines.append("const orca_font_t g_orca_fonts[ORCA_FONT_COUNT] = {")
    for name, ascent, line_h, ranges, glyphs in tables:
        lines.append("    { s_glyphs_%s, s_ranges_%s, %u, s_pixels, %u, %u }," %
                     (name, name, len(ranges), ascent, line_h))
    lines.append("};")
    lines.append("")

    path = os.path.join(root, OUT_C)
    with open(path, "w") as fh:
        fh.write("\n".join(lines))

    total = len(blob)
    print("глифов: %d, пикселей: %.1f КБ, файл: %s" %
          (sum(len(g[4]) for g in tables), total / 1024.0, OUT_C))
    for name, ascent, line_h, ranges, glyphs in tables:
        print("  %-8s ascent=%2d line=%2d глифов=%3d" %
              (name, ascent, line_h, len(glyphs)))


if __name__ == "__main__":
    main()
