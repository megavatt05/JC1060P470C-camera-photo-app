#!/usr/bin/env python3
"""Generate 8x8 Cyrillic glyphs for CamBrowser from DejaVu Sans Mono.

Output: C table app_overlay_font8x8_cyr[66][8] (bit0 = leftmost pixel,
matching app_overlay_font8x8) + ASCII-art preview for visual inspection.

Index: 0..31 = А..Я (U+0410..U+042F), 32..63 = а..я (U+0430..U+044F),
64 = Ё (U+0401), 65 = ё (U+0451).

The source font is looked up in ./ttf/ next to this script first, then in
the system font directory - so the tool works from a fresh repo clone.

Usage:  python3 gen_cyr_font.py [output.c]     (default: cyr_font_table.c)
The generated table is pasted into main/app_overlay.c (CI builds it from
there; fonts/ is a tool directory and is not compiled).
"""
import os
import sys

from PIL import Image, ImageFont, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
CANDIDATES = [
    os.path.join(HERE, "ttf", "DejaVuSansMono.ttf"),                # in-repo copy
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",          # system-wide
]
FONT_PATH = next((p for p in CANDIDATES if os.path.exists(p)), CANDIDATES[0])

UPPER = "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"          # 32, no Ё
LOWER = "абвгдежзийклмнопрстуфхцчшщъыьэюя"          # 32, no ё
CHARS = UPPER + LOWER + "Ёё"                        # 66 total

def render_cell(ch, size, y_shift, thr):
    """Render ch into a fixed 8x8 window with the font's own baseline.

    All glyphs are drawn at the same origin on a large canvas, then the
    same 8x8 window is cut out - so caps, x-height letters and descenders
    keep the baseline the font designed them for (no per-glyph jiggle)."""
    font = ImageFont.truetype(FONT_PATH, size)
    img = Image.new("L", (32, 32), 0)
    d = ImageDraw.Draw(img)
    d.text((12, 12 + y_shift), ch, font=font, fill=255)
    win = img.crop((12, 12, 20, 20))        # the 8x8 cell
    px = win.load()
    rows = []
    for r in range(8):
        b = 0
        for c in range(8):
            if px[c, r] >= thr:
                b |= 1 << c                 # bit0 = leftmost pixel
        rows.append(b)
    return rows

def art(rows):
    return "\n".join("".join("#" if (rows[r] >> c) & 1 else "."
                             for c in range(8)) for r in range(8))

# tune: fixed-baseline window; y_shift moves the window relative to the text
# origin (negative = glyph moves up). Threshold on antialiased coverage.
PARAMS = {"size": 8, "y_shift": -1, "thr": 110}

glyphs = []
for ch in CHARS:
    glyphs.append(render_cell(ch, PARAMS["size"], PARAMS["y_shift"], PARAMS["thr"]))

# ASCII art preview
print("font:", FONT_PATH)
print("=" * 30, "PREVIEW y_shift=%d thr=%d" % (PARAMS["y_shift"], PARAMS["thr"]), "=" * 30)
for i in range(0, len(CHARS), 8):
    chunk = CHARS[i:i + 8]
    arts = [art(glyphs[i + j]).split("\n") for j in range(len(chunk))]
    for r in range(8):
        print("  ".join(a[r] for a in arts))
    print("  ".join("----%s---" % c for c in chunk))
print("=" * 68)

# mixed-case sanity line rendered with the same pipeline
test = "Поиск"
tg = [render_cell(c, PARAMS["size"], PARAMS["y_shift"], PARAMS["thr"]) for c in test]
print("SANITY:", test)
for r in range(8):
    print(" ".join(a[r] for a in (art(g).split("\n") for g in tg)))

# C table
out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "cyr_font_table.c")
lines = []
lines.append("/* 8x8 Cyrillic glyphs, generated from DejaVu Sans Mono by")
lines.append(" * fonts/gen_cyr_font.py (8 px, threshold %d, bit0 = leftmost)." % PARAMS["thr"])
lines.append(" * Index 0..31 = U+0410..U+042F (А..Я), 32..63 = U+0430..U+044F (а..я),")
lines.append(" * 64 = U+0401 (Ё), 65 = U+0451 (ё). */")
lines.append("const uint8_t app_overlay_font8x8_cyr[66][8] = {")
for i, rows in enumerate(glyphs):
    body = ", ".join("0x%02X" % b for b in rows)
    name = "U+%04X %s" % (ord(CHARS[i]), CHARS[i])
    lines.append("    { %s },   /* %2d %s */" % (body, i, name))
lines.append("};")
with open(out_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print("table written: %s" % os.path.relpath(out_path, HERE))
