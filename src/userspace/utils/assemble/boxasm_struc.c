/* struc ... endstruc and istruc ... iend, as NASM writes them.
 *
 * A struc is a layout and nothing else: its fields are named offsets from 0,
 * made by reserving space (resb, resw, resd, resq, align), and NAME_size is
 * where the last ends. They are constants, so `[rbx + point.y]` and
 * `mov eax, point_size` read them like any equ; nothing is written. A
 * struc's sizes are taken where it is written.
 *
 * istruc NAME lays out one instance in data: `at point.y, dd 2` fills with
 * zeros up to the field and writes it there, and iend fills to NAME_size. */

#include "boxasm_asm.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* Name made a constant of Value. */
static int BoxAsmStrucConstant(BoxAsmAssembler *Asm, const char *Name, const BoxAsmLocation *Where,
                               uint64_t Value, uint32_t *Out)
{
    BoxAsmExpr Node;
    uint32_t Expr;
    uint32_t Index;
    int Rc;

    if ((Rc = BoxAsmSymbolClaim(Asm, Name, strlen(Name), Where, &Index)) != 0)
        return Rc;
    if (Asm->Symbols[Index].Kind != BOXASM_SYMBOL_UNDEFINED)
        return BoxAsmFail(Asm, Where, "is already defined", Asm->Symbols[Index].Name);
    memset(&Node, 0, sizeof(Node));
    Node.Kind = BOXASM_EXPR_NUMBER;
    Node.Number = Value;
    Node.Where = *Where;
    if ((Rc = BoxAsmMakeExpr(Asm, &Node, &Expr)) != 0)
        return Rc;
    Asm->Symbols[Index].Kind = BOXASM_SYMBOL_CONSTANT;
    Asm->Symbols[Index].Expr = Expr;
    Asm->Symbols[Index].Section = Asm->CurrentSection;
    Asm->Symbols[Index].Defined = *Where;
    if (Out)
        *Out = Index;
    return 0;
}

int BoxAsmStrucOpen(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                    const BoxAsmToken *Word)
{
    char *Name;
    uint32_t Index;
    int Rc;

    if (Asm->Struc.Open || Asm->Struc.Instance)
        return BoxAsmFail(Asm, &Word->Where, "a struc is not written inside a struc or istruc", NULL);
    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER || Token->Text[0] == '.')
        return BoxAsmFail(Asm, &Token->Where, "expected the struc's name: struc point", NULL);
    if (!(Name = BoxAsmCopyText(Token->Text, Token->Length)))
        return -ERR_NO_MEMORY;
    Rc = BoxAsmStrucConstant(Asm, Name, &Token->Where, 0, &Index);
    free(Name);
    if (Rc != 0)
        return Rc;
    Asm->Struc.Open = 1;
    Asm->Struc.Name = Index;
    Asm->Struc.Offset = 0;
    Asm->Struc.SavedScope = Asm->Scope;
    Asm->Struc.Where = Word->Where;
    Asm->Scope = Index;                        /* .x is NAME.x */
    return BoxAsmNextToken(Asm, Lexer, Token);
}

int BoxAsmStrucClose(BoxAsmAssembler *Asm, const BoxAsmToken *Word)
{
    const char *Name;
    char *Size;
    size_t Length;
    int Rc;

    if (!Asm->Struc.Open)
        return BoxAsmFail(Asm, &Word->Where, "endstruc has no struc before it", NULL);
    Name = Asm->Symbols[Asm->Struc.Name].Name;
    Length = strlen(Name);
    if (!(Size = (char *)malloc(Length + 6u)))
        return -ERR_NO_MEMORY;
    memcpy(Size, Name, Length);
    memcpy(Size + Length, "_size", 6u);
    Rc = BoxAsmStrucConstant(Asm, Size, &Word->Where, Asm->Struc.Offset, NULL);
    free(Size);
    Asm->Struc.Open = 0;
    Asm->Scope = Asm->Struc.SavedScope;
    return Rc;
}

/* A label inside a struc: a field, the offset where it begins. */
int BoxAsmStrucField(BoxAsmAssembler *Asm, const char *Name, const BoxAsmLocation *Where)
{
    return BoxAsmStrucConstant(Asm, Name, Where, Asm->Struc.Offset, NULL);
}

/* resb, resw, resd, resq N inside a struc, or align N: the next field moves. */
int BoxAsmStrucReserve(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                       const BoxAsmToken *Word, uint8_t Width)
{
    BoxAsmLocation Where = Token->Where;
    uint32_t Expr;
    int64_t Count;
    uint64_t Bytes;
    int Rc;

    if ((Rc = BoxAsmParseExpression(Asm, Lexer, Token, &Expr)) != 0 ||
        (Rc = BoxAsmEvaluateConstant(Asm, Expr, Asm->Struc.Offset, Asm->CurrentSection, &Count)) != 0)
        return Rc;
    if (Width == 0) {                          /* align */
        if (Count <= 0 || Count > 4096 || (Count & (Count - 1)) != 0)
            return BoxAsmFail(Asm, &Where, "aligns to a power of two from 1 to 4096", NULL);
        Asm->Struc.Offset = BoxAsmAlignUp(Asm->Struc.Offset, (uint64_t)Count);
        return 0;
    }
    if (Count < 0 || (uint64_t)Count > BOXASM_MAX_SECTION_BYTES / Width)
        return BoxAsmFail(Asm, &Where, "reserves 0 to 2^32 items", NULL);
    Bytes = (uint64_t)Count * Width;
    if (Bytes > BOXASM_MAX_SECTION_BYTES - Asm->Struc.Offset)
        return BoxAsmFail(Asm, &Word->Where, "the struc would be larger than 4 GiB", NULL);
    Asm->Struc.Offset += Bytes;
    return 0;
}

int BoxAsmInstanceOpen(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                       const BoxAsmToken *Word)
{
    char *Size;
    char Start[48];
    uint32_t Index;
    int Rc;

    if (Asm->Struc.Open || Asm->Struc.Instance)
        return BoxAsmFail(Asm, &Word->Where, "an istruc is not written inside a struc or istruc", NULL);
    if (Asm->CurrentSection == BOXASM_SECTION_BSS)
        return BoxAsmFail(Asm, &Word->Where, ".bss holds no bytes for an istruc to lay out", NULL);
    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER)
        return BoxAsmFail(Asm, &Token->Where, "expected the struc's name: istruc point", NULL);
    if (!(Size = (char *)malloc(Token->Length + 6u)))
        return -ERR_NO_MEMORY;
    memcpy(Size, Token->Text, Token->Length);
    memcpy(Size + Token->Length, "_size", 6u);
    Rc = BoxAsmSymbolClaim(Asm, Size, strlen(Size), &Token->Where, &Index);
    free(Size);
    if (Rc != 0)
        return Rc;
    if (Asm->Symbols[Index].Kind != BOXASM_SYMBOL_CONSTANT)
        return BoxAsmFail(Asm, &Token->Where, "names no struc written before it", NULL);
    Asm->Struc.Size = Index;
    /* Where the instance begins: a label of its own, as NASM's is. */
    memcpy(Start, "..@istruc.", 10);
    {
        uint32_t Number = (uint32_t)Asm->StatementCount;
        char Digits[12];
        size_t Count = 0;
        size_t At = 10;

        do {
            Digits[Count++] = (char)('0' + Number % 10u);
            Number /= 10u;
        } while (Number != 0);
        while (Count != 0)
            Start[At++] = Digits[--Count];
        Start[At] = '\0';
    }
    if ((Rc = BoxAsmDefineLabel(Asm, Start, &Word->Where, &Asm->Struc.Start)) != 0)
        return Rc;
    Asm->Struc.Instance = 1;
    Asm->Struc.InstanceWhere = Word->Where;
    return BoxAsmNextToken(Asm, Lexer, Token);
}

/* Zeros from where the instance stands now up to Field bytes into it. */
int BoxAsmInstancePad(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, uint32_t Field)
{
    uint32_t Index;
    int Rc;

    if (!Asm->Struc.Instance)
        return BoxAsmFail(Asm, Where, "at and iend stand inside an istruc", NULL);
    if ((Rc = BoxAsmAddStatement(Asm, BOXASM_STMT_PAD, Where, &Index)) != 0)
        return Rc;
    Asm->Statements[Index].Symbol = Asm->Struc.Start;
    Asm->Statements[Index].Count = Field;
    return 0;
}

int BoxAsmInstanceClose(BoxAsmAssembler *Asm, const BoxAsmToken *Word)
{
    BoxAsmExpr Node;
    uint32_t Field;
    int Rc;

    if (!Asm->Struc.Instance)
        return BoxAsmFail(Asm, &Word->Where, "iend has no istruc before it", NULL);
    memset(&Node, 0, sizeof(Node));
    Node.Kind = BOXASM_EXPR_SYMBOL;
    Node.Symbol = Asm->Struc.Size;
    Node.Where = Word->Where;
    if ((Rc = BoxAsmMakeExpr(Asm, &Node, &Field)) != 0 ||
        (Rc = BoxAsmInstancePad(Asm, &Word->Where, Field)) != 0)
        return Rc;
    Asm->Struc.Instance = 0;
    return 0;
}

int BoxAsmStrucCheckClosed(BoxAsmAssembler *Asm)
{
    if (Asm->Struc.Open)
        return BoxAsmFail(Asm, &Asm->Struc.Where, "this struc is not closed by endstruc", NULL);
    if (Asm->Struc.Instance)
        return BoxAsmFail(Asm, &Asm->Struc.InstanceWhere, "this istruc is not closed by iend", NULL);
    return 0;
}
