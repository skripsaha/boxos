#include "boxasm_isa_table.h"

/* ---- x87 ----------------------------------------------------------------
 * The floating-point stack: st0 is its top, st1 the one below. A register
 * operand is added to the last opcode byte; memory is ModRM.rm with a digit.
 * Where an instruction reads memory of several sizes the size is said (fld
 * dword, fild word); an instruction with one size, or a block of its own
 * (fsave, fxsave), takes memory as it is written. Forms are in the order
 * NASM chooses among them: of two stack registers, st(i), st0 before st0,
 * st(i) — fdiv st0, st0 is DC F8. */

#define NO_OPERANDS(First, Second)                                        \
    { C(NONE, NONE, NONE), 0, { (First), (Second) }, 2, NO, 0, 0 }
#define WAITING(First, Second)                                            \
    { C(NONE, NONE, NONE), 0, { (First), (Second) }, 2, NO, BOXASM_FORM_WAIT, 0 }
#define STACK(Class, First, Second)                                       \
    { C(Class, NONE, NONE), 0, { (First), (Second) }, 2, NO, 0, 0 }
#define STACK_PAIR(A, B, First, Second)                                   \
    { C(A, B, NONE), 0, { (First), (Second) }, 2, NO, 0, 0 }
#define MEMORY(Class, Opcode, Digit)                                      \
    { C(Class, NONE, NONE), 0, { (Opcode) }, 1, (Digit), 0, 0 }
#define MEMORY_WAITING(Class, Opcode, Digit)                              \
    { C(Class, NONE, NONE), 0, { (Opcode) }, 1, (Digit), BOXASM_FORM_WAIT, 0 }

/* fadd, fmul, fsub, fsubr, fdiv, fdivr: a dword or qword of memory into st0;
 * st(i) = st(i) op st0 (DC, where sub and div are written reversed); st0 =
 * st0 op st(i) (D8), the same with st0 left out; and with no operand, the
 * popping form on st1. */
#define ARITHMETIC(Name, Digit, Reversed)                                       \
    static const BoxAsmForm Name##Forms[] = {                                   \
        NO_OPERANDS(0xDE, (Reversed) + 1),                                      \
        MEMORY(SIZED_M32, 0xD8, (Digit)), MEMORY(SIZED_M64, 0xDC, (Digit)),     \
        STACK_PAIR(FPU, FPU0, 0xDC, (Reversed)),                                \
        STACK_PAIR(FPU0, FPU, 0xD8, 0xC0 + 8 * (Digit)),                        \
        STACK(FPU, 0xD8, 0xC0 + 8 * (Digit)) };                                 \
    static const BoxAsmForm Name##pForms[] = {                                  \
        NO_OPERANDS(0xDE, (Reversed) + 1),                                      \
        STACK_PAIR(FPU, FPU0, 0xDE, (Reversed)), STACK(FPU, 0xDE, (Reversed)) };
ARITHMETIC(Fadd, 0, 0xC0)
ARITHMETIC(Fmul, 1, 0xC8)
ARITHMETIC(Fsub, 4, 0xE8)
ARITHMETIC(Fsubr, 5, 0xE0)
ARITHMETIC(Fdiv, 6, 0xF8)
ARITHMETIC(Fdivr, 7, 0xF0)

/* fiadd and its kin: a word or dword integer of memory with st0. */
#define INTEGER_MEMORY(Name, Digit)                                             \
    static const BoxAsmForm Name##Forms[] = {                                   \
        MEMORY(SIZED_M16, 0xDE, (Digit)), MEMORY(SIZED_M32, 0xDA, (Digit)) };
INTEGER_MEMORY(Fiadd, 0)
INTEGER_MEMORY(Fimul, 1)
INTEGER_MEMORY(Ficom, 2)
INTEGER_MEMORY(Ficomp, 3)
INTEGER_MEMORY(Fisub, 4)
INTEGER_MEMORY(Fisubr, 5)
INTEGER_MEMORY(Fidiv, 6)
INTEGER_MEMORY(Fidivr, 7)

/* st0 with st(i): written st0, st(i), or st(i) alone, or left out for st1. */
#define WITH_TOP(Name, First, Second)                                           \
    static const BoxAsmForm Name##Forms[] = {                                   \
        NO_OPERANDS((First), (Second) + 1), STACK(FPU, (First), (Second)),      \
        STACK_PAIR(FPU0, FPU, (First), (Second)) };
WITH_TOP(Fucom, 0xDD, 0xE0)
WITH_TOP(Fucomp, 0xDD, 0xE8)
WITH_TOP(Fcomi, 0xDB, 0xF0)
WITH_TOP(Fcomip, 0xDF, 0xF0)
WITH_TOP(Fucomi, 0xDB, 0xE8)
WITH_TOP(Fucomip, 0xDF, 0xE8)
WITH_TOP(Fcmovb, 0xDA, 0xC0)
WITH_TOP(Fcmove, 0xDA, 0xC8)
WITH_TOP(Fcmovbe, 0xDA, 0xD0)
WITH_TOP(Fcmovu, 0xDA, 0xD8)
WITH_TOP(Fcmovnb, 0xDB, 0xC0)
WITH_TOP(Fcmovne, 0xDB, 0xC8)
WITH_TOP(Fcmovnbe, 0xDB, 0xD0)
WITH_TOP(Fcmovnu, 0xDB, 0xD8)

/* fcom and fcomp also compare st0 with a dword or qword of memory. */
#define COMPARE(Name, Digit)                                                    \
    static const BoxAsmForm Name##Forms[] = {                                   \
        NO_OPERANDS(0xD8, 0xC1 + 8 * (Digit)),                                  \
        MEMORY(SIZED_M32, 0xD8, (Digit)), MEMORY(SIZED_M64, 0xDC, (Digit)),     \
        STACK(FPU, 0xD8, 0xC0 + 8 * (Digit)),                                   \
        STACK_PAIR(FPU0, FPU, 0xD8, 0xC0 + 8 * (Digit)) };
COMPARE(Fcom, 2)
COMPARE(Fcomp, 3)

/* fld, fst and fstp of memory or of a stack register; left out, st1. */
static const BoxAsmForm FldForms[] = {
    NO_OPERANDS(0xD9, 0xC1),
    MEMORY(SIZED_M32, 0xD9, 0), MEMORY(SIZED_M64, 0xDD, 0), MEMORY(SIZED_M80, 0xDB, 5),
    STACK(FPU, 0xD9, 0xC0) };
static const BoxAsmForm FstForms[] = {
    NO_OPERANDS(0xDD, 0xD1),
    MEMORY(SIZED_M32, 0xD9, 2), MEMORY(SIZED_M64, 0xDD, 2), STACK(FPU, 0xDD, 0xD0) };
static const BoxAsmForm FstpForms[] = {
    NO_OPERANDS(0xDD, 0xD9),
    MEMORY(SIZED_M32, 0xD9, 3), MEMORY(SIZED_M64, 0xDD, 3), MEMORY(SIZED_M80, 0xDB, 7),
    STACK(FPU, 0xDD, 0xD8) };
static const BoxAsmForm FildForms[] = {
    MEMORY(SIZED_M16, 0xDF, 0), MEMORY(SIZED_M32, 0xDB, 0), MEMORY(SIZED_M64, 0xDF, 5) };
static const BoxAsmForm FistForms[] = {
    MEMORY(SIZED_M16, 0xDF, 2), MEMORY(SIZED_M32, 0xDB, 2) };
static const BoxAsmForm FistpForms[] = {
    MEMORY(SIZED_M16, 0xDF, 3), MEMORY(SIZED_M32, 0xDB, 3), MEMORY(SIZED_M64, 0xDF, 7) };
static const BoxAsmForm FisttpForms[] = {
    MEMORY(SIZED_M16, 0xDF, 1), MEMORY(SIZED_M32, 0xDB, 1), MEMORY(SIZED_M64, 0xDD, 1) };
static const BoxAsmForm FbldForms[]  = { MEMORY(M80, 0xDF, 4) };
static const BoxAsmForm FbstpForms[] = { MEMORY(M80, 0xDF, 6) };

/* fxch with st1 when left out, and st0 on either side. */
static const BoxAsmForm FxchForms[] = {
    NO_OPERANDS(0xD9, 0xC9), STACK(FPU, 0xD9, 0xC8),
    STACK_PAIR(FPU, FPU0, 0xD9, 0xC8), STACK_PAIR(FPU0, FPU, 0xD9, 0xC8) };
static const BoxAsmForm FfreeForms[]  = { NO_OPERANDS(0xDD, 0xC1), STACK(FPU, 0xDD, 0xC0) };
static const BoxAsmForm FfreepForms[] = { NO_OPERANDS(0xDF, 0xC1), STACK(FPU, 0xDF, 0xC0) };

/* The control word, the status word, the environment and the whole state. */
static const BoxAsmForm FldcwForms[]   = { MEMORY(M16, 0xD9, 5) };
static const BoxAsmForm FstcwForms[]   = { MEMORY_WAITING(M16, 0xD9, 7) };
static const BoxAsmForm FnstcwForms[]  = { MEMORY(M16, 0xD9, 7) };
static const BoxAsmForm FstswForms[]   = {
    { C(AX, NONE, NONE), 0, { 0xDF, 0xE0 }, 2, NO, BOXASM_FORM_WAIT, 0 },
    MEMORY_WAITING(M16, 0xDD, 7) };
static const BoxAsmForm FnstswForms[]  = {
    { C(AX, NONE, NONE), 0, { 0xDF, 0xE0 }, 2, NO, 0, 0 }, MEMORY(M16, 0xDD, 7) };
static const BoxAsmForm FldenvForms[]  = { MEMORY(STATE, 0xD9, 4) };
static const BoxAsmForm FstenvForms[]  = { MEMORY_WAITING(STATE, 0xD9, 6) };
static const BoxAsmForm FnstenvForms[] = { MEMORY(STATE, 0xD9, 6) };
static const BoxAsmForm FrstorForms[]  = { MEMORY(STATE, 0xDD, 4) };
static const BoxAsmForm FsaveForms[]   = { MEMORY_WAITING(STATE, 0xDD, 6) };
static const BoxAsmForm FnsaveForms[]  = { MEMORY(STATE, 0xDD, 6) };
static const BoxAsmForm FinitForms[]   = { WAITING(0xDB, 0xE3) };
static const BoxAsmForm FninitForms[]  = { NO_OPERANDS(0xDB, 0xE3) };
static const BoxAsmForm FclexForms[]   = { WAITING(0xDB, 0xE2) };
static const BoxAsmForm FnclexForms[]  = { NO_OPERANDS(0xDB, 0xE2) };
static const BoxAsmForm FwaitForms[]   = { { C(NONE, NONE, NONE), 0, { 0x9B }, 1, NO, 0, 0 } };

/* fxsave and fxrstor keep the x87, MMX and SSE state in 512 bytes on a
 * 16-byte boundary; the 64 forms keep the instruction pointers whole. */
static const BoxAsmForm FxsaveForms[] = {
    { C(M512, NONE, NONE), 0, { 0x0F, 0xAE }, 2, 0, 0, 0 } };
static const BoxAsmForm FxrstorForms[] = {
    { C(M512, NONE, NONE), 0, { 0x0F, 0xAE }, 2, 1, 0, 0 } };
static const BoxAsmForm Fxsave64Forms[] = {
    { C(M512, NONE, NONE), 0, { 0x0F, 0xAE }, 2, 0, BOXASM_FORM_REX_W, 0 } };
static const BoxAsmForm Fxrstor64Forms[] = {
    { C(M512, NONE, NONE), 0, { 0x0F, 0xAE }, 2, 1, BOXASM_FORM_REX_W, 0 } };

/* What takes no operand: D9 and a second byte, but fcompp and fucompp. */
#define ALONE_X87(Name, First, Second) \
    static const BoxAsmForm Name##Forms[] = { NO_OPERANDS((First), (Second)) };
ALONE_X87(Fnop, 0xD9, 0xD0)     ALONE_X87(Fchs, 0xD9, 0xE0)     ALONE_X87(Fabs, 0xD9, 0xE1)
ALONE_X87(Ftst, 0xD9, 0xE4)     ALONE_X87(Fxam, 0xD9, 0xE5)     ALONE_X87(Fld1, 0xD9, 0xE8)
ALONE_X87(Fldl2t, 0xD9, 0xE9)   ALONE_X87(Fldl2e, 0xD9, 0xEA)   ALONE_X87(Fldpi, 0xD9, 0xEB)
ALONE_X87(Fldlg2, 0xD9, 0xEC)   ALONE_X87(Fldln2, 0xD9, 0xED)   ALONE_X87(Fldz, 0xD9, 0xEE)
ALONE_X87(F2xm1, 0xD9, 0xF0)    ALONE_X87(Fyl2x, 0xD9, 0xF1)    ALONE_X87(Fptan, 0xD9, 0xF2)
ALONE_X87(Fpatan, 0xD9, 0xF3)   ALONE_X87(Fxtract, 0xD9, 0xF4)  ALONE_X87(Fprem1, 0xD9, 0xF5)
ALONE_X87(Fdecstp, 0xD9, 0xF6)  ALONE_X87(Fincstp, 0xD9, 0xF7)  ALONE_X87(Fprem, 0xD9, 0xF8)
ALONE_X87(Fyl2xp1, 0xD9, 0xF9)  ALONE_X87(Fsqrt, 0xD9, 0xFA)    ALONE_X87(Fsincos, 0xD9, 0xFB)
ALONE_X87(Frndint, 0xD9, 0xFC)  ALONE_X87(Fscale, 0xD9, 0xFD)   ALONE_X87(Fsin, 0xD9, 0xFE)
ALONE_X87(Fcos, 0xD9, 0xFF)     ALONE_X87(Fcompp, 0xDE, 0xD9)   ALONE_X87(Fucompp, 0xDA, 0xE9)

#define X87(Mnemonic, Name) PLAIN(Mnemonic, Name##Forms)

static const BoxAsmInstruction BoxAsmX87Instructions[] = {
    X87("fadd", Fadd),       X87("faddp", Faddp),     X87("fmul", Fmul),
    X87("fmulp", Fmulp),     X87("fsub", Fsub),       X87("fsubp", Fsubp),
    X87("fsubr", Fsubr),     X87("fsubrp", Fsubrp),   X87("fdiv", Fdiv),
    X87("fdivp", Fdivp),     X87("fdivr", Fdivr),     X87("fdivrp", Fdivrp),
    X87("fiadd", Fiadd),     X87("fimul", Fimul),     X87("ficom", Ficom),
    X87("ficomp", Ficomp),   X87("fisub", Fisub),     X87("fisubr", Fisubr),
    X87("fidiv", Fidiv),     X87("fidivr", Fidivr),   X87("fcom", Fcom),
    X87("fcomp", Fcomp),     X87("fucom", Fucom),     X87("fucomp", Fucomp),
    X87("fcomi", Fcomi),     X87("fcomip", Fcomip),   X87("fucomi", Fucomi),
    X87("fucomip", Fucomip), X87("fcmovb", Fcmovb),   X87("fcmove", Fcmove),
    X87("fcmovbe", Fcmovbe), X87("fcmovu", Fcmovu),   X87("fcmovnb", Fcmovnb),
    X87("fcmovne", Fcmovne), X87("fcmovnbe", Fcmovnbe), X87("fcmovnu", Fcmovnu),
    X87("fld", Fld),         X87("fst", Fst),         X87("fstp", Fstp),
    X87("fild", Fild),       X87("fist", Fist),       X87("fistp", Fistp),
    X87("fisttp", Fisttp),   X87("fbld", Fbld),       X87("fbstp", Fbstp),
    X87("fxch", Fxch),       X87("ffree", Ffree),     X87("ffreep", Ffreep),
    X87("fldcw", Fldcw),     X87("fstcw", Fstcw),     X87("fnstcw", Fnstcw),
    X87("fstsw", Fstsw),     X87("fnstsw", Fnstsw),   X87("fldenv", Fldenv),
    X87("fstenv", Fstenv),   X87("fnstenv", Fnstenv), X87("frstor", Frstor),
    X87("fsave", Fsave),     X87("fnsave", Fnsave),   X87("finit", Finit),
    X87("fninit", Fninit),   X87("fclex", Fclex),     X87("fnclex", Fnclex),
    X87("fwait", Fwait),     X87("wait", Fwait),      X87("fxsave", Fxsave),
    X87("fxrstor", Fxrstor), X87("fxsave64", Fxsave64), X87("fxrstor64", Fxrstor64),
    X87("fnop", Fnop),       X87("fchs", Fchs),       X87("fabs", Fabs),
    X87("ftst", Ftst),       X87("fxam", Fxam),       X87("fld1", Fld1),
    X87("fldl2t", Fldl2t),   X87("fldl2e", Fldl2e),   X87("fldpi", Fldpi),
    X87("fldlg2", Fldlg2),   X87("fldln2", Fldln2),   X87("fldz", Fldz),
    X87("f2xm1", F2xm1),     X87("fyl2x", Fyl2x),     X87("fptan", Fptan),
    X87("fpatan", Fpatan),   X87("fxtract", Fxtract), X87("fprem1", Fprem1),
    X87("fdecstp", Fdecstp), X87("fincstp", Fincstp), X87("fprem", Fprem),
    X87("fyl2xp1", Fyl2xp1), X87("fsqrt", Fsqrt),     X87("fsincos", Fsincos),
    X87("frndint", Frndint), X87("fscale", Fscale),   X87("fsin", Fsin),
    X87("fcos", Fcos),       X87("fcompp", Fcompp),   X87("fucompp", Fucompp),
};

const BoxAsmInstruction *BoxAsmFindX87Instruction(const char *Name, size_t Length)
{
    size_t Index;

    for (Index = 0; Index < sizeof(BoxAsmX87Instructions) / sizeof(BoxAsmX87Instructions[0]); Index++)
        if (BoxAsmNameEquals(Name, Length, BoxAsmX87Instructions[Index].Name))
            return &BoxAsmX87Instructions[Index];
    return NULL;
}
