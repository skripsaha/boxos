#!/bin/bash
# gangway stand — the UEFI loader's Lectern and Succession (src/boot/uefi/
# gangway_lectern*.c, gangway_succession.c), the very files Gangway links,
# built for the host and held to volume-read's oracle
# (tools/volumeread/bootread.py, bootbook.py) line for line:
#
#     tools/gangway_stand.sh [IMAGE]
#
# IMAGE (build/boxos.img when none is named) is a volume. Every
# landing of self-test's group A is written out of it (--emit) — the kernel
# BOOT names attested, its DIGESTS leaf rotten in one copy and in both, the
# newest slot torn, a newer slot whose DIGESTS root does not read, the Spine's
# ring empty, a byte of the kernel's code flipped and a byte of the zeros past
# its end; a second kernel kept in the boot role bound in the Tally or born on the ribbon
# with BOOT's flipped, the ribbon torn past its birth, an Undone of its Born, a
# Shaped of it whose base does not hold, its stitch at the start of the
# next_quire, its stitch closed and damaged; gangway.settings bound in the
# Tally, born on the ribbon, renamed so on it, kept deleted, a byte of it
# flipped, two of the name, its bytes in a Slip over a hole — and on each, and
# on IMAGE as it is, what the stand says (tools/gangway_stand.c) must be what
# `volume-read boot` says: the slots passed over, the slot stood on, every node
# read past a copy that did not hold, the ribbon and its records, each kernel
# tried and its verdict, the one Gangway jumps into or the sentence it stops
# on, and gangway.settings found, read and held to the book, or why not. Built
# under the address and undefined-behaviour sanitizers: the Lectern parses
# what a medium hands it. Then how it reads: the kernel in the fewest commands
# LECTERN_READ_MOST allows — its first block, then its run a mebibyte at a
# time — and no command longer; a long command over one of its blocks
# refused, the run read again a block at a time and nothing said otherwise;
# that block refused outright, named. The last line counts what held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}
IMG=${1:-build/boxos.img}
LANDINGS="A1 A2 A3 A4 A5 A6 A7 A8 A9 A10 A11 A12 A13 A14 A15 A16 A17 A18 A19 A20 A21 A22"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/gangway_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC_HOST $FLAGS -Isrc/boot/uefi -Isrc/include tools/gangway_stand.c src/boot/uefi/gangway_lectern*.c \
    src/boot/uefi/gangway_succession.c -o "$WORK/gangway_stand" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "gangway: harness failed — the stand did not build"; exit 2; }

start=$(python3 tools/volume-read.py "$IMG" deed 2>/dev/null | sed -nE 's/^ground +: sector ([0-9]+) .*/\1/p')
[ -n "$start" ] || { echo "gangway: harness failed — volume-read finds no ground on $IMG"; exit 2; }

passed=0; failed=0

# held ID LABEL IMAGE [REFUSE_BLOCK [long]] — the stand and the oracle on one
# image, their words compared byte for byte.
held() {
    local id=$1 label=$2 img=$3 rc
    shift 3
    "$WORK/gangway_stand" "$img" "$start" "$@" > "$WORK/$id.stand" 2> "$WORK/$id.stand.err"
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

echo "gangway: the Lectern on $IMG (ground at sector $start) and on self-test's group A, against volume-read boot"
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
# judged LABEL — a check of how the Lectern reads, its verdict in $?.
judged() {
    if [ $? -eq 0 ]; then
        echo "  ok   $1"
        passed=$((passed + 1))
    else
        echo "  FAIL $1"
        failed=$((failed + 1))
    fi
}

read -r longest blocks first onkernel <<< "$(sed -nE 's/^stand: [0-9]+ command\(s\), the longest ([0-9]+) sector\(s\); the kernel.s ([0-9]+) block\(s\) from volume block ([0-9]+) in ([0-9]+) of them$/\1 \2 \3 \4/p' "$WORK/built.stand.err")"
if [ -z "$onkernel" ]; then
    echo "  FAIL how it reads — the stand did not say: $(cat "$WORK/built.stand.err")"
    failed=$((failed + 1))
else
    [ "$longest" -le 2048 ]
    judged "no command asks for more than a mebibyte — the longest $longest sector(s)"
    [ "$onkernel" -eq $((1 + (blocks + 255) / 256)) ]
    judged "the kernel's $blocks block(s) come in $onkernel command(s): its first block, then its run a mebibyte at a time"
    half=$((blocks / 2))
    held refused-long "a long command over the kernel's block $half refused: its run read again a block at a time" \
        "$IMG" $((first + half)) long
    "$WORK/gangway_stand" "$IMG" "$start" $((first + half)) > "$WORK/refused.stand" 2>&1
    grep -qx "boot: the kernel the deed names cannot be loaded: its block $half (volume block $((first + half))) will not read" \
        "$WORK/refused.stand"
    judged "the kernel's block $half refused outright: named, its block and its volume block"
fi
echo "gangway: $passed passed, $failed failed — the Lectern and volume-read boot say the same of the kernels and the one jumped into, read in long commands"
[ $failed -eq 0 ]
