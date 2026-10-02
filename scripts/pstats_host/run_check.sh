#!/bin/sh
# Host syntax/type check for main/media/pstats.c (both sdkconfig branches).
#   scripts/pstats_host/run_check.sh
# Compile-only: catches typos, wrong printf formats, missing decls before the
# user's local ESP-IDF build does. See also scripts/uihost/run_tests.sh.
set -e
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-/home/z/my-project/build-host}
mkdir -p "$BUILD"

CFLAGS="-c -o $BUILD/pstats_host.o -Wall -Wextra -Wno-unused-parameter \
    -Iscripts/pstats_host/stubs -Iscripts/uihost/stubs -Imain \
    -DHOST_LOG_PRINTF_CHECK"

echo "[1/2] pstats.c strict compile (no runtime stats -> CPU row n/a)"
gcc $CFLAGS main/media/pstats.c

echo "[2/2] pstats.c strict compile (runtime stats enabled)"
gcc $CFLAGS -DHOST_PSTATS_RUNTIME_STATS \
    -DCONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=1 main/media/pstats.c

echo "pstats host check: OK"
