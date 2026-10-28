#!/bin/bash
# boxasm stand — the assembler and linker `assemble` runs inside BoxOS, built
# for the host from the same sources and held to independent witnesses on the
# very inputs a BoxOS image links: boxlib_start.o and libbox.a.
#
#     tools/boxasm_stand.sh
#
# The witnesses are GNU ld with src/userspace/boxlib/user.ld (the layout, the
# TLS block and every refusal it shares with us) and tools/nameplate (the table
# of names, re-derived from the image's own symbols). Each case prints "ok" or
# "FAIL" with what differed; the last line counts them.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}
ASM_DIR=src/userspace/utils/assemble
ENTRY=src/userspace/build/boxlib_start.o
ARCHIVE=src/userspace/boxlib/libbox.a
LD_SCRIPT=src/userspace/boxlib/user.ld
NAMEPLATE=tools/nameplate

WORK=$(mktemp -d "${TMPDIR:-/tmp}/boxasm_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

make -s -C src/userspace/boxlib > "$WORK/make.log" 2>&1 &&
    make -s -C src/userspace/utils ../build/boxlib_start.o ../../../tools/nameplate \
        >> "$WORK/make.log" 2>&1 ||
    { cat "$WORK/make.log"; echo "boxasm: harness failed — the runtime inputs did not build"; exit 2; }

STAND=$WORK/boxasm_stand
$CC_HOST -std=c11 -O1 -Wall -Wextra -I"$ASM_DIR" -Isrc/userspace/boxlib/include -Isrc/include \
    tools/boxasm_stand.c "$ASM_DIR"/boxasm_support.c "$ASM_DIR"/boxasm_elf.c \
    "$ASM_DIR"/boxasm_source.c "$ASM_DIR"/boxasm_link.c "$ASM_DIR"/boxasm_link_symbols.c \
    "$ASM_DIR"/boxasm_link_names.c "$ASM_DIR"/boxasm_link_output.c \
    -o "$STAND" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "boxasm: harness failed — the stand did not compile"; exit 2; }
if [ -s "$WORK/cc.log" ]; then
    cat "$WORK/cc.log"; echo "boxasm: harness failed — the toolchain compiles with warnings on the host"; exit 2
fi

passed=0; failed=0
ok()   { echo "ok   $1"; passed=$((passed + 1)); }
fail() { echo "FAIL $1: $2"; failed=$((failed + 1)); }

# A program object written in GNU syntax: main does $1, then exits 0.
program() {
    printf '.intel_syntax noprefix\n.text\n.globl main\nmain:\n endbr64\n push rbp\n mov rbp, rsp\n%s\n xor edi, edi\n call exit\n' \
        "$2" > "$WORK/$1.s" && x86_64-elf-as "$WORK/$1.s" -o "$WORK/$1.o"
}

# The archive with one more member, so a case can add a definition to it.
archive_with() {
    printf '%s\n' "$3" > "$WORK/$2.s" &&
        x86_64-elf-as "$WORK/$2.s" -o "$WORK/$2.o" &&
        cp "$ARCHIVE" "$WORK/$1" &&
        x86_64-elf-ar q "$WORK/$1" "$WORK/$2.o"
}

# The refusal of a link is the sentence expected, word for word.
refuses() {
    local name=$1 object=$2 archive=$3 want=$4 said
    said=$("$STAND" link "$ENTRY" "$object" "$archive" "$WORK/$name.elf" 2>&1)
    if [ $? -ne 1 ]; then fail "$name" "linked, or failed otherwise, instead of refusing: $said"
    elif [ "$said" != "$want" ]; then fail "$name" "said '$said', expected '$want'"
    else ok "$name"; fi
}

# ---- hello.asm, the image fixture ------------------------------------------

if "$STAND" assemble src/userspace/hello.asm "$WORK/hello.o" 2> "$WORK/asm.err"; then
    ok hello_assembles
else
    fail hello_assembles "$(cat "$WORK/asm.err")"
fi
if "$STAND" link "$ENTRY" "$WORK/hello.o" "$ARCHIVE" "$WORK/hello.elf" symbols 2> "$WORK/link.err"; then
    ok hello_links
else
    fail hello_links "$(cat "$WORK/link.err")"
fi

# The reference: GNU ld's two-pass link of the same objects, as nameplate.mk does it.
x86_64-elf-ld -nostdlib -static -T "$LD_SCRIPT" -o "$WORK/ref.pass1" "$ENTRY" "$WORK/hello.o" "$ARCHIVE" &&
    "$NAMEPLATE" build "$WORK/ref.pass1" "$WORK/ref.np.o" > /dev/null &&
    x86_64-elf-ld -nostdlib -static -T "$LD_SCRIPT" -o "$WORK/ref.elf" "$ENTRY" "$WORK/hello.o" \
        "$ARCHIVE" "$WORK/ref.np.o" ||
    { echo "boxasm: harness failed — GNU ld did not link the reference"; exit 2; }

if out=$("$NAMEPLATE" verify "$WORK/hello.elf" 2>&1); then ok nameplate_verify
else fail nameplate_verify "$out"; fi

# Names and sizes, address aside: the two linkers order archive members differently.
names() { "$NAMEPLATE" list "$1" | awk 'NR > 1 { print $3, $2 }' | sort; }
if diff <(names "$WORK/ref.elf") <(names "$WORK/hello.elf") > "$WORK/names.diff"; then
    ok nameplate_matches_gnu_ld
else
    fail nameplate_matches_gnu_ld "$(head -5 "$WORK/names.diff")"
fi

segments() { x86_64-elf-readelf -lW "$1" | awk '$1 == "LOAD" || $1 == "TLS"'; }
if segments "$WORK/hello.elf" | awk '$1 == "LOAD" && $7 ~ /W/ && $7 ~ /E/ { bad = 1 } END { exit bad }' &&
   [ "$(segments "$WORK/hello.elf" | awk '$1 == "LOAD"' | wc -l)" -eq 2 ]; then
    ok segments_wx
else
    fail segments_wx "$(segments "$WORK/hello.elf" | tr '\n' ';')"
fi

tls() { segments "$1" | awk '$1 == "TLS" { print $5, $6, $NF }'; }
if [ "$(tls "$WORK/hello.elf")" = "$(tls "$WORK/ref.elf")" ]; then ok tls_block_matches_gnu_ld
else fail tls_block_matches_gnu_ld "ours '$(tls "$WORK/hello.elf")', GNU ld '$(tls "$WORK/ref.elf")'"; fi

entry() { x86_64-elf-readelf -hW "$1" | awk '/Entry point/ { print $NF }'; }
start() { x86_64-elf-readelf -sW "$1" | awk '$NF == "_start" { print "0x" substr($2, match($2, /[1-9a-f]/)) }'; }
if [ "$(entry "$WORK/hello.elf")" = "$(start "$WORK/hello.elf")" ]; then ok entry_is_start
else fail entry_is_start "entry $(entry "$WORK/hello.elf"), _start $(start "$WORK/hello.elf")"; fi

# ---- refusals, each named to the object, section, offset and symbol ---------

program dup ' lea rdi, [rip + msg]
 call print
.globl print
print: ret
.section .rodata
msg: .asciz "x"'
refuses duplicate_strong "$WORK/dup.o" "$ARCHIVE" \
    "'print' is defined twice: in dup.o and in libbox.a(print.o)"

program undefined ' call nosuch'
refuses undefined_symbol "$WORK/undefined.o" "$ARCHIVE" \
    "undefined.o(.text+0x9): R_X86_64_PLT32 against 'nosuch': the symbol is defined nowhere"

program common ' mov rax, [rip + cvar]
.comm cvar,64,8'
refuses common_symbol "$WORK/common.o" "$ARCHIVE" \
    "'cvar' is a COMMON symbol, which this linker does not allocate, in common.o"

program tlspc ' mov rax, [rip + tv]
.section .tbss,"awT",@nobits
tv: .zero 8'
refuses tls_through_pc32 "$WORK/tlspc.o" "$ARCHIVE" \
    "tlspc.o(.text+0xb): R_X86_64_PC32 against 'tv': a thread-local symbol is reached only through R_X86_64_TPOFF32"

program prio ' nop
.section .init_array.00100,"aw"
.quad main'
refuses prioritized_constructor "$WORK/prio.o" "$ARCHIVE" \
    "prio.o: section '.init_array.00100' has no place in the BoxOS image layout"

program wide ' mov eax, OFFSET big'
archive_with libbox_big.a bigdef '.globl big
.set big, 0x100000000'
refuses relocation_overflow "$WORK/wide.o" "$WORK/libbox_big.a" \
    "wide.o(.text+0x9): R_X86_64_32 against 'big' does not fit in 32 bits (value 0x100000000)"

program long ' call boxasm_long_probe2
.globl boxasm_long_probe
boxasm_long_probe: ret'
archive_with libbox_long.a a_member_name_longer_than_sixteen '.text
.globl boxasm_long_probe, boxasm_long_probe2
boxasm_long_probe: ret
boxasm_long_probe2: ret'
refuses long_member_name "$WORK/long.o" "$WORK/libbox_long.a" \
    "'boxasm_long_probe' is defined twice: in long.o and in libbox_long.a(a_member_name_longer_than_sixteen.o)"

echo "boxasm: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
