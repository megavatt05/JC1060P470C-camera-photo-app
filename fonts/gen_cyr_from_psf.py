#!/usr/bin/env python3
"""Generate the 8x8 Cyrillic glyph table for CamBrowser.

Source: the hand-designed console font Cyr_a8x8 (kbd package,
data/consolefonts/Cyr_a8x8.psfu).  Its metrics match the ASCII font
embedded in main/app_overlay.c exactly - caps rows 0..6, x-height
letters rows 2..6, descenders down to row 7 - unlike any 8 px
auto-rasterisation of a vector TTF, which blurs letters like ш ж щ.

Ё/ё are absent from Cyr_a8x8 (cp866 "io" positions) and are composed
here from its Е/е glyphs plus a two-dot diacritic row (same pattern
the font itself uses for Й/й).

Output bit order matches app_overlay_font8x8: bit0 = LEFTMOST pixel.
Rows are PSF MSB-left, so every row byte is bit-reversed.

Usage:  python3 gen_cyr_from_psf.py [output.c]   (default: cyr_font_table.c)

Index: 0..31 = А..Я (U+0410..U+042F), 32..63 = а..я (U+0430..U+044F),
64 = Ё (U+0401), 65 = ё (U+0451).
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "psf", "Cyr_a8x8.psfu")

UPPER = "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"          # 32, no Ё
LOWER = "абвгдежзийклмнопрстуфхцчшщъыьэюя"          # 32, no ё
CHARS = UPPER + LOWER + "Ёё"                        # 66 total

def load_psf2(path):
    """Parse a PSF2 font -> ({codepoint: glyph_index}, [glyph bytes])"""
    data = open(path, "rb").read()
    assert data[:4] == b"\x72\xB5\x4A\x86", "not PSF2: %s" % path
    ver, hdr, flags, n, cs, h, w = struct.unpack_from("<7I", data, 4)
    assert (w, h) == (8, 8), "expected 8x8 glyphs, got %dx%d" % (w, h)
    glyphs = [data[hdr + i * cs: hdr + (i + 1) * cs] for i in range(n)]
    umap = {}
    if flags & 1:                       # unicode table present (UTF-8 text)
        gi, first, idx = 0, True, hdr + n * cs
        while idx < len(data) and gi < n:
            b = data[idx]
            if b == 0xFF:               # end of this glyph's list
                gi += 1
                first = True
                idx += 1
                continue
            if b == 0xFE:               # combining sequence - skip
                first = False
                idx += 1
                continue
            if b < 0x80:
                step, cp = 1, b
            elif (b & 0xE0) == 0xC0:
                step, cp = 2, b & 0x1F
            elif (b & 0xF0) == 0xE0:
                step, cp = 3, b & 0x0F
            else:
                step, cp = 4, b & 0x07
            for byte in data[idx + 1: idx + step]:
                cp = (cp << 6) | (byte & 0x3F)
            idx += step
            if first and cp not in umap:
                umap[cp] = gi
            first = False
    return umap, glyphs

def bitrev(b):
    return int("{:08b}".format(b)[::-1], 2)

def glyph_rows(g):
    """PSF glyph (MSB left) -> our 8 rows (bit0 left)"""
    return [bitrev(g[r]) for r in range(8)]

def rows_art(rows):
    return "\n".join("".join("#" if (rows[r] >> c) & 1 else "."
                             for c in range(8)) for r in range(8))

def art_to_rows(txt):
    """ASCII art (8 lines x 8 cols) -> rows in our bit order"""
    lines = txt.strip("\n").split("\n")
    assert len(lines) == 8, "need 8 art lines"
    out = []
    for ln in lines:
        assert len(ln) == 8, "art line must be 8 px: %r" % ln
        out.append(sum(1 << c for c, ch in enumerate(ln) if ch == "#"))
    return out

umap, glyphs = load_psf2(SRC)
missing = [c for c in UPPER + LOWER if ord(c) not in umap]
assert not missing, "source font misses: %r" % missing

table = {}
for ch in UPPER + LOWER:
    table[ch] = glyph_rows(glyphs[umap[ord(ch)]])

# --- compose Ё / ё (absent from cp866-based Cyr_a8x8) -----------------------
# Dots occupy the row above the letter, mirroring how Й/й carry the breve.
# Ё: Е pushed down one row (rows 1..6), dots on row 0.
# ё: е as-is (rows 2..6), dots on row 0..1 -> single row 1 keeps й's look;
#     final choice after visual check below.
def with_dots(base_rows, dot_rows, cols):
    rows = list(base_rows)
    for dr in dot_rows:
        rows[dr] = sum(1 << c for c in cols)
    return rows

E = table["Е"]          # rows 0..6
e = table["е"]          # rows 2..6
# Ё: Е occupies 7 rows (0..6) - it cannot simply shift down or its bottom
# bar would fall onto row 7, below the baseline.  Compress it into rows
# 1..6 by dropping one stem row: [top, stem, mid, stem, stem, bottom].
# Dots go on row 0 - the same place the font puts Й's breve.
parts = [E[0], E[1], E[3], E[4], E[5], E[6]]
Ee = [0] * 8
for k, r in enumerate(range(1, 7)):
    Ee[r] = parts[k]
# ё: е as-is (rows 2..6), dots on row 0 - mirrors lowercase й (breve row 0)
table["Ё"] = with_dots(Ee, [0], (1, 5))
table["ё"] = with_dots(e, [0], (1, 5))

# --- hand overrides (visual iteration against the preview) ------------------
OVERRIDES = {}

for ch, art in OVERRIDES.items():
    table[ch] = art_to_rows(art)

# --- preview ----------------------------------------------------------------
print("== CamBrowser 8x8 Cyrillic (source: %s) ==" % os.path.basename(SRC))
arts = [rows_art(table[ch]).split("\n") for ch in CHARS]
for i in range(0, len(CHARS), 8):
    chunk, block = CHARS[i:i + 8], arts[i:i + 8]
    for r in range(8):
        print("  ".join(a[r] for a in block))
    print("  ".join("----%s---" % c for c in chunk))

# sanity: mixed-case word
for word in ("Поиск", "ШщЛжДъы"):
    print("SANITY:", word)
    wa = [rows_art(table[c]).split("\n") for c in word]
    for r in range(8):
        print(" ".join(a[r] for a in wa))

# --- C table ----------------------------------------------------------------
out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "cyr_font_table.c")
lines = []
lines.append("/* 8x8 Cyrillic glyphs for CamBrowser.")
lines.append(" * Source: hand-designed console font Cyr_a8x8 (kbd package,")
lines.append(" * fonts/psf/Cyr_a8x8.psfu), converted by fonts/gen_cyr_from_psf.py.")
lines.append(" * Ё/ё composed from Е/е + dots (cp866 io positions are absent).")
lines.append(" * Bit0 = leftmost pixel, matching app_overlay_font8x8.")
lines.append(" * Index 0..31 = U+0410..U+042F (А..Я), 32..63 = U+0430..U+044F (а..я),")
lines.append(" * 64 = U+0401 (Ё), 65 = U+0451 (ё). */")
lines.append("const uint8_t app_overlay_font8x8_cyr[66][8] = {")
for i, ch in enumerate(CHARS):
    body = ", ".join("0x%02X" % b for b in table[ch])
    lines.append("    { %s },   /* %2d U+%04X %s */" % (body, i, ord(ch), ch))
lines.append("};")
with open(out_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print("table written: %s" % out_path)
