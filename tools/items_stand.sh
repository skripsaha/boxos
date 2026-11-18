#!/bin/bash
# items stand — the display's items and the program's side of their language,
# the very sources BoxOS builds, built for the host under the address and
# undefined-behaviour sanitizers and held to what they promise:
#
#     tools/items_stand.sh            every check, natively and as x86-64
#     tools/items_stand.sh sheet OUT  a scene of items composed, a PPM
#
# The display's parts that need no system — the language read (item_wire),
# the items' tree, their look, their drawing, the ring read (item_reception)
# and the compositor — with Painter's engine, its writer of changes
# (painter/gui/changes.h), Typecase and the ring's one rule (boxlib
# items_ring.c). The checks (tools/items_stand*.cpp): what a program says
# arrives as it said it, a step whole or not at all, a full ring's last
# words, every refusal, every bound, a lying ring broken, random rings read
# whole, the cost of a change; and the screen composed only where it changed
# the very pixels of the screen composed whole. The last line counts them.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CXX_HOST=${CXX_HOST:-c++}
CC_HOST=${CC_HOST:-cc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/items_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

D=src/userspace/display
P=src/userspace/painter
SOURCES="tools/items_stand*.cpp $D/item_wire.cpp $D/item_tree.cpp $D/item_look.cpp $D/item_draw.cpp $D/item_reception.cpp \
         $D/compositor.cpp $(ls $P/src/core/*.cpp | grep -v system_fonts.cpp) $P/src/gui/changes.cpp \
         src/userspace/typecase/src/*.cpp"
INCLUDES="-Isrc/include -I$P/include -I$P/src/core -Isrc/userspace/typecase/include -Isrc/userspace/boxlib/include -I$D -Itools"

if [ "${1:-}" = sheet ]; then
    $CC_HOST -std=c11 -O2 $INCLUDES -c src/userspace/boxlib/src/items_ring.c -o "$WORK/ring.o" &&
        $CXX_HOST -std=c++23 -O2 -Wno-macro-redefined $INCLUDES $SOURCES "$WORK/ring.o" -o "$WORK/stand" ||
        { echo "items: harness failed — the stand did not build"; exit 2; }
    "$WORK/stand" sheet "${2:?a file to write}"
    exit $?
fi

FLAGS="-g -O1 -Wall -Wextra -Werror -Wno-macro-redefined -fsanitize=address,undefined -fno-sanitize-recover=all"
ARCHES=native
if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] && arch -x86_64 /usr/bin/true 2>/dev/null; then
    ARCHES="native x86_64"
fi
HELD=0
FAILED=0
for ARCH in $ARCHES; do
    ARCHFLAG=""
    [ "$ARCH" = native ] || ARCHFLAG="-arch $ARCH"
    { $CC_HOST $ARCHFLAG -std=c11 $FLAGS $INCLUDES -c src/userspace/boxlib/src/items_ring.c -o "$WORK/ring.$ARCH.o" &&
      $CXX_HOST $ARCHFLAG -std=c++23 $FLAGS $INCLUDES $SOURCES "$WORK/ring.$ARCH.o" -o "$WORK/stand.$ARCH"; } \
        > "$WORK/cc.$ARCH.log" 2>&1 ||
        { cat "$WORK/cc.$ARCH.log" | head -60; echo "items: harness failed — the stand did not build ($ARCH)"; exit 2; }
    echo "== items stand, $ARCH"
    "$WORK/stand.$ARCH" checks | tee "$WORK/checks.$ARCH.log" | grep -v '^items stand:'
    H=$(sed -n 's/^items stand: \([0-9]*\) held.*/\1/p' "$WORK/checks.$ARCH.log")
    F=$(sed -n 's/^items stand: [0-9]* held, \([0-9]*\) failed/\1/p' "$WORK/checks.$ARCH.log")
    if [ -z "$H" ] || [ -z "$F" ]; then
        echo "items: harness failed — the checks did not finish on $ARCH (a sanitizer stopped them?)"
        exit 2
    fi
    HELD=$((HELD + H))
    FAILED=$((FAILED + F))
done
echo "items: $HELD passed, $FAILED failed"
[ "$FAILED" = 0 ]
