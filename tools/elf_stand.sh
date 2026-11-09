#!/bin/bash
# elf stand — the kernel's ELF reader (src/kernel/core/process/elf_image.c),
# built for the host and held to exact verdicts:
#
#     tools/elf_stand.sh
#
# one image the reader must accept, and from it every way a file can be wrong
# that the loader once let through or must refuse — offsets and sizes whose sum
# wraps past 2^64, segments outside the file or outside the room a program's
# image has, two segments on one page, a segment writable and executable at
# once, an interpreter, an executable stack, an entry outside the code — each
# to its verdict and the program header it names (tools/elf_stand.c). Then
# every executable the tree has built at a fixed address — what the kernel is
# handed as an ELF — is read as the kernel reads it, and must be accepted; a
# program linked position-independent goes onto the volume as a box, and the
# pack stand holds it. Built under the address and undefined-behaviour
# sanitizers. The last line counts the cases that held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/elf_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined"

# The reader takes nothing of the kernel but its integer types (ktypes.h). The
# kernel's own ktypes.h defines more than integers, and some of it the host's
# compiler defines its own way, so here the integers come from the host.
mkdir -p "$WORK/include"
printf '#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n' > "$WORK/include/ktypes.h"

$CC_HOST $FLAGS -I"$WORK/include" -c src/kernel/core/process/elf_image.c -o "$WORK/elf_image.o" \
    > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "elf: harness failed — elf_image.c did not build"; exit 2; }
$CC_HOST $FLAGS -Isrc/kernel/core/process tools/elf_stand.c "$WORK/elf_image.o" \
    -o "$WORK/elf_stand" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "elf: harness failed — the stand did not build"; exit 2; }

# Every executable at a fixed address the tree has built (ET_EXEC: byte 16 of
# the header is 2), if it has built any: a fresh clone reads the made-up images
# alone, and the count on the last line says so.
built=()
while IFS= read -r path; do
    [ "$(od -An -t u1 -j 16 -N 1 "$path" 2>/dev/null | tr -d ' ')" = 2 ] && built+=("$path")
done < <(find src/userspace build -maxdepth 4 -name '*.elf' ! -name 'kernel*' ! -name '._*' 2>/dev/null | sort)

"$WORK/elf_stand" ${built[@]+"${built[@]}"}
