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
#define SW   BOXASM_SIZE_WIDE
#define R    BOXASM_DIGIT_REG
#define NO   BOXASM_DIGIT_NONE

#define ALU(Base, Digit)                                                  \
    { C(RM, REG, NONE),    S8, { (Base) + 0 }, 1, R, 0 },                 \
    { C(RM, REG, NONE),    SW, { (Base) + 1 }, 1, R, 0 },                 \
    { C(REG, RM, NONE),    S8, { (Base) + 2 }, 1, R, 0 },                 \
    { C(REG, RM, NONE),    SW, { (Base) + 3 }, 1, R, 0 },                 \
    { C(RM, IMM8S, NONE),  SW, { 0x83 }, 1, (Digit), 0 },                 \
    { C(ACC, IMM, NONE),   S8, { (Base) + 4 }, 1, NO, 0 },                \
    { C(ACC, IMM, NONE),   SW, { (Base) + 5 }, 1, NO, 0 },                \
    { C(RM, IMM, NONE),    S8, { 0x80 }, 1, (Digit), 0 },                 \
    { C(RM, IMM, NONE),    SW, { 0x81 }, 1, (Digit), 0 }

#define UNARY(Digit, Byte, Wide)                                          \
    { C(RM, NONE, NONE), S8, { (Byte) }, 1, (Digit), 0 },                 \
    { C(RM, NONE, NONE), SW, { (Wide) }, 1, (Digit), 0 }

#define SHIFT(Digit)                                                      \
    { C(RM, ONE, NONE),  S8, { 0xD0 }, 1, (Digit), 0 },                   \
    { C(RM, CL, NONE),   S8, { 0xD2 }, 1, (Digit), 0 },                   \
    { C(RM, IMM8, NONE), S8, { 0xC0 }, 1, (Digit), 0 },                   \
    { C(RM, ONE, NONE),  SW, { 0xD1 }, 1, (Digit), 0 },                   \
    { C(RM, CL, NONE),   SW, { 0xD3 }, 1, (Digit), 0 },                   \
    { C(RM, IMM8, NONE), SW, { 0xC1 }, 1, (Digit), 0 }

#define ALONE(Sizes, ...) { C(NONE, NONE, NONE), (Sizes), { __VA_ARGS__ }, \
    sizeof((uint8_t[]){ __VA_ARGS__ }), NO, 0 }

static const BoxAsmForm AddForms[] = { ALU(0x00, 0) };
static const BoxAsmForm OrForms[]  = { ALU(0x08, 1) };
static const BoxAsmForm AdcForms[] = { ALU(0x10, 2) };
static const BoxAsmForm SbbForms[] = { ALU(0x18, 3) };
static const BoxAsmForm AndForms[] = { ALU(0x20, 4) };
static const BoxAsmForm SubForms[] = { ALU(0x28, 5) };
static const BoxAsmForm XorForms[] = { ALU(0x30, 6) };
static const BoxAsmForm CmpForms[] = { ALU(0x38, 7) };

static const BoxAsmForm TestForms[] = {
    { C(RM, REG, NONE),  S8, { 0x84 }, 1, R, 0 },
    { C(RM, REG, NONE),  SW, { 0x85 }, 1, R, 0 },
    { C(REG, RM, NONE),  S8, { 0x84 }, 1, R, 0 },
    { C(REG, RM, NONE),  SW, { 0x85 }, 1, R, 0 },
    { C(ACC, IMM, NONE), S8, { 0xA8 }, 1, NO, 0 },
    { C(ACC, IMM, NONE), SW, { 0xA9 }, 1, NO, 0 },
    { C(RM, IMM, NONE),  S8, { 0xF6 }, 1, 0, 0 },
    { C(RM, IMM, NONE),  SW, { 0xF7 }, 1, 0, 0 },
};

static const BoxAsmForm MovForms[] = {
    { C(RM, REG, NONE),      S8,        { 0x88 }, 1, R, 0 },
    { C(RM, REG, NONE),      SW,        { 0x89 }, 1, R, 0 },
    { C(REG, RM, NONE),      S8,        { 0x8A }, 1, R, 0 },
    { C(REG, RM, NONE),      SW,        { 0x8B }, 1, R, 0 },
    { C(REG, IMM, NONE),     S8,        { 0xB0 }, 1, NO, BOXASM_FORM_PLUS_REG },
    { C(REG, IMM, NONE),     S16 | S32, { 0xB8 }, 1, NO, BOXASM_FORM_PLUS_REG },
    { C(REG, IMM_U32, NONE), S64,       { 0xB8 }, 1, NO,
      BOXASM_FORM_PLUS_REG | BOXASM_FORM_AS32 },
    { C(REG, IMM_S32, NONE), S64,       { 0xC7 }, 1, 0, 0 },
    { C(REG, IMM64, NONE),   S64,       { 0xB8 }, 1, NO, BOXASM_FORM_PLUS_REG },
    { C(RM, IMM, NONE),      S8,        { 0xC6 }, 1, 0, 0 },
    { C(RM, IMM, NONE),      SW,        { 0xC7 }, 1, 0, 0 },
};

static const BoxAsmForm LeaForms[] = {
    { C(REG, ADDRESS, NONE), SW, { 0x8D }, 1, R, 0 },
};

static const BoxAsmForm XchgForms[] = {
    { C(ACC, REG, NONE), SW, { 0x90 }, 1, NO, BOXASM_FORM_PLUS_REG | BOXASM_FORM_NOT_EAX },
    { C(REG, ACC, NONE), SW, { 0x90 }, 1, NO, BOXASM_FORM_PLUS_REG | BOXASM_FORM_NOT_EAX },
    { C(REG, RM, NONE),  S8, { 0x86 }, 1, R, 0 },
    { C(REG, RM, NONE),  SW, { 0x87 }, 1, R, 0 },
    { C(RM, REG, NONE),  S8, { 0x86 }, 1, R, 0 },
    { C(RM, REG, NONE),  SW, { 0x87 }, 1, R, 0 },
};

static const BoxAsmForm PushForms[] = {
    { C(REG, NONE, NONE),   S64, { 0x50 }, 1, NO, BOXASM_FORM_PLUS_REG | BOXASM_FORM_DEFAULT64 },
    { C(MEM, NONE, NONE),   S64, { 0xFF }, 1, 6,  BOXASM_FORM_DEFAULT64 },
    { C(IMM8S, NONE, NONE), S64, { 0x6A }, 1, NO, BOXASM_FORM_DEFAULT64 },
    { C(IMM, NONE, NONE),   S64, { 0x68 }, 1, NO, BOXASM_FORM_DEFAULT64 },
};

static const BoxAsmForm PopForms[] = {
    { C(REG, NONE, NONE), S64, { 0x58 }, 1, NO, BOXASM_FORM_PLUS_REG | BOXASM_FORM_DEFAULT64 },
    { C(MEM, NONE, NONE), S64, { 0x8F }, 1, 0,  BOXASM_FORM_DEFAULT64 },
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
    { C(REG, RM, NONE),  SW, { 0x0F, 0xAF }, 2, R, 0 },
    { C(REG, RM, IMM8S), SW, { 0x6B }, 1, R, 0 },
    { C(REG, RM, IMM),   SW, { 0x69 }, 1, R, 0 },
};

static const BoxAsmForm RolForms[] = { SHIFT(0) };
static const BoxAsmForm RorForms[] = { SHIFT(1) };
static const BoxAsmForm RclForms[] = { SHIFT(2) };
static const BoxAsmForm RcrForms[] = { SHIFT(3) };
static const BoxAsmForm ShlForms[] = { SHIFT(4) };
static const BoxAsmForm ShrForms[] = { SHIFT(5) };
static const BoxAsmForm SarForms[] = { SHIFT(7) };

static const BoxAsmForm JmpForms[] = {
    { C(REL, NONE, NONE), 0,   { 0xEB }, 1, NO, BOXASM_FORM_SHORT },
    { C(REL, NONE, NONE), 0,   { 0xE9 }, 1, NO, 0 },
    { C(RM, NONE, NONE),  S64, { 0xFF }, 1, 4,  BOXASM_FORM_DEFAULT64 },
};

static const BoxAsmForm JccForms[] = {
    { C(REL, NONE, NONE), 0, { 0x70 },       1, NO, BOXASM_FORM_SHORT | BOXASM_FORM_CONDITION },
    { C(REL, NONE, NONE), 0, { 0x0F, 0x80 }, 2, NO, BOXASM_FORM_CONDITION },
};

static const BoxAsmForm CallForms[] = {
    { C(REL, NONE, NONE), 0,   { 0xE8 }, 1, NO, 0 },
    { C(RM, NONE, NONE),  S64, { 0xFF }, 1, 2,  BOXASM_FORM_DEFAULT64 },
};

static const BoxAsmForm SetccForms[] = {
    { C(RM, NONE, NONE), S8, { 0x0F, 0x90 }, 2, 0, BOXASM_FORM_CONDITION },
};

static const BoxAsmForm CmovccForms[] = {
    { C(REG, RM, NONE), SW, { 0x0F, 0x40 }, 2, R, BOXASM_FORM_CONDITION },
};

static const BoxAsmForm MovzxForms[] = {
    { C(REG, RM8, NONE),  SW,        { 0x0F, 0xB6 }, 2, R, 0 },
    { C(REG, RM16, NONE), S32 | S64, { 0x0F, 0xB7 }, 2, R, 0 },
};

static const BoxAsmForm MovsxForms[] = {
    { C(REG, RM8, NONE),  SW,        { 0x0F, 0xBE }, 2, R, 0 },
    { C(REG, RM16, NONE), S32 | S64, { 0x0F, 0xBF }, 2, R, 0 },
};

static const BoxAsmForm MovsxdForms[] = {
    { C(REG, RM32, NONE), S64, { 0x63 }, 1, R, 0 },
};

static const BoxAsmForm RetForms[] = {
    ALONE(0, 0xC3),
    { C(IMM16, NONE, NONE), 0, { 0xC2 }, 1, NO, 0 },
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

#define INSTRUCTION(Name, Forms, Kind, Conditional, Swap) \
    { Name, Forms, sizeof(Forms) / sizeof(Forms[0]), Kind, Conditional, Swap }
#define PLAIN(Name, Forms) INSTRUCTION(Name, Forms, BOXASM_KIND_PLAIN, 0, 0)

static const BoxAsmInstruction BoxAsmInstructions[] = {
    PLAIN("add", AddForms),   PLAIN("or", OrForms),     PLAIN("adc", AdcForms),
    PLAIN("sbb", SbbForms),   PLAIN("and", AndForms),   PLAIN("sub", SubForms),
    PLAIN("xor", XorForms),   PLAIN("cmp", CmpForms),   PLAIN("test", TestForms),
    PLAIN("mov", MovForms),   PLAIN("lea", LeaForms),   PLAIN("xchg", XchgForms),
    PLAIN("push", PushForms), PLAIN("pop", PopForms),
    PLAIN("inc", IncForms),   PLAIN("dec", DecForms),   PLAIN("not", NotForms),
    PLAIN("neg", NegForms),   PLAIN("mul", MulForms),   PLAIN("div", DivForms),
    PLAIN("idiv", IdivForms),
    INSTRUCTION("imul", ImulForms, BOXASM_KIND_PLAIN, 0, 1),
    PLAIN("rol", RolForms),   PLAIN("ror", RorForms),   PLAIN("rcl", RclForms),
    PLAIN("rcr", RcrForms),   PLAIN("shl", ShlForms),   PLAIN("sal", ShlForms),
    PLAIN("shr", ShrForms),   PLAIN("sar", SarForms),
    INSTRUCTION("jmp", JmpForms, BOXASM_KIND_JUMP, 0, 0),
    INSTRUCTION("call", CallForms, BOXASM_KIND_CALL, 0, 0),
    PLAIN("movzx", MovzxForms), PLAIN("movsx", MovsxForms), PLAIN("movsxd", MovsxdForms),
    PLAIN("ret", RetForms),   PLAIN("nop", NopForms),   PLAIN("int3", Int3Forms),
    PLAIN("leave", LeaveForms),
    PLAIN("cqo", CqoForms),   PLAIN("cdq", CdqForms),   PLAIN("cwd", CwdForms),
    PLAIN("cdqe", CdqeForms), PLAIN("cwde", CwdeForms), PLAIN("cbw", CbwForms),
    PLAIN("pushfq", PushfForms), PLAIN("pushf", PushfForms),
    PLAIN("popfq", PopfForms),   PLAIN("popf", PopfForms),
    PLAIN("cld", CldForms),   PLAIN("std", StdForms),   PLAIN("endbr64", Endbr64Forms),
    PLAIN("pause", PauseForms), PLAIN("ud2", Ud2Forms),
};

static const BoxAsmInstruction BoxAsmJcc =
    INSTRUCTION("j", JccForms, BOXASM_KIND_JUMP, 1, 0);
static const BoxAsmInstruction BoxAsmSetcc =
    INSTRUCTION("set", SetccForms, BOXASM_KIND_PLAIN, 1, 0);
static const BoxAsmInstruction BoxAsmCmovcc =
    INSTRUCTION("cmov", CmovccForms, BOXASM_KIND_PLAIN, 1, 0);

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
