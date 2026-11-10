#!/bin/bash
# pack stand — tools/pack, held to the programs it packs:
#
#     tools/pack_stand.sh
#
# Every ELF program the tree has built, and one small program linked three
# ways — at the fixed address the tree links at, and as a static PIE at 0 with
# its relocations as RELA and as RELR — and once more with its RELA words
# zero, as a linker that leaves the address only in the addend makes it, is
# packed, and each box is held to its
# ELF by tools/pack_stand.c: read whole by the one reader, and every segment,
# the entry, the thread template, the relocations and the carried sections
# found in it as the ELF has them, by a reading of the ELF that shares nothing
# with pack. Then ELF files pack must refuse — writable and executable code,
# two code segments, a program interpreter, a relocation that is not
# R_X86_64_RELATIVE, an object file, an address in the thread template,
# settings with a setting it cannot read, a file that is no ELF — each refused
# with
# its reason. Built under the address and undefined-behaviour sanitizers. The
# last line counts the cases that held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}
CC_CROSS=${CC_CROSS:-x86_64-elf-gcc}
LD_CROSS=${LD_CROSS:-x86_64-elf-ld}

LIB=src/userspace/boxlib
WORK=$(mktemp -d "${TMPDIR:-/tmp}/pack_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

harness() { echo "pack: harness failed — $1"; exit 2; }

FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined"
$CC_HOST $FLAGS -fno-builtin -I$LIB/include -Isrc/include -c $LIB/src/sha256.c -o "$WORK/sha256.o" \
    > "$WORK/cc.log" 2>&1 || { cat "$WORK/cc.log"; harness "sha256.c did not build"; }
$CC_HOST $FLAGS -Isrc/include -Itools tools/pack.c "$WORK/sha256.o" -o "$WORK/pack" \
    > "$WORK/cc.log" 2>&1 || { cat "$WORK/cc.log"; harness "pack did not build"; }
$CC_HOST $FLAGS -Isrc/include -Itools tools/pack_stand.c "$WORK/sha256.o" -o "$WORK/pack_stand" \
    > "$WORK/cc.log" 2>&1 || { cat "$WORK/cc.log"; harness "the stand did not build"; }

# ── one small program, linked three ways ─────────────────────────────────────
cat > "$WORK/small.c" <<'EOF'
static int counter = 5;
static const char *greeting = "hello";
int *table[] = { &counter, 0 };
const int *const fixed[] = { &counter };
__thread int local = 7;
__thread char space[64];
__attribute__((section(".boxos.settings"), used)) static const char settings[] =
    "settings 1\nlook one-of dark light default dark \"how the page looks\"\n";
__attribute__((section(".boxos.life"), used)) static const char life[] = "life 1\nstays\nrise\n";
int helper(int x) { return x + counter + local + space[x & 63] + *fixed[0]; }
const char **said(void) { return &greeting; }
void _start(void) { table[1] = &counter; helper(3); for (;;) { } }
EOF
$CC_CROSS -ffreestanding -nostdlib -fpie -O2 -c "$WORK/small.c" -o "$WORK/small.o" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; harness "the small program did not build"; }
# user.ld links a program at 0, position-independent, as the tree does; the
# executable at a fixed address is linked at the code start, where BoxASM's
# linker puts one (USER_BASE).
LINK="-static --no-dynamic-linker -e _start"
$LD_CROSS $LINK --defsym=USER_BASE=0xC000 -T $LIB/user.ld "$WORK/small.o" -o "$WORK/small_fixed.elf" > "$WORK/ld.log" 2>&1 &&
$LD_CROSS $LINK -pie -T $LIB/user.ld "$WORK/small.o" -o "$WORK/small_rela.elf" >> "$WORK/ld.log" 2>&1 &&
$LD_CROSS $LINK -pie -z pack-relative-relocs -T $LIB/user.ld "$WORK/small.o" -o "$WORK/small_relr.elf" \
    >> "$WORK/ld.log" 2>&1 || { cat "$WORK/ld.log"; harness "the small program did not link"; }
# And a static PIE with not one address in it: its box must still say it can go anywhere.
printf 'int square(int x) { return x * x; }\nvoid _start(void) { square(3); for (;;) { } }\n' > "$WORK/bare.c"
$CC_CROSS -ffreestanding -nostdlib -fpie -O2 -c "$WORK/bare.c" -o "$WORK/bare.o" > "$WORK/cc.log" 2>&1 &&
$LD_CROSS $LINK -pie -T $LIB/user.ld "$WORK/bare.o" -o "$WORK/bare_pie.elf" > "$WORK/ld.log" 2>&1 ||
    { cat "$WORK/cc.log" "$WORK/ld.log"; harness "the bare program did not build"; }

# A static PIE whose thread template holds an address: no base is added to the
# template a thread's block is copied from, so pack must refuse it.
printf 'static int x;\n__thread int *pointer = &x;\nvoid _start(void) { *pointer = 1; for (;;) { } }\n' \
    > "$WORK/tlsptr.c"
$CC_CROSS -ffreestanding -nostdlib -fpie -O2 -c "$WORK/tlsptr.c" -o "$WORK/tlsptr.o" > "$WORK/cc.log" 2>&1 &&
$LD_CROSS $LINK -pie -z pack-relative-relocs -T $LIB/user.ld "$WORK/tlsptr.o" -o "$WORK/tlsptr.elf" \
    > "$WORK/ld.log" 2>&1 || { cat "$WORK/cc.log" "$WORK/ld.log"; harness "the thread-pointer program did not build"; }

# A static PIE whose settings declare one with no default: a setting no
# program could have, refused at the build.
printf '__attribute__((section(".boxos.settings"), used)) static const char s[] =\n    "settings 1\\nlook one-of dark light \\"how it looks\\"\\n";\nvoid _start(void) { for (;;) { } }\n' \
    > "$WORK/badsettings.c"
$CC_CROSS -ffreestanding -nostdlib -fpie -O2 -c "$WORK/badsettings.c" -o "$WORK/badsettings.o" > "$WORK/cc.log" 2>&1 &&
$LD_CROSS $LINK -pie -T $LIB/user.ld "$WORK/badsettings.o" -o "$WORK/badsettings.elf" > "$WORK/ld.log" 2>&1 ||
    { cat "$WORK/cc.log" "$WORK/ld.log"; harness "the program with bad settings did not build"; }

# A static PIE whose life says a word nobody who starts it knows: one it meant
# and nobody would hear, refused at the build.
printf '__attribute__((section(".boxos.life"), used)) static const char l[] = "life 1\\nstays\\nlonely\\n";\nvoid _start(void) { for (;;) { } }\n' \
    > "$WORK/badlife.c"
$CC_CROSS -ffreestanding -nostdlib -fpie -O2 -c "$WORK/badlife.c" -o "$WORK/badlife.o" > "$WORK/cc.log" 2>&1 &&
$LD_CROSS $LINK -pie -T $LIB/user.ld "$WORK/badlife.o" -o "$WORK/badlife.elf" > "$WORK/ld.log" 2>&1 ||
    { cat "$WORK/cc.log" "$WORK/ld.log"; harness "the program with a bad life did not build"; }
# And one that lives two ways at once.
printf '__attribute__((section(".boxos.life"), used)) static const char l[] = "life 1\\nstays\\nserves\\n";\nvoid _start(void) { for (;;) { } }\n' \
    > "$WORK/twolives.c"
$CC_CROSS -ffreestanding -nostdlib -fpie -O2 -c "$WORK/twolives.c" -o "$WORK/twolives.o" > "$WORK/cc.log" 2>&1 &&
$LD_CROSS $LINK -pie -T $LIB/user.ld "$WORK/twolives.o" -o "$WORK/twolives.elf" > "$WORK/ld.log" 2>&1 ||
    { cat "$WORK/cc.log" "$WORK/ld.log"; harness "the program that lives two ways did not build"; }

# ── a copy of a program broken, or changed, one way ─────────────────────────
# patch FILE OUT KIND: a copy of FILE broken one way.
patch() {
    python3 -I - "$1" "$2" "$3" <<'PY'
import struct, sys
src, out, kind = sys.argv[1:]
b = bytearray(open(src, 'rb').read())
phoff, = struct.unpack_from('<Q', b, 0x20)
phnum, = struct.unpack_from('<H', b, 0x38)
def ph(i): return phoff + 56 * i
def find(ptype, pred=lambda o: True):
    for i in range(phnum):
        o = ph(i)
        if struct.unpack_from('<I', b, o)[0] == ptype and pred(o): return o
    raise SystemExit('no such header')
if kind == 'wx':
    o = find(1, lambda o: struct.unpack_from('<I', b, o + 4)[0] & 1)
    struct.pack_into('<I', b, o + 4, 7)
elif kind == 'twocode':
    o = find(1, lambda o: struct.unpack_from('<I', b, o + 4)[0] == 4)
    struct.pack_into('<I', b, o + 4, 5)
elif kind == 'interp':
    o = find(7)
    struct.pack_into('<I', b, o, 3)
elif kind == 'zeroaddends':
    # As a linker that leaves a RELA word zero would: the address it means is
    # only in the relocation's addend.
    shoff, = struct.unpack_from('<Q', b, 0x28)
    shnum, = struct.unpack_from('<H', b, 0x3C)
    loads = [ph(i) for i in range(phnum) if struct.unpack_from('<I', b, ph(i))[0] == 1]
    def file_at(virt):
        for o in loads:
            off, va, filesz = struct.unpack_from('<QQ', b, o + 8) + (struct.unpack_from('<Q', b, o + 32)[0],)
            if va <= virt < va + filesz: return off + virt - va
        raise SystemExit('a relocation outside the segments')
    zeroed = 0
    for i in range(shnum):
        s = shoff + 64 * i
        if struct.unpack_from('<I', b, s + 4)[0] != 4: continue
        off, size = struct.unpack_from('<Q', b, s + 24)[0], struct.unpack_from('<Q', b, s + 32)[0]
        for at in range(off, off + size, 24):
            struct.pack_into('<Q', b, file_at(struct.unpack_from('<Q', b, at)[0]), 0)
            zeroed += 1
    if zeroed == 0: raise SystemExit('no RELA')
elif kind == 'reloc64':
    shoff, = struct.unpack_from('<Q', b, 0x28)
    shnum, = struct.unpack_from('<H', b, 0x3C)
    for i in range(shnum):
        s = shoff + 64 * i
        if struct.unpack_from('<I', b, s + 4)[0] == 4 and struct.unpack_from('<Q', b, s + 32)[0] >= 24:
            off, = struct.unpack_from('<Q', b, s + 24)
            struct.pack_into('<Q', b, off + 8, 1)
            break
    else:
        raise SystemExit('no RELA')
elif kind == 'badform':
    # A Form with a control byte in it: no text as a box keeps one.
    shoff, = struct.unpack_from('<Q', b, 0x28)
    shnum, strndx = struct.unpack_from('<HH', b, 0x3C)
    names = struct.unpack_from('<Q', b, shoff + 64 * strndx + 24)[0]
    for i in range(shnum):
        s = shoff + 64 * i
        at = names + struct.unpack_from('<I', b, s)[0]
        if bytes(b[at:at + 12]) == b'.boxos.form\0':
            b[struct.unpack_from('<Q', b, s + 24)[0] + 3] = 1
            break
    else:
        raise SystemExit('no .boxos.form')
open(out, 'wb').write(b)
PY
}

patch "$WORK/small_rela.elf" "$WORK/small_zeroed.elf" zeroaddends || harness "could not zero the small program's RELA words"

# ── every program, packed and held to its ELF ────────────────────────────────
pairs=()
n=0
while IFS= read -r elf; do
    n=$((n + 1))
    box="$WORK/box.$n.box"
    if ! "$WORK/pack" "$elf" "$box" 2> "$WORK/pack.err"; then
        echo "FAIL $elf: pack refused it: $(cat "$WORK/pack.err")"
        refused_ours=$(( ${refused_ours:-0} + 1 ))
        continue
    fi
    pairs+=("$elf" "$box")
done < <( { find src/userspace build -maxdepth 4 -name '*.elf' ! -name 'kernel*' ! -name '._*' 2>/dev/null | sort
            echo "$WORK/small_fixed.elf"; echo "$WORK/small_rela.elf"; echo "$WORK/small_relr.elf"
            echo "$WORK/small_zeroed.elf"; echo "$WORK/bare_pie.elf"; } )

"$WORK/pack_stand" "${pairs[@]}" > "$WORK/stand.out" 2>&1
stand=$?
grep -v '^pack: [0-9]* passed' "$WORK/stand.out"
line=$(grep -E '^pack: [0-9]+ passed, [0-9]+ failed' "$WORK/stand.out" | tail -1)
[ -n "$line" ] || { cat "$WORK/stand.out"; harness "the stand ended with $stand and no count"; }
passed=$(echo "$line" | sed -E 's/^pack: ([0-9]+) passed.*/\1/')
failed=$(echo "$line" | sed -E 's/^pack: [0-9]+ passed, ([0-9]+) failed.*/\1/')
failed=$((failed + ${refused_ours:-0}))

refuse() {   # refuse LABEL FILE WORDS
    if "$WORK/pack" "$2" "$WORK/refused.box" 2> "$WORK/refuse.err"; then
        echo "FAIL pack took $1"
        failed=$((failed + 1))
    elif grep -qF "$3" "$WORK/refuse.err"; then
        passed=$((passed + 1))
    else
        echo "FAIL pack refused $1, but said: $(cat "$WORK/refuse.err")"
        failed=$((failed + 1))
    fi
}

# ── what pack must refuse ────────────────────────────────────────────────────
patch "$WORK/small_fixed.elf" "$WORK/wx.elf" wx || harness "could not break the small program (wx)"
refuse "writable and executable code" "$WORK/wx.elf" "writable and executable at once"
patch "$WORK/small_fixed.elf" "$WORK/twocode.elf" twocode || harness "could not break the small program (twocode)"
refuse "two code segments" "$WORK/twocode.elf" "two R+X segments"
patch "$WORK/small_fixed.elf" "$WORK/interp.elf" interp || harness "could not break the small program (interp)"
refuse "a program interpreter" "$WORK/interp.elf" "program interpreter"
patch "$WORK/small_rela.elf" "$WORK/reloc64.elf" reloc64 || harness "could not break the small program (reloc64)"
refuse "an R_X86_64_64 relocation" "$WORK/reloc64.elf" "other than R_X86_64_RELATIVE"
refuse "an object file" "$WORK/small.o" "not an executable or a static PIE"
refuse "an address in the thread template" "$WORK/tlsptr.elf" "a word of its thread template holds an address"
refuse "settings with a setting it cannot read" "$WORK/badsettings.elf" \
    "its settings, line 2: the setting look has no default"
refuse "a life with a word nobody knows" "$WORK/badlife.elf" \
    "its life, line 3: no word of how a program lives — task, stays, serves or rise"
refuse "a life of two ways" "$WORK/twolives.elf" "its life is not written as one"
refuse "a text file" "$LIB/user.ld" "not an ELF file"
patch src/userspace/utils/hw.elf "$WORK/badform.elf" badform || harness "could not break hw's Form (badform)"
refuse "a Form that is no text" "$WORK/badform.elf" "is not text as a box keeps its part form"

echo "pack: $passed passed, $failed failed — $(( ${#pairs[@]} / 2 )) programs packed"
[ "$failed" -eq 0 ]
