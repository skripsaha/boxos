#!/bin/bash
# inside stand — tools/inside, built for the host from the one source the
# utility inside is built from (src/userspace/utils/inside/inside_show.c),
# held to what it says and to its exit:
#
#     tools/inside_stand.sh
#
# a box with a part of every kind inside says something of (tools/inside_stand.c),
# said whole; one with one of each thing it counts, said in the singular; then
# the first box damaged one way at a time — a byte of its code, a byte of its
# table, a byte between two parts, its last byte cut off — each said where it
# does not hold; a Form that is no text and settings without a default; an
# ELF image and a text file,
# no box. Built under the address and undefined-behaviour sanitizers. The last
# line counts the cases that held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}

LIB=src/userspace/boxlib
WORK=$(mktemp -d "${TMPDIR:-/tmp}/inside_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT
harness() { echo "inside: harness failed — $1"; exit 2; }

FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC_HOST $FLAGS -fno-builtin -I$LIB/include -Isrc/include -c $LIB/src/sha256.c -o "$WORK/sha256.o" \
    > "$WORK/cc.log" 2>&1 || { cat "$WORK/cc.log"; harness "sha256.c did not build"; }
$CC_HOST $FLAGS -DINSIDE_HOST -Isrc/include -Itools -Isrc/userspace/utils/inside tools/inside.c \
    src/userspace/utils/inside/inside_show.c "$WORK/sha256.o" -o "$WORK/inside" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; harness "inside did not build"; }
$CC_HOST $FLAGS -Isrc/include -Itools tools/inside_stand.c "$WORK/sha256.o" -o "$WORK/inside_stand" \
    > "$WORK/cc.log" 2>&1 || { cat "$WORK/cc.log"; harness "the stand's boxes did not build"; }
"$WORK/inside_stand" "$WORK" || harness "the stand's boxes were not written"

passed=0
failed=0

# look LABEL FILE EXIT WORDS... — inside on FILE ends with EXIT and says every
# one of WORDS.
look() {
    local label=$1 file=$2 want=$3 got words
    shift 3
    "$WORK/inside" "$file" > "$WORK/said" 2>&1
    got=$?
    if [ "$got" != "$want" ]; then
        echo "FAIL $label: inside ended with $got, not $want"
        sed 's/^/    /' "$WORK/said"
        failed=$((failed + 1))
        return
    fi
    for words in "$@"; do
        if ! grep -qF -- "$words" "$WORK/said"; then
            echo "FAIL $label: inside did not say '$words'"
            sed 's/^/    /' "$WORK/said"
            failed=$((failed + 1))
            return
        fi
    done
    passed=$((passed + 1))
}

# damage FILE OUT KIND — a copy of FILE damaged one way, read by a reader of
# the box's layout of its own (box_format.h): a byte of the part `code`, a byte
# of the table's first entry, a byte between the names and the first part, or
# the last byte cut off.
damage() {
    python3 -I - "$1" "$2" "$3" <<'PY'
import struct, sys
src, out, kind = sys.argv[1:]
b = bytearray(open(src, 'rb').read())
count, table, names, names_size = struct.unpack_from('<IQQQ', b, 12)
entries = []
for i in range(count):
    e = table + 80 * i
    offset, size = struct.unpack_from('<QQ', b, e)
    length, at = struct.unpack_from('<HI', b, e + 38)
    entries.append((bytes(b[names + at:names + at + length]), offset, size))
if kind == 'code':
    offset = next(o for n, o, s in entries if n == b'code')
    b[offset + 1] ^= 0x10
elif kind == 'table':
    b[table + 48] ^= 1
elif kind == 'gap':
    first = min(o for n, o, s in entries)
    if first <= names + names_size: raise SystemExit('no gap before the first part')
    b[first - 1] = 0x5A
elif kind == 'cut':
    del b[-1]
open(out, 'wb').write(b)
PY
}

look "a box with a part of every kind" "$WORK/full.box" 0 \
    "full.box: a box, format 1" \
    "code                    32 bytes, holds to its hash, must be understood" \
    "at 0x0 from the base, run and read" \
    "at 0x1000 from the base, read and written, and 6120 bytes of zeros after it" \
    "door start at 0x4" "door stop at 0x10" \
    "2 places the base is added to: the kernel places the box where it chooses" \
    "each strand keeps 64 bytes of its own, the first 4 of them from this template, aligned to 16" \
    "the names of 2 functions" \
    "cpu sse4.2" "stack 1 MiB" "name Stand" \
    "it stays: it lives on by itself, and whoever starts it does not wait; raised again when it falls" \
    "line 4: a word this reader does not know, passed over" \
    "setting hours: one of 24, 12; 24 by default — how the hours are counted" \
    "setting column: holds what stands under it — the column" \
    "  setting width: a whole number from -5 to 200 cells, 72 by default, from the next start — how wide the column is" \
    "  setting blink: a whole number from 100 to 900 ms, or off, off by default — how often it blinks" \
    "setting greeting: text, empty by default — what it says at start" \
    "line 7: the setting future is of a kind this reader does not know" \
    "line 8: the setting inner stands inside a line that is not taken" \
    "takes words \"what to say\"" "holds to its hash, put into memory at start" \
    "kept compressed: what it says is not read here" \
    "of a later version of its kind than this reader knows" \
    "of a kind this reader does not know" \
    "role                    14 bytes, holds to its hash" "system" \
    "— the table and its names hold to it" \
    "whole: the seal holds, every part holds to its hash, and only zeros lie between them"

look "a box with one of each thing counted" "$WORK/single.box" 0 \
    "1 place the base is added to: the kernel places the box where it chooses" \
    "the names of 1 function, for a fault and a stack trace to say" \
    "each strand keeps 8 bytes of its own, all zeros at first, aligned to 8" \
    "read and written, and 1 byte of zeros after it" \
    "1 byte, holds to its hash"

damage "$WORK/full.box" "$WORK/code.box" code || harness "could not damage the code"
look "a byte of the code changed" "$WORK/code.box" 1 \
    "bytes, DAMAGED: its hash differs, must be understood" "NOT WHOLE: what is marked above does not hold"

damage "$WORK/full.box" "$WORK/table.box" table || harness "could not damage the table"
look "a byte of the table changed" "$WORK/table.box" 1 \
    "DAMAGED: the table or its names were changed after it was sealed" "NOT WHOLE"

damage "$WORK/full.box" "$WORK/gap.box" gap || harness "could not put a byte between the parts"
look "a byte between the names and the first part" "$WORK/gap.box" 1 \
    "NOT ZERO: bytes lie between the parts before" "NOT WHOLE"

damage "$WORK/full.box" "$WORK/cut.box" cut || harness "could not cut the box"
look "a box cut short" "$WORK/cut.box" 1 "cut.box: a box this reader cannot read"

look "a Form that is no text" "$WORK/badform.box" 1 \
    "NOT WRITTEN as a part of its kind is" "NOT WHOLE"

look "settings with one that has no default" "$WORK/badsettings.box" 1 \
    "line 2: the setting prompt has no default" "NOT WRITTEN as a part of its kind is" "NOT WHOLE"

printf '\177ELF\002\001\001' > "$WORK/image.elf"
look "an ELF image" "$WORK/image.elf" 2 "image.elf: an ELF image, not a box"
look "a text file" "tools/inside_stand.sh" 2 "no program — neither a box nor an ELF image"
look "a file that is not there" "$WORK/absent.box" 3 "absent.box: could not be opened"

echo "inside: $passed passed, $failed failed"
[ "$failed" = 0 ]
