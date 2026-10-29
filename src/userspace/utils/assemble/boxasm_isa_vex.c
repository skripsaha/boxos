#include "boxasm_isa_table.h"

#include "box/string.h"

/* ---- AVX ----------------------------------------------------------------
 * The VEX forms of every SSE instruction BoxASM has, and AVX's own. A form
 * names the operation by its map, pp and opcode byte; where a register goes
 * is in its classes: the destination in ModRM.reg, the first source in
 * VEX.vvvv, the other in ModRM.rm. AVX widens only the floating-point
 * operations and a few moves to 256 bits; the integer ones stay on xmm until
 * AVX2, and a form is written only for the widths the processor has. The
 * 128-bit form is listed before the 256-bit one, so memory without a size is
 * 128 bits, as NASM takes it. */

#define M0F   BOXASM_FORM_VEX_0F
#define M38   BOXASM_FORM_VEX_0F38
#define M3A   BOXASM_FORM_VEX_0F3A
#define L256  BOXASM_FORM_VEX_L256
#define W1    BOXASM_FORM_VEX_W1
#define DUP   BOXASM_FORM_DUPLICATE
#define NO_W  BOXASM_FORM_DEFAULT64   /* the register's size does not set VEX.W */

#define VEX(Classes, Sizes, Vex, Prefix, Opcode, Flags)                   \
    { Classes, (Sizes), { (Opcode) }, 1, R, (Flags) | (Vex), (Prefix) }

/* The shapes of the vector forms, each in its 128-bit (xmm) and 256-bit (_Y,
 * ymm) width; Rm is the class of the ModRM.rm operand. */
#define TRIPLE(Vex, Prefix, Opcode, Rm)                                   \
    VEX(C(XMM, XMM_V, Rm), 0, Vex, Prefix, Opcode, DUP)
#define TRIPLE_Y(Vex, Prefix, Opcode, Rm)                                 \
    VEX(C(YMM, YMM_V, Rm), 0, (Vex) | L256, Prefix, Opcode, DUP)
#define TRIPLE_IMM(Vex, Prefix, Opcode, Rm)                               \
    VEX(C4(XMM, XMM_V, Rm, IMM8), 0, Vex, Prefix, Opcode, DUP)
#define TRIPLE_IMM_Y(Vex, Prefix, Opcode, Rm)                             \
    VEX(C4(YMM, YMM_V, Rm, IMM8), 0, (Vex) | L256, Prefix, Opcode, DUP)
/* The register that selects, element by element, is the last operand. */
#define TRIPLE_SELECT(Vex, Prefix, Opcode, Rm)                            \
    VEX(C4(XMM, XMM_V, Rm, XMM_IS4), 0, Vex, Prefix, Opcode, DUP)
#define TRIPLE_SELECT_Y(Vex, Prefix, Opcode, Rm)                          \
    VEX(C4(YMM, YMM_V, Rm, YMM_IS4), 0, (Vex) | L256, Prefix, Opcode, DUP)
#define PAIR(Vex, Prefix, Opcode, Rm)                                     \
    VEX(C(XMM, Rm, NONE), 0, Vex, Prefix, Opcode, 0)
#define PAIR_Y(Vex, Prefix, Opcode, Rm)                                   \
    VEX(C(YMM, Rm, NONE), 0, (Vex) | L256, Prefix, Opcode, 0)
#define PAIR_IMM(Vex, Prefix, Opcode, Rm)                                 \
    VEX(C(XMM, Rm, IMM8), 0, Vex, Prefix, Opcode, 0)
#define PAIR_IMM_Y(Vex, Prefix, Opcode, Rm)                               \
    VEX(C(YMM, Rm, IMM8), 0, (Vex) | L256, Prefix, Opcode, 0)
#define STORE(Vex, Prefix, Opcode, Rm)                                    \
    VEX(C(Rm, XMM, NONE), 0, Vex, Prefix, Opcode, 0)
#define STORE_Y(Vex, Prefix, Opcode, Rm)                                  \
    VEX(C(Rm, YMM, NONE), 0, (Vex) | L256, Prefix, Opcode, 0)
/* A shift of an xmm register by an imm8: the result goes to VEX.vvvv. */
#define SHIFT_IMM(Opcode, Digit)                                          \
    { C(XMM_V, XMM_R, IMM8), 0, { (Opcode) }, 1, (Digit), DUP | M0F, 0x66 }
#define SHIFT_IMM_Y(Opcode, Digit)                                        \
    { C(YMM_V, YMM_R, IMM8), 0, { (Opcode) }, 1, (Digit), DUP | M0F | L256, 0x66 }

/* The packed single and double of an operation of three operands, then its
 * scalar single and double. */
#define FLOAT_PACKED(Name, Opcode)                                                      \
    static const BoxAsmForm V##Name##psForms[] = {                                      \
        TRIPLE(M0F, 0, Opcode, XMM_U128), TRIPLE_Y(M0F, 0, Opcode, YMM_U256) };         \
    static const BoxAsmForm V##Name##pdForms[] = {                                      \
        TRIPLE(M0F, 0x66, Opcode, XMM_U128), TRIPLE_Y(M0F, 0x66, Opcode, YMM_U256) };
#define FLOAT_SCALAR(Name, Opcode)                                                      \
    static const BoxAsmForm V##Name##ssForms[] = { TRIPLE(M0F, 0xF3, Opcode, XMM_M32) }; \
    static const BoxAsmForm V##Name##sdForms[] = { TRIPLE(M0F, 0xF2, Opcode, XMM_M64) };
/* A packed operation of two operands (vsqrtps): the source is not combined
 * with the destination. */
#define FLOAT_PACKED_PAIR(Name, Opcode)                                                 \
    static const BoxAsmForm V##Name##psForms[] = {                                      \
        PAIR(M0F, 0, Opcode, XMM_U128), PAIR_Y(M0F, 0, Opcode, YMM_U256) };             \
    static const BoxAsmForm V##Name##pdForms[] = {                                      \
        PAIR(M0F, 0x66, Opcode, XMM_U128), PAIR_Y(M0F, 0x66, Opcode, YMM_U256) };
/* The integer operations of SSE2, SSSE3 and SSE4.1, on xmm registers and, from
 * AVX2, on ymm ones. */
#define INTEGER(Name, Map, Opcode)                                                      \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        TRIPLE(Map, 0x66, Opcode, XMM_U128), TRIPLE_Y(Map, 0x66, Opcode, YMM_U256) };
#define INTEGER_PAIR(Name, Map, Opcode)                                                 \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        PAIR(Map, 0x66, Opcode, XMM_U128), PAIR_Y(Map, 0x66, Opcode, YMM_U256) };
#define INTEGER_IMM(Name, Opcode)                                                       \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        TRIPLE_IMM(M3A, 0x66, Opcode, XMM_U128), TRIPLE_IMM_Y(M3A, 0x66, Opcode, YMM_U256) };

FLOAT_PACKED(Add, 0x58)   FLOAT_SCALAR(Add, 0x58)
FLOAT_PACKED(Sub, 0x5C)   FLOAT_SCALAR(Sub, 0x5C)
FLOAT_PACKED(Mul, 0x59)   FLOAT_SCALAR(Mul, 0x59)
FLOAT_PACKED(Div, 0x5E)   FLOAT_SCALAR(Div, 0x5E)
FLOAT_PACKED(Min, 0x5D)   FLOAT_SCALAR(Min, 0x5D)
FLOAT_PACKED(Max, 0x5F)   FLOAT_SCALAR(Max, 0x5F)
FLOAT_PACKED_PAIR(Sqrt, 0x51)
FLOAT_SCALAR(Sqrt, 0x51)
FLOAT_PACKED(And, 0x54)
FLOAT_PACKED(Andn, 0x55)
FLOAT_PACKED(Or, 0x56)
FLOAT_PACKED(Xor, 0x57)
FLOAT_PACKED(Unpckl, 0x14)
FLOAT_PACKED(Unpckh, 0x15)

static const BoxAsmForm VRcppsForms[]   = { PAIR(M0F, 0, 0x53, XMM_U128), PAIR_Y(M0F, 0, 0x53, YMM_U256) };
static const BoxAsmForm VRcpssForms[]   = { TRIPLE(M0F, 0xF3, 0x53, XMM_M32) };
static const BoxAsmForm VRsqrtpsForms[] = { PAIR(M0F, 0, 0x52, XMM_U128), PAIR_Y(M0F, 0, 0x52, YMM_U256) };
static const BoxAsmForm VRsqrtssForms[] = { TRIPLE(M0F, 0xF3, 0x52, XMM_M32) };

/* vcmpps and its kin take the predicate as an imm8, 0 to 255; vcmpltps and
 * the rest of the 32 name it and take the three operands. */
static const BoxAsmForm VCmppsForms[] = {
    TRIPLE_IMM(M0F, 0, 0xC2, XMM_U128), TRIPLE_IMM_Y(M0F, 0, 0xC2, YMM_U256) };
static const BoxAsmForm VCmppdForms[] = {
    TRIPLE_IMM(M0F, 0x66, 0xC2, XMM_U128), TRIPLE_IMM_Y(M0F, 0x66, 0xC2, YMM_U256) };
static const BoxAsmForm VCmpssForms[] = { TRIPLE_IMM(M0F, 0xF3, 0xC2, XMM_M32) };
static const BoxAsmForm VCmpsdForms[] = { TRIPLE_IMM(M0F, 0xF2, 0xC2, XMM_M64) };

#define COMPARE_PREDICATE(Prefix, Rm, Rm256)                                            \
    VEX(C(XMM, XMM_V, Rm), 0, M0F, Prefix, 0xC2, DUP | BOXASM_FORM_PREDICATE),          \
    VEX(C(YMM, YMM_V, Rm256), 0, M0F | L256, Prefix, 0xC2, DUP | BOXASM_FORM_PREDICATE)
static const BoxAsmForm VCmpPredicatePsForms[] = { COMPARE_PREDICATE(0, XMM_U128, YMM_U256) };
static const BoxAsmForm VCmpPredicatePdForms[] = { COMPARE_PREDICATE(0x66, XMM_U128, YMM_U256) };
static const BoxAsmForm VCmpPredicateSsForms[] = {
    VEX(C(XMM, XMM_V, XMM_M32), 0, M0F, 0xF3, 0xC2, DUP | BOXASM_FORM_PREDICATE) };
static const BoxAsmForm VCmpPredicateSdForms[] = {
    VEX(C(XMM, XMM_V, XMM_M64), 0, M0F, 0xF2, 0xC2, DUP | BOXASM_FORM_PREDICATE) };

static const BoxAsmForm VComissForms[]  = { PAIR(M0F, 0, 0x2F, XMM_M32) };
static const BoxAsmForm VComisdForms[]  = { PAIR(M0F, 0x66, 0x2F, XMM_M64) };
static const BoxAsmForm VUcomissForms[] = { PAIR(M0F, 0, 0x2E, XMM_M32) };
static const BoxAsmForm VUcomisdForms[] = { PAIR(M0F, 0x66, 0x2E, XMM_M64) };
static const BoxAsmForm VShufpsForms[]  = {
    TRIPLE_IMM(M0F, 0, 0xC6, XMM_U128), TRIPLE_IMM_Y(M0F, 0, 0xC6, YMM_U256) };
static const BoxAsmForm VShufpdForms[]  = {
    TRIPLE_IMM(M0F, 0x66, 0xC6, XMM_U128), TRIPLE_IMM_Y(M0F, 0x66, 0xC6, YMM_U256) };

/* The moves: the load before the store, so a register pair is the load, as
 * NASM writes it. vmovaps and vmovdqa fault on memory off its width. */
static const BoxAsmForm VMovapsForms[] = {
    PAIR(M0F, 0, 0x28, XMM_M128), STORE(M0F, 0, 0x29, XMM_M128),
    PAIR_Y(M0F, 0, 0x28, YMM_M256), STORE_Y(M0F, 0, 0x29, YMM_M256) };
static const BoxAsmForm VMovapdForms[] = {
    PAIR(M0F, 0x66, 0x28, XMM_M128), STORE(M0F, 0x66, 0x29, XMM_M128),
    PAIR_Y(M0F, 0x66, 0x28, YMM_M256), STORE_Y(M0F, 0x66, 0x29, YMM_M256) };
static const BoxAsmForm VMovupsForms[] = {
    PAIR(M0F, 0, 0x10, XMM_U128), STORE(M0F, 0, 0x11, XMM_U128),
    PAIR_Y(M0F, 0, 0x10, YMM_U256), STORE_Y(M0F, 0, 0x11, YMM_U256) };
static const BoxAsmForm VMovupdForms[] = {
    PAIR(M0F, 0x66, 0x10, XMM_U128), STORE(M0F, 0x66, 0x11, XMM_U128),
    PAIR_Y(M0F, 0x66, 0x10, YMM_U256), STORE_Y(M0F, 0x66, 0x11, YMM_U256) };
static const BoxAsmForm VMovdqaForms[] = {
    PAIR(M0F, 0x66, 0x6F, XMM_M128), STORE(M0F, 0x66, 0x7F, XMM_M128),
    PAIR_Y(M0F, 0x66, 0x6F, YMM_M256), STORE_Y(M0F, 0x66, 0x7F, YMM_M256) };
static const BoxAsmForm VMovdquForms[] = {
    PAIR(M0F, 0xF3, 0x6F, XMM_U128), STORE(M0F, 0xF3, 0x7F, XMM_U128),
    PAIR_Y(M0F, 0xF3, 0x6F, YMM_U256), STORE_Y(M0F, 0xF3, 0x7F, YMM_U256) };

/* vmovss and vmovsd: memory is the load and the store, two registers a merge
 * into the first source. */
static const BoxAsmForm VMovssForms[] = {
    PAIR(M0F, 0xF3, 0x10, M32), STORE(M0F, 0xF3, 0x11, M32), TRIPLE(M0F, 0xF3, 0x10, XMM_R) };
static const BoxAsmForm VMovsdForms[] = {
    PAIR(M0F, 0xF2, 0x10, M64), STORE(M0F, 0xF2, 0x11, M64), TRIPLE(M0F, 0xF2, 0x10, XMM_R) };
static const BoxAsmForm VMovlpsForms[] = { TRIPLE(M0F, 0, 0x12, M64), STORE(M0F, 0, 0x13, M64) };
static const BoxAsmForm VMovhpsForms[] = { TRIPLE(M0F, 0, 0x16, M64), STORE(M0F, 0, 0x17, M64) };
static const BoxAsmForm VMovlpdForms[] = { TRIPLE(M0F, 0x66, 0x12, M64), STORE(M0F, 0x66, 0x13, M64) };
static const BoxAsmForm VMovhpdForms[] = { TRIPLE(M0F, 0x66, 0x16, M64), STORE(M0F, 0x66, 0x17, M64) };
static const BoxAsmForm VMovhlpsForms[] = { TRIPLE(M0F, 0, 0x12, XMM_R) };
static const BoxAsmForm VMovlhpsForms[] = { TRIPLE(M0F, 0, 0x16, XMM_R) };

static const BoxAsmForm VMovdForms[] = {
    VEX(C(XMM, RM, NONE), S32, M0F, 0x66, 0x6E, 0),
    VEX(C(RM, XMM, NONE), S32, M0F, 0x66, 0x7E, 0),
};
/* vmovq: xmm and xmm/m64 by F3 7E and 66 D6 first, as NASM chooses; a general
 * register by the 66 6E and 7E whose VEX.W is 1. */
static const BoxAsmForm VMovqForms[] = {
    PAIR(M0F, 0xF3, 0x7E, XMM_M64),
    STORE(M0F, 0x66, 0xD6, XMM_M64),
    VEX(C(XMM, RM, NONE), S64, M0F | W1, 0x66, 0x6E, NO_W),
    VEX(C(RM, XMM, NONE), S64, M0F | W1, 0x66, 0x7E, NO_W),
};
/* A general register of vmovmskps, vmovmskpd and vpmovmskb is written 64-bit
 * without VEX.W: the mask is 32 bits at the most and the rest is cleared. */
static const BoxAsmForm VMovmskpsForms[] = {
    VEX(C(REG, XMM_R, NONE), S32 | S64, M0F, 0, 0x50, NO_W),
    VEX(C(REG, YMM_R, NONE), S32 | S64, M0F | L256, 0, 0x50, NO_W) };
static const BoxAsmForm VMovmskpdForms[] = {
    VEX(C(REG, XMM_R, NONE), S32 | S64, M0F, 0x66, 0x50, NO_W),
    VEX(C(REG, YMM_R, NONE), S32 | S64, M0F | L256, 0x66, 0x50, NO_W) };
static const BoxAsmForm VPmovmskbForms[] = {
    VEX(C(REG, XMM_R, NONE), S32 | S64, M0F, 0x66, 0xD7, NO_W),
    VEX(C(REG, YMM_R, NONE), S32 | S64, M0F | L256, 0x66, 0xD7, NO_W) };
static const BoxAsmForm VMovntpsForms[] = { STORE(M0F, 0, 0x2B, M128), STORE_Y(M0F, 0, 0x2B, M256) };
static const BoxAsmForm VMovntpdForms[] = { STORE(M0F, 0x66, 0x2B, M128), STORE_Y(M0F, 0x66, 0x2B, M256) };
static const BoxAsmForm VMovntdqForms[] = { STORE(M0F, 0x66, 0xE7, M128), STORE_Y(M0F, 0x66, 0xE7, M256) };
static const BoxAsmForm VMovntdqaForms[] = {
    PAIR(M38, 0x66, 0x2A, M128), PAIR_Y(M38, 0x66, 0x2A, M256) };
static const BoxAsmForm VMaskmovdquForms[] = { PAIR(M0F, 0x66, 0xF7, XMM_R) };
static const BoxAsmForm VLddquForms[] = { PAIR(M0F, 0xF2, 0xF0, U128), PAIR_Y(M0F, 0xF2, 0xF0, U256) };
static const BoxAsmForm VLdmxcsrForms[] = { { C(MEM, NONE, NONE), S32, { 0xAE }, 1, 2, M0F, 0 } };
static const BoxAsmForm VStmxcsrForms[] = { { C(MEM, NONE, NONE), S32, { 0xAE }, 1, 3, M0F, 0 } };

/* Conversions. The memory of one that narrows is read at the width of its
 * source; vcvtpd2ps and its kin write an xmm register from xmm or ymm. */
static const BoxAsmForm VCvtsi2ssForms[] = { VEX(C(XMM, XMM_V, RM), S32 | S64, M0F, 0xF3, 0x2A, DUP) };
static const BoxAsmForm VCvtsi2sdForms[] = { VEX(C(XMM, XMM_V, RM), S32 | S64, M0F, 0xF2, 0x2A, DUP) };
static const BoxAsmForm VCvtss2siForms[]  = { VEX(C(REG, XMM_M32, NONE), S32 | S64, M0F, 0xF3, 0x2D, 0) };
static const BoxAsmForm VCvttss2siForms[] = { VEX(C(REG, XMM_M32, NONE), S32 | S64, M0F, 0xF3, 0x2C, 0) };
static const BoxAsmForm VCvtsd2siForms[]  = { VEX(C(REG, XMM_M64, NONE), S32 | S64, M0F, 0xF2, 0x2D, 0) };
static const BoxAsmForm VCvttsd2siForms[] = { VEX(C(REG, XMM_M64, NONE), S32 | S64, M0F, 0xF2, 0x2C, 0) };
static const BoxAsmForm VCvtss2sdForms[]  = { TRIPLE(M0F, 0xF3, 0x5A, XMM_M32) };
static const BoxAsmForm VCvtsd2ssForms[]  = { TRIPLE(M0F, 0xF2, 0x5A, XMM_M64) };
static const BoxAsmForm VCvtps2pdForms[]  = { PAIR(M0F, 0, 0x5A, XMM_M64), PAIR_Y(M0F, 0, 0x5A, XMM_U128) };
static const BoxAsmForm VCvtpd2psForms[]  = { PAIR(M0F, 0x66, 0x5A, XMM_U128), PAIR(M0F | L256, 0x66, 0x5A, YMM_U256) };
static const BoxAsmForm VCvtdq2psForms[]  = { PAIR(M0F, 0, 0x5B, XMM_U128), PAIR_Y(M0F, 0, 0x5B, YMM_U256) };
static const BoxAsmForm VCvtps2dqForms[]  = { PAIR(M0F, 0x66, 0x5B, XMM_U128), PAIR_Y(M0F, 0x66, 0x5B, YMM_U256) };
static const BoxAsmForm VCvttps2dqForms[] = { PAIR(M0F, 0xF3, 0x5B, XMM_U128), PAIR_Y(M0F, 0xF3, 0x5B, YMM_U256) };
static const BoxAsmForm VCvtdq2pdForms[]  = { PAIR(M0F, 0xF3, 0xE6, XMM_M64), PAIR_Y(M0F, 0xF3, 0xE6, XMM_U128) };
static const BoxAsmForm VCvtpd2dqForms[]  = { PAIR(M0F, 0xF2, 0xE6, XMM_U128), PAIR(M0F | L256, 0xF2, 0xE6, YMM_U256) };
static const BoxAsmForm VCvttpd2dqForms[] = { PAIR(M0F, 0x66, 0xE6, XMM_U128), PAIR(M0F | L256, 0x66, 0xE6, YMM_U256) };

INTEGER(Paddb, M0F, 0xFC)     INTEGER(Paddw, M0F, 0xFD)     INTEGER(Paddd, M0F, 0xFE)
INTEGER(Paddq, M0F, 0xD4)     INTEGER(Psubb, M0F, 0xF8)     INTEGER(Psubw, M0F, 0xF9)
INTEGER(Psubd, M0F, 0xFA)     INTEGER(Psubq, M0F, 0xFB)     INTEGER(Paddsb, M0F, 0xEC)
INTEGER(Paddsw, M0F, 0xED)    INTEGER(Paddusb, M0F, 0xDC)   INTEGER(Paddusw, M0F, 0xDD)
INTEGER(Psubsb, M0F, 0xE8)    INTEGER(Psubsw, M0F, 0xE9)    INTEGER(Psubusb, M0F, 0xD8)
INTEGER(Psubusw, M0F, 0xD9)   INTEGER(Pmullw, M0F, 0xD5)    INTEGER(Pmulhw, M0F, 0xE5)
INTEGER(Pmulhuw, M0F, 0xE4)   INTEGER(Pmuludq, M0F, 0xF4)   INTEGER(Pmaddwd, M0F, 0xF5)
INTEGER(Pavgb, M0F, 0xE0)     INTEGER(Pavgw, M0F, 0xE3)     INTEGER(Pmaxsw, M0F, 0xEE)
INTEGER(Pmaxub, M0F, 0xDE)    INTEGER(Pminsw, M0F, 0xEA)    INTEGER(Pminub, M0F, 0xDA)
INTEGER(Psadbw, M0F, 0xF6)    INTEGER(Pand, M0F, 0xDB)      INTEGER(Pandn, M0F, 0xDF)
INTEGER(Por, M0F, 0xEB)       INTEGER(Pxor, M0F, 0xEF)      INTEGER(Pcmpeqb, M0F, 0x74)
INTEGER(Pcmpeqw, M0F, 0x75)   INTEGER(Pcmpeqd, M0F, 0x76)   INTEGER(Pcmpgtb, M0F, 0x64)
INTEGER(Pcmpgtw, M0F, 0x65)   INTEGER(Pcmpgtd, M0F, 0x66)   INTEGER(Packsswb, M0F, 0x63)
INTEGER(Packssdw, M0F, 0x6B)  INTEGER(Packuswb, M0F, 0x67)  INTEGER(Punpcklbw, M0F, 0x60)
INTEGER(Punpcklwd, M0F, 0x61) INTEGER(Punpckldq, M0F, 0x62) INTEGER(Punpcklqdq, M0F, 0x6C)
INTEGER(Punpckhbw, M0F, 0x68) INTEGER(Punpckhwd, M0F, 0x69) INTEGER(Punpckhdq, M0F, 0x6A)
INTEGER(Punpckhqdq, M0F, 0x6D)

/* Each shift takes its count in an xmm register or memory, or an imm8. */
#define INTEGER_SHIFT(Name, Count, Immediate, Digit)                                    \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        TRIPLE(M0F, 0x66, Count, XMM_U128), TRIPLE_Y(M0F, 0x66, Count, XMM_U128),       \
        SHIFT_IMM(Immediate, Digit), SHIFT_IMM_Y(Immediate, Digit) };
INTEGER_SHIFT(Psllw, 0xF1, 0x71, 6)  INTEGER_SHIFT(Pslld, 0xF2, 0x72, 6)
INTEGER_SHIFT(Psllq, 0xF3, 0x73, 6)  INTEGER_SHIFT(Psrlw, 0xD1, 0x71, 2)
INTEGER_SHIFT(Psrld, 0xD2, 0x72, 2)  INTEGER_SHIFT(Psrlq, 0xD3, 0x73, 2)
INTEGER_SHIFT(Psraw, 0xE1, 0x71, 4)  INTEGER_SHIFT(Psrad, 0xE2, 0x72, 4)
static const BoxAsmForm VPslldqForms[] = { SHIFT_IMM(0x73, 7), SHIFT_IMM_Y(0x73, 7) };
static const BoxAsmForm VPsrldqForms[] = { SHIFT_IMM(0x73, 3), SHIFT_IMM_Y(0x73, 3) };

static const BoxAsmForm VPshufdForms[] = {
    PAIR_IMM(M0F, 0x66, 0x70, XMM_U128), PAIR_IMM_Y(M0F, 0x66, 0x70, YMM_U256) };
static const BoxAsmForm VPshufhwForms[] = {
    PAIR_IMM(M0F, 0xF3, 0x70, XMM_U128), PAIR_IMM_Y(M0F, 0xF3, 0x70, YMM_U256) };
static const BoxAsmForm VPshuflwForms[] = {
    PAIR_IMM(M0F, 0xF2, 0x70, XMM_U128), PAIR_IMM_Y(M0F, 0xF2, 0x70, YMM_U256) };

/* The general register of vpinsrw is 32 bits; a 64-bit one of the extracts
 * (vpextrw, vpextrb, vpextrd) is taken, written without VEX.W, as NASM takes
 * it. */
static const BoxAsmForm VPextrwForms[] = {
    VEX(C(REG, XMM_R, IMM8), S32 | S64, M0F, 0x66, 0xC5, NO_W),
    VEX(C(M16, XMM, IMM8), 0, M3A, 0x66, 0x15, 0),
};
static const BoxAsmForm VPinsrwForms[] = {
    VEX(C4(XMM, XMM_V, REG_R, IMM8), S32, M0F, 0x66, 0xC4, DUP),
    VEX(C4(XMM, XMM_V, M16, IMM8), 0, M0F, 0x66, 0xC4, DUP),
};

/* SSE3. */
static const BoxAsmForm VAddsubpsForms[] = { TRIPLE(M0F, 0xF2, 0xD0, XMM_U128), TRIPLE_Y(M0F, 0xF2, 0xD0, YMM_U256) };
static const BoxAsmForm VAddsubpdForms[] = { TRIPLE(M0F, 0x66, 0xD0, XMM_U128), TRIPLE_Y(M0F, 0x66, 0xD0, YMM_U256) };
static const BoxAsmForm VHaddpsForms[]   = { TRIPLE(M0F, 0xF2, 0x7C, XMM_U128), TRIPLE_Y(M0F, 0xF2, 0x7C, YMM_U256) };
static const BoxAsmForm VHaddpdForms[]   = { TRIPLE(M0F, 0x66, 0x7C, XMM_U128), TRIPLE_Y(M0F, 0x66, 0x7C, YMM_U256) };
static const BoxAsmForm VHsubpsForms[]   = { TRIPLE(M0F, 0xF2, 0x7D, XMM_U128), TRIPLE_Y(M0F, 0xF2, 0x7D, YMM_U256) };
static const BoxAsmForm VHsubpdForms[]   = { TRIPLE(M0F, 0x66, 0x7D, XMM_U128), TRIPLE_Y(M0F, 0x66, 0x7D, YMM_U256) };
static const BoxAsmForm VMovddupForms[]  = { PAIR(M0F, 0xF2, 0x12, XMM_M64), PAIR_Y(M0F, 0xF2, 0x12, YMM_U256) };
static const BoxAsmForm VMovshdupForms[] = { PAIR(M0F, 0xF3, 0x16, XMM_U128), PAIR_Y(M0F, 0xF3, 0x16, YMM_U256) };
static const BoxAsmForm VMovsldupForms[] = { PAIR(M0F, 0xF3, 0x12, XMM_U128), PAIR_Y(M0F, 0xF3, 0x12, YMM_U256) };

/* SSSE3. */
INTEGER(Pshufb, M38, 0x00)    INTEGER(Phaddw, M38, 0x01)    INTEGER(Phaddd, M38, 0x02)
INTEGER(Phaddsw, M38, 0x03)   INTEGER(Pmaddubsw, M38, 0x04) INTEGER(Phsubw, M38, 0x05)
INTEGER(Phsubd, M38, 0x06)    INTEGER(Phsubsw, M38, 0x07)   INTEGER(Psignb, M38, 0x08)
INTEGER(Psignw, M38, 0x09)    INTEGER(Psignd, M38, 0x0A)    INTEGER(Pmulhrsw, M38, 0x0B)
INTEGER_PAIR(Pabsb, M38, 0x1C)  INTEGER_PAIR(Pabsw, M38, 0x1D)  INTEGER_PAIR(Pabsd, M38, 0x1E)
INTEGER_IMM(Palignr, 0x0F)

/* SSE4.1 and SSE4.2. vptest is the integer operation AVX does take to 256
 * bits; vblendvps and vblendvpd select by the sign of each element of a
 * register, vpblendvb of each byte. */
INTEGER(Pmuldq, M38, 0x28)    INTEGER(Pcmpeqq, M38, 0x29)   INTEGER(Packusdw, M38, 0x2B)
INTEGER(Pminsb, M38, 0x38)    INTEGER(Pminsd, M38, 0x39)    INTEGER(Pminuw, M38, 0x3A)
INTEGER(Pminud, M38, 0x3B)    INTEGER(Pmaxsb, M38, 0x3C)    INTEGER(Pmaxsd, M38, 0x3D)
INTEGER(Pmaxuw, M38, 0x3E)    INTEGER(Pmaxud, M38, 0x3F)    INTEGER(Pmulld, M38, 0x40)
INTEGER(Pcmpgtq, M38, 0x37)
static const BoxAsmForm VPhminposuwForms[] = { PAIR(M38, 0x66, 0x41, XMM_U128) };
static const BoxAsmForm VPtestForms[] = {
    PAIR(M38, 0x66, 0x17, XMM_U128), PAIR_Y(M38, 0x66, 0x17, YMM_U256) };
static const BoxAsmForm VBlendpsForms[] = {
    TRIPLE_IMM(M3A, 0x66, 0x0C, XMM_U128), TRIPLE_IMM_Y(M3A, 0x66, 0x0C, YMM_U256) };
static const BoxAsmForm VBlendpdForms[] = {
    TRIPLE_IMM(M3A, 0x66, 0x0D, XMM_U128), TRIPLE_IMM_Y(M3A, 0x66, 0x0D, YMM_U256) };
static const BoxAsmForm VDppsForms[] = {
    TRIPLE_IMM(M3A, 0x66, 0x40, XMM_U128), TRIPLE_IMM_Y(M3A, 0x66, 0x40, YMM_U256) };
INTEGER_IMM(Pblendw, 0x0E)    INTEGER_IMM(Mpsadbw, 0x42)
static const BoxAsmForm VDppdForms[] = { TRIPLE_IMM(M3A, 0x66, 0x41, XMM_U128) };
static const BoxAsmForm VPcmpestrmForms[] = { PAIR_IMM(M3A, 0x66, 0x60, XMM_U128) };
static const BoxAsmForm VPcmpestriForms[] = { PAIR_IMM(M3A, 0x66, 0x61, XMM_U128) };
static const BoxAsmForm VPcmpistrmForms[] = { PAIR_IMM(M3A, 0x66, 0x62, XMM_U128) };
static const BoxAsmForm VPcmpistriForms[] = { PAIR_IMM(M3A, 0x66, 0x63, XMM_U128) };
static const BoxAsmForm VBlendvpsForms[] = {
    TRIPLE_SELECT(M3A, 0x66, 0x4A, XMM_U128), TRIPLE_SELECT_Y(M3A, 0x66, 0x4A, YMM_U256) };
static const BoxAsmForm VBlendvpdForms[] = {
    TRIPLE_SELECT(M3A, 0x66, 0x4B, XMM_U128), TRIPLE_SELECT_Y(M3A, 0x66, 0x4B, YMM_U256) };
static const BoxAsmForm VPblendvbForms[] = {
    TRIPLE_SELECT(M3A, 0x66, 0x4C, XMM_U128), TRIPLE_SELECT_Y(M3A, 0x66, 0x4C, YMM_U256) };

static const BoxAsmForm VPmovsxbwForms[] = {
    PAIR(M38, 0x66, 0x20, XMM_M64), PAIR_Y(M38, 0x66, 0x20, XMM_U128) };
static const BoxAsmForm VPmovsxbdForms[] = {
    PAIR(M38, 0x66, 0x21, XMM_M32), PAIR_Y(M38, 0x66, 0x21, XMM_M64) };
static const BoxAsmForm VPmovsxbqForms[] = {
    PAIR(M38, 0x66, 0x22, XMM_M16), PAIR_Y(M38, 0x66, 0x22, XMM_M32) };
static const BoxAsmForm VPmovsxwdForms[] = {
    PAIR(M38, 0x66, 0x23, XMM_M64), PAIR_Y(M38, 0x66, 0x23, XMM_U128) };
static const BoxAsmForm VPmovsxwqForms[] = {
    PAIR(M38, 0x66, 0x24, XMM_M32), PAIR_Y(M38, 0x66, 0x24, XMM_M64) };
static const BoxAsmForm VPmovsxdqForms[] = {
    PAIR(M38, 0x66, 0x25, XMM_M64), PAIR_Y(M38, 0x66, 0x25, XMM_U128) };
static const BoxAsmForm VPmovzxbwForms[] = {
    PAIR(M38, 0x66, 0x30, XMM_M64), PAIR_Y(M38, 0x66, 0x30, XMM_U128) };
static const BoxAsmForm VPmovzxbdForms[] = {
    PAIR(M38, 0x66, 0x31, XMM_M32), PAIR_Y(M38, 0x66, 0x31, XMM_M64) };
static const BoxAsmForm VPmovzxbqForms[] = {
    PAIR(M38, 0x66, 0x32, XMM_M16), PAIR_Y(M38, 0x66, 0x32, XMM_M32) };
static const BoxAsmForm VPmovzxwdForms[] = {
    PAIR(M38, 0x66, 0x33, XMM_M64), PAIR_Y(M38, 0x66, 0x33, XMM_U128) };
static const BoxAsmForm VPmovzxwqForms[] = {
    PAIR(M38, 0x66, 0x34, XMM_M32), PAIR_Y(M38, 0x66, 0x34, XMM_M64) };
static const BoxAsmForm VPmovzxdqForms[] = {
    PAIR(M38, 0x66, 0x35, XMM_M64), PAIR_Y(M38, 0x66, 0x35, XMM_U128) };

static const BoxAsmForm VRoundpsForms[] = {
    PAIR_IMM(M3A, 0x66, 0x08, XMM_U128), PAIR_IMM_Y(M3A, 0x66, 0x08, YMM_U256) };
static const BoxAsmForm VRoundpdForms[] = {
    PAIR_IMM(M3A, 0x66, 0x09, XMM_U128), PAIR_IMM_Y(M3A, 0x66, 0x09, YMM_U256) };
static const BoxAsmForm VRoundssForms[]  = { TRIPLE_IMM(M3A, 0x66, 0x0A, XMM_M32) };
static const BoxAsmForm VRoundsdForms[]  = { TRIPLE_IMM(M3A, 0x66, 0x0B, XMM_M64) };
static const BoxAsmForm VInsertpsForms[] = { TRIPLE_IMM(M3A, 0x66, 0x21, XMM_M32) };

/* vextractps to a 64-bit register is EVEX in NASM, and BoxASM has no EVEX:
 * only the 32-bit register is a form. */
static const BoxAsmForm VExtractpsForms[] = {
    VEX(C(REG_R, XMM, IMM8), S32, M3A, 0x66, 0x17, 0),
    VEX(C(M32, XMM, IMM8), 0, M3A, 0x66, 0x17, 0),
};
static const BoxAsmForm VPextrbForms[] = {
    VEX(C(REG_R, XMM, IMM8), S32 | S64, M3A, 0x66, 0x14, NO_W),
    VEX(C(M8, XMM, IMM8), 0, M3A, 0x66, 0x14, 0),
};
static const BoxAsmForm VPextrdForms[] = {
    VEX(C(REG_R, XMM, IMM8), S32 | S64, M3A, 0x66, 0x16, NO_W),
    VEX(C(M32, XMM, IMM8), 0, M3A, 0x66, 0x16, 0),
};
static const BoxAsmForm VPextrqForms[] = { VEX(C(RM, XMM, IMM8), S64, M3A | W1, 0x66, 0x16, NO_W) };
static const BoxAsmForm VPinsrbForms[] = {
    VEX(C4(XMM, XMM_V, REG_R, IMM8), S32, M3A, 0x66, 0x20, DUP),
    VEX(C4(XMM, XMM_V, M8, IMM8), 0, M3A, 0x66, 0x20, DUP),
};
static const BoxAsmForm VPinsrdForms[] = { VEX(C4(XMM, XMM_V, RM, IMM8), S32, M3A, 0x66, 0x22, DUP) };
static const BoxAsmForm VPinsrqForms[] = {
    VEX(C4(XMM, XMM_V, RM, IMM8), S64, M3A | W1, 0x66, 0x22, DUP | NO_W) };

/* AVX's own. The broadcasts read memory, and from AVX2 an xmm register. The
 * masked moves have no shorthand: NASM takes none of them. */
static const BoxAsmForm VBroadcastssForms[] = {
    PAIR(M38, 0x66, 0x18, XMM_M32), PAIR_Y(M38, 0x66, 0x18, XMM_M32) };
static const BoxAsmForm VBroadcastsdForms[]  = { PAIR_Y(M38, 0x66, 0x19, XMM_M64) };
static const BoxAsmForm VBroadcastf128Forms[] = { PAIR_Y(M38, 0x66, 0x1A, U128) };
static const BoxAsmForm VExtractf128Forms[] = {
    VEX(C(XMM_U128, YMM, IMM8), 0, M3A | L256, 0x66, 0x19, 0) };
static const BoxAsmForm VInsertf128Forms[] = {
    VEX(C4(YMM, YMM_V, XMM_U128, IMM8), 0, M3A | L256, 0x66, 0x18, DUP) };
static const BoxAsmForm VMaskmovpsForms[] = {
    VEX(C(XMM, XMM_V, U128), 0, M38, 0x66, 0x2C, 0),
    VEX(C(YMM, YMM_V, U256), 0, M38 | L256, 0x66, 0x2C, 0),
    VEX(C(U128, XMM_V, XMM), 0, M38, 0x66, 0x2E, 0),
    VEX(C(U256, YMM_V, YMM), 0, M38 | L256, 0x66, 0x2E, 0) };
static const BoxAsmForm VMaskmovpdForms[] = {
    VEX(C(XMM, XMM_V, U128), 0, M38, 0x66, 0x2D, 0),
    VEX(C(YMM, YMM_V, U256), 0, M38 | L256, 0x66, 0x2D, 0),
    VEX(C(U128, XMM_V, XMM), 0, M38, 0x66, 0x2F, 0),
    VEX(C(U256, YMM_V, YMM), 0, M38 | L256, 0x66, 0x2F, 0) };
static const BoxAsmForm VPermilpsForms[] = {
    TRIPLE(M38, 0x66, 0x0C, XMM_U128), TRIPLE_Y(M38, 0x66, 0x0C, YMM_U256),
    PAIR_IMM(M3A, 0x66, 0x04, XMM_U128), PAIR_IMM_Y(M3A, 0x66, 0x04, YMM_U256) };
static const BoxAsmForm VPermilpdForms[] = {
    TRIPLE(M38, 0x66, 0x0D, XMM_U128), TRIPLE_Y(M38, 0x66, 0x0D, YMM_U256),
    PAIR_IMM(M3A, 0x66, 0x05, XMM_U128), PAIR_IMM_Y(M3A, 0x66, 0x05, YMM_U256) };
static const BoxAsmForm VPerm2f128Forms[] = { TRIPLE_IMM_Y(M3A, 0x66, 0x06, YMM_U256) };
static const BoxAsmForm VTestpsForms[] = {
    PAIR(M38, 0x66, 0x0E, XMM_U128), PAIR_Y(M38, 0x66, 0x0E, YMM_U256) };
static const BoxAsmForm VTestpdForms[] = {
    PAIR(M38, 0x66, 0x0F, XMM_U128), PAIR_Y(M38, 0x66, 0x0F, YMM_U256) };
static const BoxAsmForm VZeroupperForms[] = { { C(NONE, NONE, NONE), 0, { 0x77 }, 1, NO, M0F, 0 } };
static const BoxAsmForm VZeroallForms[]   = {
    { C(NONE, NONE, NONE), 0, { 0x77 }, 1, NO, M0F | L256, 0 } };

/* ---- AVX2 ----------------------------------------------------------------
 * The integer operations on ymm registers are the forms above; these are the
 * instructions AVX2 adds. The 128-bit integer loads and stores of the masked
 * moves and the variable shifts keep their xmm form. */

#define BROADCAST(Name, Opcode, Rm)                                                     \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        PAIR(M38, 0x66, Opcode, Rm), PAIR_Y(M38, 0x66, Opcode, Rm) };
BROADCAST(Pbroadcastb, 0x78, XMM_M8)    BROADCAST(Pbroadcastw, 0x79, XMM_M16)
BROADCAST(Pbroadcastd, 0x58, XMM_M32)   BROADCAST(Pbroadcastq, 0x59, XMM_M64)
static const BoxAsmForm VBroadcasti128Forms[] = { PAIR_Y(M38, 0x66, 0x5A, U128) };
static const BoxAsmForm VExtracti128Forms[] = {
    VEX(C(XMM_U128, YMM, IMM8), 0, M3A | L256, 0x66, 0x39, 0) };
static const BoxAsmForm VInserti128Forms[] = {
    VEX(C4(YMM, YMM_V, XMM_U128, IMM8), 0, M3A | L256, 0x66, 0x38, DUP) };
static const BoxAsmForm VPerm2i128Forms[] = { TRIPLE_IMM_Y(M3A, 0x66, 0x46, YMM_U256) };
static const BoxAsmForm VPermdForms[]     = { TRIPLE_Y(M38, 0x66, 0x36, YMM_U256) };
static const BoxAsmForm VPermpsForms[]    = { TRIPLE_Y(M38, 0x66, 0x16, YMM_U256) };
static const BoxAsmForm VPermqForms[]     = { PAIR_IMM_Y(M3A | W1, 0x66, 0x00, YMM_U256) };
static const BoxAsmForm VPermpdForms[]    = { PAIR_IMM_Y(M3A | W1, 0x66, 0x01, YMM_U256) };
static const BoxAsmForm VPblenddForms[] = {
    TRIPLE_IMM(M3A, 0x66, 0x02, XMM_U128), TRIPLE_IMM_Y(M3A, 0x66, 0x02, YMM_U256) };

/* NASM takes the shorthand of the integer loads, unlike the float ones. */
#define MASKED_MOVE(Name, Load, Store, Wide)                                            \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        VEX(C(XMM, XMM_V, U128), 0, M38 | (Wide), 0x66, Load, DUP),                       \
        VEX(C(YMM, YMM_V, U256), 0, M38 | L256 | (Wide), 0x66, Load, DUP),                \
        VEX(C(U128, XMM_V, XMM), 0, M38 | (Wide), 0x66, Store, 0),                      \
        VEX(C(U256, YMM_V, YMM), 0, M38 | L256 | (Wide), 0x66, Store, 0) };
MASKED_MOVE(Pmaskmovd, 0x8C, 0x8E, 0)
MASKED_MOVE(Pmaskmovq, 0x8C, 0x8E, W1)

/* The shifts by a count per element: dword with W0, qword with W1. */
#define VARIABLE_SHIFT(Name, Opcode, Wide)                                              \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        TRIPLE(M38 | (Wide), 0x66, Opcode, XMM_U128),                                   \
        TRIPLE_Y(M38 | (Wide), 0x66, Opcode, YMM_U256) };
VARIABLE_SHIFT(Psllvd, 0x47, 0)   VARIABLE_SHIFT(Psllvq, 0x47, W1)
VARIABLE_SHIFT(Psravd, 0x46, 0)
VARIABLE_SHIFT(Psrlvd, 0x45, 0)   VARIABLE_SHIFT(Psrlvq, 0x45, W1)

/* The gathers: destination, the address, and the mask in VEX.vvvv. The index
 * register is the d or q of the name (32- or 64-bit indices), in xmm or ymm
 * as many of them as the destination has elements; the processor stops the
 * program when two of the three registers are one. */
#define GATHER_DEST(Dest, Index, Mask, Vex, Opcode)                                     \
    VEX(C(Dest, Index, Mask), 0, M38 | (Vex), 0x66, Opcode, 0)
#define GATHER_D_SINGLE(Name, Opcode, Wide)                                             \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        GATHER_DEST(XMM, VSIB_X32, XMM_V, Wide, Opcode),                                \
        GATHER_DEST(YMM, VSIB_Y32, YMM_V, L256 | (Wide), Opcode) };
#define GATHER_D_DOUBLE(Name, Opcode, Wide)                                             \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        GATHER_DEST(XMM, VSIB_X64, XMM_V, Wide, Opcode),                                \
        GATHER_DEST(YMM, VSIB_X64, YMM_V, L256 | (Wide), Opcode) };
#define GATHER_Q_SINGLE(Name, Opcode, Wide)                                             \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        GATHER_DEST(XMM, VSIB_X32, XMM_V, Wide, Opcode),                                \
        GATHER_DEST(XMM, VSIB_Y32, XMM_V, L256 | (Wide), Opcode) };
#define GATHER_Q_DOUBLE(Name, Opcode, Wide)                                             \
    static const BoxAsmForm V##Name##Forms[] = {                                        \
        GATHER_DEST(XMM, VSIB_X64, XMM_V, Wide, Opcode),                                \
        GATHER_DEST(YMM, VSIB_Y64, YMM_V, L256 | (Wide), Opcode) };
GATHER_D_SINGLE(Gatherdps, 0x92, 0)   GATHER_D_DOUBLE(Gatherdpd, 0x92, W1)
GATHER_Q_SINGLE(Gatherqps, 0x93, 0)   GATHER_Q_DOUBLE(Gatherqpd, 0x93, W1)
GATHER_D_SINGLE(Pgatherdd, 0x90, 0)   GATHER_D_DOUBLE(Pgatherdq, 0x90, W1)
GATHER_Q_SINGLE(Pgatherqd, 0x91, 0)   GATHER_Q_DOUBLE(Pgatherqq, 0x91, W1)

#define PACKED_ENTRIES(Mnemonic, Name)                                   \
    PLAIN("v" Mnemonic "ps", V##Name##psForms), PLAIN("v" Mnemonic "pd", V##Name##pdForms)
#define SCALAR_ENTRIES(Mnemonic, Name)                                   \
    PLAIN("v" Mnemonic "ss", V##Name##ssForms), PLAIN("v" Mnemonic "sd", V##Name##sdForms)
#define ENTRY(Mnemonic, Name) PLAIN("v" Mnemonic, V##Name##Forms)

static const BoxAsmInstruction BoxAsmVexInstructions[] = {
    PACKED_ENTRIES("add", Add),  SCALAR_ENTRIES("add", Add),
    PACKED_ENTRIES("sub", Sub),  SCALAR_ENTRIES("sub", Sub),
    PACKED_ENTRIES("mul", Mul),  SCALAR_ENTRIES("mul", Mul),
    PACKED_ENTRIES("div", Div),  SCALAR_ENTRIES("div", Div),
    PACKED_ENTRIES("min", Min),  SCALAR_ENTRIES("min", Min),
    PACKED_ENTRIES("max", Max),  SCALAR_ENTRIES("max", Max),
    PACKED_ENTRIES("sqrt", Sqrt), SCALAR_ENTRIES("sqrt", Sqrt),
    PACKED_ENTRIES("and", And),  PACKED_ENTRIES("andn", Andn),
    PACKED_ENTRIES("or", Or),    PACKED_ENTRIES("xor", Xor),
    PACKED_ENTRIES("unpckl", Unpckl), PACKED_ENTRIES("unpckh", Unpckh),
    PLAIN("vrcpps", VRcppsForms), PLAIN("vrcpss", VRcpssForms),
    PLAIN("vrsqrtps", VRsqrtpsForms), PLAIN("vrsqrtss", VRsqrtssForms),
    PLAIN("vcmpps", VCmppsForms), PLAIN("vcmppd", VCmppdForms),
    PLAIN("vcmpss", VCmpssForms), PLAIN("vcmpsd", VCmpsdForms),
    PLAIN("vcomiss", VComissForms), PLAIN("vcomisd", VComisdForms),
    PLAIN("vucomiss", VUcomissForms), PLAIN("vucomisd", VUcomisdForms),
    PLAIN("vshufps", VShufpsForms), PLAIN("vshufpd", VShufpdForms),
    PLAIN("vmovaps", VMovapsForms), PLAIN("vmovapd", VMovapdForms),
    PLAIN("vmovups", VMovupsForms), PLAIN("vmovupd", VMovupdForms),
    PLAIN("vmovdqa", VMovdqaForms), PLAIN("vmovdqu", VMovdquForms),
    PLAIN("vmovss", VMovssForms), PLAIN("vmovsd", VMovsdForms),
    PLAIN("vmovlps", VMovlpsForms), PLAIN("vmovhps", VMovhpsForms),
    PLAIN("vmovlpd", VMovlpdForms), PLAIN("vmovhpd", VMovhpdForms),
    PLAIN("vmovhlps", VMovhlpsForms), PLAIN("vmovlhps", VMovlhpsForms),
    PLAIN("vmovd", VMovdForms), PLAIN("vmovq", VMovqForms),
    PLAIN("vmovmskps", VMovmskpsForms), PLAIN("vmovmskpd", VMovmskpdForms),
    PLAIN("vpmovmskb", VPmovmskbForms),
    PLAIN("vmovntps", VMovntpsForms), PLAIN("vmovntpd", VMovntpdForms),
    PLAIN("vmovntdq", VMovntdqForms), PLAIN("vmovntdqa", VMovntdqaForms),
    PLAIN("vmaskmovdqu", VMaskmovdquForms), PLAIN("vlddqu", VLddquForms),
    PLAIN("vldmxcsr", VLdmxcsrForms), PLAIN("vstmxcsr", VStmxcsrForms),
    PLAIN("vcvtsi2ss", VCvtsi2ssForms), PLAIN("vcvtsi2sd", VCvtsi2sdForms),
    PLAIN("vcvtss2si", VCvtss2siForms), PLAIN("vcvttss2si", VCvttss2siForms),
    PLAIN("vcvtsd2si", VCvtsd2siForms), PLAIN("vcvttsd2si", VCvttsd2siForms),
    PLAIN("vcvtss2sd", VCvtss2sdForms), PLAIN("vcvtsd2ss", VCvtsd2ssForms),
    PLAIN("vcvtps2pd", VCvtps2pdForms), PLAIN("vcvtpd2ps", VCvtpd2psForms),
    PLAIN("vcvtdq2ps", VCvtdq2psForms), PLAIN("vcvtps2dq", VCvtps2dqForms),
    PLAIN("vcvttps2dq", VCvttps2dqForms), PLAIN("vcvtdq2pd", VCvtdq2pdForms),
    PLAIN("vcvtpd2dq", VCvtpd2dqForms), PLAIN("vcvttpd2dq", VCvttpd2dqForms),
    ENTRY("paddb", Paddb), ENTRY("paddw", Paddw), ENTRY("paddd", Paddd),
    ENTRY("paddq", Paddq), ENTRY("psubb", Psubb), ENTRY("psubw", Psubw),
    ENTRY("psubd", Psubd), ENTRY("psubq", Psubq), ENTRY("paddsb", Paddsb),
    ENTRY("paddsw", Paddsw), ENTRY("paddusb", Paddusb), ENTRY("paddusw", Paddusw),
    ENTRY("psubsb", Psubsb), ENTRY("psubsw", Psubsw), ENTRY("psubusb", Psubusb),
    ENTRY("psubusw", Psubusw), ENTRY("pmullw", Pmullw), ENTRY("pmulhw", Pmulhw),
    ENTRY("pmulhuw", Pmulhuw), ENTRY("pmuludq", Pmuludq), ENTRY("pmaddwd", Pmaddwd),
    ENTRY("pavgb", Pavgb), ENTRY("pavgw", Pavgw), ENTRY("pmaxsw", Pmaxsw),
    ENTRY("pmaxub", Pmaxub), ENTRY("pminsw", Pminsw), ENTRY("pminub", Pminub),
    ENTRY("psadbw", Psadbw), ENTRY("pand", Pand), ENTRY("pandn", Pandn),
    ENTRY("por", Por), ENTRY("pxor", Pxor), ENTRY("pcmpeqb", Pcmpeqb),
    ENTRY("pcmpeqw", Pcmpeqw), ENTRY("pcmpeqd", Pcmpeqd), ENTRY("pcmpgtb", Pcmpgtb),
    ENTRY("pcmpgtw", Pcmpgtw), ENTRY("pcmpgtd", Pcmpgtd), ENTRY("packsswb", Packsswb),
    ENTRY("packssdw", Packssdw), ENTRY("packuswb", Packuswb),
    ENTRY("punpcklbw", Punpcklbw), ENTRY("punpcklwd", Punpcklwd),
    ENTRY("punpckldq", Punpckldq), ENTRY("punpcklqdq", Punpcklqdq),
    ENTRY("punpckhbw", Punpckhbw), ENTRY("punpckhwd", Punpckhwd),
    ENTRY("punpckhdq", Punpckhdq), ENTRY("punpckhqdq", Punpckhqdq),
    ENTRY("psllw", Psllw), ENTRY("pslld", Pslld), ENTRY("psllq", Psllq),
    ENTRY("psrlw", Psrlw), ENTRY("psrld", Psrld), ENTRY("psrlq", Psrlq),
    ENTRY("psraw", Psraw), ENTRY("psrad", Psrad), ENTRY("pslldq", Pslldq),
    ENTRY("psrldq", Psrldq), ENTRY("pshufd", Pshufd), ENTRY("pshufhw", Pshufhw),
    ENTRY("pshuflw", Pshuflw), ENTRY("pextrw", Pextrw), ENTRY("pinsrw", Pinsrw),
    PLAIN("vaddsubps", VAddsubpsForms), PLAIN("vaddsubpd", VAddsubpdForms),
    ENTRY("haddps", Haddps), ENTRY("haddpd", Haddpd),
    ENTRY("hsubps", Hsubps), ENTRY("hsubpd", Hsubpd), ENTRY("movddup", Movddup),
    ENTRY("movshdup", Movshdup), ENTRY("movsldup", Movsldup),
    ENTRY("pshufb", Pshufb), ENTRY("phaddw", Phaddw), ENTRY("phaddd", Phaddd),
    ENTRY("phaddsw", Phaddsw), ENTRY("pmaddubsw", Pmaddubsw), ENTRY("phsubw", Phsubw),
    ENTRY("phsubd", Phsubd), ENTRY("phsubsw", Phsubsw), ENTRY("psignb", Psignb),
    ENTRY("psignw", Psignw), ENTRY("psignd", Psignd), ENTRY("pmulhrsw", Pmulhrsw),
    ENTRY("pabsb", Pabsb), ENTRY("pabsw", Pabsw), ENTRY("pabsd", Pabsd),
    ENTRY("palignr", Palignr), ENTRY("ptest", Ptest), ENTRY("pmuldq", Pmuldq),
    ENTRY("pcmpeqq", Pcmpeqq), ENTRY("packusdw", Packusdw), ENTRY("pminsb", Pminsb),
    ENTRY("pminsd", Pminsd), ENTRY("pminuw", Pminuw), ENTRY("pminud", Pminud),
    ENTRY("pmaxsb", Pmaxsb), ENTRY("pmaxsd", Pmaxsd), ENTRY("pmaxuw", Pmaxuw),
    ENTRY("pmaxud", Pmaxud), ENTRY("pmulld", Pmulld), ENTRY("phminposuw", Phminposuw),
    ENTRY("pcmpgtq", Pcmpgtq), ENTRY("blendps", Blendps), ENTRY("blendpd", Blendpd),
    ENTRY("pblendw", Pblendw), ENTRY("dpps", Dpps), ENTRY("dppd", Dppd),
    ENTRY("mpsadbw", Mpsadbw), ENTRY("pcmpestrm", Pcmpestrm), ENTRY("pcmpestri", Pcmpestri),
    ENTRY("pcmpistrm", Pcmpistrm), ENTRY("pcmpistri", Pcmpistri),
    ENTRY("blendvps", Blendvps), ENTRY("blendvpd", Blendvpd), ENTRY("pblendvb", Pblendvb),
    ENTRY("pmovsxbw", Pmovsxbw), ENTRY("pmovsxbd", Pmovsxbd), ENTRY("pmovsxbq", Pmovsxbq),
    ENTRY("pmovsxwd", Pmovsxwd), ENTRY("pmovsxwq", Pmovsxwq), ENTRY("pmovsxdq", Pmovsxdq),
    ENTRY("pmovzxbw", Pmovzxbw), ENTRY("pmovzxbd", Pmovzxbd), ENTRY("pmovzxbq", Pmovzxbq),
    ENTRY("pmovzxwd", Pmovzxwd), ENTRY("pmovzxwq", Pmovzxwq), ENTRY("pmovzxdq", Pmovzxdq),
    ENTRY("roundps", Roundps), ENTRY("roundpd", Roundpd), ENTRY("roundss", Roundss),
    ENTRY("roundsd", Roundsd), ENTRY("insertps", Insertps), ENTRY("extractps", Extractps),
    ENTRY("pextrb", Pextrb), ENTRY("pextrd", Pextrd), ENTRY("pextrq", Pextrq),
    ENTRY("pinsrb", Pinsrb), ENTRY("pinsrd", Pinsrd), ENTRY("pinsrq", Pinsrq),
    ENTRY("broadcastss", Broadcastss), ENTRY("broadcastsd", Broadcastsd),
    ENTRY("broadcastf128", Broadcastf128), ENTRY("extractf128", Extractf128),
    ENTRY("insertf128", Insertf128), ENTRY("maskmovps", Maskmovps),
    ENTRY("maskmovpd", Maskmovpd), ENTRY("permilps", Permilps), ENTRY("permilpd", Permilpd),
    ENTRY("perm2f128", Perm2f128), ENTRY("testps", Testps), ENTRY("testpd", Testpd),
    ENTRY("zeroupper", Zeroupper), ENTRY("zeroall", Zeroall),
    ENTRY("pbroadcastb", Pbroadcastb), ENTRY("pbroadcastw", Pbroadcastw),
    ENTRY("pbroadcastd", Pbroadcastd), ENTRY("pbroadcastq", Pbroadcastq),
    ENTRY("broadcasti128", Broadcasti128), ENTRY("extracti128", Extracti128),
    ENTRY("inserti128", Inserti128), ENTRY("perm2i128", Perm2i128), ENTRY("permd", Permd),
    ENTRY("permps", Permps), ENTRY("permq", Permq), ENTRY("permpd", Permpd),
    ENTRY("pblendd", Pblendd), ENTRY("pmaskmovd", Pmaskmovd), ENTRY("pmaskmovq", Pmaskmovq),
    ENTRY("psllvd", Psllvd), ENTRY("psllvq", Psllvq), ENTRY("psravd", Psravd),
    ENTRY("psrlvd", Psrlvd), ENTRY("psrlvq", Psrlvq),
    ENTRY("gatherdps", Gatherdps), ENTRY("gatherdpd", Gatherdpd),
    ENTRY("gatherqps", Gatherqps), ENTRY("gatherqpd", Gatherqpd),
    ENTRY("pgatherdd", Pgatherdd), ENTRY("pgatherdq", Pgatherdq),
    ENTRY("pgatherqd", Pgatherqd), ENTRY("pgatherqq", Pgatherqq),
};

/* vcmpltps and its kin: vcmp, a predicate, a type; the predicate is the imm8
 * of vcmpps, vcmppd, vcmpss or vcmpsd. */
static const BoxAsmInstruction BoxAsmVexComparePredicates[] = {
    INSTRUCTION("vcmpps", VCmpPredicatePsForms, BOXASM_KIND_PLAIN, 1, 0, 0),
    INSTRUCTION("vcmppd", VCmpPredicatePdForms, BOXASM_KIND_PLAIN, 1, 0, 0),
    INSTRUCTION("vcmpss", VCmpPredicateSsForms, BOXASM_KIND_PLAIN, 1, 0, 0),
    INSTRUCTION("vcmpsd", VCmpPredicateSdForms, BOXASM_KIND_PLAIN, 1, 0, 0),
};

/* The 32 predicates of AVX, with the names NASM gives them: the first eight
 * are SSE's, the rest have the ordered or unordered signalling or quiet
 * spelling of the manual, and the short names stand for the ones the first
 * sixteen have. */
static const struct {
    const char *Name;
    uint8_t     Code;
} BoxAsmVexPredicates[] = {
    { "eq", 0 },       { "lt", 1 },       { "lt_os", 1 },    { "le", 2 },       { "le_os", 2 },
    { "unord", 3 },    { "unord_q", 3 },  { "neq", 4 },      { "neq_uq", 4 },   { "nlt", 5 },
    { "nlt_us", 5 },   { "nle", 6 },      { "nle_us", 6 },   { "ord", 7 },      { "ord_q", 7 },
    { "eq_uq", 8 },    { "nge", 9 },      { "nge_us", 9 },   { "ngt", 10 },     { "ngt_us", 10 },
    { "false", 11 },   { "false_oq", 11 }, { "neq_oq", 12 }, { "ge", 13 },      { "ge_os", 13 },
    { "gt", 14 },      { "gt_os", 14 },   { "true", 15 },    { "true_uq", 15 }, { "eq_os", 16 },
    { "lt_oq", 17 },   { "le_oq", 18 },   { "unord_s", 19 }, { "neq_us", 20 },  { "nlt_uq", 21 },
    { "nle_uq", 22 },  { "ord_s", 23 },   { "eq_us", 24 },   { "nge_uq", 25 },  { "ngt_uq", 26 },
    { "false_os", 27 }, { "neq_os", 28 }, { "ge_oq", 29 },   { "gt_oq", 30 },   { "true_us", 31 },
};

static const BoxAsmInstruction *BoxAsmVexComparePredicate(const char *Name, size_t Length,
                                                          uint8_t *Condition)
{
    size_t Type;
    size_t Predicate;

    if (Length < 8 || !BoxAsmNameEquals(Name, 4, "vcmp"))
        return NULL;
    for (Type = 0; Type < sizeof(BoxAsmVexComparePredicates) / sizeof(BoxAsmVexComparePredicates[0]);
         Type++) {
        if (!BoxAsmNameEquals(Name + Length - 2, 2, BoxAsmVexComparePredicates[Type].Name + 4))
            continue;
        for (Predicate = 0; Predicate < sizeof(BoxAsmVexPredicates) / sizeof(BoxAsmVexPredicates[0]);
             Predicate++) {
            if (BoxAsmNameEquals(Name + 4, Length - 6, BoxAsmVexPredicates[Predicate].Name)) {
                *Condition = BoxAsmVexPredicates[Predicate].Code;
                return &BoxAsmVexComparePredicates[Type];
            }
        }
    }
    return NULL;
}

const BoxAsmInstruction *BoxAsmFindVexInstruction(const char *Name, size_t Length,
                                                  uint8_t *Condition)
{
    size_t Index;

    for (Index = 0; Index < sizeof(BoxAsmVexInstructions) / sizeof(BoxAsmVexInstructions[0]); Index++)
        if (BoxAsmNameEquals(Name, Length, BoxAsmVexInstructions[Index].Name))
            return &BoxAsmVexInstructions[Index];
    return BoxAsmVexComparePredicate(Name, Length, Condition);
}
