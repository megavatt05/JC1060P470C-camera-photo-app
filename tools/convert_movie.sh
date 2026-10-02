#!/usr/bin/env bash
# Convert a movie for CamBrowser (ESP32-P4, software H.264 decoder).
#
# The SW decoder (tinyh264) plays only H.264 Constrained Baseline, and the
# board decodes ~320x180@20 in realtime (360p is a slideshow). This script
# re-encodes any input into exactly that: 320x180, 20 fps, Baseline L2.1,
# AAC-LC audio, moov moved to the front (+faststart).
#
# Usage:
#   ./convert_movie.sh input.mkv [output.mp4] [width] [height] [fps]
#
# Defaults: output "<input>_cam.mp4", 320x180@20 (~150-200 MB per 2 hours).
# Examples:
#   ./convert_movie.sh film.mkv
#   ./convert_movie.sh film.mkv film_small.mp4 320 180 20
set -euo pipefail

IN="${1:?usage: $0 input.mkv [output.mp4] [width] [height] [fps]}"
OUT="${2:-${IN%.*}_cam.mp4}"
W="${3:-320}"
H="${4:-180}"
FPS="${5:-20}"

ffmpeg -y -i "$IN" \
    -map 0:v:0 -map 0:a:0? -sn -dn \
    -vf "scale=${W}:${H}:force_original_aspect_ratio=decrease,"\
"pad=${W}:${H}:(ow-iw)/2:(oh-ih)/2,fps=${FPS}" \
    -c:v libx264 -profile:v baseline -level 2.1 -pix_fmt yuv420p \
    -preset medium -b:v 250k -maxrate 400k -bufsize 600k -g $((FPS * 2)) \
    -c:a aac -b:a 64k -ar 44100 -ac 2 \
    -movflags +faststart \
    "$OUT"

echo "OK: $OUT"
echo "Copy it to the root of the SD card (FAT32) or serve it over HTTP and"
echo "open it in CamBrowser: ВИДЕО -> СВОЙ URL / SD-КАРТА."
