#!/bin/bash
# draw stand — the drawing of BoxOS on the host: the shapes, strokes and paints
# every hand draws with (src/include/boxos_shape.h, boxos_stroke.h,
# boxos_paint.h), the very sources the system builds, held under the address
# and undefined-behaviour sanitizers to what they must do (tools/draw_stand.cpp):
#
#     tools/draw_stand.sh
#
# A shape swept row by row the very cells of every edge laid alone; EVENODD;
# rounded rectangles of four radii; strokes to their exact areas — caps,
# joins, the miter limit, dashes and dots, closed contours, ready shapes on
# their own edges, a pen stretched and mirrored; turns to the host's atan2;
# gradients and pictures past their ends, pictures mixed as light. The last
# line counts the checks that held.
set -u
TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CXX_HOST=${CXX_HOST:-c++}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/draw_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT
FLAGS="-std=c++23 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
$CXX_HOST $FLAGS -Isrc/include -Itools tools/draw_stand*.cpp -o "$WORK/stand" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "draw: harness failed — the stand did not build"; exit 2; }
"$WORK/stand" checks | tee "$WORK/checks.log" | grep -v '^draw stand:'
HELD=$(sed -n 's/^draw stand: \([0-9]*\) held.*/\1/p' "$WORK/checks.log")
FAILED=$(sed -n 's/^draw stand: [0-9]* held, \([0-9]*\) failed/\1/p' "$WORK/checks.log")
if [ -z "$HELD" ] || [ -z "$FAILED" ]; then
    echo "draw: harness failed — the checks did not finish (a sanitizer stopped them?)"
    exit 2
fi
echo "draw: $HELD held, $FAILED failed"
[ "$FAILED" = 0 ]
