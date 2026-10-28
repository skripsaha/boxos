#include "boxasm_isa.h"

#include "box/string.h"

/* Each instruction's forms, in the order NASM chooses among them: a register
 * pair as r/m,reg; an immediate as imm8 sign-extended when it fits, then the
 * accumulator's short form, then the general one; mov r64,imm as mov r32 when
 * the value fits 32 bits unsigned, then sign-extended, then movabs. */

#define C(A, B, D) { BOXASM_CLASS_##A, BOXASM_CLASS_##B, BOXASM_CLASS_##D }
#define S8   BOXASM_SIZE_8
#define S16  BOXASM_SIZE_16
#define S32  BOXASM_SIZE_32
#define S64  BOXASM_SIZE_64
#define S128 BOXASM_SIZE_128
#define SW   BOXASM_SIZE_WIDE
#define R    BOXASM_DIGIT_REG
#define NO   BOXASM_DIGIT_NONE

#define ALU(Base, Digit)                                                  \
    { C(RM, REG, NONE),    S8, { (Base) + 0 }, 1, R, 0, 0 },              \
    { C(RM, REG, NONE),    SW, { (Base) + 1 }, 1, R, 0, 0 },              \
    { C(REG, RM, NONE),    S8, { (Base) + 2 }, 1, R, 0, 0 },              \
    { C(REG, RM, NONE),    SW, { (Base) + 3 }, 1, R, 0, 0 },              \
    { C(RM, IMM8S, NONE),  SW, { 0x83 }, 1, (Digit), 0, 0 },              \
    { C(ACC, IMM, NONE),   S8, { (Base) + 4 }, 1, NO, 0, 0 },             \
    { C(ACC, IMM, NONE),   SW, { (Base) + 5 }, 1, NO, 0, 0 },             \
    { C(RM, IMM, NONE),    S8, { 0x80 }, 1, (Digit), 0, 0 },              \
    { C(RM, IMM, NONE),    SW, { 0x81 }, 1, (Digit), 0, 0 }

#define UNARY(Digit, Byte, Wide)                                          \
    { C(RM, NONE, NONE), S8, { (Byte) }, 1, (Digit), 0, 0 },              \
    { C(RM, NONE, NONE), SW, { (Wide) }, 1, (Digit), 0, 0 }

#define SHIFT(Digit)                                                      \
    { C(RM, ONE, NONE),  S8, { 0xD0 }, 1, (Digit), 0, 0 },                \
    { C(RM, CL, NONE),   S8, { 0xD2 }, 1, (Digit), 0, 0 },                \
    { C(RM, IMM8, NONE), S8, { 0xC0 }, 1, (Digit), 0, 0 },                \
    { C(RM, ONE, NONE),  SW, { 0xD1 }, 1, (Digit), 0, 0 },                \
    { C(RM, CL, NONE),   SW, { 0xD3 }, 1, (Digit), 0, 0 },                \
    { C(RM, IMM8, NONE), SW, { 0xC1 }, 1, (Digit), 0, 0 }

#define ALONE(Sizes, ...) { C(NONE, NONE, NONE), (Sizes), { __VA_ARGS__ }, \
    sizeof((uint8_t[]){ __VA_ARGS__ }), NO, 0, 0 }

/* A string instruction's four mnemonics, b w d q, one size each. */
#define STRING(Name, Byte)                                                    \
    static const BoxAsmForm Name##bForms[] = { ALONE(S8, (Byte)) };          \
    static const BoxAsmForm Name##wForms[] = { ALONE(S16, (Byte) + 1) };     \
    static const BoxAsmForm Name##dForms[] = { ALONE(S32, (Byte) + 1) };     \
    static const BoxAsmForm Name##qForms[] = { ALONE(S64, (Byte) + 1) };

/* bt, bts, btr, btc: the bit a register names, then the bit an imm8 names. */
#define BIT_TEST(Second, Digit)                                           \
    { C(RM, REG, NONE),  SW, { 0x0F, (Second) }, 2, R, 0, 0 },            \
    { C(RM, IMM8, NONE), SW, { 0x0F, 0xBA }, 2, (Digit), 0, 0 }

/* A register filled from an r/m of its size: bsf, bsr, popcnt, lzcnt, tzcnt. */
#define SCAN(Prefix, Second) { C(REG, RM, NONE), SW, { 0x0F, (Second) }, 2, R, 0, (Prefix) }

/* A destination r/m and the register it is combined with: xadd, cmpxchg. */
#define EXCHANGE(Second)                                                  \
    { C(RM, REG, NONE), S8, { 0x0F, (Second) }, 2, R, 0, 0 },             \
    { C(RM, REG, NONE), SW, { 0x0F, (Second) + 1 }, 2, R, 0, 0 }

/* shld, shrd: the count an imm8, then cl. */
#define DOUBLE_SHIFT(Second)                                              \
    { C(RM, REG, IMM8), SW, { 0x0F, (Second) }, 2, R, 0, 0 },             \
    { C(RM, REG, CL),   SW, { 0x0F, (Second) + 1 }, 2, R, 0, 0 }

/* A rel8 branch with no rel32 form: loop and jrcxz. */
#define SHORT_ONLY(Prefix, Opcode)                                        \
    { C(REL, NONE, NONE), 0, { (Opcode) }, 1, NO, BOXASM_FORM_SHORT, (Prefix) }

static const BoxAsmForm AddForms[] = { ALU(0x00, 0) };
static const BoxAsmForm OrForms[]  = { ALU(0x08, 1) };
static const BoxAsmForm AdcForms[] = { ALU(0x10, 2) };
static const BoxAsmForm SbbForms[] = { ALU(0x18, 3) };
static const BoxAsmForm AndForms[] = { ALU(0x20, 4) };
static const BoxAsmForm SubForms[] = { ALU(0x28, 5) };
static const BoxAsmForm XorForms[] = { ALU(0x30, 6) };
static const BoxAsmForm CmpForms[] = { ALU(0x38, 7) };

static const BoxAsmForm TestForms[] = {
    { C(RM, REG, NONE),  S8, { 0x84 }, 1, R, 0, 0 },
    { C(RM, REG, NONE),  SW, { 0x85 }, 1, R, 0, 0 },
    { C(REG, RM, NONE),  S8, { 0x84 }, 1, R, 0, 0 },
    { C(REG, RM, NONE),  SW, { 0x85 }, 1, R, 0, 0 },
    { C(ACC, IMM, NONE), S8, { 0xA8 }, 1, NO, 0, 0 },
    { C(ACC, IMM, NONE), SW, { 0xA9 }, 1, NO, 0, 0 },
    { C(RM, IMM, NONE),  S8, { 0xF6 }, 1, 0, 0, 0 },
    { C(RM, IMM, NONE),  SW, { 0xF7 }, 1, 0, 0, 0 },
};

static const BoxAsmForm MovForms[] = {
    { C(RM, REG, NONE),      S8,        { 0x88 }, 1, R, 0, 0 },
    { C(RM, REG, NONE),      SW,        { 0x89 }, 1, R, 0, 0 },
    { C(REG, RM, NONE),      S8,        { 0x8A }, 1, R, 0, 0 },
    { C(REG, RM, NONE),      SW,        { 0x8B }, 1, R, 0, 0 },
    { C(REG, IMM, NONE),     S8,        { 0xB0 }, 1, NO, BOXASM_FORM_PLUS_REG, 0 },
    { C(REG, IMM, NONE),     S16 | S32, { 0xB8 }, 1, NO, BOXASM_FORM_PLUS_REG, 0 },
    { C(REG, IMM_U32, NONE), S64,       { 0xB8 }, 1, NO,
      BOXASM_FORM_PLUS_REG | BOXASM_FORM_AS32, 0 },
    { C(REG, IMM_S32, NONE), S64,       { 0xC7 }, 1, 0, 0, 0 },
    { C(REG, IMM64, NONE),   S64,       { 0xB8 }, 1, NO, BOXASM_FORM_PLUS_REG, 0 },
    { C(RM, IMM, NONE),      S8,        { 0xC6 }, 1, 0, 0, 0 },
    { C(RM, IMM, NONE),      SW,        { 0xC7 }, 1, 0, 0, 0 },
};

static const BoxAsmForm LeaForms[] = {
    { C(REG, ADDRESS, NONE), SW, { 0x8D }, 1, R, 0, 0 },
};

static const BoxAsmForm XchgForms[] = {
    { C(ACC, REG, NONE), SW, { 0x90 }, 1, NO, BOXASM_FORM_PLUS_REG | BOXASM_FORM_NOT_EAX, 0 },
    { C(REG, ACC, NONE), SW, { 0x90 }, 1, NO, BOXASM_FORM_PLUS_REG | BOXASM_FORM_NOT_EAX, 0 },
    { C(REG, RM, NONE),  S8, { 0x86 }, 1, R, 0, 0 },
    { C(REG, RM, NONE),  SW, { 0x87 }, 1, R, 0, 0 },
    { C(RM, REG, NONE),  S8, { 0x86 }, 1, R, 0, 0 },
    { C(RM, REG, NONE),  SW, { 0x87 }, 1, R, 0, 0 },
};

static const BoxAsmForm PushForms[] = {
    { C(REG, NONE, NONE),   S64, { 0x50 }, 1, NO, BOXASM_FORM_PLUS_REG | BOXASM_FORM_DEFAULT64, 0 },
    { C(MEM, NONE, NONE),   S64, { 0xFF }, 1, 6,  BOXASM_FORM_DEFAULT64, 0 },
    { C(IMM8S, NONE, NONE), S64, { 0x6A }, 1, NO, BOXASM_FORM_DEFAULT64, 0 },
    { C(IMM, NONE, NONE),   S64, { 0x68 }, 1, NO, BOXASM_FORM_DEFAULT64, 0 },
};

static const BoxAsmForm PopForms[] = {
    { C(REG, NONE, NONE), S64, { 0x58 }, 1, NO, BOXASM_FORM_PLUS_REG | BOXASM_FORM_DEFAULT64, 0 },
    { C(MEM, NONE, NONE), S64, { 0x8F }, 1, 0,  BOXASM_FORM_DEFAULT64, 0 },
};

static const BoxAsmForm IncForms[]  = { UNARY(0, 0xFE, 0xFF) };
static const BoxAsmForm DecForms[]  = { UNARY(1, 0xFE, 0xFF) };
static const BoxAsmForm NotForms[]  = { UNARY(2, 0xF6, 0xF7) };
static const BoxAsmForm NegForms[]  = { UNARY(3, 0xF6, 0xF7) };
static const BoxAsmForm MulForms[]  = { UNARY(4, 0xF6, 0xF7) };
static const BoxAsmForm DivForms[]  = { UNARY(6, 0xF6, 0xF7) };
static const BoxAsmForm IdivForms[] = { UNARY(7, 0xF6, 0xF7) };

static const BoxAsmForm ImulForms[] = {
    UNARY(5, 0xF6, 0xF7),
    { C(REG, RM, NONE),  SW, { 0x0F, 0xAF }, 2, R, 0, 0 },
    { C(REG, RM, IMM8S), SW, { 0x6B }, 1, R, 0, 0 },
    { C(REG, RM, IMM),   SW, { 0x69 }, 1, R, 0, 0 },
};

static const BoxAsmForm RolForms[] = { SHIFT(0) };
static const BoxAsmForm RorForms[] = { SHIFT(1) };
static const BoxAsmForm RclForms[] = { SHIFT(2) };
static const BoxAsmForm RcrForms[] = { SHIFT(3) };
static const BoxAsmForm ShlForms[] = { SHIFT(4) };
static const BoxAsmForm ShrForms[] = { SHIFT(5) };
static const BoxAsmForm SarForms[] = { SHIFT(7) };

static const BoxAsmForm JmpForms[] = {
    { C(REL, NONE, NONE), 0,   { 0xEB }, 1, NO, BOXASM_FORM_SHORT, 0 },
    { C(REL, NONE, NONE), 0,   { 0xE9 }, 1, NO, 0, 0 },
    { C(RM, NONE, NONE),  S64, { 0xFF }, 1, 4,  BOXASM_FORM_DEFAULT64, 0 },
};

static const BoxAsmForm JccForms[] = {
    { C(REL, NONE, NONE), 0, { 0x70 },       1, NO, BOXASM_FORM_SHORT | BOXASM_FORM_CONDITION, 0 },
    { C(REL, NONE, NONE), 0, { 0x0F, 0x80 }, 2, NO, BOXASM_FORM_CONDITION, 0 },
};

static const BoxAsmForm CallForms[] = {
    { C(REL, NONE, NONE), 0,   { 0xE8 }, 1, NO, 0, 0 },
    { C(RM, NONE, NONE),  S64, { 0xFF }, 1, 2,  BOXASM_FORM_DEFAULT64, 0 },
};

static const BoxAsmForm SetccForms[] = {
    { C(RM, NONE, NONE), S8, { 0x0F, 0x90 }, 2, 0, BOXASM_FORM_CONDITION, 0 },
};

static const BoxAsmForm CmovccForms[] = {
    { C(REG, RM, NONE), SW, { 0x0F, 0x40 }, 2, R, BOXASM_FORM_CONDITION, 0 },
};

static const BoxAsmForm MovzxForms[] = {
    { C(REG, RM8, NONE),  SW,        { 0x0F, 0xB6 }, 2, R, 0, 0 },
    { C(REG, RM16, NONE), S32 | S64, { 0x0F, 0xB7 }, 2, R, 0, 0 },
};

static const BoxAsmForm MovsxForms[] = {
    { C(REG, RM8, NONE),  SW,        { 0x0F, 0xBE }, 2, R, 0, 0 },
    { C(REG, RM16, NONE), S32 | S64, { 0x0F, 0xBF }, 2, R, 0, 0 },
};

static const BoxAsmForm MovsxdForms[] = {
    { C(REG, RM32, NONE), S64, { 0x63 }, 1, R, 0, 0 },
};

static const BoxAsmForm RetForms[] = {
    ALONE(0, 0xC3),
    { C(IMM16, NONE, NONE), 0, { 0xC2 }, 1, NO, 0, 0 },
};

static const BoxAsmForm NopForms[]     = { ALONE(0, 0x90) };
static const BoxAsmForm Int3Forms[]    = { ALONE(0, 0xCC) };
static const BoxAsmForm LeaveForms[]   = { ALONE(0, 0xC9) };
static const BoxAsmForm CqoForms[]     = { ALONE(S64, 0x99) };
static const BoxAsmForm CdqForms[]     = { ALONE(S32, 0x99) };
static const BoxAsmForm CwdForms[]     = { ALONE(S16, 0x99) };
static const BoxAsmForm CdqeForms[]    = { ALONE(S64, 0x98) };
static const BoxAsmForm CwdeForms[]    = { ALONE(S32, 0x98) };
static const BoxAsmForm CbwForms[]     = { ALONE(S16, 0x98) };
static const BoxAsmForm PushfForms[]   = { ALONE(0, 0x9C) };
static const BoxAsmForm PopfForms[]    = { ALONE(0, 0x9D) };
static const BoxAsmForm CldForms[]     = { ALONE(0, 0xFC) };
static const BoxAsmForm StdForms[]     = { ALONE(0, 0xFD) };
static const BoxAsmForm Endbr64Forms[] = { ALONE(0, 0xF3, 0x0F, 0x1E, 0xFA) };
static const BoxAsmForm PauseForms[]   = { ALONE(0, 0xF3, 0x90) };
static const BoxAsmForm Ud2Forms[]     = { ALONE(0, 0x0F, 0x0B) };
static const BoxAsmForm ClcForms[]     = { ALONE(0, 0xF8) };
static const BoxAsmForm StcForms[]     = { ALONE(0, 0xF9) };
static const BoxAsmForm CmcForms[]     = { ALONE(0, 0xF5) };
static const BoxAsmForm CpuidForms[]   = { ALONE(0, 0x0F, 0xA2) };
static const BoxAsmForm RdtscForms[]   = { ALONE(0, 0x0F, 0x31) };
static const BoxAsmForm RdtscpForms[]  = { ALONE(0, 0x0F, 0x01, 0xF9) };
static const BoxAsmForm LfenceForms[]  = { ALONE(0, 0x0F, 0xAE, 0xE8) };
static const BoxAsmForm MfenceForms[]  = { ALONE(0, 0x0F, 0xAE, 0xF0) };
static const BoxAsmForm SfenceForms[]  = { ALONE(0, 0x0F, 0xAE, 0xF8) };

STRING(Movs, 0xA4)
STRING(Cmps, 0xA6)
STRING(Stos, 0xAA)
STRING(Lods, 0xAC)
STRING(Scas, 0xAE)

static const BoxAsmForm BtForms[]  = { BIT_TEST(0xA3, 4) };
static const BoxAsmForm BtsForms[] = { BIT_TEST(0xAB, 5) };
static const BoxAsmForm BtrForms[] = { BIT_TEST(0xB3, 6) };
static const BoxAsmForm BtcForms[] = { BIT_TEST(0xBB, 7) };

static const BoxAsmForm BsfForms[]    = { SCAN(0, 0xBC) };
static const BoxAsmForm BsrForms[]    = { SCAN(0, 0xBD) };
static const BoxAsmForm PopcntForms[] = { SCAN(0xF3, 0xB8) };
static const BoxAsmForm LzcntForms[]  = { SCAN(0xF3, 0xBD) };
static const BoxAsmForm TzcntForms[]  = { SCAN(0xF3, 0xBC) };

static const BoxAsmForm BswapForms[] = {
    { C(REG, NONE, NONE), S32 | S64, { 0x0F, 0xC8 }, 2, NO, BOXASM_FORM_PLUS_REG, 0 },
};

static const BoxAsmForm ShldForms[] = { DOUBLE_SHIFT(0xA4) };
static const BoxAsmForm ShrdForms[] = { DOUBLE_SHIFT(0xAC) };

static const BoxAsmForm XaddForms[]    = { EXCHANGE(0xC0) };
static const BoxAsmForm CmpxchgForms[] = { EXCHANGE(0xB0) };

static const BoxAsmForm Cmpxchg8bForms[] = {
    { C(MEM, NONE, NONE), S64, { 0x0F, 0xC7 }, 2, 1, BOXASM_FORM_DEFAULT64, 0 },
};

static const BoxAsmForm Cmpxchg16bForms[] = {
    { C(MEM, NONE, NONE), S128, { 0x0F, 0xC7 }, 2, 1, BOXASM_FORM_REX_W, 0 },
};

static const BoxAsmForm RdrandForms[] = {
    { C(REG, NONE, NONE), SW, { 0x0F, 0xC7 }, 2, 6, 0, 0 },
};

static const BoxAsmForm RdseedForms[] = {
    { C(REG, NONE, NONE), SW, { 0x0F, 0xC7 }, 2, 7, 0, 0 },
};

static const BoxAsmForm LoopForms[]   = { SHORT_ONLY(0, 0xE2) };
static const BoxAsmForm LoopeForms[]  = { SHORT_ONLY(0, 0xE1) };
static const BoxAsmForm LoopneForms[] = { SHORT_ONLY(0, 0xE0) };
static const BoxAsmForm JrcxzForms[]  = { SHORT_ONLY(0, 0xE3) };
static const BoxAsmForm JecxzForms[]  = { SHORT_ONLY(0x67, 0xE3) };

#define INSTRUCTION(Name, Forms, Kind, Conditional, Swap, Takes)          \
    { Name, Forms, sizeof(Forms) / sizeof(Forms[0]), Kind, Conditional, Swap, Takes }
#define PLAIN(Name, Forms) INSTRUCTION(Name, Forms, BOXASM_KIND_PLAIN, 0, 0, 0)
#define LOCKED(Name, Forms) INSTRUCTION(Name, Forms, BOXASM_KIND_PLAIN, 0, 0, BOXASM_TAKES_LOCK)
#define REPEATED(Name, Forms, Takes) INSTRUCTION(Name, Forms, BOXASM_KIND_PLAIN, 0, 0, Takes)
#define SWAPS(Name, Forms)                                                \
    INSTRUCTION(Name, Forms, BOXASM_KIND_PLAIN, 0, 0, BOXASM_TAKES_LOCK | BOXASM_TAKES_BOTH)
#define SHORT_BRANCH(Name, Forms)                                         \
    INSTRUCTION(Name, Forms, BOXASM_KIND_SHORT_BRANCH, 0, 0, 0)

static const BoxAsmInstruction BoxAsmInstructions[] = {
    LOCKED("add", AddForms),  LOCKED("or", OrForms),    LOCKED("adc", AdcForms),
    LOCKED("sbb", SbbForms),  LOCKED("and", AndForms),  LOCKED("sub", SubForms),
    LOCKED("xor", XorForms),  PLAIN("cmp", CmpForms),   PLAIN("test", TestForms),
    PLAIN("mov", MovForms),   PLAIN("lea", LeaForms),   SWAPS("xchg", XchgForms),
    PLAIN("push", PushForms), PLAIN("pop", PopForms),
    LOCKED("inc", IncForms),  LOCKED("dec", DecForms),  LOCKED("not", NotForms),
    LOCKED("neg", NegForms),  PLAIN("mul", MulForms),   PLAIN("div", DivForms),
    PLAIN("idiv", IdivForms),
    INSTRUCTION("imul", ImulForms, BOXASM_KIND_PLAIN, 0, 1, 0),
    PLAIN("rol", RolForms),   PLAIN("ror", RorForms),   PLAIN("rcl", RclForms),
    PLAIN("rcr", RcrForms),   PLAIN("shl", ShlForms),   PLAIN("sal", ShlForms),
    PLAIN("shr", ShrForms),   PLAIN("sar", SarForms),
    INSTRUCTION("jmp", JmpForms, BOXASM_KIND_JUMP, 0, 0, 0),
    INSTRUCTION("call", CallForms, BOXASM_KIND_CALL, 0, 0, 0),
    PLAIN("movzx", MovzxForms), PLAIN("movsx", MovsxForms), PLAIN("movsxd", MovsxdForms),
    PLAIN("ret", RetForms),   PLAIN("nop", NopForms),   PLAIN("int3", Int3Forms),
    PLAIN("leave", LeaveForms),
    PLAIN("cqo", CqoForms),   PLAIN("cdq", CdqForms),   PLAIN("cwd", CwdForms),
    PLAIN("cdqe", CdqeForms), PLAIN("cwde", CwdeForms), PLAIN("cbw", CbwForms),
    PLAIN("pushfq", PushfForms), PLAIN("pushf", PushfForms),
    PLAIN("popfq", PopfForms),   PLAIN("popf", PopfForms),
    PLAIN("cld", CldForms),   PLAIN("std", StdForms),   PLAIN("endbr64", Endbr64Forms),
    PLAIN("pause", PauseForms), PLAIN("ud2", Ud2Forms),
    PLAIN("clc", ClcForms),   PLAIN("stc", StcForms),   PLAIN("cmc", CmcForms),
    PLAIN("cpuid", CpuidForms), PLAIN("rdtsc", RdtscForms), PLAIN("rdtscp", RdtscpForms),
    PLAIN("lfence", LfenceForms), PLAIN("mfence", MfenceForms), PLAIN("sfence", SfenceForms),
    REPEATED("movsb", MovsbForms, BOXASM_TAKES_REP),
    REPEATED("movsw", MovswForms, BOXASM_TAKES_REP),
    REPEATED("movsd", MovsdForms, BOXASM_TAKES_REP),
    REPEATED("movsq", MovsqForms, BOXASM_TAKES_REP),
    REPEATED("cmpsb", CmpsbForms, BOXASM_TAKES_REPE),
    REPEATED("cmpsw", CmpswForms, BOXASM_TAKES_REPE),
    REPEATED("cmpsd", CmpsdForms, BOXASM_TAKES_REPE),
    REPEATED("cmpsq", CmpsqForms, BOXASM_TAKES_REPE),
    REPEATED("stosb", StosbForms, BOXASM_TAKES_REP),
    REPEATED("stosw", StoswForms, BOXASM_TAKES_REP),
    REPEATED("stosd", StosdForms, BOXASM_TAKES_REP),
    REPEATED("stosq", StosqForms, BOXASM_TAKES_REP),
    REPEATED("lodsb", LodsbForms, BOXASM_TAKES_REP),
    REPEATED("lodsw", LodswForms, BOXASM_TAKES_REP),
    REPEATED("lodsd", LodsdForms, BOXASM_TAKES_REP),
    REPEATED("lodsq", LodsqForms, BOXASM_TAKES_REP),
    REPEATED("scasb", ScasbForms, BOXASM_TAKES_REPE),
    REPEATED("scasw", ScaswForms, BOXASM_TAKES_REPE),
    REPEATED("scasd", ScasdForms, BOXASM_TAKES_REPE),
    REPEATED("scasq", ScasqForms, BOXASM_TAKES_REPE),
    PLAIN("bt", BtForms),     LOCKED("bts", BtsForms),  LOCKED("btr", BtrForms),
    LOCKED("btc", BtcForms),
    PLAIN("bsf", BsfForms),   PLAIN("bsr", BsrForms),   PLAIN("popcnt", PopcntForms),
    PLAIN("lzcnt", LzcntForms), PLAIN("tzcnt", TzcntForms),
    PLAIN("bswap", BswapForms), PLAIN("shld", ShldForms), PLAIN("shrd", ShrdForms),
    LOCKED("xadd", XaddForms), LOCKED("cmpxchg", CmpxchgForms),
    LOCKED("cmpxchg8b", Cmpxchg8bForms), LOCKED("cmpxchg16b", Cmpxchg16bForms),
    PLAIN("rdrand", RdrandForms), PLAIN("rdseed", RdseedForms),
    SHORT_BRANCH("loop", LoopForms),
    SHORT_BRANCH("loope", LoopeForms),   SHORT_BRANCH("loopz", LoopeForms),
    SHORT_BRANCH("loopne", LoopneForms), SHORT_BRANCH("loopnz", LoopneForms),
    SHORT_BRANCH("jrcxz", JrcxzForms),   SHORT_BRANCH("jecxz", JecxzForms),
};

static const BoxAsmInstruction BoxAsmJcc =
    INSTRUCTION("j", JccForms, BOXASM_KIND_JUMP, 1, 0, 0);
static const BoxAsmInstruction BoxAsmSetcc =
    INSTRUCTION("set", SetccForms, BOXASM_KIND_PLAIN, 1, 0, 0);
static const BoxAsmInstruction BoxAsmCmovcc =
    INSTRUCTION("cmov", CmovccForms, BOXASM_KIND_PLAIN, 1, 0, 0);

static const struct {
    const char *Suffix;
    uint8_t     Code;
} BoxAsmConditions[] = {
    { "o", 0 },   { "no", 1 },  { "b", 2 },   { "c", 2 },   { "nae", 2 },
    { "ae", 3 },  { "nb", 3 },  { "nc", 3 },  { "e", 4 },   { "z", 4 },
    { "ne", 5 },  { "nz", 5 },  { "be", 6 },  { "na", 6 },  { "a", 7 },
    { "nbe", 7 }, { "s", 8 },   { "ns", 9 },  { "p", 10 },  { "pe", 10 },
    { "np", 11 }, { "po", 11 }, { "l", 12 },  { "nge", 12 }, { "ge", 13 },
    { "nl", 13 }, { "le", 14 }, { "ng", 14 }, { "g", 15 },  { "nle", 15 },
};

/* Name, lowered, equals Word. */
static int BoxAsmNameEquals(const char *Name, size_t Length, const char *Word)
{
    size_t Index;

    if (strlen(Word) != Length)
        return 0;
    for (Index = 0; Index < Length; Index++) {
        char Ch = Name[Index];
        if (Ch >= 'A' && Ch <= 'Z')
            Ch = (char)(Ch - 'A' + 'a');
        if (Ch != Word[Index])
            return 0;
    }
    return 1;
}

static const BoxAsmInstruction *BoxAsmConditional(const BoxAsmInstruction *Family,
                                                  const char *Name, size_t Length,
                                                  uint8_t *Condition)
{
    size_t Prefix = strlen(Family->Name);
    size_t Index;

    if (Length <= Prefix || !BoxAsmNameEquals(Name, Prefix, Family->Name))
        return NULL;
    for (Index = 0; Index < sizeof(BoxAsmConditions) / sizeof(BoxAsmConditions[0]); Index++) {
        if (BoxAsmNameEquals(Name + Prefix, Length - Prefix, BoxAsmConditions[Index].Suffix)) {
            *Condition = BoxAsmConditions[Index].Code;
            return Family;
        }
    }
    return NULL;
}

const BoxAsmInstruction *BoxAsmFindInstruction(const char *Name, size_t Length,
                                               uint8_t *Condition)
{
    const BoxAsmInstruction *Found;
    size_t Index;

    *Condition = 0;
    for (Index = 0; Index < sizeof(BoxAsmInstructions) / sizeof(BoxAsmInstructions[0]); Index++)
        if (BoxAsmNameEquals(Name, Length, BoxAsmInstructions[Index].Name))
            return &BoxAsmInstructions[Index];
    Found = BoxAsmConditional(&BoxAsmCmovcc, Name, Length, Condition);
    if (!Found)
        Found = BoxAsmConditional(&BoxAsmSetcc, Name, Length, Condition);
    if (!Found)
        Found = BoxAsmConditional(&BoxAsmJcc, Name, Length, Condition);
    return Found;
}

uint8_t BoxAsmFindPrefix(const char *Name, size_t Length)
{
    static const struct {
        const char *Word;
        uint8_t     Prefix;
    } Prefixes[] = {
        { "lock", BOXASM_PREFIX_LOCK },   { "rep", BOXASM_PREFIX_REP },
        { "repe", BOXASM_PREFIX_REPE },   { "repz", BOXASM_PREFIX_REPE },
        { "repne", BOXASM_PREFIX_REPNE }, { "repnz", BOXASM_PREFIX_REPNE },
    };
    size_t Index;

    for (Index = 0; Index < sizeof(Prefixes) / sizeof(Prefixes[0]); Index++)
        if (BoxAsmNameEquals(Name, Length, Prefixes[Index].Word))
            return Prefixes[Index].Prefix;
    return BOXASM_PREFIX_NONE;
}
