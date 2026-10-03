#!/usr/bin/env bash
# Конвертация фильма для CamBrowser (ESP32-P4, только SW H.264).
#
# На P4 нет аппаратного H.264 decode. По замерам Espressif esp_player:
#   ~320×240 @ 60+ fps, ~640×480 @ ~18 fps (slideshow).
# Для ПЛАВНОГО воспроизведения по умолчанию: 320×180 @ 20 fps, Baseline.
# Профиль "max": 480×270 @ 20 — возможны подлагивания на сети.
#
# Использование:
#   ./convert_movie.sh input.mkv [output.mp4] [smooth|max|WxH@fps]
#
# Примеры:
#   ./convert_movie.sh film.mkv
#   ./convert_movie.sh film.mkv film_sd.mp4 smooth
#   ./convert_movie.sh film.mkv film_sd.mp4 max
#   ./convert_movie.sh film.mkv film_sd.mp4 400x224@20
set -euo pipefail

IN="${1:?usage: $0 input [output.mp4] [smooth|max|WxH@fps]}"
OUT="${2:-${IN%.*}_cam.mp4}"
PROF="${3:-smooth}"

case "$PROF" in
  smooth|"")
    W=320; H=180; FPS=20; BV=250k; MAXR=400k; BUF=600k; LVL=2.1
    ;;
  max)
    W=480; H=270; FPS=20; BV=500k; MAXR=800k; BUF=1200k; LVL=3.0
    ;;
  *x*@*)
    WH="${PROF%@*}"; FPS="${PROF##*@}"
    W="${WH%x*}"; H="${WH#*x}"
    BV=400k; MAXR=700k; BUF=1000k; LVL=3.0
    ;;
  *)
    echo "неизвестный профиль: $PROF (smooth|max|WxH@fps)" >&2
    exit 1
    ;;
esac

echo "профиль: ${W}x${H} @ ${FPS} fps, Baseline, bitrate ~$BV"

ffmpeg -y -i "$IN" \
    -map 0:v:0 -map 0:a:0? -sn -dn \
    -vf "scale=${W}:${H}:force_original_aspect_ratio=decrease,pad=${W}:${H}:(ow-iw)/2:(oh-ih)/2,fps=${FPS}" \
    -c:v libx264 -profile:v baseline -level "$LVL" -pix_fmt yuv420p \
    -preset medium -b:v "$BV" -maxrate "$MAXR" -bufsize "$BUF" -g $((FPS * 2)) \
    -c:a aac -b:a 64k -ar 44100 -ac 2 \
    -movflags +faststart \
    "$OUT"

echo "OK: $OUT"
echo "Скопируйте в корень SD (FAT32) или отдайте по HTTP."
echo "В CamBrowser: ВИДЕО → СВОЙ URL / SD-КАРТА."
echo "Сеть archive.org 512×288 часто тормозит — для плавности используйте SD + smooth."
