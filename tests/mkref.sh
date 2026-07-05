#!/bin/sh
# Extract one frame of edge264's own decode as raw planar YUV420P, for the
# bit-exact cross-check. Shared by the Makefile's check-bitexact target and the
# CI bit-exact step so the reference-generation lives in exactly one place.
#
# usage: sh tests/mkref.sh <edge264_test> <file.264> <frame> <out.yuv>
#
# Note: edge264_test is killed by SIGPIPE (exit 141) by design once ffmpeg has
# taken its single frame - that is expected. Do NOT add `set -o pipefail` here:
# it would turn every normal run red on the by-design 141. A truncated producer
# still yields a short/empty reference that the caller's md5 compare flags.
set -eu

E264="$1"
FILE="$2"
FRAME="$3"
OUT="$4"

"$E264" "$FILE" -oks 2>/dev/null | \
    ffmpeg -loglevel error -i - -vf "select=eq(n\,$FRAME)" -frames:v 1 \
    -f rawvideo -pix_fmt yuv420p "$OUT" -y
