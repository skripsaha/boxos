#!/bin/bash
# form stand — BoxLib's Form (src/userspace/boxlib/src/form*.c), built for the
# host and held to exact outcomes:
#
#     tools/form_stand.sh
#
# the Form's text and the image it is read from, the words of a line sorted
# into fields, every question the filler can ask and its answers, the arrow,
# and the Luggage written and read back, over a made-up volume
# (tools/form_stand.c). Built under the address and undefined-behaviour
# sanitizers. The last line counts the cases that held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}

LIB=src/userspace/boxlib
WORK=$(mktemp -d "${TMPDIR:-/tmp}/form_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-builtin -I$LIB/include -Isrc/include -Dfread=StandRead -DFileMetadataRead=StandMetadata -DFileMetadataRelease=StandMetadataRelease"

SOURCES="form form_line form_records form_luggage form_fill form_fill_assign"
objects=""
for name in $SOURCES; do
    $CC_HOST $FLAGS -c "$LIB/src/$name.c" -o "$WORK/$name.o" > "$WORK/cc.log" 2>&1 ||
        { cat "$WORK/cc.log"; echo "form: harness failed — $name.c did not build"; exit 2; }
    objects="$objects $WORK/$name.o"
done
$CC_HOST $FLAGS tools/form_stand.c $objects -o "$WORK/form_stand" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "form: harness failed — the stand did not build"; exit 2; }

"$WORK/form_stand"
