#!/bin/sh
# Host checks for main/browser/films.c - run after any films/probe change.
#   1) strict compile pass (films.c, -Wall -Wextra, esp stub headers)
#   2) functional probe tests on synthetic MP4s (fake HTTP layer)
#   3) the same probe against real ffmpeg-produced MP4s (real box layouts)
set -e
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-/home/z/my-project/build-host}
mkdir -p "$BUILD"

echo "[1/3] strict compile (films.c, -Wall -Wextra)"
gcc -c -o "$BUILD/films_host.o" -Wall -Wextra -DHOST_LOG_PRINTF_CHECK \
    -Iscripts/uihost/stubs -Imain main/browser/films.c

echo "[2/3] functional probe tests (synthetic MP4s)"
gcc -Wall -Wextra -DHOST_LOG_PRINTF_CHECK -Iscripts/uihost/stubs -Imain \
    scripts/uihost/test_films_host.c "$BUILD/films_host.o" \
    -o "$BUILD/test_films_host"
"$BUILD/test_films_host"

echo "[3/3] real-file probe tests (ffmpeg fixtures)"
if command -v ffmpeg >/dev/null 2>&1; then
    # faststart Baseline, tail-moov Main (x264 defaults: no cs1, B-frames),
    # tail-moov Main with -bf 0 (no B-frames; cs1 patched in two variants),
    # tail-moov High
    [ -f "$BUILD/real_fast.mp4" ] || \
        ffmpeg -v error -y -f lavfi -i testsrc=duration=2:size=640x272:rate=24 \
            -f lavfi -i sine=duration=2 -c:v libx264 -profile:v baseline \
            -pix_fmt yuv420p -c:a aac -movflags +faststart "$BUILD/real_fast.mp4"
    [ -f "$BUILD/real_main.mp4" ] || \
        ffmpeg -v error -y -f lavfi -i testsrc=duration=2:size=720x306:rate=24 \
            -f lavfi -i sine=duration=2 -c:v libx264 -profile:v main \
            -pix_fmt yuv420p -c:a aac "$BUILD/real_main.mp4"
    [ -f "$BUILD/real_main_bf0.mp4" ] || \
        ffmpeg -v error -y -f lavfi -i testsrc=duration=2:size=720x306:rate=24 \
            -f lavfi -i sine=duration=2 -c:v libx264 -profile:v main -bf 0 \
            -pix_fmt yuv420p -c:a aac "$BUILD/real_main_bf0.mp4"
    [ -f "$BUILD/real_high.mp4" ] || \
        ffmpeg -v error -y -f lavfi -i testsrc=duration=2:size=640x272:rate=24 \
            -f lavfi -i sine=duration=2 -c:v libx264 -profile:v high \
            -pix_fmt yuv420p -c:a aac "$BUILD/real_high.mp4"
    # cs1-patched copies of the bf=0 Main file (plays) and the default Main
    # file (B-frames -> must stay rejected even with cs1 patched)
    python3 - "$BUILD" <<'EOF'
import sys
b = sys.argv[1]
def patch(src, dst):
    d = bytearray(open(f"{b}/{src}", "rb").read())
    i = d.find(b"avcC")
    assert i >= 0
    d[i + 3] |= 0x40          # payload: ver, profile, constraint flags
    open(f"{b}/{dst}", "wb").write(d)
patch("real_main_bf0.mp4", "real_main_cs1.mp4")
patch("real_main.mp4", "real_main_cs1_ctts.mp4")
EOF
    "$BUILD/test_films_host" "$BUILD"
else
    echo "ffmpeg not found - real-file cases skipped"
fi
