#!/bin/bash
# typecase stand — Typecase (src/userspace/typecase), the very sources BoxOS
# links, built for the host under the address and undefined-behaviour
# sanitizers and held to what it must say:
#
#     tools/typecase_stand.sh [MUTATIONS]
#
# The library is first built as BoxOS builds it, without a warning. Then the
# checks (tools/typecase_stand.cpp): the rasterizer held to exact areas and to
# a fine sampling of every glyph; the sources, the chain and the cache to
# what they promise; geometry — every frame line leaving the cell at its
# weight's pixels, every mosaic and its complement filling the cell once, the
# eighths, shades, wedges and braille — at ten sizes; Duppo's two halves; the
# imprint made from the tree's font, every glyph the chain's picture; the
# stand's own fonts — every
# character map format, every way a component is put in place, nesting to
# the limit and past it, cycles, a collection — to what each was made to
# say; each named damage of
# the tree's font and MUTATIONS random ones (1000 when none is named) read and
# drawn whole. Then every font of the tree and every font the stand made is
# read again by tools/typecase_oracle.py, a reader written apart, and the two
# must say the same: the line's measures, every character the map names and
# its glyph, every glyph's advance and outline. The last line counts the
# checks that held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CXX_HOST=${CXX_HOST:-c++}
MUTATIONS=${1:-1000}

LIB=src/userspace/typecase
FONT=$LIB/fonts/JetBrainsMonoNL-Regular.ttf
WORK=$(mktemp -d "${TMPDIR:-/tmp}/typecase_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

make -s -B -C $LIB > "$WORK/make.log" 2>&1 ||
    { cat "$WORK/make.log"; echo "typecase: harness failed — the library did not build as BoxOS builds it"; exit 2; }
if grep -q 'warning:' "$WORK/make.log"; then
    grep -A3 'warning:' "$WORK/make.log"
    echo "typecase: harness failed — the library builds with a warning"
    exit 2
fi

FLAGS="-std=c++23 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
$CXX_HOST $FLAGS -I$LIB/include -Isrc/include -Itools tools/typecase_stand*.cpp tools/imprint_make.cpp $LIB/src/*.cpp \
    -o "$WORK/stand" \
    > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "typecase: harness failed — the stand did not build"; exit 2; }

mkdir "$WORK/forge"
"$WORK/stand" checks "$FONT" "$WORK/forge" "$MUTATIONS" | tee "$WORK/checks.log" | grep -v '^typecase stand:'
HELD=$(sed -n 's/^typecase stand: \([0-9]*\) held.*/\1/p' "$WORK/checks.log")
FAILED=$(sed -n 's/^typecase stand: [0-9]* held, \([0-9]*\) failed/\1/p' "$WORK/checks.log")
if [ -z "$HELD" ] || [ -z "$FAILED" ]; then
    echo "typecase: harness failed — the checks did not finish (a sanitizer stopped them?)"
    exit 2
fi

# The imprint as the build writes it — tools/imprint, the header, a C program
# reading it as the kernel does — said alike with the imprint the stand made.
$CXX_HOST -std=c++23 -O2 -Wall -Wextra -Werror -I$LIB/include -Isrc/include -Itools tools/imprint.cpp \
    tools/imprint_make.cpp $LIB/src/*.cpp -o "$WORK/imprint" > "$WORK/imprint.log" 2>&1 &&
    "$WORK/imprint" "$FONT" "$WORK/boxos_imprint_data.h" >> "$WORK/imprint.log" 2>&1 &&
    ${CC_HOST:-cc} -std=c11 -Wall -Wextra -Werror -I"$WORK" -Isrc/include tools/typecase_stand_imprint.c \
        -o "$WORK/digest" >> "$WORK/imprint.log" 2>&1 ||
    { cat "$WORK/imprint.log"; echo "typecase: harness failed — the imprint did not build as the build makes it"; exit 2; }
WRITTEN=$("$WORK/digest")
if [ "$WRITTEN" = "$("$WORK/stand" imprint-digest "$FONT")" ]; then
    echo "held the header tools/imprint writes, read as the kernel reads it, is the imprint the stand made — $WRITTEN"
    HELD=$((HELD + 1))
else
    echo "FAILED the header tools/imprint writes is not the imprint the stand made: $WRITTEN"
    FAILED=$((FAILED + 1))
fi

oracle() {
    local font=$1 index=$2 name said
    name=$(basename "$font")
    [ "$index" = 0 ] || name="$name, font $index"
    "$WORK/stand" outlines "$font" "$index" > "$WORK/said.txt"
    if said=$(python3 -I tools/typecase_oracle.py "$font" "$index" --held "$WORK/said.txt"); then
        echo "held the oracle says $name as Typecase does — $said"
        HELD=$((HELD + 1))
    else
        echo "FAILED the oracle and Typecase differ on $name: $said"
        FAILED=$((FAILED + 1))
    fi
}
for font in $LIB/fonts/*.ttf "$WORK"/forge/*.ttf; do oracle "$font" 0; done
oracle "$WORK/forge/collection.ttc" 0
oracle "$WORK/forge/collection.ttc" 1

echo "typecase: $HELD passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
