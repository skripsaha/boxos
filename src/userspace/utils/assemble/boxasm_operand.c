#include "boxasm_asm.h"

#include "box/error.h"
#include "box/string.h"

static const char *const BoxAsmRegisters64[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};
static const char *const BoxAsmRegisters32[8] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"
};
static const char *const BoxAsmRegisters16[8] = {
    "ax", "cx", "dx", "bx", "sp", "bp", "si", "di"
};
static const char *const BoxAsmRegisters8[8] = {
    "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"
};
static const char *const BoxAsmRegistersHigh8[4] = { "ah", "ch", "dh", "bh" };

static char BoxAsmLower(char Ch)
{
    return Ch >= 'A' && Ch <= 'Z' ? (char)(Ch - 'A' + 'a') : Ch;
}

static int BoxAsmWordIs(const char *Text, size_t Length, const char *Word)
{
    size_t Index;

    if (strlen(Word) != Length)
        return 0;
    for (Index = 0; Index < Length; Index++)
        if (BoxAsmLower(Text[Index]) != Word[Index])
            return 0;
    return 1;
}

static int BoxAsmFindName(const char *Text, size_t Length, const char *const *Names,
                          uint8_t Count)
{
    uint8_t Index;

    for (Index = 0; Index < Count; Index++)
        if (BoxAsmWordIs(Text, Length, Names[Index]))
            return Index;
    return -1;
}

/* xmm0..xmm15, or ymm0..ymm15 when Letter is 'y'. */
static int BoxAsmVectorRegister(const char *Text, size_t Length, char Letter,
                                BoxAsmRegister *Out)
{
    uint8_t Number;

    if (Length < 4 || Length > 5 || BoxAsmLower(Text[0]) != Letter ||
        BoxAsmLower(Text[1]) != 'm' || BoxAsmLower(Text[2]) != 'm' ||
        Text[3] < '0' || Text[3] > '9' || (Length == 5 && (Text[3] == '0' ||
                                                          Text[4] < '0' || Text[4] > '9')))
        return 0;
    Number = (uint8_t)(Text[3] - '0');
    if (Length == 5)
        Number = (uint8_t)(Number * 10u + (uint8_t)(Text[4] - '0'));
    if (Number > 15)
        return 0;
    *Out = Letter == 'x' ? (BoxAsmRegister){ BOXASM_REGISTER_XMM, Number, 16, 0 }
                         : (BoxAsmRegister){ BOXASM_REGISTER_YMM, Number, 32, 0 };
    return 1;
}

/* r8..r15 with a width suffix: none, d, w, b or l. */
static int BoxAsmExtendedRegister(const char *Text, size_t Length, BoxAsmRegister *Out)
{
    size_t Digits = 0;
    uint8_t Number = 0;
    char Suffix;

    if (Length < 2 || BoxAsmLower(Text[0]) != 'r')
        return 0;
    while (1u + Digits < Length && Text[1u + Digits] >= '0' && Text[1u + Digits] <= '9') {
        Number = (uint8_t)(Number * 10u + (uint8_t)(Text[1u + Digits] - '0'));
        Digits++;
        if (Digits > 2)
            return 0;
    }
    if (Digits == 0 || Number < 8 || Number > 15 || (Digits == 2 && Text[1] == '0'))
        return 0;
    if (1u + Digits == Length) {
        *Out = (BoxAsmRegister){ BOXASM_REGISTER_GENERAL, Number, 8, 0 };
        return 1;
    }
    if (2u + Digits != Length)
        return 0;
    Suffix = BoxAsmLower(Text[1u + Digits]);
    if (Suffix == 'd') *Out = (BoxAsmRegister){ BOXASM_REGISTER_GENERAL, Number, 4, 0 };
    else if (Suffix == 'w') *Out = (BoxAsmRegister){ BOXASM_REGISTER_GENERAL, Number, 2, 0 };
    else if (Suffix == 'b' || Suffix == 'l')
        *Out = (BoxAsmRegister){ BOXASM_REGISTER_GENERAL, Number, 1, 0 };
    else
        return 0;
    return 1;
}

int BoxAsmRegisterLookup(const char *Text, size_t Length, BoxAsmRegister *Out)
{
    int Index;

    if ((Index = BoxAsmFindName(Text, Length, BoxAsmRegisters64, 16)) >= 0) {
        *Out = (BoxAsmRegister){ BOXASM_REGISTER_GENERAL, (uint8_t)Index, 8, 0 };
        return 1;
    }
    if ((Index = BoxAsmFindName(Text, Length, BoxAsmRegisters32, 8)) >= 0) {
        *Out = (BoxAsmRegister){ BOXASM_REGISTER_GENERAL, (uint8_t)Index, 4, 0 };
        return 1;
    }
    if ((Index = BoxAsmFindName(Text, Length, BoxAsmRegisters16, 8)) >= 0) {
        *Out = (BoxAsmRegister){ BOXASM_REGISTER_GENERAL, (uint8_t)Index, 2, 0 };
        return 1;
    }
    if ((Index = BoxAsmFindName(Text, Length, BoxAsmRegisters8, 8)) >= 0) {
        *Out = (BoxAsmRegister){ BOXASM_REGISTER_GENERAL, (uint8_t)Index, 1, (uint8_t)(Index >= 4) };
        return 1;
    }
    if ((Index = BoxAsmFindName(Text, Length, BoxAsmRegistersHigh8, 4)) >= 0) {
        *Out = (BoxAsmRegister){ BOXASM_REGISTER_HIGH8, (uint8_t)(Index + 4), 1, 0 };
        return 1;
    }
    if (BoxAsmWordIs(Text, Length, "rip")) {
        *Out = (BoxAsmRegister){ BOXASM_REGISTER_RIP, 0, 8, 0 };
        return 1;
    }
    if (BoxAsmVectorRegister(Text, Length, 'x', Out) || BoxAsmVectorRegister(Text, Length, 'y', Out))
        return 1;
    return BoxAsmExtendedRegister(Text, Length, Out);
}

static int BoxAsmTokenRegister(const BoxAsmToken *Token, BoxAsmRegister *Out)
{
    return Token->Kind == BOXASM_TOKEN_IDENTIFIER &&
           BoxAsmRegisterLookup(Token->Text, Token->Length, Out);
}

int BoxAsmRegisterOperand(const char *Name, BoxAsmOperand *Out)
{
    memset(Out, 0, sizeof(*Out));
    Out->Kind = BOXASM_OPERAND_REGISTER;
    if (!BoxAsmRegisterLookup(Name, strlen(Name), &Out->Register))
        return -ERR_INVALID_ARGUMENT;
    Out->Size = Out->Register.Size;
    return 0;
}

static uint8_t BoxAsmSizeKeyword(const BoxAsmToken *Token)
{
    if (BoxAsmTokenIs(Token, "byte"))  return 1;
    if (BoxAsmTokenIs(Token, "word"))  return 2;
    if (BoxAsmTokenIs(Token, "dword")) return 4;
    if (BoxAsmTokenIs(Token, "qword")) return 8;
    if (BoxAsmTokenIs(Token, "oword")) return 16;
    if (BoxAsmTokenIs(Token, "yword")) return 32;
    return 0;
}

/* ---- Memory ------------------------------------------------------------- */

/* A register term of an address: the first plain one is the base, a scaled
 * one or a second plain one the index. */
static int BoxAsmAddRegister(BoxAsmAssembler *Asm, BoxAsmOperand *Out,
                             const BoxAsmRegister *Register, uint64_t Scale,
                             uint8_t Scaled, const BoxAsmLocation *Where)
{
    if (Register->Class == BOXASM_REGISTER_RIP) {
        if (Scaled || Out->Base.Class != BOXASM_REGISTER_NONE)
            return BoxAsmFail(Asm, Where, "rip can only be the base of an address", NULL);
        Out->Base = *Register;
        return 0;
    }
    if (Register->Class != BOXASM_REGISTER_GENERAL || Register->Size != 8)
        return BoxAsmFail(Asm, Where, "an address is made of 64-bit registers", NULL);
    if (Scale != 1 && Scale != 2 && Scale != 4 && Scale != 8)
        return BoxAsmFail(Asm, Where, "an index is scaled by 1, 2, 4 or 8", NULL);
    if (!Scaled && Out->Base.Class == BOXASM_REGISTER_NONE) {
        Out->Base = *Register;
        return 0;
    }
    if (Out->Index.Class != BOXASM_REGISTER_NONE)
        return BoxAsmFail(Asm, Where, "an address has one base and one index register", NULL);
    Out->Index = *Register;
    Out->Scale = (uint8_t)Scale;
    return 0;
}

/* A number followed by '*' and a register: the scale written first. */
static int BoxAsmScaleFirst(const BoxAsmLexer *Lexer, const BoxAsmToken *Token,
                            BoxAsmRegister *Register)
{
    BoxAsmLexer Ahead = *Lexer;
    BoxAsmToken Star, Name;
    const char *Why;

    if (Token->Kind != BOXASM_TOKEN_NUMBER || BoxAsmLexNext(&Ahead, &Star, &Why) != 0 ||
        !BoxAsmTokenIsPunct(&Star, '*') || BoxAsmLexNext(&Ahead, &Name, &Why) != 0)
        return 0;
    return BoxAsmTokenRegister(&Name, Register);
}

static int BoxAsmAddDisplacement(BoxAsmAssembler *Asm, BoxAsmOperand *Out, uint16_t Sign,
                                 uint32_t Term, const BoxAsmLocation *Where)
{
    BoxAsmExpr Node;

    memset(&Node, 0, sizeof(Node));
    Node.Where = *Where;
    if (Out->Expr == 0) {
        if (Sign != '-') {
            Out->Expr = Term;
            return 0;
        }
        Node.Kind = BOXASM_EXPR_NEGATE;
        Node.Left = Term;
        return BoxAsmMakeExpr(Asm, &Node, &Out->Expr);
    }
    Node.Kind = BOXASM_EXPR_BINARY;
    Node.Operator = Sign;
    Node.Left = Out->Expr;
    Node.Right = Term;
    return BoxAsmMakeExpr(Asm, &Node, &Out->Expr);
}

static int BoxAsmParseAddressTerm(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                                  BoxAsmOperand *Out, uint16_t Sign)
{
    BoxAsmLocation Where = Token->Where;
    BoxAsmRegister Register;
    uint32_t Term;
    int Rc;

    if (BoxAsmTokenRegister(Token, &Register) || BoxAsmScaleFirst(Lexer, Token, &Register)) {
        uint64_t Scale = 1;
        uint8_t Scaled = 0;

        if (Sign == '-')
            return BoxAsmFail(Asm, &Where, "a register in an address is added, never subtracted", NULL);
        if (Token->Kind == BOXASM_TOKEN_NUMBER) {
            Scale = Token->Number;
            Scaled = 1;
            if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0 ||
                (Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
                return Rc;
        }
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
        if (!Scaled && BoxAsmTokenIsPunct(Token, '*')) {
            if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
                return Rc;
            if (Token->Kind != BOXASM_TOKEN_NUMBER)
                return BoxAsmFail(Asm, &Token->Where, "an index is scaled by 1, 2, 4 or 8", NULL);
            Scale = Token->Number;
            Scaled = 1;
            if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
                return Rc;
        }
        return BoxAsmAddRegister(Asm, Out, &Register, Scale, Scaled, &Where);
    }
    if ((Rc = BoxAsmParseTerm(Asm, Lexer, Token, &Term)) != 0)
        return Rc;
    return BoxAsmAddDisplacement(Asm, Out, Sign, Term, &Where);
}

/* rsp cannot be an index; written as one with no scale, it is the base. */
static int BoxAsmSettleAddress(BoxAsmAssembler *Asm, BoxAsmOperand *Out,
                               const BoxAsmLocation *Where)
{
    if (Out->Index.Class == BOXASM_REGISTER_GENERAL && Out->Index.Number == 4) {
        if (Out->Scale == 1 && Out->Base.Number != 4) {
            BoxAsmRegister Base = Out->Base;
            Out->Base = Out->Index;
            Out->Index = Base;
        } else {
            return BoxAsmFail(Asm, Where, "rsp cannot be an index register", NULL);
        }
    }
    if (Out->Base.Class == BOXASM_REGISTER_RIP && Out->Index.Class != BOXASM_REGISTER_NONE)
        return BoxAsmFail(Asm, Where, "a rip-relative address has no index register", NULL);
    if (Out->Index.Class != BOXASM_REGISTER_NONE && Out->Scale == 0)
        Out->Scale = 1;
    return 0;
}

static int BoxAsmParseMemory(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                             BoxAsmOperand *Out)
{
    BoxAsmLocation Where = Token->Where;
    uint16_t Sign = '+';
    int Rc;

    Out->Kind = BOXASM_OPERAND_MEMORY;
    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
        return Rc;
    if (BoxAsmTokenIs(Token, "abs") || BoxAsmTokenIs(Token, "rel")) {
        Out->Absolute = BoxAsmTokenIs(Token, "abs");
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
    } else {
        Out->Absolute = Asm->DefaultAbsolute;
    }
    if ((BoxAsmTokenIs(Token, "fs") || BoxAsmTokenIs(Token, "gs") || BoxAsmTokenIs(Token, "ds") ||
         BoxAsmTokenIs(Token, "es") || BoxAsmTokenIs(Token, "ss") || BoxAsmTokenIs(Token, "cs")))
        return BoxAsmFail(Asm, &Token->Where, "segment overrides are not part of BoxASM", NULL);
    for (;;) {
        if ((Rc = BoxAsmParseAddressTerm(Asm, Lexer, Token, Out, Sign)) != 0)
            return Rc;
        if (BoxAsmTokenIsPunct(Token, ']'))
            break;
        if (!BoxAsmTokenIsPunct(Token, '+') && !BoxAsmTokenIsPunct(Token, '-'))
            return BoxAsmFail(Asm, &Token->Where, "expected '+', '-' or ']' in the address", NULL);
        Sign = Token->Punct;
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
    }
    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
        return Rc;
    return BoxAsmSettleAddress(Asm, Out, &Where);
}

/* A vector register BoxASM does not have — mm0..mm7 of MMX, xmm and ymm16 and
 * up and zmm of AVX-512 — named as such rather than taken for a label nobody
 * defined. */
static const char *BoxAsmVectorNotBuilt(const BoxAsmToken *Token)
{
    const char *Text = Token->Text;
    size_t Length = Token->Length;
    size_t Letters = 0;
    size_t Index;
    unsigned Number = 0;

    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER)
        return NULL;
    while (Letters < Length && Letters < 3 &&
           ((Text[Letters] >= 'a' && Text[Letters] <= 'z') ||
            (Text[Letters] >= 'A' && Text[Letters] <= 'Z')))
        Letters++;
    if (Letters == Length || Length - Letters > 2 ||
        (Length - Letters == 2 && Text[Letters] == '0'))
        return NULL;
    for (Index = Letters; Index < Length; Index++) {
        if (Text[Index] < '0' || Text[Index] > '9')
            return NULL;
        Number = Number * 10u + (unsigned)(Text[Index] - '0');
    }
    if (Letters == 2 && BoxAsmLower(Text[0]) == 'm' && BoxAsmLower(Text[1]) == 'm')
        return Number < 8 ? "is an MMX register; BoxASM computes vectors in xmm0 to xmm15 "
                            "and ymm0 to ymm15"
                          : NULL;
    if (Letters != 3 || BoxAsmLower(Text[1]) != 'm' || BoxAsmLower(Text[2]) != 'm' ||
        Number > 31)
        return NULL;
    switch (BoxAsmLower(Text[0])) {
    case 'x':
    case 'y': return Number >= 16 ? "is an AVX-512 register; BoxASM has xmm0 to xmm15 and "
                                    "ymm0 to ymm15"
                                  : NULL;
    case 'z': return "is an AVX-512 register; BoxASM has xmm0 to xmm15 and ymm0 to ymm15";
    default:  return NULL;
    }
}

/* register | [size] '[' address ']' | [size] [short|near] expression */
int BoxAsmParseOperand(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                       BoxAsmOperand *Out)
{
    uint8_t Size;
    int Rc;

    memset(Out, 0, sizeof(*Out));
    Out->Where = Token->Where;
    if ((Size = BoxAsmSizeKeyword(Token)) != 0) {
        Out->Size = Size;
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
        if (BoxAsmTokenIs(Token, "ptr"))
            return BoxAsmFail(Asm, &Token->Where,
                              "BoxASM writes `qword [x]`, without `ptr`", NULL);
    }
    if (BoxAsmTokenRegister(Token, &Out->Register)) {
        if (Size != 0)
            return BoxAsmFail(Asm, &Out->Where, "a register already has its size", NULL);
        if (Out->Register.Class == BOXASM_REGISTER_RIP)
            return BoxAsmFail(Asm, &Token->Where, "rip is reached only inside [ ]", NULL);
        Out->Kind = BOXASM_OPERAND_REGISTER;
        Out->Size = Out->Register.Size;
        return BoxAsmNextToken(Asm, Lexer, Token);
    }
    if (BoxAsmTokenIsPunct(Token, '['))
        return BoxAsmParseMemory(Asm, Lexer, Token, Out);
    if (BoxAsmVectorNotBuilt(Token)) {
        BoxAsmMessage Message;

        BoxAsmFailBegin(Asm, &Token->Where, &Message);
        BoxAsmMessageText(&Message, "'");
        BoxAsmMessageSpan(&Message, Token->Text, Token->Length);
        BoxAsmMessageText(&Message, "' ");
        BoxAsmMessageText(&Message, BoxAsmVectorNotBuilt(Token));
        return BoxAsmFailEnd(Asm, &Token->Where, &Message);
    }
    if (BoxAsmTokenIs(Token, "short") || BoxAsmTokenIs(Token, "near")) {
        Out->Reach = BoxAsmTokenIs(Token, "short") ? BOXASM_BRANCH_SHORT : BOXASM_BRANCH_NEAR;
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
    }
    Out->Kind = BOXASM_OPERAND_IMMEDIATE;
    return BoxAsmParseExpression(Asm, Lexer, Token, &Out->Expr);
}
