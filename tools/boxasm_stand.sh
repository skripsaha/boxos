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

# The stand runs the assembler on the stack a BoxOS program has: it grows to
# BOXOS_USER_STACK_RESERVE (src/include/boxos_limits.h), and the assembler's
# refusals of a source that nests too deep are measured against it. The host's
# own limit may be a little less, and a refusal must never be a crash.
STACK_KIB=$(( $(printf '#include "boxos_limits.h"\nBOXOS_USER_STACK_RESERVE\n' |
                $CC_HOST -E -P -Isrc/include - | tail -1) / 1024 )) && ulimit -s "$STACK_KIB" ||
    { echo "boxasm: harness failed — no stack of BOXOS_USER_STACK_RESERVE for the stand"; exit 2; }

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

# planets.asm reads three files: its table beside it, and lines.inc, a
# library outside the examples that finds lines_core.inc beside itself.
PLANETS=$WORK/planets_volume
mkdir -p "$PLANETS" &&
    cp src/userspace/planets.asm "$PLANETS/1" && cp src/userspace/planets.inc "$PLANETS/2" &&
    cp src/userspace/lines.inc "$PLANETS/3" && cp src/userspace/lines_core.inc "$PLANETS/4" &&
    printf '1 planets.asm code,asm,example\n2 planets.inc code,asm,example\n3 lines.inc code,asm,asmlib\n4 lines_core.inc code,asm,asmlib\n' \
        > "$PLANETS/volume"
if "$STAND" volume "$PLANETS" "code,asm,example" planets.asm "$WORK/planets.o" 2> "$WORK/asm.err" &&
   "$STAND" link "$ENTRY" "$WORK/planets.o" "$ARCHIVE" "$WORK/planets.elf" symbols 2> "$WORK/link.err"; then
    ok planets_builds
else
    fail planets_builds "$(cat "$WORK/asm.err" "$WORK/link.err" 2>/dev/null)"
fi

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

# The preprocessor's work, held to NASM's on one program using every feature
# the two share.
python3 "$ORACLE" macros > "$WORK/macros.asm" && python3 "$ORACLE" macros nasm > "$WORK/macros_nasm.asm" &&
    nasm -f elf64 -w-all "$WORK/macros_nasm.asm" -o "$WORK/macros_nasm.o" ||
    { echo "boxasm: harness failed — NASM did not assemble the macro program"; exit 2; }
if ! "$STAND" assemble "$WORK/macros.asm" "$WORK/macros.o" 2> "$WORK/macros.err"; then
    fail macros_match_nasm "$(head -3 "$WORK/macros.err")"
elif out=$(python3 "$ORACLE" compare "$WORK/macros.o" "$WORK/macros_nasm.o" 2>&1); then
    ok "macros_match_nasm ($(wc -l < "$WORK/macros.asm" | tr -d ' ') lines)"
else
    fail macros_match_nasm "$out"
fi

# Floating-point constants, ties included, against their exact rounding:
# NASM 3 rounds a decimal exactly halfway between two floats down, not to even.
python3 "$ORACLE" floats "$WORK/floats.asm" "$WORK/floats.want" ||
    { echo "boxasm: harness failed — the oracle wrote no float witness"; exit 2; }
if ! "$STAND" assemble "$WORK/floats.asm" "$WORK/floats.o" 2> "$WORK/floats.err"; then
    fail floats_round_exactly "$(head -3 "$WORK/floats.err")"
elif ! x86_64-elf-objcopy -O binary -j .data "$WORK/floats.o" "$WORK/floats.got"; then
    fail floats_round_exactly "objcopy could not read .data"
elif cmp -s "$WORK/floats.got" "$WORK/floats.want"; then
    ok "floats_round_exactly ($(grep -c '^    d' "$WORK/floats.asm") lines)"
else
    fail floats_round_exactly "$(cmp "$WORK/floats.got" "$WORK/floats.want" 2>&1 | head -1)"
fi

# ---- the linker, held to GNU ld and tools/nameplate -------------------------

# GNU ld links the reference where BoxASM's linker puts a program: at the code
# start, which user.ld takes from USER_BASE (a program the tree builds itself
# is linked at 0, position-independent, and goes onto the volume as a box).
REF_PLACE=--defsym=USER_BASE=0xC000
x86_64-elf-ld -nostdlib -static $REF_PLACE -T "$LD_SCRIPT" -o "$WORK/ref.pass1" "$ENTRY" "$WORK/hello.o" "$ARCHIVE" &&
    "$NAMEPLATE" build "$WORK/ref.pass1" "$WORK/ref.np.o" > /dev/null &&
    x86_64-elf-ld -nostdlib -static $REF_PLACE -T "$LD_SCRIPT" -o "$WORK/ref.elf" "$ENTRY" "$WORK/hello.o" \
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
# The segments user.ld's PHDRS make, in order, by their rights: code R+X,
# constants R, data RW. The linker's own segment has no memory in a link at a
# fixed place and is passed by.
rights() {
    x86_64-elf-readelf -lW "$1" |
        awk '$1 == "LOAD" && $6 != "0x000000" { r = ""; for (i = 7; i < NF; i++) r = r $i; printf "%s ", r }'
}
if [ "$(rights "$WORK/ref.elf")" = "RE R RW " ] && [ "$(rights "$WORK/hello.elf")" = "$(rights "$WORK/ref.elf")" ]; then
    ok segments_match_gnu_ld
else
    fail segments_match_gnu_ld "ours '$(rights "$WORK/hello.elf")', GNU ld '$(rights "$WORK/ref.elf")'"
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

# A constant that holds an address (.data.rel.ro, what a compiler makes of
# one in position-independent code) lies with the constants, read-only, as
# user.ld lays it.
rights_at() {
    local at address memory rights
    at=$((16#$(x86_64-elf-readelf -sW "$1" | awk -v name="$2" '$NF == name { print $2 }')))
    x86_64-elf-readelf -lW "$1" |
        awk '$1 == "LOAD" { r = ""; for (i = 7; i < NF; i++) r = r $i; print $3, $6, r }' |
        while read -r address memory rights; do
            [ "$at" -ge $((address)) ] && [ "$at" -lt $((address + memory)) ] && echo "$rights"
        done
}
program relro ' lea rax, [rip + table]
.section .data.rel.ro,"aw"
.globl table
table: .quad main'
if "$STAND" link "$ENTRY" "$WORK/relro.o" "$ARCHIVE" "$WORK/relro.elf" symbols 2> "$WORK/link.err" &&
   x86_64-elf-ld -nostdlib -static $REF_PLACE -T "$LD_SCRIPT" -o "$WORK/relro_ref.elf" "$ENTRY" \
       "$WORK/relro.o" "$ARCHIVE" &&
   [ "$(rights_at "$WORK/relro_ref.elf" table)" = R ] && [ "$(rights_at "$WORK/relro.elf" table)" = R ]; then
    ok relro_with_constants
else
    fail relro_with_constants "ours '$(rights_at "$WORK/relro.elf" table 2>/dev/null)', GNU ld '$(rights_at \
        "$WORK/relro_ref.elf" table 2>/dev/null)' $(cat "$WORK/link.err")"
fi

# ---- the Form: takes and gives ------------------------------------------------

FORM_TEXT=$'form 1\ntakes optional things "the files"\ntakes words label "the tags they get"\ntakes into "where it goes"\ngives text\n'
cat > "$WORK/form.asm" <<'EOF'
section .text
global main
extern exit
takes OPTIONAL things "the files"
takes words label "the tags they get"
takes into 'where it goes'
gives text
main:
    endbr64
    xor edi, edi
    call exit
EOF
if "$STAND" assemble "$WORK/form.asm" "$WORK/form.o" 2> "$WORK/asm.err" &&
   x86_64-elf-objcopy -O binary --only-section=.boxos.form "$WORK/form.o" "$WORK/form.got" 2>/dev/null &&
   printf '%s' "$FORM_TEXT" > "$WORK/form.want" && cmp -s "$WORK/form.got" "$WORK/form.want" &&
   x86_64-elf-readelf -SW "$WORK/form.o" | awk '/ \.boxos\.form / { print $(NF-3), $(NF-2), $NF }' |
       grep -qx 'A 0 1'; then
    ok form_section
else
    fail form_section "$(cat "$WORK/asm.err" 2>/dev/null; cmp "$WORK/form.got" "$WORK/form.want" 2>&1)"
fi

# The two symbols bound the Form in the image whose bytes they name, and
# collapse to one address when the program has none.
for form in with without; do
    { echo 'section .text'; echo 'global main'; echo 'extern exit'
      echo 'extern __boxos_form_start'; echo 'extern __boxos_form_end'
      [ $form = with ] && printf 'takes things "the files"\ngives things\n'
      printf 'main:\n    endbr64\n    xor edi, edi\n    call exit\nsection .rodata\nalign 8\nglobal bounds\nbounds: dq __boxos_form_start, __boxos_form_end\n'
    } > "$WORK/bounds_$form.asm"
    "$STAND" assemble "$WORK/bounds_$form.asm" "$WORK/bounds_$form.o" 2> "$WORK/asm.err" &&
        "$STAND" link "$ENTRY" "$WORK/bounds_$form.o" "$ARCHIVE" "$WORK/bounds_$form.elf" symbols \
            2>> "$WORK/asm.err" || { fail "form_symbols_$form" "$(cat "$WORK/asm.err")"; continue; }
    got=$(python3 - "$WORK/bounds_$form.elf" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
shoff, = struct.unpack_from('<Q', d, 0x28)
count, names = struct.unpack_from('<HH', d, 0x3c)
sec = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + 64 * i) for i in range(count)]
def name(s):
    at = sec[names][4] + s[0]
    return d[at:d.index(b'\0', at)].decode()
by = {name(s): s for s in sec}
symtab, strtab = by['.symtab'], by['.strtab']
for i in range(symtab[5] // 24):
    n, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, symtab[4] + 24 * i)
    at = strtab[4] + n
    if d[at:d.index(b'\0', at)] == b'bounds':
        sh = sec[shndx]
        start, end = struct.unpack_from('<QQ', d, sh[4] + value - sh[3])
        form = by['.boxos.form']
        text = d[form[4]:form[4] + form[5]].decode()
        print(start == form[3], end == form[3] + form[5], end - start, repr(text))
EOF
)
    if [ $form = with ] && [ "$got" = "True True 45 'form 1\ntakes things \"the files\"\ngives things\n'" ]; then ok form_symbols_with
    elif [ $form = without ] && [ "$got" = "True True 0 ''" ]; then ok form_symbols_without
    else fail "form_symbols_$form" "$got"; fi
done

# ---- the Recipe: what a program was made from --------------------------------

# The same program linked with a Recipe and without: the section carries the
# Recipe's bytes exactly, after both segments and mapped by none, and every
# byte the loader loads — the segments, the entry, the loadable sections —
# is the same in both.
printf 'recipe 1\nby assemble\ncontext demo\nfrom #41 %s planets.asm\n' \
    ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad > "$WORK/recipe.txt"
if "$STAND" link "$ENTRY" "$WORK/hello.o" "$ARCHIVE" "$WORK/plain.elf" 2> "$WORK/link.err" &&
   "$STAND" link "$ENTRY" "$WORK/hello.o" "$ARCHIVE" "$WORK/recipe.elf" "recipe=$WORK/recipe.txt" \
       2>> "$WORK/link.err"; then
    x86_64-elf-objcopy --dump-section .boxos.recipe="$WORK/recipe.got" "$WORK/recipe.elf" "$WORK/dumped.elf" \
        2>/dev/null
    if cmp -s "$WORK/recipe.got" "$WORK/recipe.txt"; then ok recipe_section
    else fail recipe_section "$(cmp "$WORK/recipe.got" "$WORK/recipe.txt" 2>&1)"; fi

    x86_64-elf-objcopy -O binary "$WORK/plain.elf" "$WORK/plain.bin" &&
        x86_64-elf-objcopy -O binary "$WORK/recipe.elf" "$WORK/recipe.bin"
    if cmp -s "$WORK/plain.bin" "$WORK/recipe.bin" &&
       [ "$(segments "$WORK/plain.elf")" = "$(segments "$WORK/recipe.elf")" ] &&
       [ "$(entry "$WORK/plain.elf")" = "$(entry "$WORK/recipe.elf")" ]; then
        ok recipe_loads_nothing
    else
        fail recipe_loads_nothing "the loaded image differs with a Recipe"
    fi

    placed=$(python3 - "$WORK/recipe.elf" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
phoff, shoff = struct.unpack_from('<QQ', d, 0x20)
phcount, = struct.unpack_from('<H', d, 0x38)
count, names = struct.unpack_from('<HH', d, 0x3c)
ends = [o + fs for (t, f, o, v, p, fs, ms, a) in
        (struct.unpack_from('<IIQQQQQQ', d, phoff + 56 * i) for i in range(phcount)) if t == 1]
sec = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + 64 * i) for i in range(count)]
def name(s):
    at = sec[names][4] + s[0]
    return d[at:d.index(b'\0', at)].decode()
r = [s for s in sec if name(s) == '.boxos.recipe'][0]
print(r[1], r[2], r[3], r[4] >= max(ends), r[8])
EOF
)
    if [ "$placed" = "1 0 0 True 1" ]; then ok recipe_after_segments
    else fail recipe_after_segments "type flags address after-segments align: $placed"; fi

    if x86_64-elf-readelf -aW "$WORK/recipe.elf" 2>&1 > /dev/null | grep -q .; then
        fail recipe_readelf "$(x86_64-elf-readelf -aW "$WORK/recipe.elf" 2>&1 > /dev/null | head -3)"
    else
        ok recipe_readelf
    fi
else
    fail recipe_link "$(cat "$WORK/link.err")"
fi

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
assembly_refuses unknown_instruction "$HEAD    vpternlogd xmm0, xmm1, xmm2, 5"$'\n' \
    "unknown_instruction.asm:4:5: 'vpternlogd' is not an instruction, a directive or a box operation"
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
assembly_refuses sse_unaligned $'section .data\nv dd 1, 2, 3, 4\n'"$HEAD    movaps xmm0, [v]"$'\n' \
    "sse_unaligned.asm:6:5: 'movaps' needs its memory on a 16-byte boundary, and 'v' is not known to be: put align 16 before the data"
assembly_refuses sse_misplaced $'section .data\nalign 16\npad db 1\nv dd 1, 2, 3, 4\n'"$HEAD    addps xmm0, [v]"$'\n' \
    "sse_misplaced.asm:8:5: 'addps' needs its memory on a 16-byte boundary, and 'v' is not on one: put align 16 before the data"
assembly_refuses sse_memory_size "$HEAD    movss xmm0, qword [rax]"$'\n' \
    "sse_memory_size.asm:4:5: the memory is read as 4 bytes (dword, or no size) by 'movss'"
assembly_refuses integer_unsized "$HEAD    cvtsi2sd xmm0, [rax]"$'\n' \
    "integer_unsized.asm:4:5: the operation size is not said: write byte, word, dword or qword before the memory operand of 'cvtsi2sd'"
assembly_refuses rep_sse_form "$HEAD    rep movsd xmm0, xmm1"$'\n' \
    "rep_sse_form.asm:4:5: rep repeats only a string instruction, written without operands, not this form of 'movsd'"
assembly_refuses avx_register "$HEAD    addps ymm0, xmm1"$'\n' \
    "avx_register.asm:4:5: 'addps' is SSE's and takes xmm registers; for ymm write 'vaddps'"
assembly_refuses sse_register_without_v_form "$HEAD    crc32 eax, ymm1"$'\n' \
    "sse_register_without_v_form.asm:4:5: these operands are not a form of 'crc32'"
assembly_refuses avx512_register "$HEAD    vaddps zmm0, zmm1, zmm2"$'\n' \
    "avx512_register.asm:4:12: 'zmm0' is an AVX-512 register; BoxASM has xmm0 to xmm15 and ymm0 to ymm15"
assembly_refuses avx512_high_register "$HEAD    vaddps ymm16, ymm1, ymm2"$'\n' \
    "avx512_high_register.asm:4:12: 'ymm16' is an AVX-512 register; BoxASM has xmm0 to xmm15 and ymm0 to ymm15"
assembly_refuses ymm_in_address "$HEAD    mov rax, [ymm0]"$'\n' \
    "ymm_in_address.asm:4:5: only a gather takes a vector register as the index of its address, not 'mov'"
assembly_refuses vector_index_load "$HEAD    vmovaps xmm1, [rax + xmm2*4]"$'\n' \
    "vector_index_load.asm:4:5: only a gather takes a vector register as the index of its address, not 'vmovaps'"
assembly_refuses two_vector_indices "$HEAD    vgatherdps xmm1, [xmm2 + ymm3], xmm4"$'\n' \
    "two_vector_indices.asm:4:30: an address has one base and one index register"
assembly_refuses gather_scale "$HEAD    vgatherdps xmm1, [rax + xmm2*3], xmm4"$'\n' \
    "gather_scale.asm:4:29: an index is scaled by 1, 2, 4 or 8"
assembly_refuses gather_32bit_base "$HEAD    vgatherdps xmm1, [eax + xmm2*4], xmm4"$'\n' \
    "gather_32bit_base.asm:4:23: an address is made of 64-bit registers"
assembly_refuses gather_16bit_base "$HEAD    vgatherdps xmm1, [bx + xmm2*4], xmm4"$'\n' \
    "gather_16bit_base.asm:4:23: an address is made of 64-bit registers"
assembly_refuses gather_no_index "$HEAD    vgatherdps xmm1, [rax], xmm4"$'\n' \
    "gather_no_index.asm:4:5: the address of a gather is indexed by an xmm or ymm register, in 'vgatherdps'"
assembly_refuses gather_general_index "$HEAD    vpgatherqq ymm1, [rax + rcx*8], ymm4"$'\n' \
    "gather_general_index.asm:4:5: the address of a gather is indexed by an xmm or ymm register, in 'vpgatherqq'"
assembly_refuses gather_index_width "$HEAD    vgatherdps xmm1, [rax + ymm2*4], xmm4"$'\n' \
    "gather_index_width.asm:4:5: 'vgatherdps' has no form with registers of these widths"
assembly_refuses gather_memory_size "$HEAD    vgatherdps xmm1, qword [rax + xmm2*4], xmm4"$'\n' \
    "gather_memory_size.asm:4:5: the memory is read as 4 bytes (dword, or no size) by 'vgatherdps'"
assembly_refuses gather_no_mask "$HEAD    vgatherdps xmm1, [rax + xmm2*4]"$'\n' \
    "gather_no_mask.asm:4:5: these operands are not a form of 'vgatherdps'"
assembly_refuses gather_destination_is_mask "$HEAD    vgatherdps xmm1, [rax + xmm2*4], xmm1"$'\n' \
    "gather_destination_is_mask.asm:4:5: the destination, the index and the mask are three different registers, or the processor stops the program, in 'vgatherdps'"
assembly_refuses gather_destination_is_index "$HEAD    vpgatherdd ymm3, [rax + ymm3*4], ymm4"$'\n' \
    "gather_destination_is_index.asm:4:5: the destination, the index and the mask are three different registers, or the processor stops the program, in 'vpgatherdd'"
assembly_refuses gather_index_is_mask "$HEAD    vgatherdpd xmm1, [rax + xmm2*8], xmm2"$'\n' \
    "gather_index_is_mask.asm:4:5: the destination, the index and the mask are three different registers, or the processor stops the program, in 'vgatherdpd'"
assembly_refuses gather_same_number_other_width "$HEAD    vgatherqps xmm1, [rax + ymm5*4], xmm5"$'\n' \
    "gather_same_number_other_width.asm:4:5: the destination, the index and the mask are three different registers, or the processor stops the program, in 'vgatherqps'"
assembly_refuses vpbroadcast_general "$HEAD    vpbroadcastb xmm1, eax"$'\n' \
    "vpbroadcast_general.asm:4:5: 'vpbroadcastb' has no form with registers of these widths"
assembly_refuses vpbroadcast_memory_size "$HEAD    vpbroadcastb ymm1, word [rax]"$'\n' \
    "vpbroadcast_memory_size.asm:4:5: the memory is read as 1 byte (byte, or no size) by 'vpbroadcastb'"
assembly_refuses vpermq_register "$HEAD    vpermq ymm1, ymm2, ymm3"$'\n' \
    "vpermq_register.asm:4:5: these operands are not a form of 'vpermq'"
assembly_refuses vpermd_xmm "$HEAD    vpermd xmm1, xmm2, xmm3"$'\n' \
    "vpermd_xmm.asm:4:5: 'vpermd' has no form with registers of these widths"
assembly_refuses vex_no_256_form "$HEAD    vphminposuw ymm0, ymm1"$'\n' \
    "vex_no_256_form.asm:4:5: 'vphminposuw' has no 256-bit form"
assembly_refuses vex_no_256_form_three "$HEAD    vdppd ymm0, ymm1, ymm2, 1"$'\n' \
    "vex_no_256_form_three.asm:4:5: 'vdppd' has no 256-bit form"
assembly_refuses vex_no_256_pair "$HEAD    vucomisd ymm0, ymm1"$'\n' \
    "vex_no_256_pair.asm:4:5: 'vucomisd' has no 256-bit form"
assembly_refuses vex_widths_mixed "$HEAD    vaddps ymm0, xmm1, ymm2"$'\n' \
    "vex_widths_mixed.asm:4:5: 'vaddps' has no form with registers of these widths"
assembly_refuses vex_widths_narrowed "$HEAD    vcvtpd2dq ymm0, ymm1"$'\n' \
    "vex_widths_narrowed.asm:4:5: 'vcvtpd2dq' has no form with registers of these widths"
assembly_refuses vex_memory_size "$HEAD    vaddps ymm0, ymm1, oword [rax]"$'\n' \
    "vex_memory_size.asm:4:5: the memory is read as 32 bytes (yword, or no size) by 'vaddps'"
assembly_refuses vex_memory_size_xmm "$HEAD    vaddps xmm0, xmm1, yword [rax]"$'\n' \
    "vex_memory_size_xmm.asm:4:5: the memory is read as 16 bytes (oword, or no size) by 'vaddps'"
assembly_refuses vex_selector_immediate "$HEAD    vblendvps xmm1, xmm2, xmm3, 5"$'\n' \
    "vex_selector_immediate.asm:4:5: the selector is a register, written last, in 'vblendvps'"
assembly_refuses vex_selector_general "$HEAD    vpblendvb xmm1, xmm2, xmm3, rax"$'\n' \
    "vex_selector_general.asm:4:5: the selector is a register, written last, in 'vpblendvb'"
assembly_refuses vex_selector_width "$HEAD    vblendvps ymm1, ymm2, ymm3, xmm4"$'\n' \
    "vex_selector_width.asm:4:5: 'vblendvps' has no form with registers of these widths"
assembly_refuses vex_five_operands "$HEAD    vblendvps xmm1, xmm2, xmm3, xmm4, xmm5"$'\n' \
    "vex_five_operands.asm:4:39: an instruction has at most four operands"
assembly_refuses vex_unaligned $'section .data\nv dd 1, 2, 3, 4, 5, 6, 7, 8\n'"$HEAD    vmovaps xmm0, [v]"$'\n' \
    "vex_unaligned.asm:6:5: 'vmovaps' needs its memory on a 16-byte boundary, and 'v' is not known to be: put align 16 before the data"
assembly_refuses vex_unaligned_256 $'section .data\nalign 16\nv dd 1, 2, 3, 4, 5, 6, 7, 8\n'"$HEAD    vmovdqa ymm0, [v]"$'\n' \
    "vex_unaligned_256.asm:7:5: 'vmovdqa' needs its memory on a 32-byte boundary, and 'v' is not known to be: put align 32 before the data"
assembly_refuses vex_misplaced_256 $'section .data\nalign 32\npad db 1\nv dd 1, 2, 3, 4, 5, 6, 7, 8\n'"$HEAD    vmovntps [v], ymm0"$'\n' \
    "vex_misplaced_256.asm:8:5: 'vmovntps' needs its memory on a 32-byte boundary, and 'v' is not on one: put align 32 before the data"
assembly_refuses vex_integer_unsized "$HEAD    vcvtsi2sd xmm0, xmm1, [rax]"$'\n' \
    "vex_integer_unsized.asm:4:5: the operation size is not said: write byte, word, dword or qword before the memory operand of 'vcvtsi2sd'"
assembly_refuses vex_extractps_wide "$HEAD    vextractps rax, xmm1, 1"$'\n' \
    "vex_extractps_wide.asm:4:5: these operands are not a form of 'vextractps'"
assembly_refuses vex_broadcast_register "$HEAD    vbroadcastsd xmm0, xmm1"$'\n' \
    "vex_broadcast_register.asm:4:5: 'vbroadcastsd' has no form with registers of these widths"
assembly_refuses fma_three_operands "$HEAD    vfmadd132ps xmm1, xmm2"$'\n' \
    "fma_three_operands.asm:4:5: these operands are not a form of 'vfmadd132ps'"
assembly_refuses fma_scalar_memory "$HEAD    vfmadd231sd xmm1, xmm2, oword [rax]"$'\n' \
    "fma_scalar_memory.asm:4:5: the memory is read as 8 bytes (qword, or no size) by 'vfmadd231sd'"
assembly_refuses f16c_widths "$HEAD    vcvtph2ps ymm0, ymm1"$'\n' \
    "f16c_widths.asm:4:5: 'vcvtph2ps' has no form with registers of these widths"
assembly_refuses f16c_memory "$HEAD    vcvtps2ph oword [rax], xmm1, 0"$'\n' \
    "f16c_memory.asm:4:5: the memory is read as 8 bytes (qword, or no size) by 'vcvtps2ph'"
assembly_refuses bmi_sixteen_bits "$HEAD    andn ax, bx, cx"$'\n' \
    "bmi_sixteen_bits.asm:4:5: these operands are not a form of 'andn'"
assembly_refuses bmi_sizes_mixed "$HEAD    mulx eax, ebx, qword [rax]"$'\n' \
    "bmi_sizes_mixed.asm:4:5: the operands are of different sizes for 'mulx'"
assembly_refuses bmi_count_size "$HEAD    shlx rax, rbx, ecx"$'\n' \
    "bmi_count_size.asm:4:5: the operands are of different sizes for 'shlx'"
assembly_refuses bmi_no_tbm "$HEAD    bextr eax, ebx, 5"$'\n' \
    "bmi_no_tbm.asm:4:5: these operands are not a form of 'bextr'"
assembly_refuses x87_unsized "$HEAD    fld [rax]"$'\n' \
    "x87_unsized.asm:4:5: the size of the memory is not said: write dword, qword or tword before the memory operand of 'fld'"
assembly_refuses x87_size "$HEAD    fist qword [rax]"$'\n' \
    "x87_size.asm:4:5: the memory is word or dword, no other size, for 'fist'"
assembly_refuses x87_one_size "$HEAD    fbld qword [rax]"$'\n' \
    "x87_one_size.asm:4:5: the memory is read as 10 bytes (tword, or no size) by 'fbld'"
assembly_refuses x87_state_sized "$HEAD    fxsave qword [rax]"$'\n' \
    "x87_state_sized.asm:4:5: no size is said for the memory of 'fxsave': it is a block of the instruction's own size"
assembly_refuses x87_parenthesized "$HEAD    fadd st(1)"$'\n' \
    "x87_parenthesized.asm:4:10: the x87 registers are written st0 to st7: st1, not st(1)"
assembly_refuses x87_two_stores "$HEAD    fstp st0, st1"$'\n' \
    "x87_two_stores.asm:4:5: these operands are not a form of 'fstp'"
assembly_refuses fxsave_unaligned $'section .data\nv dq 1\nstate: times 512 db 0\n'"$HEAD    fxsave [state]"$'\n' \
    "fxsave_unaligned.asm:7:5: 'fxsave' needs its memory on a 16-byte boundary, and 'state' is not on one: put align 16 before the data"
assembly_refuses mmx_movd_wide "$HEAD    movd rax, mm0"$'\n' \
    "mmx_movd_wide.asm:4:5: these operands are not a form of 'movd'"
assembly_refuses mmx_low_half "$HEAD    punpcklbw mm0, qword [rax]"$'\n' \
    "mmx_low_half.asm:4:5: the memory is read as 4 bytes (dword, or no size) by 'punpcklbw'"
assembly_refuses mmx_in_address "$HEAD    mov eax, [mm0]"$'\n' \
    "mmx_in_address.asm:4:15: an address is made of 64-bit registers"
assembly_refuses movbe_registers "$HEAD    movbe eax, ebx"$'\n' \
    "movbe_registers.asm:4:5: these operands are not a form of 'movbe'"
assembly_refuses movbe_lock "$HEAD    lock movbe [rax], eax"$'\n' \
    "movbe_lock.asm:4:5: lock goes only on add, adc, and, btc, btr, bts, cmpxchg, cmpxchg8b, cmpxchg16b, dec, inc, neg, not, or, sbb, sub, xadd, xchg and xor, not on 'movbe'"
assembly_refuses vex_rep "$HEAD    rep vaddps xmm0, xmm1, xmm2"$'\n' \
    "vex_rep.asm:4:5: rep goes only on movs, stos and lods, not on 'vaddps'"
assembly_refuses blendv_mask "$HEAD    blendvps xmm1, xmm2, xmm3"$'\n' \
    "blendv_mask.asm:4:5: the mask is only xmm0, written third or left out, in 'blendvps'"
assembly_refuses pextrb_word_register "$HEAD    pextrb ax, xmm1, 3"$'\n' \
    "pextrb_word_register.asm:4:5: these operands are not a form of 'pextrb'"
assembly_refuses pextrb_memory_size "$HEAD    pextrb word [rax], xmm1, 3"$'\n' \
    "pextrb_memory_size.asm:4:5: the memory is read as 1 byte (byte, or no size) by 'pextrb'"
assembly_refuses pmovzxbq_memory_size "$HEAD    pmovzxbq xmm0, dword [rax]"$'\n' \
    "pmovzxbq_memory_size.asm:4:5: the memory is read as 2 bytes (word, or no size) by 'pmovzxbq'"
assembly_refuses pinsrb_wide_register "$HEAD    pinsrb xmm0, rax, 1"$'\n' \
    "pinsrb_wide_register.asm:4:5: these operands are not a form of 'pinsrb'"
assembly_refuses lddqu_register "$HEAD    lddqu xmm0, xmm1"$'\n' \
    "lddqu_register.asm:4:5: these operands are not a form of 'lddqu'"
assembly_refuses crc32_unsized "$HEAD    crc32 eax, [rax]"$'\n' \
    "crc32_unsized.asm:4:5: these operands are not a form of 'crc32'"
assembly_refuses ssse3_unaligned $'section .data\nv dd 1, 2, 3, 4\n'"$HEAD    pshufb xmm0, [v]"$'\n' \
    "ssse3_unaligned.asm:6:5: 'pshufb' needs its memory on a 16-byte boundary, and 'v' is not known to be: put align 16 before the data"
assembly_refuses float_in_code "$HEAD    mov eax, 1.5"$'\n' \
    "float_in_code.asm:4:14: a floating-point number stands only in dd or dq data: put it there and load it from memory"
assembly_refuses float_in_word $'section .data\n    dw 1.5\n' \
    "float_in_word.asm:2:8: a floating-point number goes in dd (32-bit) or dq (64-bit)"
assembly_refuses float_too_large $'section .data\n    dd 1.e39\n' \
    "float_too_large.asm:2:8: 1.e39 does not fit a 32-bit float (dd)"
assembly_refuses float_too_small $'section .data\n    dq 1.e-400\n' \
    "float_too_small.asm:2:8: 1.e-400 is too small for a 64-bit float (dq): it would be 0"

assembly_refuses endif_alone "$HEAD%endif"$'\n' \
    "endif_alone.asm:4:1: %endif has no %if before it"
assembly_refuses if_not_closed "$HEAD%if 1"$'\n    nop\n' \
    "if_not_closed.asm:4:1: this %if is not closed by %endif"
assembly_refuses macro_not_closed $'%macro m 0\n    nop\n'"$HEAD" \
    "macro_not_closed.asm:1:1: this %macro is not closed by %endmacro"
assembly_refuses macro_count "$HEAD"$'%macro two 2\n    db %1, %2\n%endmacro\n    two 1\n' \
    "macro_count.asm:7:5: 'two' takes 2 parameters, and 1 is given here"
assembly_refuses error_directive "$HEAD"$'%error "the table is too large"\n' \
    "error_directive.asm:4:1: the table is too large"
assembly_refuses include_unquoted "$HEAD%include math"$'\n' \
    "include_unquoted.asm:4:10: expected the file's name in quotes: %include \"math.asm\""
assembly_refuses pop_empty "$HEAD%pop"$'\n' \
    "pop_empty.asm:4:1: %pop has no %push before it"
assembly_refuses pop_other "$HEAD%push loop"$'\n%pop if\n' \
    "pop_other.asm:5:6: the context on top is 'loop', not 'if'"
assembly_refuses context_none "$HEAD    jmp %\$end"$'\n' \
    "context_none.asm:4:9: '%\$end' is local to a context, and no %push has opened one"
assembly_refuses context_deep "$HEAD%push a"$'\n    jmp %$$$end\n%pop\n' \
    "context_deep.asm:5:9: '%\$\$\$end' names the context 2 below the top, and only 1 is open"
assembly_refuses context_open "$HEAD%push a"$'\n    ret\n' \
    "context_open.asm:4:1: this %push opened a context no %pop closed"
assembly_refuses substr_first "$HEAD%substr s 'abc', 0"$'\n' \
    "substr_first.asm:4:10: a string's first byte is 1"
assembly_refuses strlen_string "$HEAD%strlen n 5"$'\n' \
    "strlen_string.asm:4:11: expected a string in quotes"
printf '%s' "$HEAD"'%warning "look" here'$'\n    ret\n' > "$WORK/warning_said.asm"
said=$("$STAND" assemble "$WORK/warning_said.asm" "$WORK/warning_said.o" 2>&1)
if [ -s "$WORK/warning_said.o" ] && [ "$said" = 'warning_said.asm:4: warning: "look" here' ]; then
    ok warning_said
else
    fail warning_said "said '$said'$([ -s "$WORK/warning_said.o" ] || echo ', and no object')"
fi
assembly_refuses macro_recursion $'%macro rec 0\n    rec\n%endmacro\n'"$HEAD    rec"$'\n' \
    "macro_recursion.asm:2:5: 'rec' is called inside macro calls too deep: a macro may be calling itself"
assembly_refuses parameter_outside "$HEAD    mov eax, %1"$'\n' \
    "parameter_outside.asm:4:14: '%1' is a parameter of a macro, and this line is in none"
assembly_refuses if_label "$HEAD%if size > 2"$'\n%endif\n' \
    "if_label.asm:4:5: 'size' has no value before assembly: the preprocessor computes with numbers and the macros that stand for them"
assembly_refuses if_divide "$HEAD%define ZERO 0"$'\n%if 1/ZERO\n%endif\n' \
    "if_divide.asm:5:6: division by zero"
assembly_refuses define_count "$HEAD%define f(x) x"$'\n    mov eax, f(1, 2)\n' \
    "define_count.asm:5:14: no %define of 'f' takes 2 arguments"
assembly_refuses define_mixed $'%define f(x) x\n%define f 1\n' \
    "define_mixed.asm:2:9: is already a %define with parameters; a name takes them or not"
assembly_refuses unknown_directive "$HEAD%frobnicate"$'\n' \
    "unknown_directive.asm:4:1: '%frobnicate' is not a directive of the preprocessor"
assembly_refuses struc_instruction $'struc s\n.a: resd 1\n    mov eax, 1\nendstruc\n' \
    "struc_instruction.asm:3:5: a struc holds fields: labels, resb, resw, resd, resq and align"
assembly_refuses istruc_unknown $'section .data\nx: istruc nothing\niend\n'"$HEAD" \
    "istruc_unknown.asm:2:11: names no struc written before it"
assembly_refuses at_out_of_order $'struc s\n.a: resd 1\n.b: resd 1\nendstruc\nsection .data\nistruc s\n at s.b, dd 2\n at s.a, dd 1\niend\n'"$HEAD" \
    "at_out_of_order.asm:8:2: the field begins before what the istruc already holds: write the fields in their order, each no larger than its room"
assembly_refuses form_unknown_kind "${HEAD}takes stuff \"the files\""$'\n' \
    "form_unknown_kind.asm:4:7: 'stuff' is no kind a Form takes: thing, things, name, words, text or into"
assembly_refuses form_missing_kind "${HEAD}takes optional"$'\n' \
    "form_missing_kind.asm:4:15: takes says what it takes: thing, things, name, words, text or into"
assembly_refuses form_missing_words "${HEAD}takes things"$'\n' \
    "form_missing_words.asm:4:13: takes says what the field is in quotes: takes things \"the files to tag\""
assembly_refuses form_words_quote "${HEAD}takes things 'the \"files\"'"$'\n' \
    "form_words_quote.asm:4:14: what a field is holds no newline, NUL or double quote"
assembly_refuses form_17_takes "$HEAD$(for _ in $(seq 17); do printf 'takes words "w"\n'; done)" \
    "form_17_takes.asm:20:1: a Form takes at most 16 fields"
assembly_refuses form_two_gives "${HEAD}gives things"$'\ngives text\n' \
    "form_two_gives.asm:5:1: a Form gives once: things or text"
assembly_refuses form_gives_unknown "${HEAD}gives words"$'\n' \
    "form_gives_unknown.asm:4:7: 'words' is not what a Form gives: things or text"
assembly_refuses form_two_into "${HEAD}takes into \"a\""$'\ntakes into "b"\n' \
    "form_two_into.asm:5:7: a Form takes into once: the name the arrow points at is one"
assembly_refuses form_in_struc $'struc s\ntakes things "x"\nendstruc\n'"$HEAD" \
    "form_in_struc.asm:2:1: takes stands outside struc and istruc: it is the program's, not a layout's"
assembly_refuses form_name_shell "${HEAD}takes words them \"w\""$'\n' \
    "form_name_shell.asm:4:13: 'them' is the shell's own word and names no field"
assembly_refuses form_name_twice "${HEAD}takes words a \"w\""$'\ntakes things a "t"\n' \
    "form_name_twice.asm:5:14: 'a' names a field already"
assembly_refuses form_name_spelled "${HEAD}takes words la.bel \"w\""$'\n' \
    "form_name_spelled.asm:4:13: 'la.bel' cannot name a field: a letter first, then letters, digits and _"

# An error in a macro's expansion points at the line of the macro and names
# the calls it came through, the innermost first.
printf '%s' $'%macro load 1\n    mov eax, %1\n%endmacro\n%macro twice 1\n    load %1\n%endmacro\n'"$HEAD    twice nowhere"$'\n' > "$WORK/in_macro.asm"
said=$("$STAND" assemble "$WORK/in_macro.asm" "$WORK/in_macro.o" 2>&1)
want="in_macro.asm:2:14: 'nowhere' is never defined; a name from outside the program is declared with extern
        mov eax, nowhere
                 ^
    in the macro 'load' called at line 5
    in the macro 'twice' called at line 10"
if [ "$said" = "$want" ]; then ok macro_call_chain; else fail macro_call_chain "said '$said'"; fi

# ---- %include: files found by name and tags ----------------------------------
# A volume of tagged files stands in for TagFS: the source is found among the
# tags of the context given, as `assemble` finds it in the Use Context, and
# its %includes by name and the meaning they write.
VOL=$WORK/volume
python3 - "$VOL" <<'EOF'
import os, sys
volume = sys.argv[1]
os.makedirs(volume)
head = 'section .text\nglobal main\nmain:\n    ret\n'
files = [
    ('build.asm', 'demo', '%include "defs.inc"\nsection .text\nglobal main\nmain:\n    greet\n'
                          '    mov eax, ANSWER\n    ret\nsection .data\n    db __FILE__, 0\n%include "table.inc"\n'),
    ('defs.inc', 'demo', '%define ANSWER 42\n%macro greet 0\n    nop\n%include "inner.inc"\n%endmacro\n'),
    ('inner.inc', 'demo', '    xor eax, eax\n    db __FILE__\n'),
    ('table.inc', 'demo', 'values:\n%assign i 0\n%rep 4\n    dd i * i\n%assign i i+1\n%endrep\n    db __FILE__, 0\n'),
    ('math.asm', 'demo, fast', 'value equ 2\n'),
    ('math.asm', 'demo, old', 'value equ 1\n'),
    ('meaning.asm', 'demo', head + '%include "math.asm" fast\nsection .data\n    dd value\n'),
    ('macroed.asm', 'demo', '%macro bring 1\n%include %1\n%endmacro\n' + head +
                            'section .data\n    bring "math.asm" fast\n    dd value\n'),
    ('print.asm', 'mylib', '%include "part.asm"\nprinted equ part_value\n'),
    ('part.asm', 'mylib', 'part_value equ 7\n'),
    ('part.asm', 'demo', 'part_value equ 99\n'),
    ('library.asm', 'demo', head + '%include "print.asm" mylib everywhere\nsection .data\n    dd printed\n'),
    ('several.asm', 'demo', head + '%include "math.asm"\n'),
    ('slow.asm', 'demo', head + '%include "math.asm" slow\n'),
    ('both.asm', 'demo', head + '%include "math.asm" fast old\n'),
    ('lone.asm', 'game', 'lone_value equ 5\n'),
    ('outside.asm', 'demo', head + '%include "lone.asm"\n'),
    ('circle.asm', 'demo', head + '%include "round.asm"\n'),
    ('round.asm', 'demo', '%include "circle.asm"\n'),
    ('nowhere.asm', 'demo', head + '%include "nothing.asm"\n'),
    ('broken.inc', 'demo', '%macro load 1\n    mov eax, %1\n%endmacro\n    load nowhere_defined\n'),
    ('broken.asm', 'demo', head + '%include "broken.inc"\n'),
    ('binary.inc', 'demo', 'db 1\n\0\x01\n'),
    ('binary.asm', 'demo', head + '%include "binary.inc"\n'),
    ('angle.asm', 'demo', head + '%include <box.inc>\n'),
    ('twice.asm', 'demo, one', head),
    ('twice.asm', 'demo, two', head),
    ('away.asm', 'game', head),
]
with open(os.path.join(volume, 'volume'), 'w') as listing:
    for number, (name, tags, text) in enumerate(files, 1):
        listing.write('%d %s %s\n' % (number, name, tags))
        with open(os.path.join(volume, str(number)), 'w', newline='') as f:
            f.write(text)
EOF
# NASM reads the same files from a directory, by path.
mkdir -p "$WORK/by_path" && cp "$VOL/1" "$WORK/by_path/build.asm" && cp "$VOL/2" "$WORK/by_path/defs.inc" &&
    cp "$VOL/3" "$WORK/by_path/inner.inc" && cp "$VOL/4" "$WORK/by_path/table.inc" &&
    (cd "$WORK/by_path" && nasm -f elf64 -w-all build.asm -o build_nasm.o) ||
    { echo "boxasm: harness failed — NASM did not assemble the included files"; exit 2; }
if ! "$STAND" volume "$VOL" demo build.asm "$WORK/build.o" 2> "$WORK/build.err"; then
    fail includes_match_nasm "$(head -3 "$WORK/build.err")"
elif out=$(python3 "$ORACLE" compare "$WORK/build.o" "$WORK/by_path/build_nasm.o" 2>&1); then
    ok includes_match_nasm
else
    fail includes_match_nasm "$out"
fi

# incbin: the bytes of a file found as %include finds one — whole, past a
# skip, a count of them, repeated by times, named by a %define — held to
# NASM's incbin of the same file by its path; then what it refuses.
BIN=$WORK/incbin_volume
mkdir -p "$BIN" && python3 - "$BIN" <<'EOF'
import os, sys
volume = sys.argv[1]
head = 'section .text\nglobal main\nmain:\n'
program = """default rel
section .rodata
whole: incbin "blob.bin"
part incbin "blob.bin" data, 16, 8
rest: incbin "blob.bin" data everywhere, 760
none: incbin "blob.bin", 772
twice: times 2 incbin "blob.bin", 3, 2
%define BLOB "blob.bin"
viadef: incbin BLOB, 0, 4
section .text
global main
main:
    lea rax, [part]
    ret
"""
files = [('prog.asm', 'demo', program), ('blob.bin', 'data, demo', bytes(range(256)) * 3 + b'tail'),
         ('beyond.asm', 'demo', head + '    incbin "blob.bin", 900\n'),
         ('negative.asm', 'demo', head + '    incbin "blob.bin", -1\n'),
         ('bss.asm', 'demo', head + 'section .bss\n    incbin "blob.bin"\n'),
         ('unquoted.asm', 'demo', head + '    incbin blob\n'),
         ('far.bin', 'data, game', b'far'),
         ('outside.asm', 'demo', head + '    incbin "far.bin"\n')]
with open(os.path.join(volume, 'volume'), 'w') as listing:
    for number, (name, tags, data) in enumerate(files, 1):
        listing.write('%d %s %s\n' % (number, name, tags))
        with open(os.path.join(volume, str(number)), 'wb') as f:
            f.write(data if isinstance(data, bytes) else data.encode())
with open(os.path.join(volume, 'nasm.asm'), 'w') as f:
    f.write(program.replace('"blob.bin" data everywhere', '"2"').replace('"blob.bin" data', '"2"')
            .replace('"blob.bin"', '"2"'))
EOF
(cd "$BIN" && nasm -f elf64 -w-all nasm.asm -o nasm.o) ||
    { echo "boxasm: harness failed — NASM did not assemble the incbin program"; exit 2; }
if ! "$STAND" volume "$BIN" demo prog.asm "$WORK/incbin.o" 2> "$WORK/incbin.err"; then
    fail incbin_matches_nasm "$(head -3 "$WORK/incbin.err")"
elif out=$(python3 "$ORACLE" compare "$WORK/incbin.o" "$BIN/nasm.o" 2>&1); then
    ok incbin_matches_nasm
else
    fail incbin_matches_nasm "$out"
fi
incbin_refuses() {
    local name=$1 want=$2 said
    said=$("$STAND" volume "$BIN" demo "$name.asm" "$WORK/incbin_$name.o" 2>&1 | head -1)
    if [ -s "$WORK/incbin_$name.o" ]; then fail "incbin_$name" "built instead of refusing"
    elif [ "$said" != "$want" ]; then fail "incbin_$name" "said '$said', expected '$want'"
    else ok "incbin_$name"; fi
}
incbin_refuses beyond "beyond.asm:4:5: the file holds 772 bytes, fewer than the 900 incbin skips"
incbin_refuses negative "negative.asm:4:24: incbin skips 0 or more bytes"
incbin_refuses bss "bss.asm:5:5: .bss holds no bytes: reserve its space with resb, resw, resd or resq"
incbin_refuses unquoted "unquoted.asm:4:12: expected the file's name in quotes: incbin \"data.bin\""
incbin_refuses outside "outside.asm:4:12: there is no far.bin in demo"

# A program that builds, and the first dword of its .data: which file it took.
volume_takes() {
    local name=$1 want=$2 said
    if ! "$STAND" volume "$VOL" demo "$name.asm" "$WORK/$name.o" 2> "$WORK/$name.err"; then
        fail "include_$name" "$(head -3 "$WORK/$name.err")"; return
    fi
    said=$(python3 - "$WORK/$name.o" <<'EOF'
import struct, sys
data = open(sys.argv[1], 'rb').read()
at, count, names = struct.unpack_from('<Q', data, 0x28)[0], struct.unpack_from('<H', data, 0x3c)[0], \
    struct.unpack_from('<H', data, 0x3e)[0]
sections = [struct.unpack_from('<IIQQQQIIQQ', data, at + i * 64) for i in range(count)]
for section in sections:
    if data[sections[names][4] + section[0]:].split(b'\0')[0] == b'.data':
        print(struct.unpack_from('<I', data, section[4])[0])
EOF
)
    if [ "$said" = "$want" ]; then ok "include_$name"; else fail "include_$name" "took $said, expected $want"; fi
}
volume_takes meaning 2     # the math.asm that wears fast
volume_takes macroed 2     # a %include a macro's parameter names
volume_takes library 7     # a library reached everywhere: its own part.asm, not demo's

# A refusal, whole: what is there and what to write.
volume_says() {
    local name=$1 source=$2 want said
    want=$(cat)
    said=$("$STAND" volume "$VOL" demo "$source" "$WORK/$name.o" 2>&1)
    if [ -s "$WORK/$name.o" ]; then fail "$name" "built instead of refusing"
    elif [ "$said" != "$want" ]; then fail "$name" "said:"$'\n'"$said"$'\n'"expected:"$'\n'"$want"
    else ok "$name"; fi
}
volume_says include_several several.asm <<'EOF'
several.asm:5:10: math.asm could be any of 2 files in demo
    %include "math.asm"
             ^
  say which by a tag only it wears:
    %include "math.asm" fast    for the one wearing math, demo, fast
    %include "math.asm" old     for the one wearing math, demo, old
EOF
volume_says include_meaning_unworn slow.asm <<'EOF'
slow.asm:5:10: no math.asm in demo wears slow
    %include "math.asm" slow
             ^
  they wear:
    math, demo, fast
    math, demo, old
EOF
volume_says include_meaning_together both.asm <<'EOF'
both.asm:5:10: no math.asm in demo wears all of fast, old
    %include "math.asm" fast old
             ^
  they wear:
    math, demo, fast
    math, demo, old
EOF
volume_says include_outside outside.asm <<'EOF'
outside.asm:5:10: there is no lone.asm in demo
    %include "lone.asm"
             ^
  outside it, reached by:
    %include "lone.asm" game everywhere    for the one wearing lone, game
  or let it wear demo too
EOF
volume_says include_circle circle.asm <<'EOF'
round.asm:1:10: 'circle.asm' is already being read: the files include one another in a circle
    %include "circle.asm"
             ^
    in the file 'round.asm' included at line 5 of circle.asm
EOF
volume_says include_nowhere nowhere.asm <<'EOF'
nowhere.asm:5:10: there is no file named nothing.asm here or anywhere else
    %include "nothing.asm"
             ^
EOF
volume_says include_error_inside broken.asm <<'EOF'
broken.inc:2:14: 'nowhere_defined' is never defined; a name from outside the program is declared with extern
        mov eax, nowhere_defined
                 ^
    in the macro 'load' called at line 4
    in the file 'broken.inc' included at line 5 of broken.asm
EOF
volume_says include_binary binary.asm <<'EOF'
binary.asm:5:10: binary.inc holds a NUL byte in its line 2: it is not a source
    %include "binary.inc"
             ^
EOF
volume_says include_angle angle.asm <<'EOF'
angle.asm:5:10: files are found by name and tags, not along a path: write %include "box.inc", and after it a tag it wears, or everywhere, to say which
    %include <box.inc>
             ^
EOF
volume_says source_several twice.asm <<'EOF'
assemble: twice.asm could be any of 2 files in demo
  they wear:
    twice, demo, one
    twice, demo, two
  narrow the context with use
EOF
volume_says source_outside away.asm <<'EOF'
assemble: there is no away.asm in demo
  outside it, wearing:
    away, game
  change the context with use
EOF
assembly_refuses include_no_volume "$HEAD"$'%include "math.asm"\n' \
    "include_no_volume.asm:4:10: this source is no file of a volume, and finds no other file to include"
printf 'section .text\nglobal main\nmain:\n    db 1\0002\n' > "$WORK/nul_source.asm"
said=$("$STAND" assemble "$WORK/nul_source.asm" "$WORK/nul_source.o" 2>&1 | head -1)
if [ "$said" = "nul_source.asm:4:9: a NUL byte stands here: a source is text" ]; then ok nul_source
else fail nul_source "said '$said'"; fi

# ---- depth: the stack a BoxOS program has ------------------------------------
# A source nests as deep as it likes; the stack of `assemble` grows to
# BOXOS_USER_STACK_RESERVE with a gap that is never mapped below it. The stand
# is built again as BoxOS builds the command — no optimisation — and run on
# that much stack (STACK_KIB, above): each source below nests deeper than it
# carries and refuses in words, the place it names being wherever the stack
# ran short.
STAND0=$WORK/boxasm_stand_o0
$CC_HOST -std=c11 -O0 -w -I"$ASM_DIR" -Isrc/userspace/boxlib/include -Isrc/include \
    tools/boxasm_stand.c $(ls "$ASM_DIR"/*.c | grep -v '/assemble[^/]*\.c$') -o "$STAND0" ||
    { echo "boxasm: harness failed — the unoptimised stand did not compile"; exit 2; }
python3 - "$WORK" <<'EOF'
import sys
work = sys.argv[1]
head = 'section .text\nglobal main\nmain:\n    ret\n'
deep = {
    'deep_macro': '%macro rec 0\n    rec\n%endmacro\n' + head + '    rec\n',
    'deep_counting': '%macro count 1\n%if %1 > 0\n%assign left %1-1\n    db left & 0xFF\n    count left\n'
                     '%endif\n%endmacro\n' + head + 'section .data\n    count 100000\n',
    'deep_defines': '\n'.join(['%define d0 1'] + ['%%define d%d d%d' % (i, i - 1) for i in range(1, 40000)])
                    + '\n' + head + 'section .data\n    dd d39999\n',
    'deep_parentheses': head + 'section .data\n    dd ' + '(' * 30000 + '1' + ')' * 30000 + '\n',
    'deep_if': head + '%if ' + '(' * 30000 + '1' + ')' * 30000 + '\n%endif\n',
    'deep_sum': head + 'section .data\n    dd ' + '+'.join(['1'] * 300000) + '\n',
    'deep_equ': head + 'c0 equ 1\n' + '\n'.join('c%d equ c%d+1' % (i, i - 1) for i in range(1, 64000))
                + '\nsection .data\n    dd c63999\n',
}
for name, text in deep.items():
    with open('%s/%s.asm' % (work, name), 'w') as f:
        f.write(text)
EOF
deep_refuses() {
    local name=$1 want=$2 said rc
    said=$("$STAND0" assemble "$WORK/$name.asm" "$WORK/$name.o" 2>&1 | head -1)
    rc=$("$STAND0" assemble "$WORK/$name.asm" "$WORK/$name.o" > /dev/null 2>&1; echo $?)
    said=$(printf '%s' "$said" | sed -E 's/^[^:]+:[0-9]+:[0-9]+: //')
    if [ "$rc" != 1 ]; then fail "$name" "ended with $rc instead of refusing"
    elif [ "$said" != "$want" ]; then fail "$name" "said '$said', expected '$want'"
    else ok "$name"; fi
}
deep_refuses deep_macro "'rec' is called inside macro calls too deep: a macro may be calling itself"
deep_refuses deep_counting "'count' is called inside macro calls too deep: a macro may be calling itself"
deep_refuses deep_defines "the %defines expand inside one another too deep"
deep_refuses deep_parentheses "the expression nests too deep"
deep_refuses deep_if "the expression nests too deep"
deep_refuses deep_sum "the expression is too deep to work out: too many operations, and names defined by names, inside one another"
deep_refuses deep_equ "the expression is too deep to work out: too many operations, and names defined by names, inside one another"

# 12000 files, each including the next.
python3 - "$WORK/deep_files" <<'EOF'
import os, sys
volume = sys.argv[1]
os.makedirs(volume)
with open(os.path.join(volume, 'volume'), 'w') as listing:
    for i in range(12001):
        name = 'chain.asm' if i == 0 else 'chain%d.inc' % i
        listing.write('%d %s demo\n' % (i + 1, name))
        with open(os.path.join(volume, str(i + 1)), 'w') as f:
            f.write(('section .text\nglobal main\nmain:\n    ret\n' if i == 0 else '')
                    + ('%%include "chain%d.inc"\n' % (i + 1) if i < 12000 else 'nop\n'))
EOF
said=$("$STAND0" volume "$WORK/deep_files" demo chain.asm "$WORK/deep_files.o" 2>&1 | head -1 |
       sed -E 's/^[^:]+:[0-9]+:[0-9]+: //')
if printf '%s' "$said" | grep -q -E "^'chain[0-9]+\.inc' is included inside files and macros too deep$"; then
    ok deep_files
else
    fail deep_files "said '$said'"
fi

echo "boxasm: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
