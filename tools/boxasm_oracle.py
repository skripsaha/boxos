#!/usr/bin/env python3
"""boxasm oracle — BoxASM held to NASM, byte for byte.

    boxasm_oracle.py corpus [nasm]       print the corpus: every supported form, in
                                         BoxASM's spelling or NASM's
    boxasm_oracle.py compare OURS NASM   compare two ELF objects of one source

The corpus is generated, not written by hand: every instruction with every
operand class it takes, registers 0 to 15 of every width, the address forms,
the immediates at the edges of every field, every condition code, near and far
jumps both ways, and the data directives. NASM 3 with `-f elf64` is the
witness: its choice of encoding is the one BoxASM is held to.

The comparison is of what the linker would make of each object, not of how
it is spelled: a relocation is reduced to its section, place, kind and
target section and offset (or outside name), so NASM's section-symbol
relocations and BoxASM's label relocations compare equal when they point at
the same byte, R_X86_64_PLT32 and PC32 compare equal, and a PC-relative
reference inside its own section is applied before the bytes are compared.
"""

import struct
import subprocess
import sys

REG64 = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
         "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"] + \
        ["r%dd" % n for n in range(8, 16)]
REG16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"] + \
        ["r%dw" % n for n in range(8, 16)]
REG8 = ["al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"] + \
       ["r%db" % n for n in range(8, 16)]
HIGH8 = ["ah", "ch", "dh", "bh"]
REGS = {1: REG8, 2: REG16, 4: REG32, 8: REG64}
SIZE_WORD = {1: "byte", 2: "word", 4: "dword", 8: "qword", 16: "oword"}

MEMORY = ["[rax]", "[rbx + 8]", "[rbp]", "[rsp]", "[r12]", "[r13]", "[r13 + 0x100]",
          "[rsp + 8]", "[rax + rcx*4]", "[rax + r9*8 + 16]", "[r12 + r13*2 - 128]",
          "[rcx*8 + 0x40]", "[rbp + rax]", "[rax + rsp]", "[8*rdx + rsi]", "[r15 - 0x80000000]",
          "[data_q]", "[data_q + 16]", "[abs data_q]", "[rel data_q]", "[rip + data_q]",
          "[abs 0x1000]"]

IMMEDIATES = {
    1: [0, 1, 127, -128, 255, -1, 0x7F],
    2: [0, 1, 127, 128, -128, -129, 0x7FFF, -0x8000, 0xFFFF],
    4: [0, 1, 127, 128, -128, -129, 0x7FFFFFFF, -0x80000000, 0xFFFFFFFF],
    8: [0, 1, 127, 128, -128, -129, 0x7FFFFFFF, -0x80000000],
}
MOV64 = [0, 1, 0xFFFFFFFF, -1, 0x80000000, -0x80000000, 0x100000000,
         0x7FFFFFFFFFFFFFFF, -0x8000000000000000, 0x123456789A]

ALU = ["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"]
UNARY = ["inc", "dec", "not", "neg", "mul", "imul", "div", "idiv"]
SHIFTS = ["rol", "ror", "rcl", "rcr", "shl", "sal", "shr", "sar"]
CONDITIONS = ["o", "no", "b", "c", "nae", "ae", "nb", "nc", "e", "z", "ne", "nz",
              "be", "na", "a", "nbe", "s", "ns", "p", "pe", "np", "po", "l", "nge",
              "ge", "nl", "le", "ng", "g", "nle"]
ALONE = ["ret", "nop", "int3", "leave", "cqo", "cdq", "cwd", "cdqe", "cwde", "cbw",
         "pushfq", "popfq", "cld", "std", "endbr64", "pause", "ud2", "clc", "stc", "cmc",
         "cpuid", "rdtsc", "rdtscp", "lfence", "mfence", "sfence"]
STRINGS = {"movs": "rep", "stos": "rep", "lods": "rep", "cmps": "repe", "scas": "repe"}
LOCKABLE = ALU[:7] + ["inc", "dec", "not", "neg"]
BIT_TESTS = ["bt", "bts", "btr", "btc"]
SCANS = ["bsf", "bsr", "popcnt", "lzcnt", "tzcnt"]


def register_pairs(size):
    """Every register against a partner that differs, both halves of the
    REX range crossed, so REX.R and REX.B are each exercised alone."""
    regs = REGS[size]
    return [(regs[i], regs[(i * 5 + 3) % 16]) for i in range(16)]


def alu_lines():
    out = []
    for op in ALU:
        for size in (1, 2, 4, 8):
            for a, b in register_pairs(size):
                out.append("%s %s, %s" % (op, a, b))
            for mem in MEMORY[:12]:
                out.append("%s %s, %s" % (op, REGS[size][3], mem))
                out.append("%s %s, %s" % (op, mem, REGS[size][11]))
            for imm in IMMEDIATES[size]:
                out.append("%s %s, %d" % (op, REGS[size][0], imm))
                out.append("%s %s, %d" % (op, REGS[size][6], imm))
                out.append("%s %s, %d" % (op, REGS[size][13], imm))
                out.append("%s %s [rbx + 4], %d" % (op, SIZE_WORD[size], imm))
        out.append("%s ah, bh" % op)
        out.append("%s ch, 5" % op)
    return out


def test_lines():
    out = []
    for size in (1, 2, 4, 8):
        for a, b in register_pairs(size):
            out.append("test %s, %s" % (a, b))
        out.append("test %s, [rbx]" % REGS[size][0])
        out.append("test [rcx + 8], %s" % REGS[size][9])
        for imm in IMMEDIATES[size]:
            out.append("test %s, %d" % (REGS[size][0], imm))
            out.append("test %s, %d" % (REGS[size][10], imm))
            out.append("test %s [rax], %d" % (SIZE_WORD[size], imm))
    return out


def mov_lines():
    out = []
    for size in (1, 2, 4, 8):
        for a, b in register_pairs(size):
            out.append("mov %s, %s" % (a, b))
        for mem in MEMORY:
            out.append("mov %s, %s" % (REGS[size][2], mem))
            out.append("mov %s, %s" % (mem, REGS[size][12]))
        for imm in IMMEDIATES[size]:
            for reg in (REGS[size][0], REGS[size][7], REGS[size][14]):
                out.append("mov %s, %d" % (reg, imm))
            out.append("mov %s [r13], %d" % (SIZE_WORD[size], imm))
    for imm in MOV64:
        out.append("mov rax, %d" % imm)
        out.append("mov r11, %d" % imm)
    out += ["mov rax, data_q", "mov r9, data_q + 8", "mov eax, data_q", "mov r10d, data_q",
            "mov qword [rax], data_q", "mov ah, 5", "mov bh, ch", "mov sil, 1",
            "mov rax, code_target", "mov dword [rbx], code_target"]
    return out


def address_lines():
    out = []
    for size in (2, 4, 8):
        for mem in MEMORY:
            out.append("lea %s, %s" % (REGS[size][5], mem))
            out.append("lea %s, %s" % (REGS[size][9], mem))
    out.append("lea rax, [code_target]")
    return out


def xchg_lines():
    out = ["xchg eax, eax", "xchg ax, cx", "xchg ecx, eax", "xchg rax, rax"]
    for size in (1, 2, 4, 8):
        for a, b in register_pairs(size):
            out.append("xchg %s, %s" % (a, b))
        out.append("xchg %s, [rbx]" % REGS[size][4])
        out.append("xchg [rbx + 16], %s" % REGS[size][12])
    for reg in REG64:
        out.append("xchg rax, %s" % reg)
        out.append("xchg %s, rax" % reg)
    return out


def stack_lines():
    out = []
    for reg in REG64:
        out.append("push %s" % reg)
        out.append("pop %s" % reg)
    for mem in MEMORY[:12]:
        out.append("push qword %s" % mem)
        out.append("pop qword %s" % mem)
    for imm in IMMEDIATES[8]:
        out.append("push %d" % imm)
    out += ["push data_q", "push code_target"]
    return out


def unary_lines():
    out = []
    for op in UNARY:
        for size in (1, 2, 4, 8):
            for reg in REGS[size]:
                out.append("%s %s" % (op, reg))
            out.append("%s %s [rbx]" % (op, SIZE_WORD[size]))
            out.append("%s %s [r12 + 32]" % (op, SIZE_WORD[size]))
        out.append("%s ah" % op)
    return out


def imul_lines():
    out = []
    for size in (2, 4, 8):
        for a, b in register_pairs(size):
            out.append("imul %s, %s" % (a, b))
            out.append("imul %s, %s, 10" % (a, b))
            out.append("imul %s, %s, 1000" % (a, b))
            out.append("imul %s, 127" % a)
            out.append("imul %s, -1000" % a)
        out.append("imul %s, [rbx + 8]" % REGS[size][1])
        out.append("imul %s, [rbx], 3" % REGS[size][13])
    return out


def shift_lines():
    out = []
    for op in SHIFTS:
        for size in (1, 2, 4, 8):
            for reg in (REGS[size][0], REGS[size][5], REGS[size][12]):
                out += ["%s %s, 1" % (op, reg), "%s %s, cl" % (op, reg),
                        "%s %s, 4" % (op, reg), "%s %s, 255" % (op, reg)]
            word = SIZE_WORD[size]
            out += ["%s %s [rax], 1" % (op, word), "%s %s [rax], cl" % (op, word),
                    "%s %s [rax + 8], 7" % (op, word)]
    return out


def extend_lines():
    out = []
    for op in ("movzx", "movsx"):
        for size in (2, 4, 8):
            for i in range(16):
                out.append("%s %s, %s" % (op, REGS[size][i], REG8[(i + 7) % 16]))
            out.append("%s %s, byte [rbx + 1]" % (op, REGS[size][3]))
        for size in (4, 8):
            for i in range(16):
                out.append("%s %s, %s" % (op, REGS[size][i], REG16[(i + 9) % 16]))
            out.append("%s %s, word [rsp]" % (op, REGS[size][10]))
        out.append("%s eax, ah" % op)
    for i in range(16):
        out.append("movsxd %s, %s" % (REG64[i], REG32[(i + 3) % 16]))
    out.append("movsxd rax, dword [rbx]")
    return out


def condition_lines():
    out = []
    for cc in CONDITIONS:
        out += ["set%s al" % cc, "set%s r9b" % cc, "set%s sil" % cc,
                "set%s byte [rax]" % cc, "cmov%s rax, rbx" % cc,
                "cmov%s r12d, [rdx]" % cc, "cmov%s ax, bx" % cc]
    return out


def branch_lines():
    """Jumps both ways, near and far, and every condition."""
    out = ["near_back:", "nop"]
    for cc in CONDITIONS:
        out += ["j%s near_back" % cc, "j%s near_ahead" % cc]
    out += ["jmp near_back", "jmp near_ahead", "call near_back", "near_ahead:",
            "times 4 nop", "jmp short near_ahead", "jmp near near_ahead",
            "far_back:", "times 300 nop"]
    for cc in CONDITIONS:
        out += ["j%s far_back" % cc, "j%s far_ahead" % cc]
    out += ["jmp far_back", "jmp far_ahead", "call far_ahead", "call outside_fn",
            "jmp outside_fn", "jz outside_fn", "times 300 nop", "far_ahead:",
            "jmp rax", "jmp r12", "jmp qword [rbx]", "jmp qword [data_q]",
            "call rax", "call r13", "call qword [rsp + 8]", "call qword [rel data_q]",
            ".local_top:", "dec ecx", "jnz .local_top", "jmp .local_end", ".local_end:"]
    return out


def string_lines():
    """Every string instruction at every width, bare and repeated."""
    out = []
    for op, prefixes in STRINGS.items():
        for width in "bwdq":
            out.append("%s%s" % (op, width))
            if prefixes == "rep":
                out.append("rep %s%s" % (op, width))
            else:
                out += ["repe %s%s" % (op, width), "repz %s%s" % (op, width),
                        "repne %s%s" % (op, width), "repnz %s%s" % (op, width)]
    return out


def bit_lines():
    """bt*, the scans, bswap and the double shifts: every register of every
    width they take, memory, and the bit numbers at the edges of an imm8."""
    out = []
    for size in (2, 4, 8):
        word = SIZE_WORD[size]
        for a, b in register_pairs(size):
            for op in BIT_TESTS:
                out.append("%s %s, %s" % (op, a, b))
            for op in SCANS:
                out.append("%s %s, %s" % (op, a, b))
            out += ["shld %s, %s, 5" % (a, b), "shrd %s, %s, cl" % (a, b)]
        for op in BIT_TESTS:
            out += ["%s %s, 0" % (op, REGS[size][3]), "%s %s, 255" % (op, REGS[size][11]),
                    "%s %s [rbx + 4], 7" % (op, word),
                    "%s [rax + rcx*2], %s" % (op, REGS[size][9])]
        for op in SCANS:
            out.append("%s %s, %s [rsp + 16]" % (op, REGS[size][14], word))
        out += ["shld %s [rdi], %s, 1" % (word, REGS[size][2]),
                "shrd %s [r13], %s, cl" % (word, REGS[size][10])]
        for reg in REGS[size]:
            out += ["rdrand %s" % reg, "rdseed %s" % reg]
    for size in (4, 8):
        out += ["bswap %s" % reg for reg in REGS[size]]
    return out


def atomic_lines():
    """xadd and cmpxchg plain and locked, every lockable instruction locked on
    memory, and the double-width compare-exchanges."""
    out = []
    for size in (1, 2, 4, 8):
        word = SIZE_WORD[size]
        for a, b in register_pairs(size)[:6]:
            out += ["xadd %s, %s" % (a, b), "cmpxchg %s, %s" % (a, b)]
        out += ["lock xchg [rdx], %s" % REGS[size][7], "lock xchg %s, [r9 + 4]" % REGS[size][12],
                "lock xadd [rbx], %s" % REGS[size][4], "lock cmpxchg [r12 + 8], %s" % REGS[size][13],
                "xadd %s [rsp], %s" % (word, REGS[size][0])]
        for op in LOCKABLE[:7]:
            out += ["lock %s %s [rdi + 8], %s" % (op, word, REGS[size][6]),
                    "lock %s %s [rax], 1" % (op, word)]
        for op in LOCKABLE[7:]:
            out.append("lock %s %s [rcx + rdx*8]" % (op, word))
        if size > 1:
            for op in BIT_TESTS[1:]:
                out += ["lock %s %s [rsi], %s" % (op, word, REGS[size][15]),
                        "lock %s %s [rsi], 9" % (op, word)]
    for mem in MEMORY[:12]:
        out += ["cmpxchg8b %s" % mem, "lock cmpxchg8b qword %s" % mem,
                "cmpxchg16b %s" % mem, "lock cmpxchg16b oword %s" % mem]
    return out


def short_branch_lines():
    """loop and jrcxz, which have only a rel8, both ways."""
    out = ["loop_top:", "nop"]
    for op in ("loop", "loope", "loopz", "loopne", "loopnz", "jrcxz", "jecxz"):
        out += ["%s loop_top" % op, "%s loop_end" % op]
    out += ["times 20 nop", "loop_end:"]
    return out


def program(nasm):
    """The corpus in BoxASM's spelling, or NASM's where the two differ: NASM
    writes [rip + x] as [rel x], keeps a jump near only as `strict near`,
    and pads alignment in data with zeros only when told `db 0`."""
    rip = "[rel " if nasm else "[rip + "
    near = " strict near " if nasm else " near "
    data_align = "    align 8, db 0" if nasm else "    align 8"
    lines = ["default rel", "bits 64", "extern outside_fn", "global main",
             "section .text", "main:"]
    for part in (alu_lines, test_lines, mov_lines, address_lines, xchg_lines,
                 stack_lines, unary_lines, imul_lines, shift_lines, extend_lines,
                 condition_lines, branch_lines, string_lines, bit_lines, atomic_lines,
                 short_branch_lines):
        lines += ["    " + line if not line.endswith(":") else line for line in part()]
    lines += ["    " + op for op in ALONE]
    lines += ["    ret 16", "    align 16", "code_target:", "    endbr64", "    ret",
              "    mov eax, message_length", "    mov ecx, (1 << 4) | 3",
              "    mov edx, 100 / 7 + 100 % 7", "    mov esi, ~0 & 0xFF",
              "    mov rdi, words_end - words", "    align 8", "    db 0xCC",
              "section .rodata", "message db \"Hello, BoxOS\", 10, 0",
              "message_length equ $ - message", "words:", "    dw 1, -1, 0xFFFF, \"abc\"",
              "    dd 0, -1, 0xFFFFFFFF, \"wxyz!\"", "    dq -1, 0x123456789ABCDEF0",
              "    times 5 db 7", data_align, "words_end:",
              "section .data", "data_q:", "    dq data_q, data_q + 8, code_target, message",
              "    dd data_q", "    times 3 dq 0x11", "    db 'A', 'BC', \"D\"",
              "section .bss", "    resb 3", "    resw 2", "    resd 5", "    resq 4"]
    text = "\n".join(lines) + "\n"
    return text.replace("[rip + ", rip).replace(" near ", near)


# ---- Comparison --------------------------------------------------------------

R_64, R_PC32, R_PLT32, R_32, R_32S = 1, 2, 4, 10, 11


class Object:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        shoff, = struct.unpack_from("<Q", d, 0x28)
        shnum, shstrndx = struct.unpack_from("<HH", d, 0x3C)
        self.sections = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * 64)
                         for i in range(shnum)]
        names_at = self.sections[shstrndx][4]
        self.names = [self.string(names_at, s[0]) for s in self.sections]
        self.symbols = []
        for s in self.sections:
            if s[1] == 2:
                strtab = self.sections[s[6]][4]
                for at in range(s[4], s[4] + s[5], 24):
                    name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", d, at)
                    self.symbols.append((self.string(strtab, name), shndx, value))

    def string(self, table, offset):
        end = self.data.index(b"\0", table + offset)
        return self.data[table + offset:end].decode()

    def section(self, name):
        if name not in self.names:
            return None
        return self.names.index(name)

    def normalized(self, name):
        """(size, bytes, relocations) of a section as the linker sees it."""
        index = self.section(name)
        if index is None:
            return (0, b"", [])
        header = self.sections[index]
        size = header[5]
        content = bytearray(b"" if header[1] == 8 else
                            self.data[header[4]:header[4] + size])
        relocations = []
        for rela_index, rela in enumerate(self.sections):
            if rela[1] != 4 or rela[7] != index:
                continue
            for at in range(rela[4], rela[4] + rela[5], 24):
                place, info, addend = struct.unpack_from("<QQq", self.data, at)
                kind = info & 0xFFFFFFFF
                symbol, shndx, value = self.symbols[info >> 32]
                kind = R_PC32 if kind == R_PLT32 else kind
                if shndx == 0:
                    target = ("outside", symbol, addend)
                else:
                    target = ("section", self.names[shndx], value + addend)
                if kind == R_PC32 and target[0] == "section" and target[1] == name:
                    struct.pack_into("<i", content, place, target[2] - place)
                    continue
                relocations.append((place, kind, target))
        return (size, bytes(content), sorted(relocations))


def show(path, start):
    subprocess.call(["x86_64-elf-objdump", "-d", "-M", "intel", "--no-show-raw-insn",
                     "--start-address=%d" % max(0, start - 16),
                     "--stop-address=%d" % (start + 24), path],
                    stdout=sys.stderr)


def compare(ours, theirs):
    a, b = Object(ours), Object(theirs)
    same = True
    for name in (".text", ".rodata", ".data", ".bss"):
        size_a, bytes_a, rel_a = a.normalized(name)
        size_b, bytes_b, rel_b = b.normalized(name)
        if size_a != size_b:
            print("%s: %d bytes from BoxASM, %d from NASM" % (name, size_a, size_b))
            same = False
        if bytes_a != bytes_b:
            at = next(i for i in range(min(len(bytes_a), len(bytes_b)) + 1)
                      if i >= len(bytes_a) or i >= len(bytes_b) or bytes_a[i] != bytes_b[i])
            print("%s: the bytes differ first at 0x%x" % (name, at))
            if name == ".text":
                print("BoxASM:", file=sys.stderr)
                show(ours, at)
                print("NASM:", file=sys.stderr)
                show(theirs, at)
            same = False
        if rel_a != rel_b:
            extra = [r for r in rel_a if r not in rel_b][:3]
            missing = [r for r in rel_b if r not in rel_a][:3]
            print("%s: relocations differ; BoxASM only %s; NASM only %s" % (name, extra, missing))
            same = False
    return same


def main(argv):
    if len(argv) in (2, 3) and argv[1] == "corpus" and argv[2:] in ([], ["nasm"]):
        sys.stdout.write(program(argv[2:] == ["nasm"]))
        return 0
    if len(argv) == 4 and argv[1] == "compare":
        return 0 if compare(argv[2], argv[3]) else 1
    print(__doc__.strip().splitlines()[2:5], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
