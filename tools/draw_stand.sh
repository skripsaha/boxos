#!/bin/bash
# draw stand — the drawing of BoxOS on the host: the shapes, strokes and paints
# every hand draws with (src/include/boxos_shape.h, boxos_stroke.h,
# boxos_paint.h) and Painter's engine (src/userspace/painter, painter/core),
# the very sources the system builds, held under the address and
# undefined-behaviour sanitizers to what they must do (tools/draw_stand.cpp):
#
#     tools/draw_stand.sh            the checks
#     tools/draw_stand.sh speed      how long the engine's work takes here
#
# Painter is first built as BoxOS builds it, without a warning. Then the
# checks — on the host as it is, and, where the host is not x86-64 but can run
# x86-64 (a Mac with Rosetta, which runs AVX2 though it does not say so), once
# more built for x86-64, so the SSE2 and AVX2 ways are held to the plain way
# bit for bit on every machine the stand runs on. The last line counts the
# checks that held.
set -u
TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CXX_HOST=${CXX_HOST:-c++}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/draw_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT
LIB=src/userspace/painter
make -s -B -C $LIB > "$WORK/make.log" 2>&1 ||
    { cat "$WORK/make.log"; echo "draw: harness failed — Painter did not build as BoxOS builds it"; exit 2; }
if grep -q 'warning:' "$WORK/make.log"; then
    grep -A3 'warning:' "$WORK/make.log"
    echo "draw: harness failed — Painter builds with a warning"
    exit 2
fi
# BoxLib's NULL is its own; the host's headers have theirs.
SOURCES="tools/draw_stand*.cpp $LIB/src/core/*.cpp"
INCLUDES="-Isrc/include -I$LIB/include -I$LIB/src/core -Isrc/userspace/boxlib/include -Itools"
if [ "${1:-}" = speed ]; then
    $CXX_HOST -std=c++23 -O2 -Wno-macro-redefined $INCLUDES $SOURCES -o "$WORK/speed" ||
        { echo "draw: harness failed — the stand did not build"; exit 2; }
    "$WORK/speed" speed
    exit 0
fi
FLAGS="-std=c++23 -g -O1 -Wall -Wextra -Werror -Wno-macro-redefined -fsanitize=address,undefined -fno-sanitize-recover=all"
ARCHES=native
if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] && arch -x86_64 /usr/bin/true 2>/dev/null; then
    ARCHES="native x86_64"
fi
HELD=0
FAILED=0
for ARCH in $ARCHES; do
    ARCHFLAG=""
    [ "$ARCH" = native ] || ARCHFLAG="-arch $ARCH"
    $CXX_HOST $ARCHFLAG $FLAGS $INCLUDES $SOURCES -o "$WORK/stand.$ARCH" > "$WORK/cc.$ARCH.log" 2>&1 ||
        { cat "$WORK/cc.$ARCH.log"; echo "draw: harness failed — the stand did not build ($ARCH)"; exit 2; }
    echo "== draw stand, $ARCH"
    "$WORK/stand.$ARCH" checks | tee "$WORK/checks.$ARCH.log" | grep -v '^draw stand:'
    H=$(sed -n 's/^draw stand: \([0-9]*\) held.*/\1/p' "$WORK/checks.$ARCH.log")
    F=$(sed -n 's/^draw stand: [0-9]* held, \([0-9]*\) failed/\1/p' "$WORK/checks.$ARCH.log")
    if [ -z "$H" ] || [ -z "$F" ]; then
        echo "draw: harness failed — the checks did not finish on $ARCH (a sanitizer stopped them?)"
        exit 2
    fi
    HELD=$((HELD + H))
    FAILED=$((FAILED + F))
done
echo "draw: $HELD passed, $FAILED failed"
[ "$FAILED" = 0 ]
