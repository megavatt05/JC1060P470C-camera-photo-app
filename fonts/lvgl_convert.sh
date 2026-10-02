#!/usr/bin/env bash
# Convert a TTF into an LVGL (v8/v9) C font covering ASCII + Cyrillic,
# using the official LVGL converter (lv_font_conv) via npx.
#
# Usage:
#   ./lvgl_convert.sh [ttf-file] [size-px] [output.c]
#   ./lvgl_convert.sh ttf/DejaVuSansMono.ttf 16 lv_font_mono_16.c
#
# Requirements: nodejs/npm (npx downloads lv_font_conv on first run).
# Output: one C file with an lv_font_t symbol named after the output file
# (e.g. lv_font_mono_16.c -> lv_font_mono_16). Add it to the LVGL project
# and reference the symbol as a normal LVGL font.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"

TTF="${1:-$HERE/ttf/DejaVuSansMono.ttf}"
SIZE="${2:-16}"
OUT="${3:-$HERE/lvgl_font_${SIZE}.c}"

# Ranges: printable ASCII, Ё/ё, А..я
npx --yes lv_font_conv \
    --font "$TTF" \
    -r 0x20-0x7F \
    -r 0x401 -r 0x451 \
    -r 0x410-0x44F \
    --size "$SIZE" \
    --bpp 4 \
    --format lvgl \
    --no-compress \
    -o "$OUT"

echo "OK -> $OUT"
