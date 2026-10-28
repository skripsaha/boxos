#include "boxasm_isa.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* box.* operations are BoxOS calls written as one line. Each is expanded
 * here into ordinary instructions that call BoxLib, so it is assembled,
 * relaxed and relocated like the code around it. */

static const char *const BoxAsmSaved[] = {
    "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"
};
#define BOXASM_SAVED_COUNT (sizeof(BoxAsmSaved) / sizeof(BoxAsmSaved[0]))

/* print and exit come from BoxLib; a program that defines its own name for
 * either cannot also use the box operation that calls it. */
static int BoxAsmRuntimeName(BoxAsmAssembler *Asm, const char *Name,
                             const BoxAsmLocation *Where, uint32_t *Out)
{
    BoxAsmSymbol *Symbol;
    int Rc = BoxAsmSymbolClaim(Asm, Name, strlen(Name), Where, Out);

    if (Rc != 0)
        return Rc;
    Symbol = &Asm->Symbols[*Out];
    if (Symbol->Kind == BOXASM_SYMBOL_UNDEFINED)
        Symbol->Kind = BOXASM_SYMBOL_EXTERN;
    if (Symbol->Kind != BOXASM_SYMBOL_EXTERN)
        return BoxAsmFail(Asm, Where, "is BoxLib's, and this program defines its own", Name);
    return 0;
}

static int BoxAsmSymbolExpr(BoxAsmAssembler *Asm, uint32_t Symbol, const BoxAsmLocation *Where,
                            uint32_t *Out)
{
    BoxAsmExpr Node;

    memset(&Node, 0, sizeof(Node));
    Node.Kind = BOXASM_EXPR_SYMBOL;
    Node.Symbol = Symbol;
    Node.Where = *Where;
    return BoxAsmMakeExpr(Asm, &Node, Out);
}

static int BoxAsmNumberOperand(BoxAsmAssembler *Asm, int64_t Value, const BoxAsmLocation *Where,
                               BoxAsmOperand *Out)
{
    BoxAsmExpr Node;

    memset(Out, 0, sizeof(*Out));
    memset(&Node, 0, sizeof(Node));
    Node.Kind = BOXASM_EXPR_NUMBER;
    Node.Number = (uint64_t)Value;
    Node.Where = *Where;
    Out->Kind = BOXASM_OPERAND_IMMEDIATE;
    Out->Where = *Where;
    return BoxAsmMakeExpr(Asm, &Node, &Out->Expr);
}

static int BoxAsmEmit1(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, const char *Name,
                       const char *Register)
{
    BoxAsmOperand Operand;

    if (!Register)
        return BoxAsmAddInstruction(Asm, Where, Name, NULL, 0);
    if (BoxAsmRegisterOperand(Register, &Operand) != 0)
        return -ERR_INVALID_ARGUMENT;
    return BoxAsmAddInstruction(Asm, Where, Name, &Operand, 1);
}

static int BoxAsmEmit2(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, const char *Name,
                       const BoxAsmOperand *Left, const BoxAsmOperand *Right)
{
    BoxAsmOperand Operands[2] = { *Left, *Right };

    return BoxAsmAddInstruction(Asm, Where, Name, Operands, 2);
}

/* `and rsp, -16`: the C call ABI wants rsp aligned to 16 at a call. */
static int BoxAsmAlignStack(BoxAsmAssembler *Asm, const BoxAsmLocation *Where)
{
    BoxAsmOperand Stack, Mask;
    int Rc;

    if ((Rc = BoxAsmRegisterOperand("rsp", &Stack)) != 0 ||
        (Rc = BoxAsmNumberOperand(Asm, -16, Where, &Mask)) != 0)
        return Rc;
    return BoxAsmEmit2(Asm, Where, "and", &Stack, &Mask);
}

static int BoxAsmCall(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, uint32_t Function)
{
    BoxAsmOperand Target;
    int Rc;

    memset(&Target, 0, sizeof(Target));
    Target.Kind = BOXASM_OPERAND_IMMEDIATE;
    Target.Where = *Where;
    if ((Rc = BoxAsmSymbolExpr(Asm, Function, Where, &Target.Expr)) != 0)
        return Rc;
    return BoxAsmAddInstruction(Asm, Where, "call", &Target, 1);
}

/* The text of `box.say "..."`, NUL-ended in .rodata under a name no source
 * can spell. */
static int BoxAsmSayText(BoxAsmAssembler *Asm, const BoxAsmToken *Text, uint32_t *Out)
{
    BoxAsmBuffer Bytes = { 0 };
    BoxAsmMessage Message;
    uint8_t Section = Asm->CurrentSection;
    uint32_t Label;
    char Name[32];
    int Rc;

    BoxAsmMessageBegin(&Message, Name, sizeof(Name));
    BoxAsmMessageText(&Message, "box.say@");
    BoxAsmMessageDecimal(&Message, ++Asm->SayCount);
    Rc = BoxAsmLexStringBytes(Text, &Bytes);
    if (Rc == 0)
        Rc = BoxAsmBufferAppend(&Bytes, "", 1u);
    Asm->CurrentSection = BOXASM_SECTION_RODATA;
    if (Rc == 0)
        Rc = BoxAsmDefineLabel(Asm, Name, &Text->Where, &Label);
    if (Rc == 0)
        Rc = BoxAsmAddBytes(Asm, &Text->Where, Bytes.Data, Bytes.Size);
    Asm->CurrentSection = Section;
    BoxAsmBufferRelease(&Bytes);
    if (Rc == 0)
        Rc = BoxAsmSymbolExpr(Asm, Label, &Text->Where, Out);
    return Rc;
}

/* box.say "text" | box.say label — print through BoxLib's print, the screen
 * Current. Every general register and RFLAGS are as they were after it. */
static int BoxAsmSay(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                     const BoxAsmLocation *Where)
{
    BoxAsmOperand Address, Frame, Stack, Destination;
    uint32_t Print;
    size_t Index;
    int Rc;

    memset(&Address, 0, sizeof(Address));
    Address.Kind = BOXASM_OPERAND_MEMORY;
    Address.Where = Token->Where;
    if (Token->Kind == BOXASM_TOKEN_STRING) {
        if ((Rc = BoxAsmSayText(Asm, Token, &Address.Expr)) != 0 ||
            (Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
    } else if ((Rc = BoxAsmParseExpression(Asm, Lexer, Token, &Address.Expr)) != 0) {
        return Rc;
    }
    if ((Rc = BoxAsmRuntimeName(Asm, "print", Where, &Print)) != 0 ||
        (Rc = BoxAsmRegisterOperand("rbp", &Frame)) != 0 ||
        (Rc = BoxAsmRegisterOperand("rsp", &Stack)) != 0 ||
        (Rc = BoxAsmRegisterOperand("rdi", &Destination)) != 0 ||
        (Rc = BoxAsmEmit1(Asm, Where, "pushfq", NULL)) != 0)
        return Rc;
    for (Index = 0; Index < BOXASM_SAVED_COUNT; Index++)
        if ((Rc = BoxAsmEmit1(Asm, Where, "push", BoxAsmSaved[Index])) != 0)
            return Rc;
    if ((Rc = BoxAsmEmit1(Asm, Where, "push", "rbp")) != 0 ||
        (Rc = BoxAsmEmit2(Asm, Where, "mov", &Frame, &Stack)) != 0 ||
        (Rc = BoxAsmAlignStack(Asm, Where)) != 0 ||
        (Rc = BoxAsmEmit1(Asm, Where, "cld", NULL)) != 0 ||
        (Rc = BoxAsmEmit2(Asm, Where, "lea", &Destination, &Address)) != 0 ||
        (Rc = BoxAsmCall(Asm, Where, Print)) != 0 ||
        (Rc = BoxAsmEmit2(Asm, Where, "mov", &Stack, &Frame)) != 0 ||
        (Rc = BoxAsmEmit1(Asm, Where, "pop", "rbp")) != 0)
        return Rc;
    for (Index = BOXASM_SAVED_COUNT; Index > 0; Index--)
        if ((Rc = BoxAsmEmit1(Asm, Where, "pop", BoxAsmSaved[Index - 1u])) != 0)
            return Rc;
    return BoxAsmEmit1(Asm, Where, "popfq", NULL);
}

/* box.exit code | box.exit register — end the program through BoxLib's
 * exit with the code's low 32 bits. The code is taken before the stack is
 * aligned, so rsp itself may hold it. */
static int BoxAsmExit(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                      const BoxAsmLocation *Where)
{
    BoxAsmOperand Code, Destination;
    uint32_t Exit;
    int Rc;

    if ((Rc = BoxAsmParseOperand(Asm, Lexer, Token, &Code)) != 0)
        return Rc;
    if (Code.Kind == BOXASM_OPERAND_MEMORY)
        return BoxAsmFail(Asm, &Code.Where, "box.exit takes a number or a register", NULL);
    if (Code.Kind == BOXASM_OPERAND_REGISTER) {
        if (Code.Register.Class != BOXASM_REGISTER_GENERAL || Code.Register.Size < 4)
            return BoxAsmFail(Asm, &Code.Where, "box.exit takes a 32- or 64-bit register", NULL);
        Code.Register.Size = 4;
        Code.Size = 4;
    }
    if ((Rc = BoxAsmRuntimeName(Asm, "exit", Where, &Exit)) != 0 ||
        (Rc = BoxAsmRegisterOperand("edi", &Destination)) != 0 ||
        (Rc = BoxAsmEmit2(Asm, Where, "mov", &Destination, &Code)) != 0 ||
        (Rc = BoxAsmAlignStack(Asm, Where)) != 0)
        return Rc;
    return BoxAsmCall(Asm, Where, Exit);
}

int BoxAsmParseBox(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                   const BoxAsmToken *Word)
{
    if (Asm->CurrentSection != BOXASM_SECTION_TEXT)
        return BoxAsmFail(Asm, &Word->Where, "box operations belong in .text", NULL);
    if (Word->Length == 7 && memcmp(Word->Text, "box.say", 7u) == 0)
        return BoxAsmSay(Asm, Lexer, Token, &Word->Where);
    if (Word->Length == 8 && memcmp(Word->Text, "box.exit", 8u) == 0)
        return BoxAsmExit(Asm, Lexer, Token, &Word->Where);
    {
        BoxAsmMessage Message;
        BoxAsmFailBegin(Asm, &Word->Where, &Message);
        BoxAsmMessageText(&Message, "'");
        BoxAsmMessageSpan(&Message, Word->Text, Word->Length);
        BoxAsmMessageText(&Message, "' is not a box operation; BoxASM knows box.say and box.exit");
        return BoxAsmFailEnd(Asm, &Word->Where, &Message);
    }
}
