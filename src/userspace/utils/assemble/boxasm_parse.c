#include "boxasm_float.h"
#include "boxasm_isa.h"
#include "boxasm_macro.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* ---- Building statements ------------------------------------------------ */

int BoxAsmAddStatement(BoxAsmAssembler *Asm, uint8_t Kind, const BoxAsmLocation *Where,
                       uint32_t *Out)
{
    BoxAsmStatement *Statement;

    if (Asm->StatementCount >= UINT32_MAX ||
        BoxAsmGrowArray((void **)&Asm->Statements, &Asm->StatementCapacity,
                        Asm->StatementCount, sizeof(BoxAsmStatement)) != 0)
        return -ERR_NO_MEMORY;
    Statement = &Asm->Statements[Asm->StatementCount];
    memset(Statement, 0, sizeof(*Statement));
    Statement->Kind = Kind;
    Statement->Section = Asm->CurrentSection;
    Statement->Where = *Where;
    Asm->Sections[Asm->CurrentSection].Used = 1;
    *Out = (uint32_t)Asm->StatementCount++;
    return 0;
}

int BoxAsmDefineLabel(BoxAsmAssembler *Asm, const char *Name, const BoxAsmLocation *Where,
                      uint32_t *Out)
{
    BoxAsmSymbol *Symbol;
    uint32_t Statement;
    int Rc = BoxAsmSymbolClaim(Asm, Name, strlen(Name), Where, Out);

    if (Rc != 0)
        return Rc;
    Symbol = &Asm->Symbols[*Out];
    if (Symbol->Kind == BOXASM_SYMBOL_EXTERN)
        return BoxAsmFail(Asm, Where, "is declared extern, so this program cannot define it",
                          Symbol->Name);
    if (Symbol->Kind != BOXASM_SYMBOL_UNDEFINED)
        return BoxAsmFail(Asm, Where, "is already defined", Symbol->Name);
    Symbol->Kind = BOXASM_SYMBOL_LABEL;
    Symbol->Section = Asm->CurrentSection;
    Symbol->Defined = *Where;
    if ((Rc = BoxAsmAddStatement(Asm, BOXASM_STMT_LABEL, Where, &Statement)) != 0)
        return Rc;
    Asm->Statements[Statement].Symbol = *Out;
    return 0;
}

int BoxAsmAddInstruction(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, const char *Name,
                         const BoxAsmOperand *Operands, uint8_t Count)
{
    const BoxAsmInstruction *Instruction;
    BoxAsmStatement *Statement;
    uint8_t Condition;
    uint32_t Index;
    int Rc;

    Instruction = BoxAsmFindInstruction(Name, strlen(Name), &Condition);
    if (!Instruction || Count > 3)
        return -ERR_INVALID_ARGUMENT;
    if ((Rc = BoxAsmAddStatement(Asm, BOXASM_STMT_INSTRUCTION, Where, &Index)) != 0)
        return Rc;
    Statement = &Asm->Statements[Index];
    Statement->Instruction = Instruction;
    Statement->Condition = Condition;
    Statement->OperandCount = Count;
    if (Count != 0)
        memcpy(Statement->Operands, Operands, Count * sizeof(BoxAsmOperand));
    return 0;
}

static int BoxAsmAddItem(BoxAsmAssembler *Asm, const BoxAsmDataItem *Item)
{
    if (Asm->ItemCount >= UINT32_MAX ||
        BoxAsmGrowArray((void **)&Asm->Items, &Asm->ItemCapacity, Asm->ItemCount,
                        sizeof(BoxAsmDataItem)) != 0)
        return -ERR_NO_MEMORY;
    Asm->Items[Asm->ItemCount++] = *Item;
    return 0;
}

static int BoxAsmAddStringItem(BoxAsmAssembler *Asm, const BoxAsmLocation *Where,
                               const uint8_t *Bytes, size_t Length)
{
    BoxAsmDataItem Item;

    if (Asm->StringBytes.Size > UINT32_MAX || Length > UINT32_MAX ||
        BoxAsmBufferAppend(&Asm->StringBytes, Bytes, Length) != 0)
        return -ERR_NO_MEMORY;
    memset(&Item, 0, sizeof(Item));
    Item.IsString = 1;
    Item.StringOffset = (uint32_t)(Asm->StringBytes.Size - Length);
    Item.StringLength = (uint32_t)Length;
    Item.Where = *Where;
    return BoxAsmAddItem(Asm, &Item);
}

int BoxAsmAddBytes(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, const uint8_t *Bytes,
                   size_t Length)
{
    uint32_t Index;
    int Rc = BoxAsmAddStatement(Asm, BOXASM_STMT_DATA, Where, &Index);

    if (Rc != 0)
        return Rc;
    Asm->Statements[Index].Width = 1;
    Asm->Statements[Index].FirstItem = (uint32_t)Asm->ItemCount;
    Asm->Statements[Index].ItemCount = 1;
    return BoxAsmAddStringItem(Asm, Where, Bytes, Length);
}

/* ---- Words -------------------------------------------------------------- */

static uint8_t BoxAsmDataWidth(const BoxAsmToken *Token, uint8_t *Reserve)
{
    static const char *const Data[4] = { "db", "dw", "dd", "dq" };
    static const char *const Space[4] = { "resb", "resw", "resd", "resq" };
    uint8_t Index;

    for (Index = 0; Index < 4; Index++) {
        if (BoxAsmTokenIs(Token, Data[Index]) || BoxAsmTokenIs(Token, Space[Index])) {
            *Reserve = BoxAsmTokenIs(Token, Space[Index]);
            return (uint8_t)(1u << Index);
        }
    }
    return 0;
}

static int BoxAsmIsDirective(const BoxAsmToken *Token)
{
    static const char *const Words[] = {
        "section", "segment", "global", "extern", "default", "bits", "align", "times", "equ",
        "struc", "endstruc", "istruc", "iend", "alignb", "takes", "gives", "incbin"
    };
    uint8_t Reserve;
    size_t Index;

    for (Index = 0; Index < sizeof(Words) / sizeof(Words[0]); Index++)
        if (BoxAsmTokenIs(Token, Words[Index]))
            return 1;
    return BoxAsmDataWidth(Token, &Reserve) != 0;
}

static int BoxAsmIsBox(const BoxAsmToken *Token)
{
    return Token->Kind == BOXASM_TOKEN_IDENTIFIER && Token->Length > 4 &&
           memcmp(Token->Text, "box.", 4u) == 0;
}

/* Whether a word means something here, so it is not taken for a label. */
static int BoxAsmIsKnownWord(const BoxAsmToken *Token)
{
    uint8_t Condition;

    return BoxAsmIsDirective(Token) || BoxAsmIsBox(Token) ||
           BoxAsmFindInstruction(Token->Text, Token->Length, &Condition) != NULL ||
           BoxAsmFindPrefix(Token->Text, Token->Length) != BOXASM_PREFIX_NONE;
}

static int BoxAsmEndOfStatement(const BoxAsmToken *Token)
{
    return Token->Kind == BOXASM_TOKEN_NEWLINE || Token->Kind == BOXASM_TOKEN_END;
}

/* Whether the token after this one ends a list item: so a string is the
 * item, not the start of an expression. */
static int BoxAsmStringStandsAlone(const BoxAsmLexer *Lexer, const BoxAsmToken *Token)
{
    BoxAsmLexer Ahead = *Lexer;
    BoxAsmToken Next;
    const char *Why;

    return Token->Kind == BOXASM_TOKEN_STRING && BoxAsmLexNext(&Ahead, &Next, &Why) == 0 &&
           (BoxAsmTokenIsPunct(&Next, ',') || BoxAsmEndOfStatement(&Next));
}

/* ---- Directives --------------------------------------------------------- */

static int BoxAsmParseSection(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token)
{
    uint8_t Index;

    for (Index = 0; Index < BOXASM_SECTION_COUNT; Index++) {
        if (Token->Kind == BOXASM_TOKEN_IDENTIFIER &&
            Token->Length == strlen(BoxAsmSectionNames[Index]) &&
            memcmp(Token->Text, BoxAsmSectionNames[Index], Token->Length) == 0) {
            Asm->CurrentSection = Index;
            return BoxAsmNextToken(Asm, Lexer, Token);
        }
    }
    return BoxAsmFail(Asm, &Token->Where,
                      "BoxASM places code and data in .text, .rodata, .data and .bss", NULL);
}

static int BoxAsmParseNames(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                            uint8_t Extern)
{
    for (;;) {
        BoxAsmSymbol *Symbol;
        uint32_t Index;
        char *Name;
        int Rc;

        if (Token->Kind != BOXASM_TOKEN_IDENTIFIER || Token->Text[0] == '.')
            return BoxAsmFail(Asm, &Token->Where, "expected a plain name", NULL);
        if ((Rc = BoxAsmLabelName(Asm, Token, &Name)) != 0)
            return Rc;
        Rc = BoxAsmSymbolClaim(Asm, Name, strlen(Name), &Token->Where, &Index);
        free(Name);
        if (Rc != 0)
            return Rc;
        Symbol = &Asm->Symbols[Index];
        if (Extern && Symbol->Kind != BOXASM_SYMBOL_UNDEFINED && Symbol->Kind != BOXASM_SYMBOL_EXTERN)
            return BoxAsmFail(Asm, &Token->Where, "is defined here, so it is not extern", Symbol->Name);
        if ((Extern && Symbol->Global) ||
            (!Extern && Symbol->Kind == BOXASM_SYMBOL_EXTERN))
            return BoxAsmFail(Asm, &Token->Where, "cannot be both global and extern", Symbol->Name);
        if (Extern)
            Symbol->Kind = BOXASM_SYMBOL_EXTERN;
        else
            Symbol->Global = 1;
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
        if (!BoxAsmTokenIsPunct(Token, ','))
            return 0;
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
    }
}

static int BoxAsmIsFloatWord(const BoxAsmToken *Token)
{
    uint64_t Bits;

    return Token->Kind == BOXASM_TOKEN_FLOAT ||
           (Token->Kind == BOXASM_TOKEN_IDENTIFIER &&
            BoxAsmFloatSpecial(Token->Text, Token->Length, 0, 8, &Bits));
}

/* Whether the item ahead is a floating-point constant: one, or a sign
 * before one. */
static int BoxAsmFloatAhead(const BoxAsmLexer *Lexer, const BoxAsmToken *Token)
{
    BoxAsmLexer Ahead = *Lexer;
    BoxAsmToken Next;
    const char *Why;

    if (BoxAsmIsFloatWord(Token))
        return 1;
    return (BoxAsmTokenIsPunct(Token, '-') || BoxAsmTokenIsPunct(Token, '+')) &&
           BoxAsmLexNext(&Ahead, &Next, &Why) == 0 && BoxAsmIsFloatWord(&Next);
}

/* A floating-point item of dd (binary32) or dq (binary64), its bits made a
 * number the item holds. */
static int BoxAsmParseFloatItem(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                                uint8_t Width, BoxAsmDataItem *Item)
{
    BoxAsmExpr Node;
    BoxAsmMessage Message;
    const char *Why = NULL;
    int Negative = 0;
    int Rc;

    if (Width != 4 && Width != 8)
        return BoxAsmFail(Asm, &Token->Where,
                          "a floating-point number goes in dd (32-bit) or dq (64-bit)", NULL);
    if (BoxAsmTokenIsPunct(Token, '-') || BoxAsmTokenIsPunct(Token, '+')) {
        Negative = BoxAsmTokenIsPunct(Token, '-');
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
    }
    memset(&Node, 0, sizeof(Node));
    Node.Kind = BOXASM_EXPR_NUMBER;
    Node.Where = Token->Where;
    if (Token->Kind == BOXASM_TOKEN_IDENTIFIER)
        (void)BoxAsmFloatSpecial(Token->Text, Token->Length, Negative, Width, &Node.Number);
    else if (BoxAsmFloatBits(Token->Text, Token->Length, Negative, Width, &Node.Number, &Why) != 0) {
        BoxAsmFailBegin(Asm, &Token->Where, &Message);
        BoxAsmMessageSpan(&Message, Token->Text, Token->Length);
        BoxAsmMessageText(&Message, " ");
        BoxAsmMessageText(&Message, Why);
        return BoxAsmFailEnd(Asm, &Token->Where, &Message);
    }
    if ((Rc = BoxAsmMakeExpr(Asm, &Node, &Item->Expr)) != 0 ||
        (Rc = BoxAsmAddItem(Asm, Item)) != 0)
        return Rc;
    return BoxAsmNextToken(Asm, Lexer, Token);
}

static int BoxAsmParseData(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                           uint32_t Index)
{
    for (;;) {
        BoxAsmDataItem Item;
        int Rc;

        memset(&Item, 0, sizeof(Item));
        Item.Where = Token->Where;
        if (BoxAsmFloatAhead(Lexer, Token)) {
            Rc = BoxAsmParseFloatItem(Asm, Lexer, Token, Asm->Statements[Index].Width, &Item);
        } else if (BoxAsmStringStandsAlone(Lexer, Token)) {
            BoxAsmBuffer Bytes = { 0 };
            Rc = BoxAsmLexStringBytes(Token, &Bytes);
            if (Rc == 0)
                Rc = BoxAsmAddStringItem(Asm, &Token->Where, Bytes.Data, Bytes.Size);
            BoxAsmBufferRelease(&Bytes);
            if (Rc == 0)
                Rc = BoxAsmNextToken(Asm, Lexer, Token);
        } else {
            Rc = BoxAsmParseExpression(Asm, Lexer, Token, &Item.Expr);
            if (Rc == 0)
                Rc = BoxAsmAddItem(Asm, &Item);
        }
        if (Rc != 0)
            return Rc;
        Asm->Statements[Index].ItemCount++;
        if (!BoxAsmTokenIsPunct(Token, ','))
            return 0;
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
    }
}

/* db dw dd dq and resb resw resd resq: data in its sections, space in .bss. */
static int BoxAsmParseStorage(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                              const BoxAsmToken *Word, uint32_t Times)
{
    uint8_t Reserve;
    uint8_t Width = BoxAsmDataWidth(Word, &Reserve);
    int InBss = Asm->CurrentSection == BOXASM_SECTION_BSS;
    uint32_t Index;
    int Rc;

    if (Reserve && !InBss)
        return BoxAsmFail(Asm, &Word->Where,
                          "reserves space only in .bss; elsewhere write `times N db 0`", NULL);
    if (!Reserve && InBss)
        return BoxAsmFail(Asm, &Word->Where,
                          ".bss holds no bytes: reserve its space with resb, resw, resd or resq", NULL);
    if ((Rc = BoxAsmAddStatement(Asm, Reserve ? BOXASM_STMT_RESERVE : BOXASM_STMT_DATA,
                                 &Word->Where, &Index)) != 0)
        return Rc;
    Asm->Statements[Index].Width = Width;
    Asm->Statements[Index].Times = Times;
    Asm->Statements[Index].FirstItem = (uint32_t)Asm->ItemCount;
    if (Reserve)
        return BoxAsmParseExpression(Asm, Lexer, Token, &Asm->Statements[Index].Count);
    return BoxAsmParseData(Asm, Lexer, Token, Index);
}

/* incbin's skip or count: a number known where the line stands, 0 or more. */
static int BoxAsmIncbinNumber(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                              const char *What, int64_t *Out)
{
    uint32_t Expr;
    int Rc;

    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0 ||
        (Rc = BoxAsmParseExpression(Asm, Lexer, Token, &Expr)) != 0 ||
        (Rc = BoxAsmEvaluateConstant(Asm, Expr, 0, Asm->CurrentSection, Out)) != 0)
        return Rc;
    if (*Out < 0)
        return BoxAsmFail(Asm, &Asm->Exprs[Expr].Where, What, NULL);
    return 0;
}

/* incbin "name" [tags] [everywhere][, skip[, count]]: the bytes of the file
 * the preprocessor found for the line, past the first skip, count of them or
 * as many as there are, laid out as db lays out a string. */
static int BoxAsmParseIncbin(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                             const BoxAsmToken *Word, uint32_t Times)
{
    const BoxAsmIncbin *Read = BoxAsmExpansionIncbin(Asm->Expansion, Word->Where.Line);
    int64_t Skip = 0;
    int64_t Count = -1;
    uint32_t Index;
    int Rc;

    if (Asm->CurrentSection == BOXASM_SECTION_BSS)
        return BoxAsmFail(Asm, &Word->Where,
                          ".bss holds no bytes: reserve its space with resb, resw, resd or resq", NULL);
    if (!Read)
        return BoxAsmFail(Asm, &Word->Where, "incbin names its file in quotes: incbin \"data.bin\"", NULL);
    while (!BoxAsmEndOfStatement(Token) && !BoxAsmTokenIsPunct(Token, ','))   /* the file, found */
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
    if (BoxAsmTokenIsPunct(Token, ',') &&
        (Rc = BoxAsmIncbinNumber(Asm, Lexer, Token, "incbin skips 0 or more bytes", &Skip)) != 0)
        return Rc;
    if (BoxAsmTokenIsPunct(Token, ',') &&
        (Rc = BoxAsmIncbinNumber(Asm, Lexer, Token, "incbin takes 0 or more bytes", &Count)) != 0)
        return Rc;
    if ((uint64_t)Skip > Read->Size) {
        BoxAsmMessage Message;

        BoxAsmFailBegin(Asm, &Word->Where, &Message);
        BoxAsmMessageText(&Message, "the file holds ");
        BoxAsmMessageDecimal(&Message, (int64_t)Read->Size);
        BoxAsmMessageText(&Message, " bytes, fewer than the ");
        BoxAsmMessageDecimal(&Message, Skip);
        BoxAsmMessageText(&Message, " incbin skips");
        return BoxAsmFailEnd(Asm, &Word->Where, &Message);
    }
    if (Count < 0 || (uint64_t)Count > Read->Size - (uint64_t)Skip)
        Count = (int64_t)(Read->Size - (uint64_t)Skip);
    if (Asm->StringBytes.Size + (uint64_t)Count > UINT32_MAX)
        return BoxAsmFail(Asm, &Word->Where,
                          "the program's strings and included files pass 4 GiB", NULL);
    if ((Rc = BoxAsmAddStatement(Asm, BOXASM_STMT_DATA, &Word->Where, &Index)) != 0)
        return Rc;
    Asm->Statements[Index].Width = 1;
    Asm->Statements[Index].Times = Times;
    Asm->Statements[Index].FirstItem = (uint32_t)Asm->ItemCount;
    Asm->Statements[Index].ItemCount = 1;
    return BoxAsmAddStringItem(Asm, &Word->Where,
                               Asm->Expansion->Binary.Data + Read->Offset + (uint64_t)Skip,
                               (size_t)Count);
}

static int BoxAsmParseInstruction(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                                  const BoxAsmToken *Word, uint32_t Times, uint8_t Prefix)
{
    BoxAsmOperand Operands[4] = { { 0 } };
    uint8_t Count = 0;
    uint8_t Condition;
    const BoxAsmInstruction *Instruction =
        BoxAsmFindInstruction(Word->Text, Word->Length, &Condition);
    uint32_t Index;
    int Rc;

    if (!Instruction) {
        BoxAsmMessage Message;
        BoxAsmFailBegin(Asm, &Word->Where, &Message);
        BoxAsmMessageText(&Message, "'");
        BoxAsmMessageSpan(&Message, Word->Text, Word->Length);
        BoxAsmMessageText(&Message, "' is not an instruction, a directive or a box operation");
        return BoxAsmFailEnd(Asm, &Word->Where, &Message);
    }
    if (Asm->CurrentSection == BOXASM_SECTION_BSS)
        return BoxAsmFail(Asm, &Word->Where, ".bss holds no code", NULL);
    while (!BoxAsmEndOfStatement(Token)) {
        if (Count == 4)
            return BoxAsmFail(Asm, &Token->Where, "an instruction has at most four operands", NULL);
        if ((Rc = BoxAsmParseOperand(Asm, Lexer, Token, &Operands[Count])) != 0)
            return Rc;
        Count++;
        if (BoxAsmEndOfStatement(Token))
            break;
        if (!BoxAsmTokenIsPunct(Token, ','))
            return BoxAsmFail(Asm, &Token->Where, "expected ',' between operands", NULL);
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
    }
    if ((Rc = BoxAsmAddStatement(Asm, BOXASM_STMT_INSTRUCTION, &Word->Where, &Index)) != 0)
        return Rc;
    Asm->Statements[Index].Instruction = Instruction;
    Asm->Statements[Index].Condition = Condition;
    Asm->Statements[Index].OperandCount = Count;
    Asm->Statements[Index].Times = Times;
    Asm->Statements[Index].Prefix = Prefix;
    memcpy(Asm->Statements[Index].Operands, Operands, sizeof(Operands));
    return 0;
}

/* lock, rep, repe or repne, and the instruction it goes before on its line. */
static int BoxAsmParsePrefixed(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                               const BoxAsmToken *Word, uint32_t Times)
{
    BoxAsmToken Instruction = *Token;
    BoxAsmMessage Message;
    uint8_t Condition;
    int Rc;

    if (Token->Kind == BOXASM_TOKEN_IDENTIFIER &&
        BoxAsmFindPrefix(Token->Text, Token->Length) != BOXASM_PREFIX_NONE)
        return BoxAsmFail(Asm, &Token->Where, "an instruction takes one prefix", NULL);
    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER ||
        !BoxAsmFindInstruction(Token->Text, Token->Length, &Condition)) {
        BoxAsmFailBegin(Asm, &Word->Where, &Message);
        BoxAsmMessageText(&Message, "'");
        BoxAsmMessageSpan(&Message, Word->Text, Word->Length);
        BoxAsmMessageText(&Message, "' is a prefix: the instruction it goes before follows "
                                    "it on its line");
        return BoxAsmFailEnd(Asm, &Word->Where, &Message);
    }
    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0 ||
        (Rc = BoxAsmParseInstruction(Asm, Lexer, Token, &Instruction, Times,
                                     BoxAsmFindPrefix(Word->Text, Word->Length))) != 0)
        return Rc;
    /* The statement begins at its prefix, and what is said of it points there. */
    Asm->Statements[Asm->StatementCount - 1u].Where = Word->Where;
    return 0;
}

static int BoxAsmParseStatement(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                                const BoxAsmToken *Word, uint32_t Times);

static int BoxAsmParseTimes(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                            const BoxAsmToken *Word)
{
    BoxAsmToken Repeated;
    uint32_t Count;
    uint8_t Condition;
    uint8_t Reserve = 0;
    int Rc;

    if ((Rc = BoxAsmParseExpression(Asm, Lexer, Token, &Count)) != 0)
        return Rc;
    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER ||
        !((BoxAsmDataWidth(Token, &Reserve) != 0 && !Reserve) || BoxAsmTokenIs(Token, "incbin") ||
          BoxAsmFindInstruction(Token->Text, Token->Length, &Condition) != NULL ||
          BoxAsmFindPrefix(Token->Text, Token->Length) != BOXASM_PREFIX_NONE))
        return BoxAsmFail(Asm, &Word->Where, "times repeats one instruction or db, dw, dd, dq, incbin",
                          NULL);
    Repeated = *Token;
    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
        return Rc;
    return BoxAsmParseStatement(Asm, Lexer, Token, &Repeated, Count);
}

static int BoxAsmParseSetting(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                              const BoxAsmToken *Word)
{
    if (BoxAsmTokenIs(Word, "default")) {
        if (!BoxAsmTokenIs(Token, "rel") && !BoxAsmTokenIs(Token, "abs"))
            return BoxAsmFail(Asm, &Token->Where, "default is rel or abs", NULL);
        Asm->DefaultAbsolute = BoxAsmTokenIs(Token, "abs");
        return BoxAsmNextToken(Asm, Lexer, Token);
    }
    if (Token->Kind != BOXASM_TOKEN_NUMBER || Token->Number != 64)
        return BoxAsmFail(Asm, &Token->Where, "BoxASM assembles 64-bit code: bits 64", NULL);
    return BoxAsmNextToken(Asm, Lexer, Token);
}

/* at FIELD[, what it holds]: zeros up to the field, then its statement. */
static int BoxAsmParseAt(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                         const BoxAsmToken *Word)
{
    BoxAsmToken Held;
    uint32_t Field;
    int Rc;

    if ((Rc = BoxAsmParseExpression(Asm, Lexer, Token, &Field)) != 0 ||
        (Rc = BoxAsmInstancePad(Asm, &Word->Where, Field)) != 0)
        return Rc;
    if (BoxAsmEndOfStatement(Token))
        return 0;
    if (!BoxAsmTokenIsPunct(Token, ','))
        return BoxAsmFail(Asm, &Token->Where,
                          "expected ',' and what the field holds: at point.x, dd 1", NULL);
    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
        return Rc;
    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER)
        return BoxAsmFail(Asm, &Token->Where, "expected what the field holds: at point.x, dd 1", NULL);
    Held = *Token;
    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
        return Rc;
    return BoxAsmParseStatement(Asm, Lexer, Token, &Held, 0);
}

/* A statement between struc and endstruc: a reservation or an align. */
static int BoxAsmParseField(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                            const BoxAsmToken *Word, uint32_t Times)
{
    uint8_t Reserve;
    uint8_t Width = BoxAsmDataWidth(Word, &Reserve);

    if (Times == 0 && BoxAsmTokenIs(Word, "endstruc"))
        return BoxAsmStrucClose(Asm, Word);
    if (Times == 0 && Width != 0 && Reserve)
        return BoxAsmStrucReserve(Asm, Lexer, Token, Word, Width);
    if (Times == 0 && (BoxAsmTokenIs(Word, "align") || BoxAsmTokenIs(Word, "alignb")))
        return BoxAsmStrucReserve(Asm, Lexer, Token, Word, 0);
    return BoxAsmFail(Asm, &Word->Where,
                      "a struc holds fields: labels, resb, resw, resd, resq and align", NULL);
}

static int BoxAsmParseStatement(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                                const BoxAsmToken *Word, uint32_t Times)
{
    uint8_t Reserve;
    uint32_t Index;
    int Rc;

    if (BoxAsmTokenIs(Word, "takes") || BoxAsmTokenIs(Word, "gives"))
        return BoxAsmParseForm(Asm, Lexer, Token, Word);
    if (Asm->Struc.Open)
        return BoxAsmParseField(Asm, Lexer, Token, Word, Times);
    if (Times == 0 && BoxAsmTokenIs(Word, "struc"))
        return BoxAsmStrucOpen(Asm, Lexer, Token, Word);
    if (Times == 0 && BoxAsmTokenIs(Word, "endstruc"))
        return BoxAsmStrucClose(Asm, Word);
    if (Times == 0 && BoxAsmTokenIs(Word, "istruc"))
        return BoxAsmInstanceOpen(Asm, Lexer, Token, Word);
    if (Times == 0 && BoxAsmTokenIs(Word, "iend"))
        return BoxAsmInstanceClose(Asm, Word);
    if (Times == 0 && Asm->Struc.Instance && BoxAsmTokenIs(Word, "at"))
        return BoxAsmParseAt(Asm, Lexer, Token, Word);
    if (BoxAsmDataWidth(Word, &Reserve) != 0)
        return BoxAsmParseStorage(Asm, Lexer, Token, Word, Times);
    if (BoxAsmTokenIs(Word, "incbin"))
        return BoxAsmParseIncbin(Asm, Lexer, Token, Word, Times);
    if (Times == 0 && (BoxAsmTokenIs(Word, "section") || BoxAsmTokenIs(Word, "segment")))
        return BoxAsmParseSection(Asm, Lexer, Token);
    if (Times == 0 && (BoxAsmTokenIs(Word, "global") || BoxAsmTokenIs(Word, "extern")))
        return BoxAsmParseNames(Asm, Lexer, Token, BoxAsmTokenIs(Word, "extern"));
    if (Times == 0 && (BoxAsmTokenIs(Word, "default") || BoxAsmTokenIs(Word, "bits")))
        return BoxAsmParseSetting(Asm, Lexer, Token, Word);
    if (Times == 0 && BoxAsmTokenIs(Word, "times"))
        return BoxAsmParseTimes(Asm, Lexer, Token, Word);
    if (Times == 0 && BoxAsmTokenIs(Word, "align")) {
        if ((Rc = BoxAsmAddStatement(Asm, BOXASM_STMT_ALIGN, &Word->Where, &Index)) != 0)
            return Rc;
        return BoxAsmParseExpression(Asm, Lexer, Token, &Asm->Statements[Index].Count);
    }
    if (BoxAsmTokenIs(Word, "equ"))
        return BoxAsmFail(Asm, &Word->Where, "equ needs a name before it: `size equ 16`", NULL);
    if (Times == 0 && BoxAsmIsBox(Word))
        return BoxAsmParseBox(Asm, Lexer, Token, Word);
    if (BoxAsmFindPrefix(Word->Text, Word->Length) != BOXASM_PREFIX_NONE)
        return BoxAsmParsePrefixed(Asm, Lexer, Token, Word, Times);
    return BoxAsmParseInstruction(Asm, Lexer, Token, Word, Times, BOXASM_PREFIX_NONE);
}

/* ---- Lines -------------------------------------------------------------- */

static int BoxAsmParseEqu(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                          const BoxAsmToken *Name)
{
    BoxAsmSymbol *Symbol;
    uint32_t Index, Statement;
    char *Text;
    int Rc;

    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0 ||
        (Rc = BoxAsmLabelName(Asm, Name, &Text)) != 0)
        return Rc;
    Rc = BoxAsmSymbolClaim(Asm, Text, strlen(Text), &Name->Where, &Index);
    free(Text);
    if (Rc != 0)
        return Rc;
    Symbol = &Asm->Symbols[Index];
    if (Symbol->Kind != BOXASM_SYMBOL_UNDEFINED)
        return BoxAsmFail(Asm, &Name->Where, "is already defined", Symbol->Name);
    if ((Rc = BoxAsmParseExpression(Asm, Lexer, Token, &Asm->Symbols[Index].Expr)) != 0)
        return Rc;
    Symbol = &Asm->Symbols[Index];
    Symbol->Kind = BOXASM_SYMBOL_CONSTANT;
    Symbol->Section = Asm->CurrentSection;
    Symbol->Defined = Name->Where;
    if ((Rc = BoxAsmAddStatement(Asm, BOXASM_STMT_LABEL, &Name->Where, &Statement)) != 0)
        return Rc;
    Asm->Statements[Statement].Symbol = Index;
    return 0;
}

static int BoxAsmParseLabel(BoxAsmAssembler *Asm, const BoxAsmToken *Name)
{
    uint32_t Index;
    char *Text;
    int Rc = BoxAsmLabelName(Asm, Name, &Text);

    if (Rc != 0)
        return Rc;
    if (Asm->Struc.Open) {                     /* a field: the offset it begins at */
        Rc = BoxAsmStrucField(Asm, Text, &Name->Where);
        if (Rc == 0 && Name->Text[0] != '.')
            Rc = BoxAsmSymbolClaim(Asm, Text, strlen(Text), &Name->Where, &Asm->Scope);
        free(Text);
        return Rc;
    }
    Rc = BoxAsmDefineLabel(Asm, Text, &Name->Where, &Index);
    free(Text);
    if (Rc == 0 && Name->Text[0] != '.')
        Asm->Scope = Index;
    return Rc;
}

/* Whether a word before this one is a label written without its colon. */
static int BoxAsmTakesLabel(const BoxAsmToken *Token)
{
    uint8_t Reserve;

    return Token->Kind == BOXASM_TOKEN_IDENTIFIER &&
           (BoxAsmDataWidth(Token, &Reserve) != 0 || BoxAsmTokenIs(Token, "times") ||
            BoxAsmTokenIs(Token, "equ") || BoxAsmTokenIs(Token, "incbin"));
}

/* [label:] [statement] — a label may also stand without its colon before
 * db, dw, dd, dq, resb..resq, times, incbin and equ, as `msg db "Hi", 0`. */
static int BoxAsmParseLine(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token)
{
    BoxAsmToken Word = *Token;
    int Colon;
    int Rc;

    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER)
        return BoxAsmFail(Asm, &Token->Where,
                          "a line starts with a label, an instruction or a directive", NULL);
    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
        return Rc;
    Colon = BoxAsmTokenIsPunct(Token, ':');
    if (!Colon && (BoxAsmIsKnownWord(&Word) || !BoxAsmTakesLabel(Token)))
        return BoxAsmParseStatement(Asm, Lexer, Token, &Word, 0);
    if (Colon && (Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
        return Rc;
    if (BoxAsmTokenIs(Token, "equ"))
        return BoxAsmParseEqu(Asm, Lexer, Token, &Word);
    if ((Rc = BoxAsmParseLabel(Asm, &Word)) != 0)
        return Rc;
    if (BoxAsmEndOfStatement(Token))
        return 0;
    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER)
        return BoxAsmFail(Asm, &Token->Where, "expected an instruction or a directive", NULL);
    Word = *Token;
    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
        return Rc;
    return BoxAsmParseStatement(Asm, Lexer, Token, &Word, 0);
}

/* What the whole source must hold once it is read: every name it uses
 * defined or declared extern, only labels global, and main to start at. */
static int BoxAsmCheckProgram(BoxAsmAssembler *Asm, const BoxAsmLocation *Start)
{
    uint32_t Main = 0;
    size_t Index;

    for (Index = BOXASM_SECTION_COUNT + 1u; Index < Asm->SymbolCount; Index++) {
        BoxAsmSymbol *Symbol = &Asm->Symbols[Index];

        if (Symbol->Kind == BOXASM_SYMBOL_UNDEFINED && Symbol->Global)
            return BoxAsmFail(Asm, &Symbol->Used, "is declared global but never defined",
                              Symbol->Name);
        if (Symbol->Kind == BOXASM_SYMBOL_UNDEFINED)
            return BoxAsmFail(Asm, &Symbol->Used,
                              "is never defined; a name from outside the program is declared "
                              "with extern", Symbol->Name);
        if (Symbol->Kind == BOXASM_SYMBOL_CONSTANT && Symbol->Global)
            return BoxAsmFail(Asm, &Symbol->Defined, "is a constant; only a label can be global",
                              Symbol->Name);
    }
    if (!BoxAsmNameIndexFind(&Asm->SymbolIndex, "main", &Main) ||
        Asm->Symbols[Main].Kind != BOXASM_SYMBOL_LABEL ||
        Asm->Symbols[Main].Section != BOXASM_SECTION_TEXT || !Asm->Symbols[Main].Global)
        return BoxAsmFail(Asm, Start,
                          "a program starts at main: write `global main` and put `main:` in "
                          "section .text", NULL);
    return 0;
}

int BoxAsmParse(BoxAsmAssembler *Asm, const char *Source, size_t Size)
{
    BoxAsmLexer Lexer;
    BoxAsmToken Token;
    BoxAsmLocation Start = { 1, 1, Source };
    int Rc;

    BoxAsmLexBegin(&Lexer, Source, Size);
    if ((Rc = BoxAsmNextToken(Asm, &Lexer, &Token)) != 0)
        return Rc;
    while (Token.Kind != BOXASM_TOKEN_END) {
        if (Token.Kind == BOXASM_TOKEN_NEWLINE) {
            if ((Rc = BoxAsmNextToken(Asm, &Lexer, &Token)) != 0)
                return Rc;
            continue;
        }
        if ((Rc = BoxAsmParseLine(Asm, &Lexer, &Token)) != 0)
            return Rc;
        if (!BoxAsmEndOfStatement(&Token)) {
            BoxAsmMessage Message;
            BoxAsmFailBegin(Asm, &Token.Where, &Message);
            BoxAsmMessageText(&Message, "unexpected '");
            BoxAsmMessageSpan(&Message, Token.Text, Token.Length);
            BoxAsmMessageText(&Message, "' after the statement");
            return BoxAsmFailEnd(Asm, &Token.Where, &Message);
        }
    }
    if ((Rc = BoxAsmStrucCheckClosed(Asm)) != 0)
        return Rc;
    return BoxAsmCheckProgram(Asm, &Start);
}
