#!/bin/bash
# boxasm stand — the assembler and linker `assemble` runs inside BoxOS, built
# for the host from the same sources and held to independent witnesses on the
# very inputs a BoxOS image links: boxlib_start.o and libbox.a.
#
#     tools/boxasm_stand.sh
#
# The witnesses are NASM (every instruction form, byte for byte, through
# tools/boxasm_oracle.py), GNU ld with src/userspace/boxlib/user.ld (the
# layout, the TLS block and the refusals it shares with us) and
# tools/nameplate (the table of names, re-derived from the image's own
# symbols). Each case prints "ok" or "FAIL" with what differed; the last
# line counts them.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}
ASM_DIR=src/userspace/utils/assemble
ENTRY=src/userspace/build/boxlib_start.o
ARCHIVE=src/userspace/boxlib/libbox.a
LD_SCRIPT=src/userspace/boxlib/user.ld
NAMEPLATE=tools/nameplate
ORACLE=tools/boxasm_oracle.py

WORK=$(mktemp -d "${TMPDIR:-/tmp}/boxasm_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

make -s -C src/userspace/boxlib > "$WORK/make.log" 2>&1 &&
    make -s -C src/userspace/utils ../build/boxlib_start.o ../../../tools/nameplate \
        >> "$WORK/make.log" 2>&1 ||
    { cat "$WORK/make.log"; echo "boxasm: harness failed — the runtime inputs did not build"; exit 2; }

STAND=$WORK/boxasm_stand
# Every part of the toolchain but the BoxOS command's own files.
$CC_HOST -std=c11 -O1 -Wall -Wextra -I"$ASM_DIR" -Isrc/userspace/boxlib/include -Isrc/include \
    tools/boxasm_stand.c $(ls "$ASM_DIR"/*.c | grep -v '/assemble[^/]*\.c$') \
    -o "$STAND" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "boxasm: harness failed — the stand did not compile"; exit 2; }
if [ -s "$WORK/cc.log" ]; then
    cat "$WORK/cc.log"; echo "boxasm: harness failed — the toolchain compiles with warnings on the host"; exit 2
fi

passed=0; failed=0
ok()   { echo "ok   $1"; passed=$((passed + 1)); }
fail() { echo "FAIL $1: $2"; failed=$((failed + 1)); }

# ---- the image's programs ----------------------------------------------------

for program in hello countdown; do
    if "$STAND" assemble "src/userspace/$program.asm" "$WORK/$program.o" 2> "$WORK/asm.err" &&
       "$STAND" link "$ENTRY" "$WORK/$program.o" "$ARCHIVE" "$WORK/$program.elf" symbols \
           2> "$WORK/link.err"; then
        ok "${program}_builds"
    else
        fail "${program}_builds" "$(cat "$WORK/asm.err" "$WORK/link.err" 2>/dev/null)"
    fi
done

# ---- every encoding, held to NASM --------------------------------------------

python3 "$ORACLE" corpus > "$WORK/corpus.asm" &&
    python3 "$ORACLE" corpus nasm > "$WORK/corpus_nasm.asm" &&
    nasm -f elf64 -w-all "$WORK/corpus_nasm.asm" -o "$WORK/corpus_nasm.o" ||
    { echo "boxasm: harness failed — NASM did not assemble the corpus"; exit 2; }
if ! "$STAND" assemble "$WORK/corpus.asm" "$WORK/corpus.o" 2> "$WORK/corpus.err"; then
    fail encodings_match_nasm "$(head -3 "$WORK/corpus.err")"
elif out=$(python3 "$ORACLE" compare "$WORK/corpus.o" "$WORK/corpus_nasm.o" 2>&1); then
    ok "encodings_match_nasm ($(wc -l < "$WORK/corpus.asm" | tr -d ' ') lines)"
else
    fail encodings_match_nasm "$out"
fi

# ---- the linker, held to GNU ld and tools/nameplate -------------------------

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

# A program object written in GNU syntax: main does $2, then exits 0.
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
link_refuses() {
    local name=$1 object=$2 archive=$3 want=$4 said
    said=$("$STAND" link "$ENTRY" "$object" "$archive" "$WORK/$name.elf" 2>&1)
    if [ $? -ne 1 ]; then fail "$name" "linked, or failed otherwise, instead of refusing: $said"
    elif [ "$said" != "$want" ]; then fail "$name" "said '$said', expected '$want'"
    else ok "$name"; fi
}

program dup ' lea rdi, [rip + msg]
 call print
.globl print
print: ret
.section .rodata
msg: .asciz "x"'
link_refuses duplicate_strong "$WORK/dup.o" "$ARCHIVE" \
    "'print' is defined twice: in dup.o and in libbox.a(print.o)"

program undefined ' call nosuch'
link_refuses undefined_symbol "$WORK/undefined.o" "$ARCHIVE" \
    "undefined.o(.text+0x9): R_X86_64_PLT32 against 'nosuch': the symbol is defined nowhere"

program common ' mov rax, [rip + cvar]
.comm cvar,64,8'
link_refuses common_symbol "$WORK/common.o" "$ARCHIVE" \
    "'cvar' is a COMMON symbol, which this linker does not allocate, in common.o"

program tlspc ' mov rax, [rip + tv]
.section .tbss,"awT",@nobits
tv: .zero 8'
link_refuses tls_through_pc32 "$WORK/tlspc.o" "$ARCHIVE" \
    "tlspc.o(.text+0xb): R_X86_64_PC32 against 'tv': a thread-local symbol is reached only through R_X86_64_TPOFF32"

program prio ' nop
.section .init_array.00100,"aw"
.quad main'
link_refuses prioritized_constructor "$WORK/prio.o" "$ARCHIVE" \
    "prio.o: section '.init_array.00100' has no place in the BoxOS image layout"

program wide ' mov eax, OFFSET big'
archive_with libbox_big.a bigdef '.globl big
.set big, 0x100000000'
link_refuses relocation_overflow "$WORK/wide.o" "$WORK/libbox_big.a" \
    "wide.o(.text+0x9): R_X86_64_32 against 'big' does not fit in 32 bits (value 0x100000000)"

program long ' call boxasm_long_probe2
.globl boxasm_long_probe
boxasm_long_probe: ret'
archive_with libbox_long.a a_member_name_longer_than_sixteen '.text
.globl boxasm_long_probe, boxasm_long_probe2
boxasm_long_probe: ret
boxasm_long_probe2: ret'
link_refuses long_member_name "$WORK/long.o" "$WORK/libbox_long.a" \
    "'boxasm_long_probe' is defined twice: in long.o and in libbox_long.a(a_member_name_longer_than_sixteen.o)"

# ---- what the assembler says about a wrong program ---------------------------

# The first line of the refusal — file, line, column and why — word for word.
assembly_refuses() {
    local name=$1 source=$2 want=$3 said
    printf '%s' "$source" > "$WORK/$name.asm"
    said=$("$STAND" assemble "$WORK/$name.asm" "$WORK/$name.o" 2>&1 | head -1)
    if [ -s "$WORK/$name.o" ]; then fail "$name" "assembled instead of refusing"
    elif [ "$said" != "$want" ]; then fail "$name" "said '$said', expected '$want'"
    else ok "$name"; fi
}

HEAD=$'section .text\nglobal main\nmain:\n'
assembly_refuses unknown_instruction "$HEAD    movq rax, 2"$'\n' \
    "unknown_instruction.asm:4:5: 'movq' is not an instruction, a directive or a box operation"
assembly_refuses undefined_name "$HEAD    jmp nowhere"$'\n' \
    "undefined_name.asm:4:9: 'nowhere' is never defined; a name from outside the program is declared with extern"
assembly_refuses unsized_memory "$HEAD    inc [rax]"$'\n' \
    "unsized_memory.asm:4:5: the operation size is not said: write byte, word, dword or qword before the memory operand of 'inc'"
assembly_refuses immediate_range "$HEAD    mov al, 300"$'\n' \
    "immediate_range.asm:4:5: 300 does not fit the 8-bit operation of 'mov'"
assembly_refuses address_without_endbr64 "$HEAD    lea rax, [handler]"$'\n    ret\nhandler:\n    ret\n' \
    "address_without_endbr64.asm:4:5: 'handler' is used as an address of code, so it must begin with endbr64: on a CET machine an indirect jump or call to it stops the program"
assembly_refuses no_main $'section .text\nstart:\n    ret\n' \
    "no_main.asm:1:1: a program starts at main: write \`global main\` and put \`main:\` in section .text"
assembly_refuses rsp_index "$HEAD    mov rax, [rbx + rsp*2]"$'\n' \
    "rsp_index.asm:4:14: rsp cannot be an index register"
assembly_refuses short_out_of_reach "$HEAD    jmp short far"$'\n    times 200 nop\nfar:\n    ret\n' \
    "short_out_of_reach.asm:4:5: a short jump reaches -128 to 127 bytes, and its target is 200 away"
assembly_refuses equ_cycle $'a equ b + 1\nb equ a\n'"$HEAD    mov eax, a"$'\n' \
    "equ_cycle.asm:2:7: 'a' is defined in terms of itself"
assembly_refuses label_twice "${HEAD}main:"$'\n    ret\n' \
    "label_twice.asm:4:1: 'main' is already defined"
assembly_refuses high8_with_rex "$HEAD    mov ah, sil"$'\n' \
    "high8_with_rex.asm:4:5: ah, bh, ch and dh cannot be used in an instruction that needs REX"
assembly_refuses unclosed_string "$HEAD    box.say \"unclosed"$'\n' \
    "unclosed_string.asm:4:13: string is not closed on its line"
assembly_refuses bytes_in_bss $'section .bss\n    db 1\n' \
    "bytes_in_bss.asm:2:5: .bss holds no bytes: reserve its space with resb, resw, resd or resq"

assembly_refuses lock_not_lockable "$HEAD    lock mov [rax], eax"$'\n' \
    "lock_not_lockable.asm:4:5: lock goes only on add, adc, and, btc, btr, bts, cmpxchg, cmpxchg8b, cmpxchg16b, dec, inc, neg, not, or, sbb, sub, xadd, xchg and xor, not on 'mov'"
assembly_refuses lock_register "$HEAD    lock add eax, 1"$'\n' \
    "lock_register.asm:4:5: lock needs a memory destination: the first operand of 'add'"
assembly_refuses lock_memory_source "$HEAD    lock add eax, [rax]"$'\n' \
    "lock_memory_source.asm:4:5: lock needs a memory destination: the first operand of 'add'"
assembly_refuses rep_not_string "$HEAD    rep add eax, 1"$'\n' \
    "rep_not_string.asm:4:5: rep goes only on movs, stos and lods, not on 'add'"
assembly_refuses rep_on_compare "$HEAD    rep cmpsb"$'\n' \
    "rep_on_compare.asm:4:5: say when to stop with repe (while equal) or repne (while not equal), not rep, before 'cmpsb'"
assembly_refuses repe_on_move "$HEAD    repe movsb"$'\n' \
    "repe_on_move.asm:4:5: movs, stos and lods repeat with rep; repe and repne go on cmps and scas, not on 'movsb'"
assembly_refuses two_prefixes "$HEAD    lock rep movsb"$'\n' \
    "two_prefixes.asm:4:10: an instruction takes one prefix"
assembly_refuses prefix_alone "$HEAD    rep"$'\n' \
    "prefix_alone.asm:4:5: 'rep' is a prefix: the instruction it goes before follows it on its line"
assembly_refuses loop_near "$HEAD    loop near main"$'\n' \
    "loop_near.asm:4:5: 'loop' has no near form: it reaches -128 to 127 bytes"
assembly_refuses loop_out_of_reach "$HEAD    loop far"$'\n    times 200 nop\nfar:\n    ret\n' \
    "loop_out_of_reach.asm:4:5: 'loop' reaches -128 to 127 bytes, and its target is 200 away"
assembly_refuses short_to_extern $'section .text\nextern printf\nglobal main\nmain:\n    jmp short printf\n' \
    "short_to_extern.asm:5:5: a short jump reaches only a label of its own section"
echo "boxasm: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
