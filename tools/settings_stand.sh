#!/bin/bash
# settings stand — the one reader and the one pen of a program's settings
# (src/include/settings_reader.h, settings_pen.h), built for the host and held
# to exact outcomes by tools/settings_stand.c:
#
#     tools/settings_stand.sh
#
# a declaration of every kind read field for field, each way one can be wrong
# to its verdict, every kind's values good and bad, the names a typo is near,
# the person's file line by line, the pen's changes byte for byte with every
# value read back, and bytes made at random read inside their bounds; then
# BoxLib's settings put together from a declaration and a file
# (tools/settings_lib_stand.c, src/userspace/boxlib/src/settings_read.c built
# on the host) — every value, and every sentence said of a line not taken,
# word for word. Built under the address and undefined-behaviour sanitizers.
# The last line counts the cases that held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/settings_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

LIB=src/userspace/boxlib
FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC_HOST $FLAGS -Isrc/include tools/settings_stand.c -o "$WORK/settings_stand" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "settings: harness failed — the stand did not build"; exit 2; }
# BoxLib's side, built as BoxOS builds it, seeing only BoxLib's headers.
$CC_HOST $FLAGS -fno-builtin -I$LIB/include -Isrc/include tools/settings_lib_stand.c $LIB/src/settings_read.c \
    -o "$WORK/settings_lib_stand" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "settings: harness failed — BoxLib's settings did not build on the host"; exit 2; }

# Each prints its cases, then "...: N passed, M failed"; the last line adds them up.
passed=0; failed=0
for stand in settings_stand settings_lib_stand; do
    "$WORK/$stand" > "$WORK/$stand.out" 2>&1
    grep -v ': [0-9]* passed, [0-9]* failed$' "$WORK/$stand.out"
    counts=$(grep -E ': [0-9]+ passed, [0-9]+ failed$' "$WORK/$stand.out" | tail -1)
    [ -n "$counts" ] || { echo "settings: harness failed — $stand said no count"; exit 2; }
    passed=$((passed + $(echo "$counts" | sed -E 's/.*: ([0-9]+) passed.*/\1/')))
    failed=$((failed + $(echo "$counts" | sed -E 's/.* ([0-9]+) failed$/\1/')))
done
echo "settings: $passed passed, $failed failed"
[ "$failed" = 0 ]
