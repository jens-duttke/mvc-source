#!/bin/sh
# coretest's dump mode (the path make check-bitexact drives) must fail cleanly on
# an unwritable output path, not segfault on fwrite(NULL) after an unchecked
# fopen (audit L13).
#
# usage: sh tests/dumperr.sh <coretest> <file.264>
set -u
CORETEST="$1"
FILE="$2"
BAD="/nonexistent-dir-$$/out.yuv"   # parent dir absent -> fopen returns NULL

msg=$("$CORETEST" "$FILE" 0 5 "$BAD" 2>&1 >/dev/null)
rc=$?

if [ "$rc" -ge 128 ]; then
	echo "FAIL[dump]: coretest crashed (signal $((rc - 128))) on an unwritable path"
	exit 1
fi
if [ "$rc" -ne 0 ]; then
	echo "ok[dump]: clean failure on an unwritable path (rc=$rc): $msg"
	exit 0
fi
echo "FAIL[dump]: coretest reported success writing to an unwritable path"
exit 1
