#!/bin/bash
# tagboot stand — the UEFI loader's Lectern and Succession (src/boot/uefi/
# tagboot_lectern*.c, tagboot_succession.c), the very files TagBoot links,
# built for the host and held to volume-read's oracle
# (tools/volumeread/bootread.py, bootbook.py) line for line:
#
#     tools/tagboot_stand.sh [IMAGE]
#
# IMAGE (build/boxos.img when none is named) is a format-v2 volume. Every
# landing of self-test's group A is written out of it (--emit) — the kernel
# BOOT names attested, its DIGESTS leaf rotten in one copy and in both, the
# newest slot torn, a newer slot whose DIGESTS root does not read, the Spine's
# ring empty, a byte of the kernel's code flipped and a byte of the zeros past
# its end; a second kernel kept in the boot role bound in the Tally or born on the ribbon
# with BOOT's flipped, the ribbon torn past its birth, an Undone of its Born, a
# Shaped of it whose base does not hold, its stitch at the start of the
# next_quire, its stitch closed and damaged — and on each, and on IMAGE as it is,
# what the stand says (tools/tagboot_stand.c) must be what `volume-read boot`
# says: the slots passed over, the slot stood on, every node read past a copy
# that did not hold, the ribbon and its records, each kernel tried and its
# verdict, and the one TagBoot jumps into or the sentence it stops on (spec_v2
# §16.7 пп.2–6). Built under the address and undefined-behaviour sanitizers:
# the Lectern parses what a medium hands it. The last line counts the
# landings that held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}
IMG=${1:-build/boxos.img}
LANDINGS="A1 A2 A3 A4 A5 A6 A7 A8 A9 A10 A11 A12 A13 A14 A15"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/tagboot_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC_HOST $FLAGS -Isrc/boot/uefi -Isrc/include tools/tagboot_stand.c src/boot/uefi/tagboot_lectern*.c \
    src/boot/uefi/tagboot_succession.c -o "$WORK/tagboot_stand" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "tagboot: harness failed — the stand did not build"; exit 2; }

start=$(python3 tools/volume-read.py "$IMG" deed 2>/dev/null | sed -nE 's/^ground +: sector ([0-9]+) .*/\1/p')
[ -n "$start" ] || { echo "tagboot: harness failed — volume-read finds no ground on $IMG"; exit 2; }

passed=0; failed=0

# held ID LABEL IMAGE — the stand and the oracle on one image, their words
# compared byte for byte.
held() {
    local id=$1 label=$2 img=$3 rc
    "$WORK/tagboot_stand" "$img" "$start" > "$WORK/$id.stand" 2> "$WORK/$id.stand.err"
    rc=$?
    python3 tools/volume-read.py "$img" boot > "$WORK/$id.oracle" 2>&1
    if [ $rc -eq 0 ] && [ -s "$WORK/$id.oracle" ] && cmp -s "$WORK/$id.stand" "$WORK/$id.oracle"; then
        echo "  ok   $label — $(tail -1 "$WORK/$id.stand" | sed 's/^boot: //')"
        passed=$((passed + 1))
    else
        echo "  FAIL $label — the stand (exit $rc) and the oracle part:"
        diff "$WORK/$id.stand" "$WORK/$id.oracle" | sed 's/^/         /'
        sed 's/^/         stderr: /' "$WORK/$id.stand.err" | head -20
        failed=$((failed + 1))
    fi
}

echo "tagboot: the Lectern on $IMG (ground at sector $start) and on self-test's group A, against volume-read boot"
held built "the image as built" "$IMG"
for case in $LANDINGS; do
    if ! python3 tools/volume-read.py "$IMG" self-test --emit "$case" --out "$WORK/$case.img" \
            > "$WORK/$case.emit" 2>&1; then
        echo "  FAIL $case — self-test would not write it out: $(tail -1 "$WORK/$case.emit")"
        failed=$((failed + 1))
        continue
    fi
    held "$case" "$(sed -nE 's/^self-test: (A[0-9]+ .*) written to .*/\1/p' "$WORK/$case.emit")" "$WORK/$case.img"
    rm -f "$WORK/$case.img"
done
echo "tagboot: $passed passed, $failed failed — the Lectern and volume-read boot say the same of the kernels and the one jumped into"
[ $failed -eq 0 ]
