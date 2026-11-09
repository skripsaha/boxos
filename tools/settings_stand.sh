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
# value read back, and bytes made at random read inside their bounds. Built
# under the address and undefined-behaviour sanitizers. The last line counts
# the cases that held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/settings_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC_HOST $FLAGS -Isrc/include tools/settings_stand.c -o "$WORK/settings_stand" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "settings: harness failed — the stand did not build"; exit 2; }
"$WORK/settings_stand"
