#!/bin/bash
# box stand — the one reader and the one pen of a box (src/include/box_format.h,
# box_reader.h, box_pen.h), held to exact outcomes:
#
#     tools/box_stand.sh
#
# First the promise that there is one reader: the same headers built as the
# kernel builds them (its ktypes.h, the cross compiler, freestanding) and as
# BoxLib builds them (box/defs.h) — beside the host build the stand itself is.
# Then the stand (tools/box_stand.c): a box of every kind of part written and
# read back, and broken one way at a time, each to its exact verdict and the
# part it names. Built under the address and undefined-behaviour sanitizers,
# with BoxLib's SHA-256. The last line counts the cases that held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}
CC_CROSS=${CC_CROSS:-x86_64-elf-gcc}

LIB=src/userspace/boxlib
WORK=$(mktemp -d "${TMPDIR:-/tmp}/box_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

CROSS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -mno-red-zone -O2 -Wall -Wextra -Werror"
printf '#include "ktypes.h"\n#include "box_pen.h"\n' > "$WORK/kernel.c"
$CC_CROSS $CROSS -Isrc/lib/kernel -Isrc/include -c "$WORK/kernel.c" -o "$WORK/kernel.o" \
    > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "box: harness failed — the headers do not build as the kernel builds them"; exit 2; }
printf '#include "box/defs.h"\n#include "box_pen.h"\n' > "$WORK/boxlib.c"
$CC_CROSS $CROSS -I$LIB/include -Isrc/include -c "$WORK/boxlib.c" -o "$WORK/boxlib.o" \
    > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "box: harness failed — the headers do not build as BoxLib builds them"; exit 2; }

FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC_HOST $FLAGS -fno-builtin -I$LIB/include -Isrc/include -c $LIB/src/sha256.c -o "$WORK/sha256.o" \
    > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "box: harness failed — sha256.c did not build"; exit 2; }
$CC_HOST $FLAGS -Isrc/include -Itools tools/box_stand.c "$WORK/sha256.o" -o "$WORK/box_stand" \
    > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "box: harness failed — the stand did not build"; exit 2; }

"$WORK/box_stand"
