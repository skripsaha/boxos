#!/usr/bin/env python3
"""boxasm oracle — BoxASM held to NASM, byte for byte.

    boxasm_oracle.py corpus [nasm]       print the corpus: every supported form, in
                                         BoxASM's spelling or NASM's
    boxasm_oracle.py compare OURS NASM   compare two ELF objects of one source
    boxasm_oracle.py macros [nasm]       print the program written with every preprocessor
                                         feature both assemblers share, in BoxASM's
                                         spelling or NASM's
    boxasm_oracle.py floats ASM BYTES    write a program of floating-point data, ties
                                         included, and the bytes of its .data rounded
                                         exactly — the witness where NASM 3 is wrong

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

import random
import struct
import subprocess
import sys
from fractions import Fraction

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
SIZE_WORD = {1: "byte", 2: "word", 4: "dword", 8: "qword", 16: "oword", 32: "yword"}

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
         "cpuid", "rdtsc", "rdtscp", "lfence", "mfence", "sfence", "lahf", "sahf", "xgetbv"]
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


XMM = ["xmm%d" % n for n in range(16)]
SSE_MEMORY = ["[rax]", "[rsp + 8]", "[r13]", "[rbx + rcx*4 + 16]", "[sse_data]"]
FLOAT_OPS = ["add", "sub", "mul", "div", "min", "max", "sqrt"]
FLOAT_TYPES = {"ps": "oword", "pd": "oword", "ss": "dword", "sd": "qword"}
PACKED_OPS = ["and", "andn", "or", "xor", "unpckl", "unpckh"]
PREDICATES = ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"]
SSE_INTEGER = ["paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq",
               "paddsb", "paddsw", "paddusb", "paddusw", "psubsb", "psubsw", "psubusb",
               "psubusw", "pmullw", "pmulhw", "pmulhuw", "pmuludq", "pmaddwd", "pavgb", "pavgw",
               "pmaxsw", "pmaxub", "pminsw", "pminub", "psadbw", "pand", "pandn", "por", "pxor",
               "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd", "packsswb",
               "packssdw", "packuswb", "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
               "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq"]
SSE3_OPERATIONS = [("addsubps", "oword"), ("addsubpd", "oword"), ("haddps", "oword"),
                   ("haddpd", "oword"), ("hsubps", "oword"), ("hsubpd", "oword"),
                   ("movddup", "qword"), ("movshdup", "oword"), ("movsldup", "oword")]
SSSE3_OPERATIONS = ["pshufb", "phaddw", "phaddd", "phaddsw", "pmaddubsw", "phsubw", "phsubd",
                    "phsubsw", "psignb", "psignw", "psignd", "pmulhrsw", "pabsb", "pabsw", "pabsd"]
SSE41_OPERATIONS = ["ptest", "pmuldq", "pcmpeqq", "packusdw", "pminsb", "pminsd", "pminuw",
                    "pminud", "pmaxsb", "pmaxsd", "pmaxuw", "pmaxud", "pmulld", "phminposuw",
                    "pcmpgtq"]
SSE_IMMEDIATE_OPERATIONS = [("palignr", "oword"), ("blendps", "oword"), ("blendpd", "oword"),
                            ("pblendw", "oword"), ("dpps", "oword"), ("dppd", "oword"),
                            ("mpsadbw", "oword"), ("pcmpestrm", "oword"), ("pcmpestri", "oword"),
                            ("pcmpistrm", "oword"), ("pcmpistri", "oword"), ("roundps", "oword"),
                            ("roundpd", "oword"), ("roundss", "dword"), ("roundsd", "qword"),
                            ("insertps", "dword")]
SSE_WIDENING = [("pmovsxbw", "qword"), ("pmovsxbd", "dword"), ("pmovsxbq", "word"),
                ("pmovsxwd", "qword"), ("pmovsxwq", "dword"), ("pmovsxdq", "qword"),
                ("pmovzxbw", "qword"), ("pmovzxbd", "dword"), ("pmovzxbq", "word"),
                ("pmovzxwd", "qword"), ("pmovzxwq", "dword"), ("pmovzxdq", "qword")]
SSE_SHIFTS = ["psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"]


def xmm_pairs():
    return [(XMM[i], XMM[(i * 5 + 3) % 16]) for i in range(16)]


def sse_operation(op, size_word):
    """An xmm operation on every register pair and on memory, sized and not."""
    out = ["%s %s, %s" % (op, a, b) for a, b in xmm_pairs()]
    out += ["%s %s, %s" % (op, XMM[(i * 3) % 16], mem) for i, mem in enumerate(SSE_MEMORY)]
    out.append("%s xmm11, %s [rdx + 32]" % (op, size_word))
    return out


def sse_lines():
    """Every SSE and SSE2 instruction BoxASM has, in each of its forms."""
    out = []
    for op in FLOAT_OPS:
        for kind, word in FLOAT_TYPES.items():
            out += sse_operation(op + kind, word)
    for op in PACKED_OPS:
        for kind in ("ps", "pd"):
            out += sse_operation(op + kind, "oword")
    for op, word in (("rcpps", "oword"), ("rcpss", "dword"), ("rsqrtps", "oword"),
                     ("rsqrtss", "dword"), ("comiss", "dword"), ("comisd", "qword"),
                     ("ucomiss", "dword"), ("ucomisd", "qword"), ("cvtss2sd", "dword"),
                     ("cvtsd2ss", "qword"), ("cvtps2pd", "qword"), ("cvtpd2ps", "oword"),
                     ("cvtdq2ps", "oword"), ("cvtps2dq", "oword"), ("cvttps2dq", "oword"),
                     ("cvtdq2pd", "qword"), ("cvtpd2dq", "oword"), ("cvttpd2dq", "oword")):
        out += sse_operation(op, word)
    for op in SSE_INTEGER:
        out += sse_operation(op, "oword")
    for kind, word in FLOAT_TYPES.items():
        for predicate in PREDICATES:
            out += ["cmp%s%s xmm2, xmm13" % (predicate, kind),
                    "cmp%s%s xmm9, %s [rax]" % (predicate, kind, word)]
        for imm in range(8):
            out.append("cmp%s xmm%d, xmm%d, %d" % (kind, imm, 15 - imm, imm))
        out.append("cmp%s xmm4, [rsp + 8], 6" % kind)
    for op in ("shufps", "shufpd", "pshufd", "pshufhw", "pshuflw"):
        out += ["%s %s, %s, 0x%02X" % (op, a, b, (i * 37) & 0xFF)
                for i, (a, b) in enumerate(xmm_pairs()[:6])]
        out.append("%s xmm10, [sse_data], 0x1B" % op)
    for op in ("movaps", "movapd", "movups", "movupd", "movdqa", "movdqu"):
        out += ["%s %s, %s" % (op, a, b) for a, b in xmm_pairs()[:8]]
        for i, mem in enumerate(SSE_MEMORY):
            out += ["%s %s, %s" % (op, XMM[(i * 7) % 16], mem),
                    "%s %s, %s" % (op, mem, XMM[(i * 11) % 16])]
    out += ["movups xmm1, [rax + 1]", "movdqu [rbx + 3], xmm12"]
    for op, word in (("movss", "dword"), ("movsd", "qword")):
        out += ["%s %s, %s" % (op, a, b) for a, b in xmm_pairs()[:8]]
        out += ["%s xmm5, %s [rcx]" % (op, word), "%s %s [rcx + 4], xmm14" % (op, word),
                "%s xmm8, [sse_data]" % op, "%s [sse_data], xmm0" % op]
    for op in ("movlps", "movhps", "movlpd", "movhpd"):
        out += ["%s xmm3, [rdi]" % op, "%s xmm12, qword [r9 + 8]" % op, "%s [rsi], xmm7" % op,
                "%s [sse_data], xmm15" % op]
    out += ["movhlps %s, %s" % pair for pair in xmm_pairs()[:6]]
    out += ["movlhps %s, %s" % pair for pair in xmm_pairs()[:6]]
    for i in range(16):
        out += ["movd %s, %s" % (XMM[i], REG32[(i + 5) % 16]),
                "movd %s, %s" % (REG32[(i + 9) % 16], XMM[i]),
                "movq %s, %s" % (XMM[i], REG64[(i + 5) % 16]),
                "movq %s, %s" % (REG64[(i + 9) % 16], XMM[i])]
    out += ["movd xmm3, [rax]", "movd xmm11, dword [rsp + 4]", "movd [rbx], xmm6",
            "movq xmm1, xmm9", "movq xmm13, [rdx]", "movq xmm2, qword [r12]",
            "movq [rax], xmm10", "movq qword [rsp], xmm4"]
    for i, reg in enumerate(("eax", "r9d", "rax", "r12")):
        out += ["movmskps %s, %s" % (reg, XMM[i * 4]), "movmskpd %s, %s" % (reg, XMM[i * 4 + 1])]
    out += ["pmovmskb eax, xmm3", "pmovmskb r10d, xmm14"]
    out += ["movntps [rax], xmm1", "movntpd [r8], xmm9", "movntdq [sse_data], xmm2",
            "movnti [rdi], eax", "movnti [rdi + 8], r11", "maskmovdqu xmm1, xmm2",
            "maskmovdqu xmm10, xmm15"]
    for op, kind in (("cvtsi2ss", "dword"), ("cvtsi2sd", "dword")):
        for i, reg in enumerate(("eax", "r10d", "rax", "r15")):
            out.append("%s %s, %s" % (op, XMM[i * 5 % 16], reg))
        out += ["%s xmm6, dword [rax]" % op, "%s xmm14, qword [rbx + 8]" % op]
    for op, word in (("cvtss2si", "dword"), ("cvttss2si", "dword"),
                     ("cvtsd2si", "qword"), ("cvttsd2si", "qword")):
        for i, reg in enumerate(("eax", "r11d", "rax", "r14")):
            out.append("%s %s, %s" % (op, reg, XMM[i * 6 % 16]))
        out += ["%s ecx, %s [rax]" % (op, word), "%s r8, [rsp + 8]" % op]
    for op in SSE_SHIFTS:
        out += sse_operation(op, "oword")[:4]
        out += ["%s xmm3, 1" % op, "%s xmm12, 15" % op, "%s xmm0, [rax]" % op]
    out += ["pslldq xmm1, 4", "pslldq xmm14, 15", "psrldq xmm2, 8", "psrldq xmm9, 1"]
    for reg in ("eax", "r9d", "rax", "r13"):
        out += ["pextrw %s, xmm5, 3" % reg, "pinsrw xmm11, %s, 7" % reg]
    out += ["pinsrw xmm2, [rax], 1", "pinsrw xmm2, word [rsp + 2], 0"]
    out += ["ldmxcsr [rsp]", "stmxcsr [rsp + 4]", "ldmxcsr dword [sse_data]",
            "prefetchnta [rax]", "prefetcht0 [rsi + 64]", "prefetcht1 [r12]",
            "prefetcht2 byte [sse_data]", "clflush [rdi]"]
    return out


def sse_immediate(op, size_word):
    """An xmm operation with an imm8, on register pairs and on memory, sized and not."""
    out = ["%s %s, %s, 0x%02X" % (op, a, b, (i * 37 + 5) & 0xFF)
           for i, (a, b) in enumerate(xmm_pairs())]
    out += ["%s %s, %s, %d" % (op, XMM[(i * 3) % 16], mem, (i * 71) & 0xFF)
            for i, mem in enumerate(SSE_MEMORY)]
    out += ["%s xmm11, %s [rdx + 32], 255" % (op, size_word), "%s xmm0, xmm15, 0" % op]
    return out


def crc32_lines(nasm):
    """crc32 of every width; a 64-bit destination over a byte as NASM writes it,
    without REX.W."""
    out = []
    for i in range(16):
        out += ["crc32 %s, %s" % (REG32[i], REG8[(i + 7) % 16]),
                "crc32 %s, %s" % (REG32[(i + 5) % 16], REG16[(i + 3) % 16]),
                "crc32 %s, %s" % (REG32[(i + 9) % 16], REG32[i]),
                "crc32 %s, %s" % (REG64[i], REG64[(i * 5 + 3) % 16]),
                "crc32 %s, %s" % (REG64[i], REG8[(i + 7) % 16])]
    for mem in MEMORY[:12]:
        out += ["crc32 eax, byte %s" % mem, "crc32 r9d, word %s" % mem,
                "crc32 ebx, dword %s" % mem, "crc32 rdx, qword %s" % mem,
                "crc32 r11, byte %s" % mem]
    out += ["crc32 eax, ah", "crc32 r10d, sil", "crc32 ecx, bh"]
    return out


def sse_extension_lines(nasm):
    """SSE3, SSSE3, SSE4.1 and SSE4.2, each instruction in each of its forms."""
    out = []
    for op, word in SSE3_OPERATIONS:
        out += sse_operation(op, word)
    out += ["lddqu %s, %s" % (XMM[(i * 5) % 16], mem) for i, mem in enumerate(SSE_MEMORY)]
    out += ["lddqu xmm3, [rax + 1]", "lddqu xmm12, oword [rdx + 33]", "monitor", "mwait"]
    for op in SSSE3_OPERATIONS + SSE41_OPERATIONS:
        out += sse_operation(op, "oword")
    for op in ("blendvps", "blendvpd", "pblendvb"):
        out += ["%s %s, %s" % (op, a, b) for a, b in xmm_pairs()]
        out += ["%s %s, %s, xmm0" % (op, a, b) for a, b in xmm_pairs()]
        out += ["%s xmm%d, %s" % (op, 2 + i, mem) for i, mem in enumerate(SSE_MEMORY)]
        out += ["%s xmm%d, %s, xmm0" % (op, 9 + i, mem) for i, mem in enumerate(SSE_MEMORY)]
        out += ["%s xmm0, xmm0, xmm0" % op, "%s xmm5, oword [rdx + 32], xmm0" % op]
    out += ["movntdqa %s, %s" % (XMM[(i * 3) % 16], mem) for i, mem in enumerate(SSE_MEMORY)]
    out.append("movntdqa xmm14, oword [rdx + 16]")
    for op, word in SSE_WIDENING:
        out += sse_operation(op, word)
    for op, word in SSE_IMMEDIATE_OPERATIONS:
        out += sse_immediate(op, word)
    for i in range(16):
        out += ["pextrb %s, %s, %d" % (REG32[i], XMM[(i * 3) % 16], i),
                "pextrb %s, %s, %d" % (REG64[(i + 6) % 16], XMM[(i * 7) % 16], 255 - i),
                "extractps %s, %s, %d" % (REG32[(i + 2) % 16], XMM[(i * 5) % 16], i % 4),
                "extractps %s, %s, %d" % (REG64[(i + 4) % 16], XMM[(i * 9) % 16], 3 - i % 4),
                "pextrd %s, %s, %d" % (REG32[(i + 8) % 16], XMM[(i * 11) % 16], i % 4),
                "pextrq %s, %s, %d" % (REG64[(i + 1) % 16], XMM[(i * 13) % 16], i % 2),
                "pinsrb %s, %s, %d" % (XMM[(i * 3) % 16], REG32[(i + 5) % 16], 15 - i),
                "pinsrd %s, %s, %d" % (XMM[(i * 5) % 16], REG32[(i + 3) % 16], i % 4),
                "pinsrq %s, %s, %d" % (XMM[(i * 7) % 16], REG64[(i + 7) % 16], i % 2)]
    for i, mem in enumerate(SSE_MEMORY):
        out += ["pextrb %s, %s, %d" % (mem, XMM[(i * 4) % 16], i),
                "pextrb byte %s, %s, %d" % (mem, XMM[(i * 4 + 1) % 16], 15 - i),
                "pextrw %s, %s, %d" % (mem, XMM[(i * 4 + 2) % 16], i),
                "pextrw word %s, %s, %d" % (mem, XMM[(i * 4 + 3) % 16], 7 - i),
                "extractps %s, %s, %d" % (mem, XMM[(i * 3) % 16], i % 4),
                "extractps dword %s, %s, %d" % (mem, XMM[(i * 3 + 1) % 16], 3 - i % 4),
                "pextrd %s, %s, %d" % (mem, XMM[(i * 5) % 16], i % 4),
                "pextrd dword %s, %s, 1" % (mem, XMM[(i * 5 + 1) % 16]),
                "pextrq %s, %s, %d" % (mem, XMM[(i * 7) % 16], i % 2),
                "pextrq qword %s, %s, 0" % (mem, XMM[(i * 7 + 1) % 16]),
                "pinsrb %s, %s, %d" % (XMM[(i * 3) % 16], mem, i),
                "pinsrb %s, byte %s, 15" % (XMM[(i * 3 + 1) % 16], mem),
                "pinsrd %s, %s, %d" % (XMM[(i * 5) % 16], mem, i % 4),
                "pinsrd %s, dword %s, 2" % (XMM[(i * 5 + 1) % 16], mem),
                "pinsrq %s, %s, %d" % (XMM[(i * 7) % 16], mem, i % 2),
                "pinsrq %s, qword %s, 1" % (XMM[(i * 7 + 1) % 16], mem)]
    out += crc32_lines(nasm)
    return out


YMM = ["ymm%d" % n for n in range(16)]
AVX_MEMORY = ["[rax]", "[rsp + 8]", "[r13]", "[rbx + rcx*4 + 16]", "[r9 + r10*8 - 128]",
              "[r12 + 0x100]", "[sse_data]"]
AVX_ALIGNED = ["[rax]", "[rsp + 8]", "[r13]", "[rbx + rcx*4 + 16]", "[r9 + r10*8]",
               "[avx_data]", "[avx_data + 64]"]
VEX_PREDICATES = [
    "eq", "lt", "lt_os", "le", "le_os", "unord", "unord_q", "neq", "neq_uq", "nlt", "nlt_us",
    "nle", "nle_us", "ord", "ord_q", "eq_uq", "nge", "nge_us", "ngt", "ngt_us", "false",
    "false_oq", "neq_oq", "ge", "ge_os", "gt", "gt_os", "true", "true_uq", "eq_os", "lt_oq",
    "le_oq", "unord_s", "neq_us", "nlt_uq", "nle_uq", "ord_s", "eq_us", "nge_uq", "ngt_uq",
    "false_os", "neq_os", "ge_oq", "gt_oq", "true_us"]
# Registers that put VEX.R, VEX.vvvv and ModRM.rm each alone in the high half, and all three.
VEX_PICKS = [(0, 0, 0), (9, 1, 2), (1, 10, 2), (3, 4, 12), (11, 12, 13), (7, 15, 6), (15, 14, 15)]


def vex_picks(count=16):
    """Register numbers for a destination, a first source and a second: the
    edges VEX.R, vvvv and B sit on, then every register in each place."""
    return VEX_PICKS + [(i, (i * 5 + 3) % 16, (i * 7 + 1) % 16) for i in range(count)]


def vector_triple(op, dest, first, second, word, imm=False, duplicate=True):
    """op dest, first, second in registers, with memory sized and not, and —
    unless the instruction takes no such form — the second operand left out,
    which NASM reads as the destination again. Left unsized without a word:
    NASM 3 takes a qword for the memory of vcmpss and its kin, and refuses the
    dword the manual gives."""
    out = []
    for n, (a, b, c) in enumerate(vex_picks()):
        out.append("%s %s, %s, %s" % (op, dest[a], first[b], second[c]) +
                   (", %d" % ((n * 37 + 5) & 255) if imm else ""))
    for n, mem in enumerate(AVX_MEMORY):
        out.append("%s %s, %s, %s" % (op, dest[(n * 3) % 16], first[(n * 5 + 2) % 16], mem) +
                   (", %d" % ((n * 71) & 255) if imm else ""))
    sized = word + " " if word else ""
    out.append("%s %s, %s, %s[rdx + 32]" % (op, dest[11], first[3], sized) +
               (", 255" if imm else ""))
    if not duplicate:
        return out
    tail = ", 0x1B" if imm else ""
    out += ["%s %s, %s%s" % (op, dest[1], second[2], tail),
            "%s %s, %s%s" % (op, dest[10], second[12], tail),
            "%s %s, %s%s" % (op, dest[9], AVX_MEMORY[(len(op)) % len(AVX_MEMORY)], tail),
            "%s %s, %s[r8 + 16]%s" % (op, dest[4], sized, tail)]
    return out


def vector_pair(op, dest, source, word, imm=False):
    """op dest, source in registers and with memory sized and not."""
    out = []
    for n, (a, b, _) in enumerate(vex_picks()):
        out.append("%s %s, %s" % (op, dest[a], source[b]) +
                   (", %d" % ((n * 37 + 5) & 255) if imm else ""))
    for n, mem in enumerate(AVX_MEMORY):
        out.append("%s %s, %s" % (op, dest[(n * 3) % 16], mem) +
                   (", %d" % ((n * 71) & 255) if imm else ""))
    out.append("%s %s, %s [rdx + 32]" % (op, dest[11], word) + (", 255" if imm else ""))
    return out


def vector_store(op, source, word, memories=AVX_MEMORY):
    """op memory, source: the store form, sized and not."""
    out = ["%s %s, %s" % (op, mem, source[(n * 5 + 1) % 16]) for n, mem in enumerate(memories)]
    out.append("%s %s [r11 + 8], %s" % (op, word, source[14]))
    return out


def both_widths(op, xmm_word="oword", imm=False, pair=False, duplicate=True):
    """An operation on xmm registers, then on ymm ones."""
    form = vector_pair if pair else vector_triple
    if pair:
        return form(op, XMM, XMM, xmm_word, imm) + form(op, YMM, YMM, "yword", imm)
    return form(op, XMM, XMM, XMM, xmm_word, imm, duplicate) + \
        form(op, YMM, YMM, YMM, "yword", imm, duplicate)


AVX_FLOAT = ["add", "sub", "mul", "div", "min", "max"]
AVX_PACKED = ["and", "andn", "or", "xor", "unpckl", "unpckh", "addsub"]


def avx_float_lines():
    out = []
    for op in AVX_FLOAT:
        for kind in ("ps", "pd"):
            out += both_widths("v" + op + kind)
        out += vector_triple("v" + op + "ss", XMM, XMM, XMM, "dword")
        out += vector_triple("v" + op + "sd", XMM, XMM, XMM, "qword")
    for kind in ("ps", "pd"):
        out += both_widths("vsqrt" + kind, pair=True)
    out += vector_triple("vsqrtss", XMM, XMM, XMM, "dword") + \
           vector_triple("vsqrtsd", XMM, XMM, XMM, "qword")
    out += both_widths("vrcpps", pair=True) + both_widths("vrsqrtps", pair=True)
    out += vector_triple("vrcpss", XMM, XMM, XMM, "dword") + \
           vector_triple("vrsqrtss", XMM, XMM, XMM, "dword")
    for op in AVX_PACKED[:-1]:
        for kind in ("ps", "pd"):
            out += both_widths("v" + op + kind)
    for op in ("vaddsubps", "vaddsubpd", "vhaddps", "vhaddpd", "vhsubps", "vhsubpd"):
        out += both_widths(op)
    out += vector_pair("vmovddup", XMM, XMM, "qword") + vector_pair("vmovddup", YMM, YMM, "yword")
    out += both_widths("vmovshdup", pair=True) + both_widths("vmovsldup", pair=True)
    for op, word in (("vcomiss", "dword"), ("vcomisd", "qword"), ("vucomiss", "dword"),
                     ("vucomisd", "qword")):
        out += vector_pair(op, XMM, XMM, word)
    for op in ("vshufps", "vshufpd", "vcmpps", "vcmppd"):
        out += both_widths(op, imm=True)
    out += vector_triple("vcmpss", XMM, XMM, XMM, None, imm=True)
    out += vector_triple("vcmpsd", XMM, XMM, XMM, "qword", imm=True)
    for n, name in enumerate(VEX_PREDICATES):
        out += ["vcmp%sps xmm%d, xmm%d, xmm%d" % (name, n % 16, (n * 3) % 16, (n * 7 + 1) % 16),
                "vcmp%spd ymm%d, ymm%d, [rax + %d]" % (name, (n * 5) % 16, n % 16, n * 32),
                "vcmp%sss xmm%d, xmm%d, [rbx]" % (name, (n + 8) % 16, n % 16),
                "vcmp%ssd xmm%d, xmm%d" % (name, (n * 3) % 16, (n * 11 + 2) % 16)]
    out += ["vcmpltps ymm9, ymm1, ymm15", "vcmpeq_uqps ymm3, yword [r9]", "vcmpneq_oqpd xmm0, xmm1",
            "vcmpord_sss xmm4, [sse_data]", "vcmpps xmm1, xmm2, xmm3, 32",
            "vcmpps ymm1, ymm2, ymm3, 255", "vcmpps xmm1, xmm2, 31"]
    return out


def avx_move_lines():
    out = []
    for op in ("vmovaps", "vmovapd", "vmovdqa"):
        for regs, word in ((XMM, "oword"), (YMM, "yword")):
            out += ["%s %s, %s" % (op, a, b) for a, b in zip(regs, regs[3:] + regs[:3])]
            for n, mem in enumerate(AVX_ALIGNED):
                out += ["%s %s, %s" % (op, regs[(n * 7) % 16], mem),
                        "%s %s, %s" % (op, mem, regs[(n * 11 + 1) % 16])]
            out += ["%s %s, %s [r12]" % (op, regs[3], word), "%s %s [r9 + 32], %s" % (op, word, regs[9])]
    for op in ("vmovups", "vmovupd", "vmovdqu"):
        for regs, word in ((XMM, "oword"), (YMM, "yword")):
            out += ["%s %s, %s" % (op, a, b) for a, b in zip(regs, regs[5:] + regs[:5])]
            for n, mem in enumerate(AVX_MEMORY + ["[rax + 1]", "[sse_data + 3]"]):
                out += ["%s %s, %s" % (op, regs[(n * 7) % 16], mem),
                        "%s %s, %s" % (op, mem, regs[(n * 11 + 1) % 16])]
            out += ["%s %s, %s [r12]" % (op, regs[3], word), "%s %s [r9 + 32], %s" % (op, word, regs[9])]
    for op, word in (("vmovss", "dword"), ("vmovsd", "qword")):
        out += ["%s %s, %s, %s" % (op, XMM[a], XMM[b], XMM[c]) for a, b, c in vex_picks(8)]
        out += ["%s %s, %s" % (op, a, b) for a, b in xmm_pairs()[:8]]
        out += ["%s xmm5, %s [rcx]" % (op, word), "%s %s [rcx + 4], xmm14" % (op, word),
                "%s xmm8, [sse_data]" % op, "%s [sse_data], xmm0" % op, "%s xmm13, [r9]" % op]
    for op in ("vmovlps", "vmovhps", "vmovlpd", "vmovhpd"):
        out += ["%s xmm3, xmm9, [rdi]" % op, "%s xmm12, xmm1, qword [r9 + 8]" % op,
                "%s xmm6, [rsi]" % op, "%s [rsi], xmm7" % op, "%s [sse_data], xmm15" % op,
                "%s qword [r10], xmm2" % op]
    for op in ("vmovhlps", "vmovlhps"):
        out += ["%s %s, %s, %s" % (op, XMM[a], XMM[b], XMM[c]) for a, b, c in vex_picks(6)]
        out += ["%s xmm1, xmm2" % op, "%s xmm10, xmm13" % op]
    for i in range(16):
        out += ["vmovd %s, %s" % (XMM[i], REG32[(i + 5) % 16]),
                "vmovd %s, %s" % (REG32[(i + 9) % 16], XMM[i]),
                "vmovq %s, %s" % (XMM[i], REG64[(i + 5) % 16]),
                "vmovq %s, %s" % (REG64[(i + 9) % 16], XMM[i]),
                "vmovq %s, %s" % (XMM[i], XMM[(i * 5 + 3) % 16])]
    out += ["vmovd xmm3, [rax]", "vmovd xmm11, dword [rsp + 4]", "vmovd [rbx], xmm6",
            "vmovd dword [r9], xmm2", "vmovq xmm13, [rdx]", "vmovq xmm2, qword [r12]",
            "vmovq [rax], xmm10", "vmovq qword [rsp], xmm4", "vmovq [sse_data], xmm1",
            "vmovq xmm1, [r13 + r9*8]"]
    for i, reg in enumerate(("eax", "r9d", "rax", "r12")):
        out += ["vmovmskps %s, %s" % (reg, XMM[i * 4]), "vmovmskps %s, %s" % (reg, YMM[i * 4 + 2]),
                "vmovmskpd %s, %s" % (reg, XMM[i * 4 + 1]), "vmovmskpd %s, %s" % (reg, YMM[i * 4 + 3]),
                "vpmovmskb %s, %s" % (reg, XMM[i * 4 + 3]),
                "vpmovmskb %s, %s" % (reg, YMM[i * 4 + 2])]
    for op in ("vmovntps", "vmovntpd", "vmovntdq"):
        out += vector_store(op, XMM, "oword", AVX_ALIGNED) + vector_store(op, YMM, "yword", AVX_ALIGNED)
    out += ["vmovntdqa xmm%d, %s" % (n * 3 % 16, mem) for n, mem in enumerate(AVX_ALIGNED)]
    out += ["vmovntdqa ymm%d, %s" % (n * 5 % 16, mem) for n, mem in enumerate(AVX_ALIGNED)]
    out.append("vmovntdqa ymm13, yword [r9]")
    out += ["vlddqu %s, %s" % (XMM[n * 5 % 16], mem) for n, mem in enumerate(AVX_MEMORY)]
    out += ["vlddqu %s, %s" % (YMM[n * 5 % 16], mem) for n, mem in enumerate(AVX_MEMORY)]
    out += ["vlddqu xmm12, oword [rdx + 33]", "vlddqu ymm3, yword [rdx + 33]",
            "vmaskmovdqu xmm1, xmm2", "vmaskmovdqu xmm10, xmm15", "vmaskmovdqu xmm9, xmm1",
            "vldmxcsr [rsp]", "vstmxcsr [rsp + 4]", "vldmxcsr dword [sse_data]",
            "vstmxcsr [r9 + r10*4]", "vzeroupper", "vzeroall"]
    return out


def avx_convert_lines():
    out = []
    for op in ("vcvtsi2ss", "vcvtsi2sd"):
        for i, reg in enumerate(("eax", "r10d", "rax", "r15")):
            out += ["%s %s, %s, %s" % (op, XMM[i * 5 % 16], XMM[(i * 3 + 1) % 16], reg),
                    "%s %s, %s" % (op, XMM[i * 5 % 16], reg)]
        out += ["%s xmm6, xmm9, dword [rax]" % op, "%s xmm14, xmm3, qword [rbx + 8]" % op,
                "%s xmm1, dword [r9]" % op, "%s xmm15, qword [sse_data]" % op]
    for op, word in (("vcvtss2si", "dword"), ("vcvttss2si", "dword"),
                     ("vcvtsd2si", "qword"), ("vcvttsd2si", "qword")):
        for i, reg in enumerate(("eax", "r11d", "rax", "r14")):
            out.append("%s %s, %s" % (op, reg, XMM[i * 6 % 16]))
        out += ["%s ecx, %s [rax]" % (op, word), "%s r8, [rsp + 8]" % op,
                "%s r9, %s [r10]" % (op, word)]
    out += vector_triple("vcvtss2sd", XMM, XMM, XMM, "dword")
    out += vector_triple("vcvtsd2ss", XMM, XMM, XMM, "qword")
    out += vector_pair("vcvtps2pd", XMM, XMM, "qword") + vector_pair("vcvtps2pd", YMM, XMM, "oword")
    out += vector_pair("vcvtdq2pd", XMM, XMM, "qword") + vector_pair("vcvtdq2pd", YMM, XMM, "oword")
    for op in ("vcvtpd2ps", "vcvtpd2dq", "vcvttpd2dq"):
        out += vector_pair(op, XMM, XMM, "oword") + vector_pair(op, XMM, YMM, "yword")
    for op in ("vcvtdq2ps", "vcvtps2dq", "vcvttps2dq"):
        out += both_widths(op, pair=True)
    return out


def avx_integer_lines():
    """The integer operations, on xmm registers and on ymm ones."""
    out = []
    for op in SSE_INTEGER + SSSE3_OPERATIONS[:-3] + SSE41_OPERATIONS[1:-2] + ["pcmpgtq"]:
        out += both_widths("v" + op)
    for op in ("pabsb", "pabsw", "pabsd"):
        out += both_widths("v" + op, pair=True)
    out += vector_pair("vphminposuw", XMM, XMM, "oword")
    out += both_widths("vptest", pair=True)
    doubled = {"word": "dword", "dword": "qword", "qword": "oword"}
    for op, word in SSE_WIDENING:
        out += vector_pair("v" + op, XMM, XMM, word)
        out += vector_pair("v" + op, YMM, XMM, doubled[word])
    for op in SSE_SHIFTS:
        out += vector_triple("v" + op, XMM, XMM, XMM, "oword")[:30]
        out += vector_triple("v" + op, YMM, YMM, XMM, "oword")[:30]
        out += ["v%s xmm3, xmm9, 1" % op, "v%s xmm12, xmm1, 15" % op, "v%s xmm0, xmm0, 7" % op,
                "v%s xmm10, 3" % op, "v%s xmm2, 255" % op,
                "v%s ymm3, ymm9, 1" % op, "v%s ymm12, ymm1, 15" % op, "v%s ymm0, ymm0, 7" % op,
                "v%s ymm10, 3" % op, "v%s ymm2, 255" % op, "v%s ymm4, xmm5" % op,
                "v%s ymm8, oword [rax]" % op]
    for op, digit in (("vpslldq", 4), ("vpsrldq", 8)):
        out += ["%s xmm1, xmm2, %d" % (op, digit), "%s xmm14, xmm9, 15" % op,
                "%s xmm3, xmm15, 1" % op, "%s xmm5, 2" % op, "%s xmm11, 9" % op,
                "%s ymm1, ymm2, %d" % (op, digit), "%s ymm14, ymm9, 15" % op,
                "%s ymm3, ymm15, 1" % op, "%s ymm5, 2" % op, "%s ymm11, 9" % op]
    for op in ("vpshufd", "vpshufhw", "vpshuflw"):
        out += both_widths(op, imm=True, pair=True)
    for op in ("vpalignr", "vpblendw", "vmpsadbw"):
        out += both_widths(op, imm=True)
    out += vector_triple("vdppd", XMM, XMM, XMM, "oword", imm=True)
    for op in ("vblendps", "vblendpd", "vdpps"):
        out += both_widths(op, imm=True)
    for op in ("vpcmpestrm", "vpcmpestri", "vpcmpistrm", "vpcmpistri"):
        out += vector_pair(op, XMM, XMM, "oword", imm=True)
    for op in ("vroundps", "vroundpd"):
        out += both_widths(op, imm=True, pair=True)
    out += vector_triple("vroundss", XMM, XMM, XMM, "dword", imm=True)
    out += vector_triple("vroundsd", XMM, XMM, XMM, "qword", imm=True)
    out += vector_triple("vinsertps", XMM, XMM, XMM, "dword", imm=True)
    for op in ("vblendvps", "vblendvpd", "vpblendvb"):
        for regs, word in ((XMM, "oword"), (YMM, "yword")):
            out += ["%s %s, %s, %s, %s" % (op, regs[a], regs[b], regs[c], regs[(a * 3 + b) % 16])
                    for a, b, c in vex_picks(8)]
            out += ["%s %s, %s, %s, %s" % (op, regs[(n * 3) % 16], regs[(n * 5 + 2) % 16], mem,
                                           regs[(n * 7 + 5) % 16])
                    for n, mem in enumerate(AVX_MEMORY)]
            out += ["%s %s, %s, %s [rdx + 32], %s" % (op, regs[11], regs[3], word, regs[15]),
                    "%s %s, %s, %s" % (op, regs[1], regs[2], regs[3]),
                    "%s %s, [rax], %s" % (op, regs[9], regs[14])]
    return out


def avx_extract_lines():
    out = []
    for i in range(16):
        out += ["vpextrw %s, %s, %d" % (REG32[i], XMM[(i * 3) % 16], i % 8),
                "vpextrw %s, %s, %d" % (REG64[(i + 3) % 16], XMM[(i * 5) % 16], 7 - i % 8),
                "vpextrb %s, %s, %d" % (REG32[i], XMM[(i * 3) % 16], i),
                "vpextrb %s, %s, %d" % (REG64[(i + 6) % 16], XMM[(i * 7) % 16], 255 - i),
                "vextractps %s, %s, %d" % (REG32[(i + 2) % 16], XMM[(i * 5) % 16], i % 4),
                "vpextrd %s, %s, %d" % (REG32[(i + 8) % 16], XMM[(i * 11) % 16], i % 4),
                "vpextrd %s, %s, %d" % (REG64[(i + 4) % 16], XMM[(i * 3 + 2) % 16], 3 - i % 4),
                "vpextrq %s, %s, %d" % (REG64[(i + 1) % 16], XMM[(i * 13) % 16], i % 2),
                "vpinsrw %s, %s, %s, %d" % (XMM[i], XMM[(i * 3 + 1) % 16], REG32[(i + 5) % 16], i % 8),
                "vpinsrb %s, %s, %s, %d" % (XMM[(i * 3) % 16], XMM[(i * 5 + 7) % 16], REG32[(i + 5) % 16], 15 - i),
                "vpinsrd %s, %s, %s, %d" % (XMM[(i * 5) % 16], XMM[(i + 4) % 16], REG32[(i + 3) % 16], i % 4),
                "vpinsrq %s, %s, %s, %d" % (XMM[(i * 7) % 16], XMM[(i * 3 + 2) % 16], REG64[(i + 7) % 16], i % 2),
                "vpinsrw %s, %s, %d" % (XMM[i], REG32[(i + 5) % 16], i % 8),
                "vpinsrq %s, %s, %d" % (XMM[(i * 7) % 16], REG64[(i + 7) % 16], i % 2)]
    for i, mem in enumerate(AVX_MEMORY):
        out += ["vpextrb %s, %s, %d" % (mem, XMM[(i * 4) % 16], i),
                "vpextrb byte %s, %s, %d" % (mem, XMM[(i * 4 + 1) % 16], 15 - i),
                "vpextrw %s, %s, %d" % (mem, XMM[(i * 4 + 2) % 16], i),
                "vpextrw word %s, %s, %d" % (mem, XMM[(i * 4 + 3) % 16], 7 - i),
                "vextractps %s, %s, %d" % (mem, XMM[(i * 3) % 16], i % 4),
                "vextractps dword %s, %s, %d" % (mem, XMM[(i * 3 + 1) % 16], 3 - i % 4),
                "vpextrd %s, %s, %d" % (mem, XMM[(i * 5) % 16], i % 4),
                "vpextrd dword %s, %s, 1" % (mem, XMM[(i * 5 + 1) % 16]),
                "vpextrq %s, %s, %d" % (mem, XMM[(i * 7) % 16], i % 2),
                "vpextrq qword %s, %s, 0" % (mem, XMM[(i * 7 + 1) % 16]),
                "vpinsrb %s, %s, %s, %d" % (XMM[(i * 3) % 16], XMM[(i * 7 + 1) % 16], mem, i),
                "vpinsrb %s, %s, byte %s, 15" % (XMM[(i * 3 + 1) % 16], XMM[(i + 9) % 16], mem),
                "vpinsrw %s, %s, %s, %d" % (XMM[(i * 3) % 16], XMM[(i * 7 + 1) % 16], mem, i),
                "vpinsrw %s, %s, word %s, 7" % (XMM[(i * 3 + 1) % 16], XMM[(i + 9) % 16], mem),
                "vpinsrd %s, %s, %s, %d" % (XMM[(i * 5) % 16], XMM[(i + 2) % 16], mem, i % 4),
                "vpinsrd %s, %s, dword %s, 2" % (XMM[(i * 5 + 1) % 16], XMM[(i + 3) % 16], mem),
                "vpinsrq %s, %s, %s, %d" % (XMM[(i * 7) % 16], XMM[(i + 5) % 16], mem, i % 2),
                "vpinsrq %s, %s, qword %s, 1" % (XMM[(i * 7 + 1) % 16], XMM[(i + 6) % 16], mem)]
    return out


def avx_only_lines():
    """The instructions AVX adds to SSE's."""
    out = []
    for n, mem in enumerate(AVX_MEMORY):
        out += ["vbroadcastss xmm%d, %s" % (n * 3 % 16, mem), "vbroadcastss ymm%d, %s" % (n * 5 % 16, mem),
                "vbroadcastsd ymm%d, %s" % (n * 7 % 16, mem), "vbroadcastf128 ymm%d, %s" % (n * 9 % 16, mem),
                "vextractf128 %s, ymm%d, %d" % (mem, n * 3 % 16, n % 2),
                "vinsertf128 ymm%d, ymm%d, %s, %d" % (n * 5 % 16, n * 3 % 16, mem, (n + 1) % 2),
                "vmaskmovps xmm%d, xmm%d, %s" % (n * 3 % 16, n * 5 % 16, mem),
                "vmaskmovps ymm%d, ymm%d, %s" % (n * 3 % 16, n * 5 % 16, mem),
                "vmaskmovpd xmm%d, xmm%d, %s" % (n * 7 % 16, n * 5 % 16, mem),
                "vmaskmovpd ymm%d, ymm%d, %s" % (n * 7 % 16, n * 5 % 16, mem),
                "vmaskmovps %s, xmm%d, xmm%d" % (mem, n * 5 % 16, n * 3 % 16),
                "vmaskmovps %s, ymm%d, ymm%d" % (mem, n * 5 % 16, n * 3 % 16),
                "vmaskmovpd %s, xmm%d, xmm%d" % (mem, n * 5 % 16, n * 7 % 16),
                "vmaskmovpd %s, ymm%d, ymm%d" % (mem, n * 5 % 16, n * 7 % 16),
                "vperm2f128 ymm%d, ymm%d, %s, %d" % (n * 3 % 16, n * 5 % 16, mem, n * 40)]
    out += ["vbroadcastss xmm9, dword [r9]", "vbroadcastss ymm2, dword [rsp]",
            "vbroadcastsd ymm10, qword [rax + r9*2]", "vbroadcastf128 ymm1, oword [rdx]",
            "vextractf128 oword [rax], ymm5, 1", "vinsertf128 ymm1, ymm2, oword [r8], 0",
            "vmaskmovps ymm1, ymm2, yword [rax]", "vmaskmovpd oword [rax], xmm1, xmm2"]
    for a, b, c in vex_picks(8):
        out += ["vextractf128 xmm%d, ymm%d, %d" % (a, b, c % 2),
                "vinsertf128 ymm%d, ymm%d, xmm%d, %d" % (a, b, c, c % 2),
                "vperm2f128 ymm%d, ymm%d, ymm%d, %d" % (a, b, c, a * 16 + b)]
    out += ["vinsertf128 ymm1, xmm2, 1", "vinsertf128 ymm9, oword [rax], 0",
            "vperm2f128 ymm3, ymm4, 0x31"]
    for op in ("vpermilps", "vpermilpd"):
        out += both_widths(op)
        out += vector_pair(op, XMM, XMM, "oword", imm=True) + vector_pair(op, YMM, YMM, "yword", imm=True)
    for op in ("vtestps", "vtestpd"):
        out += both_widths(op, pair=True)
    out += ["vaddps ymm1, ymm2, [sse_data + 4]", "vpshufb xmm1, xmm2, [sse_data + 1]",
            "vmulpd xmm3, xmm4, [avx_data + 8]", "vptest ymm5, [sse_data + 2]"]
    return out


GATHERS = [("vgatherdps", 4, "xmm", "xmm", "xmm"), ("vgatherdps", 4, "ymm", "ymm", "ymm"),
           ("vgatherdpd", 8, "xmm", "xmm", "xmm"), ("vgatherdpd", 8, "ymm", "xmm", "ymm"),
           ("vgatherqps", 4, "xmm", "xmm", "xmm"), ("vgatherqps", 4, "xmm", "ymm", "xmm"),
           ("vgatherqpd", 8, "xmm", "xmm", "xmm"), ("vgatherqpd", 8, "ymm", "ymm", "ymm"),
           ("vpgatherdd", 4, "xmm", "xmm", "xmm"), ("vpgatherdd", 4, "ymm", "ymm", "ymm"),
           ("vpgatherdq", 8, "xmm", "xmm", "xmm"), ("vpgatherdq", 8, "ymm", "xmm", "ymm"),
           ("vpgatherqd", 4, "xmm", "xmm", "xmm"), ("vpgatherqd", 4, "xmm", "ymm", "xmm"),
           ("vpgatherqq", 8, "xmm", "xmm", "xmm"), ("vpgatherqq", 8, "ymm", "ymm", "ymm")]
VSIB_BASES = ["rax + ", "rbx + ", "rsp + ", "rbp + ", "r12 + ", "r13 + ", "r9 + ", "", "rax + 8 + ",
              "r13 + 0x40 + ", "rsp + 0x1000 + ", "", "0x20 + ", "abs 0x1000 + "]


def gather_lines():
    """Each gather over its widths, with every kind of base, scale and
    displacement and the registers' high halves, sized memory and not; the
    destination, the index and the mask three different registers."""
    out = []
    for n, (op, element, dest, index, mask) in enumerate(GATHERS):
        word = "dword" if element == 4 else "qword"
        for i in range(14):
            d, x, m = (i * 3) % 16, (i * 3 + 5) % 16, (i * 3 + 11) % 16
            base = VSIB_BASES[i]
            scale = "*%d" % (1 << (i % 4)) if i % 5 else ""
            tail = ["", " + 16", " - 128", " + 0x12345", ""][i % 5]
            sized = word + " " if i % 2 else ""
            out.append("%s %s%d, %s[%s%s%d%s%s], %s%d" % (op, dest, d, sized, base, index, x,
                                                          scale, tail, mask, m))
        out.append("%s %s1, [%s2*%d], %s3" % (op, dest, index, 1 << (n % 4), mask))
        out.append("%s %s9, [r8 + %s10*8], %s11" % (op, dest, index, mask))
        out.append("%s %s2, [%d*%s5 + rdx], %s6" % (op, dest, 1 << (n % 4), index, mask))
    return out


def avx2_lines():
    """The instructions AVX2 adds to AVX's."""
    out = []
    for op, word in (("vpbroadcastb", "byte"), ("vpbroadcastw", "word"),
                     ("vpbroadcastd", "dword"), ("vpbroadcastq", "qword")):
        out += vector_pair(op, XMM, XMM, word) + vector_pair(op, YMM, XMM, word)
    out += ["vbroadcastss xmm%d, xmm%d" % (a, b) for a, b, _ in vex_picks(8)]
    out += ["vbroadcastss ymm%d, xmm%d" % (a, b) for a, b, _ in vex_picks(8)]
    out += ["vbroadcastsd ymm%d, xmm%d" % (a, b) for a, b, _ in vex_picks(8)]
    for n, mem in enumerate(AVX_MEMORY):
        out += ["vbroadcasti128 ymm%d, %s" % (n * 9 % 16, mem),
                "vextracti128 %s, ymm%d, %d" % (mem, n * 3 % 16, n % 2),
                "vinserti128 ymm%d, ymm%d, %s, %d" % (n * 5 % 16, n * 3 % 16, mem, (n + 1) % 2),
                "vperm2i128 ymm%d, ymm%d, %s, %d" % (n * 3 % 16, n * 5 % 16, mem, n * 40),
                "vpermd ymm%d, ymm%d, %s" % (n * 3 % 16, n * 5 % 16, mem),
                "vpermps ymm%d, ymm%d, %s" % (n * 3 % 16, n * 5 % 16, mem),
                "vpermq ymm%d, %s, %d" % (n * 3 % 16, mem, n * 30),
                "vpermpd ymm%d, %s, %d" % (n * 7 % 16, mem, n * 31)]
    out += ["vbroadcasti128 ymm1, oword [rdx]", "vextracti128 oword [rax], ymm5, 1",
            "vinserti128 ymm1, ymm2, oword [r8], 0", "vpermq ymm3, yword [rax], 77"]
    for a, b, c in vex_picks(8):
        out += ["vextracti128 xmm%d, ymm%d, %d" % (a, b, c % 2),
                "vinserti128 ymm%d, ymm%d, xmm%d, %d" % (a, b, c, c % 2),
                "vperm2i128 ymm%d, ymm%d, ymm%d, %d" % (a, b, c, a * 16 + b),
                "vpermd ymm%d, ymm%d, ymm%d" % (a, b, c), "vpermps ymm%d, ymm%d, ymm%d" % (a, b, c),
                "vpermq ymm%d, ymm%d, %d" % (a, b, a * 16 + c),
                "vpermpd ymm%d, ymm%d, %d" % (a, b, b * 16 + c)]
    out += ["vinserti128 ymm1, xmm2, 1", "vinserti128 ymm9, oword [rax], 0",
            "vperm2i128 ymm3, ymm4, 0x31", "vpermd ymm1, ymm2"]
    for op in ("vpblendd",):
        out += both_widths(op, imm=True)
    for op in ("vpmaskmovd", "vpmaskmovq"):
        for n, mem in enumerate(AVX_MEMORY):
            out += ["%s xmm%d, xmm%d, %s" % (op, n * 3 % 16, n * 5 % 16, mem),
                    "%s ymm%d, ymm%d, %s" % (op, n * 3 % 16, n * 5 % 16, mem),
                    "%s %s, xmm%d, xmm%d" % (op, mem, n * 5 % 16, n * 3 % 16),
                    "%s %s, ymm%d, ymm%d" % (op, mem, n * 5 % 16, n * 3 % 16)]
        out += ["%s ymm1, ymm2, yword [rax]" % op, "%s oword [rax], xmm1, xmm2" % op,
                "%s xmm3, [rax]" % op, "%s ymm9, yword [r9]" % op]
    for op in ("vpsllvd", "vpsllvq", "vpsravd", "vpsrlvd", "vpsrlvq"):
        out += both_widths(op)
    return out + gather_lines()


FMA_ORDERS = ("132", "213", "231")
BMI_SOURCE_LAST = ("andn", "mulx", "pdep", "pext")     # op r, vvvv, r/m
BMI_CONTROL_LAST = ("bextr", "bzhi", "sarx", "shlx", "shrx")   # op r, r/m, vvvv
BMI_ONE_SOURCE = ("blsi", "blsmsk", "blsr")             # op vvvv, r/m


def fma_lines():
    """Every FMA instruction in its three orders, packed on both widths and
    scalar with the element's memory; its three operands always written."""
    out = []
    for family in ("fmaddsub", "fmsubadd", "fmadd", "fmsub", "fnmadd", "fnmsub"):
        for order in FMA_ORDERS:
            for kind in ("ps", "pd"):
                out += both_widths("v" + family + order + kind, duplicate=False)
            if family in ("fmaddsub", "fmsubadd"):
                continue
            out += vector_triple("v" + family + order + "ss", XMM, XMM, XMM, "dword",
                                 duplicate=False)
            out += vector_triple("v" + family + order + "sd", XMM, XMM, XMM, "qword",
                                 duplicate=False)
    return out


def f16c_lines():
    """Halves to singles from a register or memory, and back with each
    rounding the imm8 names, to a register or memory."""
    out = vector_pair("vcvtph2ps", XMM, XMM, "qword") + vector_pair("vcvtph2ps", YMM, XMM, "oword")
    for source, word in ((XMM, "qword"), (YMM, "oword")):
        for n, (a, b, _) in enumerate(vex_picks()):
            out.append("vcvtps2ph %s, %s, %d" % (XMM[a], source[b], (n * 37 + 4) & 255))
        for n, mem in enumerate(AVX_MEMORY):
            out.append("vcvtps2ph %s, %s, %d" % (mem, source[(n * 5 + 3) % 16], n & 7))
        out.append("vcvtps2ph %s [r10 + 24], %s, 255" % (word, source[12]))
    return out


def bmi_lines():
    """BMI1 and BMI2 on 32- and 64-bit registers: the destination, the source
    in VEX.vvvv and the r/m each in the high half and not, then memory, sized
    and not; the three-operand ones with the second left out, which NASM
    reads as the first again; rorx with the edges of its imm8."""
    out = []
    for size in (4, 8):
        regs, word = REGS[size], SIZE_WORD[size]
        for n, (a, b, c) in enumerate(vex_picks()):
            out += ["%s %s, %s, %s" % (op, regs[a], regs[b], regs[c]) for op in BMI_SOURCE_LAST]
            out += ["%s %s, %s, %s" % (op, regs[a], regs[c], regs[b]) for op in BMI_CONTROL_LAST]
            out += ["%s %s, %s" % (op, regs[b], regs[c]) for op in BMI_ONE_SOURCE]
            out.append("rorx %s, %s, %d" % (regs[a], regs[c], (n * 41) & 255))
        for n, mem in enumerate(MEMORY):
            a, b = regs[(n * 3) % 16], regs[(n * 5 + 2) % 16]
            out += ["%s %s, %s, %s" % (op, a, b, mem) for op in BMI_SOURCE_LAST]
            out += ["%s %s, %s, %s" % (op, a, mem, b) for op in BMI_CONTROL_LAST]
            out += ["%s %s, %s" % (op, b, mem) for op in BMI_ONE_SOURCE]
            out.append("rorx %s, %s, %d" % (a, mem, [0, 1, 31, 63, 127, 255][n % 6]))
        for a, b, _ in vex_picks(4):
            out += ["%s %s, %s" % (op, regs[a], regs[b]) for op in BMI_SOURCE_LAST + BMI_CONTROL_LAST]
        out += ["%s %s, [rcx + 4]" % (op, regs[6]) for op in BMI_SOURCE_LAST]
        out += ["%s %s, %s, %s [rbx + 8]" % (op, regs[1], regs[9], word) for op in BMI_SOURCE_LAST]
        out += ["%s %s, %s [rbx + 8], %s" % (op, regs[10], word, regs[2]) for op in BMI_CONTROL_LAST]
        out += ["%s %s, %s [r12]" % (op, regs[13], word) for op in BMI_ONE_SOURCE]
        out.append("rorx %s, %s [rsp], 7" % (regs[15], word))
    return out


def movbe_lines():
    """movbe in both directions on every register of each width it takes,
    with the memory sized and not."""
    out = []
    for size in (2, 4, 8):
        for n, reg in enumerate(REGS[size]):
            mem = MEMORY[n % len(MEMORY)]
            out += ["movbe %s, %s" % (reg, mem), "movbe %s, %s" % (mem, reg)]
        out += ["movbe %s, %s [rax + 2]" % (REGS[size][7], SIZE_WORD[size]),
                "movbe %s [r9], %s" % (SIZE_WORD[size], REGS[size][12])]
    return out


X87_ALONE = ["fnop", "fchs", "fabs", "ftst", "fxam", "fld1", "fldl2t", "fldl2e", "fldpi",
             "fldlg2", "fldln2", "fldz", "f2xm1", "fyl2x", "fptan", "fpatan", "fxtract",
             "fprem1", "fdecstp", "fincstp", "fprem", "fyl2xp1", "fsqrt", "fsincos", "frndint",
             "fscale", "fsin", "fcos", "fcompp", "fucompp", "finit", "fninit", "fclex",
             "fnclex", "fwait", "wait"]
X87_ARITHMETIC = ["fadd", "fmul", "fsub", "fsubr", "fdiv", "fdivr"]
X87_INTEGER = ["fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr"]
X87_WITH_TOP = ["fucom", "fucomp", "fcomi", "fcomip", "fucomi", "fucomip", "fcmovb", "fcmove",
                "fcmovbe", "fcmovu", "fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu"]
# Memory sizes each x87 load, store and integer operation reads, said.
X87_SIZED = {"fld": ("dword", "qword", "tword"), "fst": ("dword", "qword"),
             "fstp": ("dword", "qword", "tword"), "fild": ("word", "dword", "qword"),
             "fist": ("word", "dword"), "fistp": ("word", "dword", "qword"),
             "fisttp": ("word", "dword", "qword"), "fcom": ("dword", "qword"),
             "fcomp": ("dword", "qword")}
X87_STATE = ["fldenv", "fstenv", "fnstenv", "frstor", "fsave", "fnsave"]
FXSAVE_MEMORY = ["[rax]", "[rsp + 16]", "[r13]", "[rbx + rcx*4 + 16]", "[r9 + r10*8]",
                 "[sse_data]", "[avx_data + 32]"]


def x87_lines():
    """Every x87 instruction: alone, with each stack register in each place
    it takes, with each memory size it reads, said, and the state it keeps."""
    out = list(X87_ALONE)
    st = ["st%d" % i for i in range(8)]
    for op in X87_ARITHMETIC:
        out += [op, op + "p"]
        for i, reg in enumerate(st):
            out += ["%s st0, %s" % (op, reg), "%s %s, st0" % (op, reg), "%s %s" % (op, reg),
                    "%sp %s, st0" % (op, reg), "%sp %s" % (op, reg)]
        for n, mem in enumerate(MEMORY):
            out.append("%s %s %s" % (op, ("dword", "qword")[n % 2], mem))
    for op in X87_WITH_TOP + ["fcom", "fcomp"]:
        out.append(op)
        out += ["%s %s" % (op, reg) for reg in st] + ["%s st0, %s" % (op, reg) for reg in st]
    for op in ("fld", "fst", "fstp", "ffree", "ffreep", "fxch"):
        out += ["%s %s" % (op, reg) for reg in st]
    out += ["fxch", "ffree", "ffreep", "fld", "fst", "fstp"] + ["fxch st0, %s" % reg for reg in st] + \
           ["fxch %s, st0" % reg for reg in st]
    for op, words in X87_SIZED.items():
        for n, mem in enumerate(MEMORY):
            out.append("%s %s %s" % (op, words[n % len(words)], mem))
    for op in X87_INTEGER:
        for n, mem in enumerate(MEMORY):
            out.append("%s %s %s" % (op, ("word", "dword")[n % 2], mem))
    for n, mem in enumerate(MEMORY):
        out += ["fbld %s" % mem, "fbstp %s" % mem, "fbld tword %s" % mem,
                "fldcw %s" % mem, "fstcw %s" % mem, "fnstcw word %s" % mem,
                "fstsw %s" % mem, "fnstsw word %s" % mem]
        out += ["%s %s" % (op, mem) for op in X87_STATE]
    out += ["fstsw ax", "fnstsw ax"]
    for mem in FXSAVE_MEMORY:
        out += ["%s %s" % (op, mem) for op in ("fxsave", "fxrstor", "fxsave64", "fxrstor64")]
    return out


MMX_INTEGER = ["paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq", "paddsb",
               "paddsw", "paddusb", "paddusw", "psubsb", "psubsw", "psubusb", "psubusw", "pmullw",
               "pmulhw", "pmulhuw", "pmuludq", "pmaddwd", "pavgb", "pavgw", "pmaxsw", "pmaxub",
               "pminsw", "pminub", "psadbw", "pand", "pandn", "por", "pxor", "pcmpeqb", "pcmpeqw",
               "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd", "packsswb", "packssdw", "packuswb",
               "punpckhbw", "punpckhwd", "punpckhdq", "pshufb", "phaddw", "phaddd", "phaddsw",
               "pmaddubsw", "phsubw", "phsubd", "phsubsw", "psignb", "psignw", "psignd",
               "pmulhrsw", "pabsb", "pabsw", "pabsd", "maskmovq"]
MMX_SHIFTS = ["psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"]
MM = ["mm%d" % i for i in range(8)]


def mmx_lines():
    """Every MMX form: each operation on every pair of mm registers and on
    memory, sized and not; the shifts by a register, memory and an imm8; the
    moves to and from general registers and memory; SSE's conversions and
    moves between mm and xmm."""
    out = []
    pairs = [(a, b) for a in range(8) for b in range(8) if (a * 3 + b) % 5 == 0] + [(0, 0), (7, 7)]
    for op in MMX_INTEGER:
        out += ["%s %s, %s" % (op, MM[a], MM[b]) for a, b in pairs]
        if op != "maskmovq":
            out += ["%s %s, %s" % (op, MM[n % 8], mem) for n, mem in enumerate(MEMORY)]
            out.append("%s mm3, qword [r9 + 8]" % op)
    for op in ("punpcklbw", "punpcklwd", "punpckldq"):
        out += ["%s %s, %s" % (op, MM[a], MM[b]) for a, b in pairs]
        out += ["%s %s, %s" % (op, MM[n % 8], mem) for n, mem in enumerate(MEMORY)]
        out.append("%s mm6, dword [rdx]" % op)
    for op in MMX_SHIFTS:
        out += ["%s %s, %s" % (op, MM[a], MM[b]) for a, b in pairs[:6]]
        out += ["%s %s, %s" % (op, MM[n % 8], mem) for n, mem in enumerate(MEMORY[:6])]
        out += ["%s %s, %d" % (op, MM[i], [0, 1, 7, 15, 31, 63, 127, 255][i]) for i in range(8)]
    for n, mem in enumerate(MEMORY):
        out += ["movd %s, %s" % (MM[n % 8], mem), "movd %s, %s" % (mem, MM[(n + 3) % 8]),
                "movq %s, %s" % (MM[n % 8], mem), "movq %s, %s" % (mem, MM[(n + 5) % 8]),
                "pshufw %s, %s, %d" % (MM[n % 8], mem, n * 11), "movntq %s, %s" % (mem, MM[n % 8]),
                "pinsrw %s, %s, %d" % (MM[n % 8], mem, n % 4),
                "cvtpi2ps xmm%d, %s" % (n % 16, mem), "cvtpi2pd xmm%d, %s" % ((n * 3) % 16, mem),
                "cvtps2pi %s, %s" % (MM[n % 8], mem), "cvttps2pi %s, %s" % (MM[n % 8], mem),
                "palignr %s, %s, %d" % (MM[n % 8], mem, n)]
    for n, mem in enumerate(AVX_ALIGNED[:5] + ["[sse_data]"]):
        out += ["cvtpd2pi %s, %s" % (MM[n % 8], mem), "cvttpd2pi %s, %s" % (MM[n % 8], mem)]
    for i in range(16):
        m = MM[i % 8]
        out += ["movd %s, %s" % (m, REG32[i]), "movd %s, %s" % (REG32[(i + 5) % 16], m),
                "movq %s, %s" % (m, REG64[i]), "movq %s, %s" % (REG64[(i + 9) % 16], m),
                "movq %s, %s" % (m, MM[(i * 3) % 8]),
                "pextrw %s, %s, %d" % (REG32[i], m, i % 4),
                "pinsrw %s, %s, %d" % (m, REG32[(i + 2) % 16], i % 4),
                "pmovmskb %s, %s" % (REG32[i], m),
                "movq2dq xmm%d, %s" % (i, m), "movdq2q %s, xmm%d" % (m, 15 - i),
                "cvtpi2ps xmm%d, %s" % (i, m), "cvtps2pi %s, xmm%d" % (m, i),
                "cvtpd2pi %s, xmm%d" % (m, (i * 7) % 16), "pshufw %s, %s, %d" % (m, MM[(i + 1) % 8], i * 17),
                "palignr %s, %s, %d" % (m, MM[(i + 2) % 8], i * 9)]
    out += ["movd mm1, dword [rax]", "movd dword [r8], mm2", "movq mm3, qword [rbx]",
            "movq qword [rsi], mm4", "pinsrw mm5, word [rdi], 2", "movntq qword [r10], mm6",
            "cvtpi2ps xmm9, qword [rcx]", "cvtps2pi mm1, qword [rdx]", "cvtpd2pi mm2, oword [rax]",
            "emms"]
    return out


def v3_lines():
    """What x86-64-v3 adds to AVX2: FMA, F16C, BMI1, BMI2 and MOVBE."""
    return fma_lines() + f16c_lines() + bmi_lines() + movbe_lines()


def avx_lines():
    """Every AVX and AVX2 instruction BoxASM has, in each of its forms."""
    return (avx_float_lines() + avx_move_lines() + avx_convert_lines() + avx_integer_lines() +
            avx_extract_lines() + avx_only_lines() + avx2_lines())


def exact_value(text):
    """The rational a decimal floating-point constant writes."""
    text = text.replace("_", "")
    mantissa, _, exponent = text.partition("e")
    whole, _, fraction = mantissa.partition(".")
    return Fraction(int(whole + fraction or "0")) * \
        Fraction(10) ** (int(exponent or "0") - len(fraction))


def float_bits(text, precision, smallest, bias):
    """IEEE 754 bits rounded to nearest, ties to even, from the exact value;
    None past the largest or rounding to zero, and 'tie' with the bits when
    the value sits exactly halfway between two."""
    value = exact_value(text)
    if value == 0:
        return 0, False
    top = value.numerator.bit_length() - value.denominator.bit_length()
    while Fraction(2) ** top > value:
        top -= 1
    while Fraction(2) ** (top + 1) <= value:
        top += 1
    lowest = max(top - (precision - 1), smallest - (precision - 1))
    scaled = value / Fraction(2) ** lowest
    mantissa = scaled.numerator // scaled.denominator
    rest = scaled - mantissa
    if rest > Fraction(1, 2) or (rest == Fraction(1, 2) and mantissa % 2 == 1):
        mantissa += 1
    if mantissa == 0:
        return None, False
    if mantissa >> precision:
        mantissa >>= 1
        lowest += 1
    if mantissa >> (precision - 1):
        field = lowest + precision - 1 + bias
        if field > 2 * bias:
            return None, False
        bits = field << (precision - 1) | (mantissa & ((1 << (precision - 1)) - 1))
    else:
        bits = mantissa
    return bits, rest == Fraction(1, 2)


FORMATS = {8: (53, -1022, 1023), 4: (24, -126, 127)}
FLOAT_EDGES = ["0.0", "1.0", "2.5", "0.1", "0.3", "1.5e300", "1.7976931348623157e308",
               "2.2250738585072014e-308", "2.2250738585072011e-308", "4.9406564584124654e-324",
               "0.1000000000000000055511151231257827021181583404541015625",
               "1." + "0" * 900 + "1", "0." + "0" * 300 + "17", "1_000.5", "0.000_1", "1.e10",
               "3.4028234663852886e38", "1.17549435e-38", "1.4e-45", "9007199254740993.0",
               "16777217.0", "9007199254740995.0", "16777219.0", "2.4703282292062328e-324"]


def float_literals(count, seed):
    rng = random.Random(seed)
    out = list(FLOAT_EDGES)
    for _ in range(count):
        digits = "".join(rng.choice("0123456789") for _ in range(rng.randint(1, 25)))
        point = rng.randint(1, len(digits))
        text = digits[:point] + "." + digits[point:]
        out.append(text + ("e%d" % rng.randint(-340, 320) if rng.random() < 0.8 else ""))
    return out


def float_data(width, count, seed, ties):
    """Constants that fit the width, each with its negation, and their bits;
    without the ties when NASM is to read them (it rounds a tie down)."""
    lines, data = [], b""
    for text in float_literals(count, seed):
        bits, tie = float_bits(text, *FORMATS[width])
        if bits is None or (tie and not ties):
            continue
        lines.append("    %s %s, -%s" % ("dq" if width == 8 else "dd", text, text))
        sign = 1 << (width * 8 - 1)
        data += struct.pack("<QQ" if width == 8 else "<II", bits, bits | sign)
    return lines, data


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
    sse_align = "    align 16, db 0" if nasm else "    align 16"
    avx_align = "    align 32, db 0" if nasm else "    align 32"
    lines = ["default rel", "bits 64", "extern outside_fn", "global main",
             "section .text", "main:"]
    for part in (alu_lines, test_lines, mov_lines, address_lines, xchg_lines,
                 stack_lines, unary_lines, imul_lines, shift_lines, extend_lines,
                 condition_lines, branch_lines, string_lines, bit_lines, atomic_lines,
                 short_branch_lines, sse_lines, lambda: sse_extension_lines(nasm), avx_lines,
                 v3_lines, x87_lines, mmx_lines):
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
              sse_align, "sse_data:", "    dq 1.5, -2.25", "    dd 0.5, -0.0, 1.e-3, 7.0"] + \
             float_data(8, 300, 64, False)[0] + float_data(4, 300, 32, False)[0] + \
             ["    dq __Infinity__, -__Infinity__, __QNaN__, __NaN__, __SNaN__",
              "    dd __Infinity__, -__QNaN__, __SNaN__",
              avx_align, "avx_data:", "    times 16 dq 0x0123456789ABCDEF",
              "section .bss", "    resb 3", "    resw 2", "    resd 5", "    resq 4"]
    text = "\n".join(lines) + "\n"
    return text.replace("[rip + ", rip).replace(" near ", near)


# ---- The preprocessor ------------------------------------------------------------

# Single-line and multi-line macros, their parameters, defaults, greedy and
# rotated lists, %% labels, %+, overloads, %unmacro, a macro defining a
# macro, a macro named like an instruction, %if and every test it takes,
# %rep and %exitrep nested, __LINE__, struc and istruc. NASM pads alignment
# in data with nop unless told db 0.
MACRO_PROGRAM = r"""; every preprocessor feature both assemblers share
%define COUNT 4
%define sq(x) ((x)*(x))
%define add3(a, b, c) (a + b + c)
%xdefine BASE COUNT * 2
%assign total COUNT + 1
%define lazy later
%define later 77
%macro prologue 0
    push rbp
    mov rbp, rsp
%endmacro
%macro epilogue 0
    mov rsp, rbp
    pop rbp
    ret
%endmacro
%macro defs 1-3 20, 30
    dd %0, %1, %2, %3
%endmacro
%macro loop_to 2
    mov ecx, %2
%%again:
    dec ecx
    jnz %%again
    mov %1, ecx
%endmacro
%macro greedy 1+
    db %1
%endmacro
%macro rot 1-*
%rep %0
    db %1
%rotate 1
%endrep
%endmacro
%macro paste 2
    %1%+_label: dd %{2}0
%endmacro
%macro save 1-*
%rep %0
    push %1
%rotate 1
%endrep
%endmacro
%macro restore 1-*
%rep %0
%rotate -1
    pop %1
%endrep
%endmacro
%macro clear 1
%ifidni %1, eax
    xor eax, eax
%elifidn %1, ebx
    xor ebx, ebx
%else
    mov %1, 0
%endif
%endmacro
%macro typed 1
%ifnum %1
    dd %1
%elifstr %1
    db %1, 0
%elifid %1
    dq %1
%endif
%endmacro

%macro nodef 1-4
    dd %0, 0%4
%endmacro
%macro somedef 1-4 9
    dd %0, %2
%endmacro
%macro inner 1
%%self: dd %1, %%self - $$
%endmacro
%macro outer 2
%%mine: inner %1
    inner %2
    dd %%mine - $$
%endmacro
%macro maker 1
%macro made_%1 0
    db '%1'
%endmacro
%endmacro
%macro over 1
    db 1, %1
%endmacro
%macro over 2
    db 2, %1, %2
%endmacro
%macro gone 0
    db 0x55
%endmacro
%macro push 2
    push %1
    push %2
%endmacro
%macro list 3
    dd %1 + 0%2 + %3
%endmacro
%macro braced 2
    db %1
    db %2
%endmacro
%macro where 0
    dd __LINE__
%endmacro
%define again(x) x + 1
%define again(x, y) x + y
global main
section .text
main:
    push rax
    push rbx, rcx
    prologue
    mov eax, sq(3)
    mov ebx, add3(1, 2, 3)
    mov ecx, BASE
    mov edx, total
    mov esi, lazy
.inner:
    loop_to eax, 10
    loop_to ebx, COUNT
    jmp .inner
helper: loop_to edx, 3
    save rbx, r12, r13
    restore rbx, r12, r13
    clear eax
    clear ebx
    clear ecx
%ifdef COUNT
    nop
%endif
%ifndef NOPE
    int3
%endif
%if COUNT > 3 && total == 5 || 0
    cld
%elif 1
    std
%else
    ud2
%endif
%if 0
    this would not assemble
%endif
%ifmacro rot
    pause
%endif
%assign i 0
%rep 3
    mov eax, i
%assign i i+1
%endrep
%rep 100
%if i > 5
%exitrep
%endif
    inc eax
%assign i i+1
%endrep
    epilogue

%push outer
%define %$v 7
%$top:
%push inner
%$top:
    dd %$$v
    jmp %$$top
%ifctx inner
    db 1
%endif
%repl renamed
%ifctx outer renamed
    db 2
%endif
%ifnctx inner
    db 3
%endif
%pop
%ifdef %$v
    db 4
%endif
%define atuse %$z
%push third
%$z: jmp atuse
%pop third
%pop outer
%strlen slen 'hello'
    dd slen
%substr sub1 'hello', 2, 3
%substr sub2 'hello', 2
%substr sub3 'hello', 2, -1
%substr sub4 'hello', 9, 2
    db sub1, sub2, sub3, sub4, 0
%strcat cat 'ab', "cd", 'e'
%strcat quote "it's", 'x'
    db cat, quote
%idefine Big 9
    dd BIG, big, Big
%imacro Twice 1
    db %1, %1
%endmacro
    TWICE 3
    twice 4
%ixdefine Mx Big+1
    dd mx
%iassign Cnt 3
%iassign cnt CNT+1
    dd cNT
%undef bIG
%ifndef Big
    db 6
%endif
%unimacro TWICE 1
%ifnmacro twice
    db 7
%endif
%macro outerm 0
%imacro innerm 0
    db 8
%endmacro
%endmacro
    outerm
    INNERM
section .data
    defs 1
    defs 1, 2
    defs 1, 2, 3
    greedy 1, 2, 3
    rot 4, 5, 6
    paste here, 7
    typed 42
    typed 'hi'
    typed p1
%undef COUNT
%ifndef COUNT
    db 0xEE
%endif

struc point
.x: resd 1
.y: resd 1
.name: resb 10
    alignb 4
.tail: resq 1
endstruc

align 16
p1: istruc point
    at point.x, dd 1
    at point.y, dd 2
    at point.name, db "corner"
iend
    dd point_size, point.y, point.name, point.tail
section .data
    nodef 5
    nodef 5, 6, 7
    somedef 5
    somedef 5, 6
    somedef 5, 6, 7
    outer 1, 2
    outer 3, 4
    maker one
    made_one
    over 7
    over 8, 9
    gone
%unmacro gone 0
%macro gone 0
    db 0x66
%endmacro
    gone
    list 1,,3
    braced {1, 2, 3}, 4
    dd again(1), again(2, 3)
%if 'ab' == 0x6261 && "c" == 99
    db 0x77
%endif
%assign n 0
%rep 4
%assign m 0
%rep 10
%if m >= 2
%exitrep
%endif
    db n, m
%assign m m+1
%endrep
%assign n n+1
%endrep
    where
    dd __LINE__
%define ra rb
%define rb ra
%define ra_val 5
    dd ra_val
"""


def macros(nasm):
    return MACRO_PROGRAM.replace("\nalign 16\n", "\nalign 16, db 0\n") if nasm else MACRO_PROGRAM


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
    if len(argv) in (2, 3) and argv[1] == "macros" and argv[2:] in ([], ["nasm"]):
        sys.stdout.write(macros(argv[2:] == ["nasm"]))
        return 0
    if len(argv) == 4 and argv[1] == "compare":
        return 0 if compare(argv[2], argv[3]) else 1
    if len(argv) == 4 and argv[1] == "floats":
        doubles, double_bytes = float_data(8, 3000, 20261028, True)
        singles, single_bytes = float_data(4, 3000, 1028, True)
        with open(argv[2], "w") as source:
            source.write("section .text\nglobal main\nmain:\n    ret\nsection .data\n")
            source.write("\n".join(doubles + singles) + "\n")
        with open(argv[3], "wb") as data:
            data.write(double_bytes + single_bytes)
        return 0
    print("\n".join(__doc__.strip().splitlines()[2:8]), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
