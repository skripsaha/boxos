#include "boxasm_isa_table.h"

#include "box/string.h"

/* Each instruction's forms, in the order NASM chooses among them: a register
 * pair as r/m,reg; an immediate as imm8 sign-extended when it fits, then the
 * accumulator's short form, then the general one; mov r64,imm as mov r32 when
 * the value fits 32 bits unsigned, then sign-extended, then movabs. */

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

/* A string instruction's b, w and q mnemonics, one size each; its d form is
 * written out beside it, because movsd and cmpsd are also SSE2's. */
#define STRING(Name, Byte)                                                    \
    static const BoxAsmForm Name##bForms[] = { ALONE(S8, (Byte)) };          \
    static const BoxAsmForm Name##wForms[] = { ALONE(S16, (Byte) + 1) };     \
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
static const BoxAsmForm LahfForms[]    = { ALONE(0, 0x9F) };
static const BoxAsmForm SahfForms[]    = { ALONE(0, 0x9E) };
static const BoxAsmForm XgetbvForms[]  = { ALONE(0, 0x0F, 0x01, 0xD0) };

STRING(Movs, 0xA4)
STRING(Cmps, 0xA6)
STRING(Stos, 0xAA)
STRING(Lods, 0xAC)
STRING(Scas, 0xAE)
static const BoxAsmForm StosdForms[] = { ALONE(S32, 0xAB) };
static const BoxAsmForm LodsdForms[] = { ALONE(S32, 0xAD) };
static const BoxAsmForm ScasdForms[] = { ALONE(S32, 0xAF) };

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

/* A load or a store with its bytes reversed: one operand memory. */
static const BoxAsmForm MovbeForms[] = {
    { C(REG, MEM, NONE), SW, { 0x0F, 0x38, 0xF0 }, 3, R, 0, 0 },
    { C(MEM, REG, NONE), SW, { 0x0F, 0x38, 0xF1 }, 3, R, 0, 0 },
};

static const BoxAsmForm LoopForms[]   = { SHORT_ONLY(0, 0xE2) };
static const BoxAsmForm LoopeForms[]  = { SHORT_ONLY(0, 0xE1) };
static const BoxAsmForm LoopneForms[] = { SHORT_ONLY(0, 0xE0) };
static const BoxAsmForm JrcxzForms[]  = { SHORT_ONLY(0, 0xE3) };
static const BoxAsmForm JecxzForms[]  = { SHORT_ONLY(0x67, 0xE3) };

/* ---- SSE and SSE2 -------------------------------------------------------
 * Every form is 0F and a second byte, after the prefix that picks the type
 * (none: packed single, 66: packed double or integer, F3: scalar single,
 * F2: scalar double). The xmm register is ModRM.reg; the other operand,
 * register or memory, is ModRM.rm. */

#define SSE(Prefix, Second, Source)                                       \
    { C(XMM, Source, NONE), 0, { 0x0F, (Second) }, 2, R, 0, (Prefix) }
#define SSE_IMM(Prefix, Second, Source)                                   \
    { C(XMM, Source, IMM8), 0, { 0x0F, (Second) }, 2, R, 0, (Prefix) }
#define SSE_STORE(Prefix, Second, Destination)                            \
    { C(Destination, XMM, NONE), 0, { 0x0F, (Second) }, 2, R, 0, (Prefix) }
/* A shift of an xmm register by an imm8: 66 0F 71/72/73 with a digit. */
#define SSE_SHIFT_IMM(Second, Digit)                                      \
    { C(XMM_R, IMM8, NONE), 0, { 0x0F, (Second) }, 2, (Digit), 0, 0x66 }
/* A compare written with its predicate in the name (cmpltps). */
#define SSE_PREDICATE(Prefix, Source)                                     \
    { C(XMM, Source, NONE), 0, { 0x0F, 0xC2 }, 2, R, BOXASM_FORM_PREDICATE, (Prefix) }

/* The four types of one float operation: ps, pd, ss, sd. */
#define SSE_FLOAT(Name, Second)                                                     \
    static const BoxAsmForm Name##psForms[] = { SSE(0, (Second), XMM_M128) };       \
    static const BoxAsmForm Name##pdForms[] = { SSE(0x66, (Second), XMM_M128) };    \
    static const BoxAsmForm Name##ssForms[] = { SSE(0xF3, (Second), XMM_M32) };     \
    static const BoxAsmForm Name##sdForms[] = { SSE(0xF2, (Second), XMM_M64) };
/* The packed types of one: ps and pd. */
#define SSE_PACKED(Name, Second)                                                    \
    static const BoxAsmForm Name##psForms[] = { SSE(0, (Second), XMM_M128) };       \
    static const BoxAsmForm Name##pdForms[] = { SSE(0x66, (Second), XMM_M128) };
/* MMX: the same operation on an mm register and mm/m64, with no prefix. */
#define MMX(Second, Source)                                               \
    { C(MM, Source, NONE), 0, { 0x0F, (Second) }, 2, R, 0, 0 }
#define MMX_SHIFT_IMM(Second, Digit)                                      \
    { C(MM_R, IMM8, NONE), 0, { 0x0F, (Second) }, 2, (Digit), 0, 0 }
/* A packed integer operation of SSE2, 66 0F with xmm/m128, and of MMX. */
#define SSE_INTEGER(Name, Second)                                                   \
    static const BoxAsmForm Name##Forms[] = {                                      \
        SSE(0x66, (Second), XMM_M128), MMX((Second), MM_M64) };
/* The low halves interleaved: MMX reads only the 4 bytes it takes. */
#define SSE_INTEGER_LOW(Name, Second)                                               \
    static const BoxAsmForm Name##Forms[] = {                                      \
        SSE(0x66, (Second), XMM_M128), MMX((Second), MM_M32) };
/* SSE2's own, of no MMX form. */
#define SSE_INTEGER_XMM(Name, Second)                                               \
    static const BoxAsmForm Name##Forms[] = { SSE(0x66, (Second), XMM_M128) };

SSE_FLOAT(Add, 0x58)
SSE_FLOAT(Sub, 0x5C)
SSE_FLOAT(Mul, 0x59)
SSE_FLOAT(Div, 0x5E)
SSE_FLOAT(Min, 0x5D)
SSE_FLOAT(Max, 0x5F)
SSE_FLOAT(Sqrt, 0x51)
SSE_PACKED(And, 0x54)
SSE_PACKED(Andn, 0x55)
SSE_PACKED(Or, 0x56)
SSE_PACKED(Xor, 0x57)
SSE_PACKED(Unpckl, 0x14)
SSE_PACKED(Unpckh, 0x15)

static const BoxAsmForm RcppsForms[]   = { SSE(0, 0x53, XMM_M128) };
static const BoxAsmForm RcpssForms[]   = { SSE(0xF3, 0x53, XMM_M32) };
static const BoxAsmForm RsqrtpsForms[] = { SSE(0, 0x52, XMM_M128) };
static const BoxAsmForm RsqrtssForms[] = { SSE(0xF3, 0x52, XMM_M32) };

static const BoxAsmForm CmppsForms[] = { SSE_IMM(0, 0xC2, XMM_M128) };
static const BoxAsmForm CmppdForms[] = { SSE_IMM(0x66, 0xC2, XMM_M128) };
static const BoxAsmForm CmpssForms[] = { SSE_IMM(0xF3, 0xC2, XMM_M32) };
static const BoxAsmForm CmpsdForms[] = { ALONE(S32, 0xA7), SSE_IMM(0xF2, 0xC2, XMM_M64) };
static const BoxAsmForm CmpPredicatePsForms[] = { SSE_PREDICATE(0, XMM_M128) };
static const BoxAsmForm CmpPredicatePdForms[] = { SSE_PREDICATE(0x66, XMM_M128) };
static const BoxAsmForm CmpPredicateSsForms[] = { SSE_PREDICATE(0xF3, XMM_M32) };
static const BoxAsmForm CmpPredicateSdForms[] = { SSE_PREDICATE(0xF2, XMM_M64) };
static const BoxAsmForm ComissForms[]  = { SSE(0, 0x2F, XMM_M32) };
static const BoxAsmForm ComisdForms[]  = { SSE(0x66, 0x2F, XMM_M64) };
static const BoxAsmForm UcomissForms[] = { SSE(0, 0x2E, XMM_M32) };
static const BoxAsmForm UcomisdForms[] = { SSE(0x66, 0x2E, XMM_M64) };
static const BoxAsmForm ShufpsForms[]  = { SSE_IMM(0, 0xC6, XMM_M128) };
static const BoxAsmForm ShufpdForms[]  = { SSE_IMM(0x66, 0xC6, XMM_M128) };

static const BoxAsmForm MovapsForms[] = { SSE(0, 0x28, XMM_M128), SSE_STORE(0, 0x29, XMM_M128) };
static const BoxAsmForm MovapdForms[] = { SSE(0x66, 0x28, XMM_M128), SSE_STORE(0x66, 0x29, XMM_M128) };
static const BoxAsmForm MovupsForms[] = { SSE(0, 0x10, XMM_U128), SSE_STORE(0, 0x11, XMM_U128) };
static const BoxAsmForm MovupdForms[] = { SSE(0x66, 0x10, XMM_U128), SSE_STORE(0x66, 0x11, XMM_U128) };
static const BoxAsmForm MovssForms[]  = { SSE(0xF3, 0x10, XMM_M32), SSE_STORE(0xF3, 0x11, XMM_M32) };
static const BoxAsmForm MovsdForms[]  = {
    ALONE(S32, 0xA5), SSE(0xF2, 0x10, XMM_M64), SSE_STORE(0xF2, 0x11, XMM_M64)
};
static const BoxAsmForm MovlpsForms[] = { SSE(0, 0x12, M64), SSE_STORE(0, 0x13, M64) };
static const BoxAsmForm MovhpsForms[] = { SSE(0, 0x16, M64), SSE_STORE(0, 0x17, M64) };
static const BoxAsmForm MovlpdForms[] = { SSE(0x66, 0x12, M64), SSE_STORE(0x66, 0x13, M64) };
static const BoxAsmForm MovhpdForms[] = { SSE(0x66, 0x16, M64), SSE_STORE(0x66, 0x17, M64) };
static const BoxAsmForm MovhlpsForms[] = { SSE(0, 0x12, XMM_R) };
static const BoxAsmForm MovlhpsForms[] = { SSE(0, 0x16, XMM_R) };
static const BoxAsmForm MovdqaForms[] = { SSE(0x66, 0x6F, XMM_M128), SSE_STORE(0x66, 0x7F, XMM_M128) };
static const BoxAsmForm MovdquForms[] = { SSE(0xF3, 0x6F, XMM_U128), SSE_STORE(0xF3, 0x7F, XMM_U128) };
static const BoxAsmForm MovdForms[] = {
    { C(XMM, RM, NONE), S32, { 0x0F, 0x6E }, 2, R, 0, 0x66 },
    { C(RM, XMM, NONE), S32, { 0x0F, 0x7E }, 2, R, 0, 0x66 },
    { C(MM, RM, NONE),  S32, { 0x0F, 0x6E }, 2, R, 0, 0 },
    { C(RM, MM, NONE),  S32, { 0x0F, 0x7E }, 2, R, 0, 0 },
};
/* movq: xmm and xmm/m64 by F3 0F 7E and 66 0F D6 first, as NASM chooses;
 * a general register by 66 REX.W 0F 6E and 7E. Of mm, mm/m64 by 0F 6F and
 * 7F, a general register by REX.W 0F 6E and 7E. */
static const BoxAsmForm MovqForms[] = {
    SSE(0xF3, 0x7E, XMM_M64),
    SSE_STORE(0x66, 0xD6, XMM_M64),
    { C(XMM, RM, NONE), S64, { 0x0F, 0x6E }, 2, R, 0, 0x66 },
    { C(RM, XMM, NONE), S64, { 0x0F, 0x7E }, 2, R, 0, 0x66 },
    MMX(0x6F, MM_M64),
    { C(MM_M64, MM, NONE), 0, { 0x0F, 0x7F }, 2, R, 0, 0 },
    { C(MM, RM, NONE),  S64, { 0x0F, 0x6E }, 2, R, 0, 0 },
    { C(RM, MM, NONE),  S64, { 0x0F, 0x7E }, 2, R, 0, 0 },
};
static const BoxAsmForm MovmskpsForms[] = { { C(REG, XMM_R, NONE), S32 | S64, { 0x0F, 0x50 }, 2, R, 0, 0 } };
static const BoxAsmForm MovmskpdForms[] = { { C(REG, XMM_R, NONE), S32 | S64, { 0x0F, 0x50 }, 2, R, 0, 0x66 } };
static const BoxAsmForm MovntpsForms[] = { SSE_STORE(0, 0x2B, M128) };
static const BoxAsmForm MovntpdForms[] = { SSE_STORE(0x66, 0x2B, M128) };
static const BoxAsmForm MovntdqForms[] = { SSE_STORE(0x66, 0xE7, M128) };
static const BoxAsmForm MovntiForms[] = { { C(MEM, REG, NONE), S32 | S64, { 0x0F, 0xC3 }, 2, R, 0, 0 } };
static const BoxAsmForm MaskmovdquForms[] = { SSE(0x66, 0xF7, XMM_R) };

static const BoxAsmForm Cvtsi2ssForms[] = { { C(XMM, RM, NONE), S32 | S64, { 0x0F, 0x2A }, 2, R, 0, 0xF3 } };
static const BoxAsmForm Cvtsi2sdForms[] = { { C(XMM, RM, NONE), S32 | S64, { 0x0F, 0x2A }, 2, R, 0, 0xF2 } };
static const BoxAsmForm Cvtss2siForms[]  = { { C(REG, XMM_M32, NONE), S32 | S64, { 0x0F, 0x2D }, 2, R, 0, 0xF3 } };
static const BoxAsmForm Cvttss2siForms[] = { { C(REG, XMM_M32, NONE), S32 | S64, { 0x0F, 0x2C }, 2, R, 0, 0xF3 } };
static const BoxAsmForm Cvtsd2siForms[]  = { { C(REG, XMM_M64, NONE), S32 | S64, { 0x0F, 0x2D }, 2, R, 0, 0xF2 } };
static const BoxAsmForm Cvttsd2siForms[] = { { C(REG, XMM_M64, NONE), S32 | S64, { 0x0F, 0x2C }, 2, R, 0, 0xF2 } };
static const BoxAsmForm Cvtss2sdForms[]  = { SSE(0xF3, 0x5A, XMM_M32) };
static const BoxAsmForm Cvtsd2ssForms[]  = { SSE(0xF2, 0x5A, XMM_M64) };
static const BoxAsmForm Cvtps2pdForms[]  = { SSE(0, 0x5A, XMM_M64) };
static const BoxAsmForm Cvtpd2psForms[]  = { SSE(0x66, 0x5A, XMM_M128) };
static const BoxAsmForm Cvtdq2psForms[]  = { SSE(0, 0x5B, XMM_M128) };
static const BoxAsmForm Cvtps2dqForms[]  = { SSE(0x66, 0x5B, XMM_M128) };
static const BoxAsmForm Cvttps2dqForms[] = { SSE(0xF3, 0x5B, XMM_M128) };
static const BoxAsmForm Cvtdq2pdForms[]  = { SSE(0xF3, 0xE6, XMM_M64) };
static const BoxAsmForm Cvtpd2dqForms[]  = { SSE(0xF2, 0xE6, XMM_M128) };
static const BoxAsmForm Cvttpd2dqForms[] = { SSE(0x66, 0xE6, XMM_M128) };

SSE_INTEGER(Paddb, 0xFC)   SSE_INTEGER(Paddw, 0xFD)   SSE_INTEGER(Paddd, 0xFE)
SSE_INTEGER(Paddq, 0xD4)   SSE_INTEGER(Psubb, 0xF8)   SSE_INTEGER(Psubw, 0xF9)
SSE_INTEGER(Psubd, 0xFA)   SSE_INTEGER(Psubq, 0xFB)   SSE_INTEGER(Paddsb, 0xEC)
SSE_INTEGER(Paddsw, 0xED)  SSE_INTEGER(Paddusb, 0xDC) SSE_INTEGER(Paddusw, 0xDD)
SSE_INTEGER(Psubsb, 0xE8)  SSE_INTEGER(Psubsw, 0xE9)  SSE_INTEGER(Psubusb, 0xD8)
SSE_INTEGER(Psubusw, 0xD9) SSE_INTEGER(Pmullw, 0xD5)  SSE_INTEGER(Pmulhw, 0xE5)
SSE_INTEGER(Pmulhuw, 0xE4) SSE_INTEGER(Pmuludq, 0xF4) SSE_INTEGER(Pmaddwd, 0xF5)
SSE_INTEGER(Pavgb, 0xE0)   SSE_INTEGER(Pavgw, 0xE3)   SSE_INTEGER(Pmaxsw, 0xEE)
SSE_INTEGER(Pmaxub, 0xDE)  SSE_INTEGER(Pminsw, 0xEA)  SSE_INTEGER(Pminub, 0xDA)
SSE_INTEGER(Psadbw, 0xF6)  SSE_INTEGER(Pand, 0xDB)    SSE_INTEGER(Pandn, 0xDF)
SSE_INTEGER(Por, 0xEB)     SSE_INTEGER(Pxor, 0xEF)    SSE_INTEGER(Pcmpeqb, 0x74)
SSE_INTEGER(Pcmpeqw, 0x75) SSE_INTEGER(Pcmpeqd, 0x76) SSE_INTEGER(Pcmpgtb, 0x64)
SSE_INTEGER(Pcmpgtw, 0x65) SSE_INTEGER(Pcmpgtd, 0x66) SSE_INTEGER(Packsswb, 0x63)
SSE_INTEGER(Packssdw, 0x6B) SSE_INTEGER(Packuswb, 0x67) SSE_INTEGER_LOW(Punpcklbw, 0x60)
SSE_INTEGER_LOW(Punpcklwd, 0x61) SSE_INTEGER_LOW(Punpckldq, 0x62)
SSE_INTEGER_XMM(Punpcklqdq, 0x6C)
SSE_INTEGER(Punpckhbw, 0x68) SSE_INTEGER(Punpckhwd, 0x69) SSE_INTEGER(Punpckhdq, 0x6A)
SSE_INTEGER_XMM(Punpckhqdq, 0x6D)

/* A shift by a count in a register or memory, or in an imm8; of xmm and mm. */
#define SHIFTS(Name, Second, Immediate, Digit)                                      \
    static const BoxAsmForm Name##Forms[] = {                                      \
        SSE(0x66, (Second), XMM_M128), SSE_SHIFT_IMM((Immediate), (Digit)),        \
        MMX((Second), MM_M64), MMX_SHIFT_IMM((Immediate), (Digit)) };
SHIFTS(Psllw, 0xF1, 0x71, 6)   SHIFTS(Pslld, 0xF2, 0x72, 6)   SHIFTS(Psllq, 0xF3, 0x73, 6)
SHIFTS(Psrlw, 0xD1, 0x71, 2)   SHIFTS(Psrld, 0xD2, 0x72, 2)   SHIFTS(Psrlq, 0xD3, 0x73, 2)
SHIFTS(Psraw, 0xE1, 0x71, 4)   SHIFTS(Psrad, 0xE2, 0x72, 4)
static const BoxAsmForm PslldqForms[] = { SSE_SHIFT_IMM(0x73, 7) };
static const BoxAsmForm PsrldqForms[] = { SSE_SHIFT_IMM(0x73, 3) };
static const BoxAsmForm PshufdForms[]  = { SSE_IMM(0x66, 0x70, XMM_M128) };
static const BoxAsmForm PshufhwForms[] = { SSE_IMM(0xF3, 0x70, XMM_M128) };
static const BoxAsmForm PshuflwForms[] = { SSE_IMM(0xF2, 0x70, XMM_M128) };
/* pextrw and pinsrw take a 64-bit register as NASM does, written without REX.W;
 * pextrw to memory is SSE4.1's. */
static const BoxAsmForm PextrwForms[] = {
    { C(REG, XMM_R, IMM8), S32, { 0x0F, 0xC5 }, 2, R, 0, 0x66 },
    { C(REG, XMM_R, IMM8), S64, { 0x0F, 0xC5 }, 2, R, BOXASM_FORM_DEFAULT64, 0x66 },
    { C(M16, XMM, IMM8),   0,   { 0x0F, 0x3A, 0x15 }, 3, R, 0, 0x66 },
    { C(REG, MM_R, IMM8),  S32, { 0x0F, 0xC5 }, 2, R, 0, 0 },
};
/* Of mm, as NASM takes them, only a 32-bit register. */
static const BoxAsmForm PinsrwForms[] = {
    { C(XMM, R32_M16, IMM8), 0,   { 0x0F, 0xC4 }, 2, R, 0, 0x66 },
    { C(MM, REG_R, IMM8),    S32, { 0x0F, 0xC4 }, 2, R, 0, 0 },
    { C(MM, M16, IMM8),      0,   { 0x0F, 0xC4 }, 2, R, 0, 0 },
};
static const BoxAsmForm PmovmskbForms[] = {
    { C(REG, XMM_R, NONE), S32, { 0x0F, 0xD7 }, 2, R, 0, 0x66 },
    { C(REG, MM_R, NONE),  S32, { 0x0F, 0xD7 }, 2, R, 0, 0 },
};

/* MMX's own, and SSE's moves and conversions between mm and xmm. */
static const BoxAsmForm EmmsForms[]     = { ALONE(0, 0x0F, 0x77) };
static const BoxAsmForm PshufwForms[]   = { { C(MM, MM_M64, IMM8), 0, { 0x0F, 0x70 }, 2, R, 0, 0 } };
static const BoxAsmForm MaskmovqForms[] = { MMX(0xF7, MM_R) };
static const BoxAsmForm MovntqForms[]   = { { C(M64, MM, NONE), 0, { 0x0F, 0xE7 }, 2, R, 0, 0 } };
static const BoxAsmForm Movq2dqForms[]  = { SSE(0xF3, 0xD6, MM_R) };
static const BoxAsmForm Movdq2qForms[]  = { { C(MM, XMM_R, NONE), 0, { 0x0F, 0xD6 }, 2, R, 0, 0xF2 } };
static const BoxAsmForm Cvtpi2psForms[] = { SSE(0, 0x2A, MM_M64) };
static const BoxAsmForm Cvtpi2pdForms[] = { SSE(0x66, 0x2A, MM_M64) };
static const BoxAsmForm Cvtps2piForms[]  = { { C(MM, XMM_M64, NONE), 0, { 0x0F, 0x2D }, 2, R, 0, 0 } };
static const BoxAsmForm Cvttps2piForms[] = { { C(MM, XMM_M64, NONE), 0, { 0x0F, 0x2C }, 2, R, 0, 0 } };
static const BoxAsmForm Cvtpd2piForms[]  = { { C(MM, XMM_M128, NONE), 0, { 0x0F, 0x2D }, 2, R, 0, 0x66 } };
static const BoxAsmForm Cvttpd2piForms[] = { { C(MM, XMM_M128, NONE), 0, { 0x0F, 0x2C }, 2, R, 0, 0x66 } };

static const BoxAsmForm LdmxcsrForms[]     = { { C(MEM, NONE, NONE), S32, { 0x0F, 0xAE }, 2, 2, 0, 0 } };
static const BoxAsmForm StmxcsrForms[]     = { { C(MEM, NONE, NONE), S32, { 0x0F, 0xAE }, 2, 3, 0, 0 } };
static const BoxAsmForm PrefetchntaForms[] = { { C(MEM, NONE, NONE), S8, { 0x0F, 0x18 }, 2, 0, 0, 0 } };
static const BoxAsmForm Prefetcht0Forms[]  = { { C(MEM, NONE, NONE), S8, { 0x0F, 0x18 }, 2, 1, 0, 0 } };
static const BoxAsmForm Prefetcht1Forms[]  = { { C(MEM, NONE, NONE), S8, { 0x0F, 0x18 }, 2, 2, 0, 0 } };
static const BoxAsmForm Prefetcht2Forms[]  = { { C(MEM, NONE, NONE), S8, { 0x0F, 0x18 }, 2, 3, 0, 0 } };
static const BoxAsmForm ClflushForms[]     = { { C(MEM, NONE, NONE), S8, { 0x0F, 0xAE }, 2, 7, 0, 0 } };

/* ---- SSE3, SSSE3, SSE4.1 and SSE4.2 -------------------------------------
 * The operand rules of SSE2 behind 0F 38 or 0F 3A, whose third opcode byte
 * names the operation; the prefix is 66 but for SSE3's F2 and F3. */

#define SSE_38(Prefix, Third, Source)                                     \
    { C(XMM, Source, NONE), 0, { 0x0F, 0x38, (Third) }, 3, R, 0, (Prefix) }
#define SSE_3A(Prefix, Third, Source)                                     \
    { C(XMM, Source, IMM8), 0, { 0x0F, 0x3A, (Third) }, 3, R, 0, (Prefix) }
/* The mask is xmm0, written third or left out: blendvps, blendvpd, pblendvb. */
#define SSE_MASKED(Third)                                                 \
    { C(XMM, XMM_M128, XMM0), 0, { 0x0F, 0x38, (Third) }, 3, R, 0, 0x66 }, \
    { C(XMM, XMM_M128, NONE), 0, { 0x0F, 0x38, (Third) }, 3, R, 0, 0x66 }
/* An operation on an xmm register and xmm/m128 behind 66 0F 38; the same
 * behind 0F 3A with an imm8. */
#define SSE_OPERATION_38(Name, Third)                                                 \
    static const BoxAsmForm Name##Forms[] = { SSE_38(0x66, (Third), XMM_M128) };
#define SSE_OPERATION_3A(Name, Third)                                                 \
    static const BoxAsmForm Name##Forms[] = { SSE_3A(0x66, (Third), XMM_M128) };

static const BoxAsmForm AddsubpsForms[] = { SSE(0xF2, 0xD0, XMM_M128) };
static const BoxAsmForm AddsubpdForms[] = { SSE(0x66, 0xD0, XMM_M128) };
static const BoxAsmForm HaddpsForms[]   = { SSE(0xF2, 0x7C, XMM_M128) };
static const BoxAsmForm HaddpdForms[]   = { SSE(0x66, 0x7C, XMM_M128) };
static const BoxAsmForm HsubpsForms[]   = { SSE(0xF2, 0x7D, XMM_M128) };
static const BoxAsmForm HsubpdForms[]   = { SSE(0x66, 0x7D, XMM_M128) };
static const BoxAsmForm LddquForms[]    = { SSE(0xF2, 0xF0, U128) };
static const BoxAsmForm MovddupForms[]  = { SSE(0xF2, 0x12, XMM_M64) };
static const BoxAsmForm MovshdupForms[] = { SSE(0xF3, 0x16, XMM_M128) };
static const BoxAsmForm MovsldupForms[] = { SSE(0xF3, 0x12, XMM_M128) };
static const BoxAsmForm MonitorForms[]  = { ALONE(0, 0x0F, 0x01, 0xC8) };
static const BoxAsmForm MwaitForms[]    = { ALONE(0, 0x0F, 0x01, 0xC9) };

/* SSSE3 also on mm registers and mm/m64, with no prefix. */
#define SSSE3_OPERATION(Name, Third)                                                  \
    static const BoxAsmForm Name##Forms[] = {                                        \
        SSE_38(0x66, (Third), XMM_M128),                                             \
        { C(MM, MM_M64, NONE), 0, { 0x0F, 0x38, (Third) }, 3, R, 0, 0 } };
SSSE3_OPERATION(Pshufb, 0x00)   SSSE3_OPERATION(Phaddw, 0x01)    SSSE3_OPERATION(Phaddd, 0x02)
SSSE3_OPERATION(Phaddsw, 0x03)  SSSE3_OPERATION(Pmaddubsw, 0x04) SSSE3_OPERATION(Phsubw, 0x05)
SSSE3_OPERATION(Phsubd, 0x06)   SSSE3_OPERATION(Phsubsw, 0x07)   SSSE3_OPERATION(Psignb, 0x08)
SSSE3_OPERATION(Psignw, 0x09)   SSSE3_OPERATION(Psignd, 0x0A)    SSSE3_OPERATION(Pmulhrsw, 0x0B)
SSSE3_OPERATION(Pabsb, 0x1C)    SSSE3_OPERATION(Pabsw, 0x1D)     SSSE3_OPERATION(Pabsd, 0x1E)
static const BoxAsmForm PalignrForms[] = {
    SSE_3A(0x66, 0x0F, XMM_M128),
    { C(MM, MM_M64, IMM8), 0, { 0x0F, 0x3A, 0x0F }, 3, R, 0, 0 } };

SSE_OPERATION_38(Ptest, 0x17)    SSE_OPERATION_38(Pmuldq, 0x28)    SSE_OPERATION_38(Pcmpeqq, 0x29)
SSE_OPERATION_38(Packusdw, 0x2B) SSE_OPERATION_38(Pminsb, 0x38)    SSE_OPERATION_38(Pminsd, 0x39)
SSE_OPERATION_38(Pminuw, 0x3A)   SSE_OPERATION_38(Pminud, 0x3B)    SSE_OPERATION_38(Pmaxsb, 0x3C)
SSE_OPERATION_38(Pmaxsd, 0x3D)   SSE_OPERATION_38(Pmaxuw, 0x3E)    SSE_OPERATION_38(Pmaxud, 0x3F)
SSE_OPERATION_38(Pmulld, 0x40)   SSE_OPERATION_38(Phminposuw, 0x41)
SSE_OPERATION_38(Pcmpgtq, 0x37)
SSE_OPERATION_3A(Blendps, 0x0C)  SSE_OPERATION_3A(Blendpd, 0x0D)   SSE_OPERATION_3A(Pblendw, 0x0E)
SSE_OPERATION_3A(Dpps, 0x40)     SSE_OPERATION_3A(Dppd, 0x41)      SSE_OPERATION_3A(Mpsadbw, 0x42)
SSE_OPERATION_3A(Pcmpestrm, 0x60) SSE_OPERATION_3A(Pcmpestri, 0x61)
SSE_OPERATION_3A(Pcmpistrm, 0x62) SSE_OPERATION_3A(Pcmpistri, 0x63)

static const BoxAsmForm BlendvpsForms[] = { SSE_MASKED(0x14) };
static const BoxAsmForm BlendvpdForms[] = { SSE_MASKED(0x15) };
static const BoxAsmForm PblendvbForms[] = { SSE_MASKED(0x10) };
static const BoxAsmForm MovntdqaForms[] = { SSE_38(0x66, 0x2A, M128) };

/* pmovsx and pmovzx widen what the source holds; the memory they read is
 * the bytes that widen, not an oword. */
static const BoxAsmForm PmovsxbwForms[] = { SSE_38(0x66, 0x20, XMM_M64) };
static const BoxAsmForm PmovsxbdForms[] = { SSE_38(0x66, 0x21, XMM_M32) };
static const BoxAsmForm PmovsxbqForms[] = { SSE_38(0x66, 0x22, XMM_M16) };
static const BoxAsmForm PmovsxwdForms[] = { SSE_38(0x66, 0x23, XMM_M64) };
static const BoxAsmForm PmovsxwqForms[] = { SSE_38(0x66, 0x24, XMM_M32) };
static const BoxAsmForm PmovsxdqForms[] = { SSE_38(0x66, 0x25, XMM_M64) };
static const BoxAsmForm PmovzxbwForms[] = { SSE_38(0x66, 0x30, XMM_M64) };
static const BoxAsmForm PmovzxbdForms[] = { SSE_38(0x66, 0x31, XMM_M32) };
static const BoxAsmForm PmovzxbqForms[] = { SSE_38(0x66, 0x32, XMM_M16) };
static const BoxAsmForm PmovzxwdForms[] = { SSE_38(0x66, 0x33, XMM_M64) };
static const BoxAsmForm PmovzxwqForms[] = { SSE_38(0x66, 0x34, XMM_M32) };
static const BoxAsmForm PmovzxdqForms[] = { SSE_38(0x66, 0x35, XMM_M64) };

static const BoxAsmForm RoundpsForms[]  = { SSE_3A(0x66, 0x08, XMM_M128) };
static const BoxAsmForm RoundpdForms[]  = { SSE_3A(0x66, 0x09, XMM_M128) };
static const BoxAsmForm RoundssForms[]  = { SSE_3A(0x66, 0x0A, XMM_M32) };
static const BoxAsmForm RoundsdForms[]  = { SSE_3A(0x66, 0x0B, XMM_M64) };
static const BoxAsmForm InsertpsForms[] = { SSE_3A(0x66, 0x21, XMM_M32) };

/* The general register or the memory of pextrb, extractps, pextrd and pextrq
 * is ModRM.rm and the xmm register ModRM.reg; a 64-bit register of pextrb is
 * written without REX.W, as pextrw's is, and extractps's with it. */
static const BoxAsmForm PextrbForms[] = {
    { C(REG_R, XMM, IMM8), S32 | S64, { 0x0F, 0x3A, 0x14 }, 3, R, BOXASM_FORM_DEFAULT64, 0x66 },
    { C(M8, XMM, IMM8),    0,         { 0x0F, 0x3A, 0x14 }, 3, R, 0, 0x66 },
};
static const BoxAsmForm ExtractpsForms[] = {
    { C(REG_R, XMM, IMM8), S32 | S64, { 0x0F, 0x3A, 0x17 }, 3, R, 0, 0x66 },
    { C(M32, XMM, IMM8),   0,         { 0x0F, 0x3A, 0x17 }, 3, R, 0, 0x66 },
};
static const BoxAsmForm PextrdForms[] = { { C(RM, XMM, IMM8), S32, { 0x0F, 0x3A, 0x16 }, 3, R, 0, 0x66 } };
static const BoxAsmForm PextrqForms[] = { { C(RM, XMM, IMM8), S64, { 0x0F, 0x3A, 0x16 }, 3, R, 0, 0x66 } };
static const BoxAsmForm PinsrbForms[] = {
    { C(XMM, REG_R, IMM8), S32, { 0x0F, 0x3A, 0x20 }, 3, R, 0, 0x66 },
    { C(XMM, M8, IMM8),    0,   { 0x0F, 0x3A, 0x20 }, 3, R, 0, 0x66 },
};
static const BoxAsmForm PinsrdForms[] = { { C(XMM, RM, IMM8), S32, { 0x0F, 0x3A, 0x22 }, 3, R, 0, 0x66 } };
static const BoxAsmForm PinsrqForms[] = { { C(XMM, RM, IMM8), S64, { 0x0F, 0x3A, 0x22 }, 3, R, 0, 0x66 } };

/* crc32 accumulates over r/m8, 16, 32 or 64; the 16-bit one is the 32-bit
 * form behind a 66, written before the F2. A 64-bit register over r/m8 is
 * written without REX.W, as NASM writes it: the sum is 32 bits, the upper
 * half cleared either way. */
static const BoxAsmForm Crc32Forms[] = {
    { C(REG, RM8, NONE),  S32 | S64, { 0x0F, 0x38, 0xF0 }, 3, R, BOXASM_FORM_DEFAULT64, 0xF2 },
    { C(REG, RM16, NONE), S32,       { 0x0F, 0x38, 0xF1 }, 3, R, BOXASM_FORM_DATA16, 0xF2 },
    { C(REG, RM32, NONE), S32,       { 0x0F, 0x38, 0xF1 }, 3, R, 0, 0xF2 },
    { C(REG, RM64, NONE), S64,       { 0x0F, 0x38, 0xF1 }, 3, R, 0, 0xF2 },
};

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
    PLAIN("lahf", LahfForms), PLAIN("sahf", SahfForms), PLAIN("xgetbv", XgetbvForms),
    PLAIN("emms", EmmsForms), PLAIN("pshufw", PshufwForms), PLAIN("maskmovq", MaskmovqForms),
    PLAIN("movntq", MovntqForms), PLAIN("movq2dq", Movq2dqForms), PLAIN("movdq2q", Movdq2qForms),
    PLAIN("cvtpi2ps", Cvtpi2psForms), PLAIN("cvtpi2pd", Cvtpi2pdForms),
    PLAIN("cvtps2pi", Cvtps2piForms), PLAIN("cvttps2pi", Cvttps2piForms),
    PLAIN("cvtpd2pi", Cvtpd2piForms), PLAIN("cvttpd2pi", Cvttpd2piForms),
    PLAIN("movbe", MovbeForms),
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
    PLAIN("addps", AddpsForms), PLAIN("addpd", AddpdForms), PLAIN("addss", AddssForms),
    PLAIN("addsd", AddsdForms), PLAIN("subps", SubpsForms), PLAIN("subpd", SubpdForms),
    PLAIN("subss", SubssForms), PLAIN("subsd", SubsdForms), PLAIN("mulps", MulpsForms),
    PLAIN("mulpd", MulpdForms), PLAIN("mulss", MulssForms), PLAIN("mulsd", MulsdForms),
    PLAIN("divps", DivpsForms), PLAIN("divpd", DivpdForms), PLAIN("divss", DivssForms),
    PLAIN("divsd", DivsdForms), PLAIN("minps", MinpsForms), PLAIN("minpd", MinpdForms),
    PLAIN("minss", MinssForms), PLAIN("minsd", MinsdForms), PLAIN("maxps", MaxpsForms),
    PLAIN("maxpd", MaxpdForms), PLAIN("maxss", MaxssForms), PLAIN("maxsd", MaxsdForms),
    PLAIN("sqrtps", SqrtpsForms), PLAIN("sqrtpd", SqrtpdForms),
    PLAIN("sqrtss", SqrtssForms), PLAIN("sqrtsd", SqrtsdForms), PLAIN("andps", AndpsForms),
    PLAIN("andpd", AndpdForms), PLAIN("andnps", AndnpsForms), PLAIN("andnpd", AndnpdForms),
    PLAIN("orps", OrpsForms), PLAIN("orpd", OrpdForms), PLAIN("xorps", XorpsForms),
    PLAIN("xorpd", XorpdForms), PLAIN("unpcklps", UnpcklpsForms),
    PLAIN("unpcklpd", UnpcklpdForms), PLAIN("unpckhps", UnpckhpsForms),
    PLAIN("unpckhpd", UnpckhpdForms), PLAIN("rcpps", RcppsForms),
    PLAIN("rcpss", RcpssForms), PLAIN("rsqrtps", RsqrtpsForms),
    PLAIN("rsqrtss", RsqrtssForms), PLAIN("cmpps", CmppsForms), PLAIN("cmppd", CmppdForms),
    PLAIN("cmpss", CmpssForms), PLAIN("comiss", ComissForms), PLAIN("comisd", ComisdForms),
    PLAIN("ucomiss", UcomissForms), PLAIN("ucomisd", UcomisdForms),
    PLAIN("shufps", ShufpsForms), PLAIN("shufpd", ShufpdForms),
    PLAIN("movaps", MovapsForms), PLAIN("movapd", MovapdForms),
    PLAIN("movups", MovupsForms), PLAIN("movupd", MovupdForms), PLAIN("movss", MovssForms),
    PLAIN("movlps", MovlpsForms), PLAIN("movhps", MovhpsForms),
    PLAIN("movlpd", MovlpdForms), PLAIN("movhpd", MovhpdForms),
    PLAIN("movhlps", MovhlpsForms), PLAIN("movlhps", MovlhpsForms),
    PLAIN("movdqa", MovdqaForms), PLAIN("movdqu", MovdquForms), PLAIN("movd", MovdForms),
    PLAIN("movq", MovqForms), PLAIN("movmskps", MovmskpsForms),
    PLAIN("movmskpd", MovmskpdForms), PLAIN("movntps", MovntpsForms),
    PLAIN("movntpd", MovntpdForms), PLAIN("movntdq", MovntdqForms),
    PLAIN("movnti", MovntiForms), PLAIN("maskmovdqu", MaskmovdquForms),
    PLAIN("cvtsi2ss", Cvtsi2ssForms), PLAIN("cvtsi2sd", Cvtsi2sdForms),
    PLAIN("cvtss2si", Cvtss2siForms), PLAIN("cvttss2si", Cvttss2siForms),
    PLAIN("cvtsd2si", Cvtsd2siForms), PLAIN("cvttsd2si", Cvttsd2siForms),
    PLAIN("cvtss2sd", Cvtss2sdForms), PLAIN("cvtsd2ss", Cvtsd2ssForms),
    PLAIN("cvtps2pd", Cvtps2pdForms), PLAIN("cvtpd2ps", Cvtpd2psForms),
    PLAIN("cvtdq2ps", Cvtdq2psForms), PLAIN("cvtps2dq", Cvtps2dqForms),
    PLAIN("cvttps2dq", Cvttps2dqForms), PLAIN("cvtdq2pd", Cvtdq2pdForms),
    PLAIN("cvtpd2dq", Cvtpd2dqForms), PLAIN("cvttpd2dq", Cvttpd2dqForms),
    PLAIN("paddb", PaddbForms), PLAIN("paddw", PaddwForms), PLAIN("paddd", PadddForms),
    PLAIN("paddq", PaddqForms), PLAIN("psubb", PsubbForms), PLAIN("psubw", PsubwForms),
    PLAIN("psubd", PsubdForms), PLAIN("psubq", PsubqForms), PLAIN("paddsb", PaddsbForms),
    PLAIN("paddsw", PaddswForms), PLAIN("paddusb", PaddusbForms),
    PLAIN("paddusw", PadduswForms), PLAIN("psubsb", PsubsbForms),
    PLAIN("psubsw", PsubswForms), PLAIN("psubusb", PsubusbForms),
    PLAIN("psubusw", PsubuswForms), PLAIN("pmullw", PmullwForms),
    PLAIN("pmulhw", PmulhwForms), PLAIN("pmulhuw", PmulhuwForms),
    PLAIN("pmuludq", PmuludqForms), PLAIN("pmaddwd", PmaddwdForms),
    PLAIN("pavgb", PavgbForms), PLAIN("pavgw", PavgwForms), PLAIN("pmaxsw", PmaxswForms),
    PLAIN("pmaxub", PmaxubForms), PLAIN("pminsw", PminswForms),
    PLAIN("pminub", PminubForms), PLAIN("psadbw", PsadbwForms), PLAIN("pand", PandForms),
    PLAIN("pandn", PandnForms), PLAIN("por", PorForms), PLAIN("pxor", PxorForms),
    PLAIN("pcmpeqb", PcmpeqbForms), PLAIN("pcmpeqw", PcmpeqwForms),
    PLAIN("pcmpeqd", PcmpeqdForms), PLAIN("pcmpgtb", PcmpgtbForms),
    PLAIN("pcmpgtw", PcmpgtwForms), PLAIN("pcmpgtd", PcmpgtdForms),
    PLAIN("packsswb", PacksswbForms), PLAIN("packssdw", PackssdwForms),
    PLAIN("packuswb", PackuswbForms), PLAIN("punpcklbw", PunpcklbwForms),
    PLAIN("punpcklwd", PunpcklwdForms), PLAIN("punpckldq", PunpckldqForms),
    PLAIN("punpcklqdq", PunpcklqdqForms), PLAIN("punpckhbw", PunpckhbwForms),
    PLAIN("punpckhwd", PunpckhwdForms), PLAIN("punpckhdq", PunpckhdqForms),
    PLAIN("punpckhqdq", PunpckhqdqForms), PLAIN("psllw", PsllwForms),
    PLAIN("pslld", PslldForms), PLAIN("psllq", PsllqForms), PLAIN("psrlw", PsrlwForms),
    PLAIN("psrld", PsrldForms), PLAIN("psrlq", PsrlqForms), PLAIN("psraw", PsrawForms),
    PLAIN("psrad", PsradForms), PLAIN("pslldq", PslldqForms), PLAIN("psrldq", PsrldqForms),
    PLAIN("pshufd", PshufdForms), PLAIN("pshufhw", PshufhwForms),
    PLAIN("pshuflw", PshuflwForms), PLAIN("pextrw", PextrwForms),
    PLAIN("pinsrw", PinsrwForms), PLAIN("pmovmskb", PmovmskbForms),
    PLAIN("ldmxcsr", LdmxcsrForms), PLAIN("stmxcsr", StmxcsrForms),
    PLAIN("prefetchnta", PrefetchntaForms), PLAIN("prefetcht0", Prefetcht0Forms),
    PLAIN("prefetcht1", Prefetcht1Forms), PLAIN("prefetcht2", Prefetcht2Forms),
    PLAIN("clflush", ClflushForms),
    PLAIN("addsubps", AddsubpsForms), PLAIN("addsubpd", AddsubpdForms),
    PLAIN("haddps", HaddpsForms), PLAIN("haddpd", HaddpdForms),
    PLAIN("hsubps", HsubpsForms), PLAIN("hsubpd", HsubpdForms), PLAIN("lddqu", LddquForms),
    PLAIN("movddup", MovddupForms), PLAIN("movshdup", MovshdupForms),
    PLAIN("movsldup", MovsldupForms), PLAIN("monitor", MonitorForms),
    PLAIN("mwait", MwaitForms), PLAIN("pshufb", PshufbForms), PLAIN("phaddw", PhaddwForms),
    PLAIN("phaddd", PhadddForms), PLAIN("phaddsw", PhaddswForms),
    PLAIN("pmaddubsw", PmaddubswForms), PLAIN("phsubw", PhsubwForms),
    PLAIN("phsubd", PhsubdForms), PLAIN("phsubsw", PhsubswForms),
    PLAIN("psignb", PsignbForms), PLAIN("psignw", PsignwForms),
    PLAIN("psignd", PsigndForms), PLAIN("pmulhrsw", PmulhrswForms),
    PLAIN("pabsb", PabsbForms), PLAIN("pabsw", PabswForms), PLAIN("pabsd", PabsdForms),
    PLAIN("palignr", PalignrForms), PLAIN("ptest", PtestForms),
    PLAIN("pmuldq", PmuldqForms), PLAIN("pcmpeqq", PcmpeqqForms),
    PLAIN("packusdw", PackusdwForms), PLAIN("pminsb", PminsbForms),
    PLAIN("pminsd", PminsdForms), PLAIN("pminuw", PminuwForms),
    PLAIN("pminud", PminudForms), PLAIN("pmaxsb", PmaxsbForms),
    PLAIN("pmaxsd", PmaxsdForms), PLAIN("pmaxuw", PmaxuwForms),
    PLAIN("pmaxud", PmaxudForms), PLAIN("pmulld", PmulldForms),
    PLAIN("phminposuw", PhminposuwForms), PLAIN("pcmpgtq", PcmpgtqForms),
    PLAIN("blendps", BlendpsForms), PLAIN("blendpd", BlendpdForms),
    PLAIN("pblendw", PblendwForms), PLAIN("dpps", DppsForms), PLAIN("dppd", DppdForms),
    PLAIN("mpsadbw", MpsadbwForms), PLAIN("pcmpestrm", PcmpestrmForms),
    PLAIN("pcmpestri", PcmpestriForms), PLAIN("pcmpistrm", PcmpistrmForms),
    PLAIN("pcmpistri", PcmpistriForms), PLAIN("blendvps", BlendvpsForms),
    PLAIN("blendvpd", BlendvpdForms), PLAIN("pblendvb", PblendvbForms),
    PLAIN("movntdqa", MovntdqaForms), PLAIN("pmovsxbw", PmovsxbwForms),
    PLAIN("pmovsxbd", PmovsxbdForms), PLAIN("pmovsxbq", PmovsxbqForms),
    PLAIN("pmovsxwd", PmovsxwdForms), PLAIN("pmovsxwq", PmovsxwqForms),
    PLAIN("pmovsxdq", PmovsxdqForms), PLAIN("pmovzxbw", PmovzxbwForms),
    PLAIN("pmovzxbd", PmovzxbdForms), PLAIN("pmovzxbq", PmovzxbqForms),
    PLAIN("pmovzxwd", PmovzxwdForms), PLAIN("pmovzxwq", PmovzxwqForms),
    PLAIN("pmovzxdq", PmovzxdqForms), PLAIN("roundps", RoundpsForms),
    PLAIN("roundpd", RoundpdForms), PLAIN("roundss", RoundssForms),
    PLAIN("roundsd", RoundsdForms), PLAIN("insertps", InsertpsForms),
    PLAIN("pextrb", PextrbForms), PLAIN("extractps", ExtractpsForms),
    PLAIN("pextrd", PextrdForms), PLAIN("pextrq", PextrqForms),
    PLAIN("pinsrb", PinsrbForms), PLAIN("pinsrd", PinsrdForms),
    PLAIN("pinsrq", PinsrqForms), PLAIN("crc32", Crc32Forms),
};

static const BoxAsmInstruction BoxAsmJcc =
    INSTRUCTION("j", JccForms, BOXASM_KIND_JUMP, 1, 0, 0);
static const BoxAsmInstruction BoxAsmSetcc =
    INSTRUCTION("set", SetccForms, BOXASM_KIND_PLAIN, 1, 0, 0);
static const BoxAsmInstruction BoxAsmCmovcc =
    INSTRUCTION("cmov", CmovccForms, BOXASM_KIND_PLAIN, 1, 0, 0);

/* cmpltps and its kin: cmp, a predicate, a type; the predicate is the imm8
 * of cmpps, cmppd, cmpss or cmpsd. */
static const BoxAsmInstruction BoxAsmComparePredicates[] = {
    INSTRUCTION("ps", CmpPredicatePsForms, BOXASM_KIND_PLAIN, 1, 0, 0),
    INSTRUCTION("pd", CmpPredicatePdForms, BOXASM_KIND_PLAIN, 1, 0, 0),
    INSTRUCTION("ss", CmpPredicateSsForms, BOXASM_KIND_PLAIN, 1, 0, 0),
    INSTRUCTION("sd", CmpPredicateSdForms, BOXASM_KIND_PLAIN, 1, 0, 0),
};

static const char *const BoxAsmPredicates[8] = {
    "eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"
};

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

int BoxAsmNameEquals(const char *Name, size_t Length, const char *Word)
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

/* The compare the name means, its predicate in *Condition: cmp + eq, lt, le,
 * unord, neq, nlt, nle or ord + ps, pd, ss or sd. */
static const BoxAsmInstruction *BoxAsmComparePredicate(const char *Name, size_t Length,
                                                       uint8_t *Condition)
{
    size_t Type;
    uint8_t Predicate;

    if (Length < 7 || !BoxAsmNameEquals(Name, 3, "cmp"))
        return NULL;
    for (Type = 0; Type < 4; Type++) {
        if (!BoxAsmNameEquals(Name + Length - 2, 2, BoxAsmComparePredicates[Type].Name))
            continue;
        for (Predicate = 0; Predicate < 8; Predicate++) {
            if (BoxAsmNameEquals(Name + 3, Length - 5, BoxAsmPredicates[Predicate])) {
                *Condition = Predicate;
                return &BoxAsmComparePredicates[Type];
            }
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
    if (!Found)
        Found = BoxAsmComparePredicate(Name, Length, Condition);
    if (!Found)
        Found = BoxAsmFindVexInstruction(Name, Length, Condition);
    if (!Found)
        Found = BoxAsmFindX87Instruction(Name, Length);
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
