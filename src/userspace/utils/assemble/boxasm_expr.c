#include "boxasm_asm.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

typedef enum {
    BOXASM_LEVEL_OR = 0,
    BOXASM_LEVEL_XOR,
    BOXASM_LEVEL_AND,
    BOXASM_LEVEL_SHIFT,
    BOXASM_LEVEL_SUM,
    BOXASM_LEVEL_PRODUCT
} BoxAsmLevel;

int BoxAsmNextToken(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token)
{
    const char *Why = NULL;

    if (BoxAsmLexNext(Lexer, Token, &Why) != 0)
        return BoxAsmFail(Asm, &Token->Where, Why, NULL);
    return 0;
}

int BoxAsmMakeExpr(BoxAsmAssembler *Asm, const BoxAsmExpr *Node, uint32_t *Out)
{
    if (Asm->ExprCount == 0)
        Asm->ExprCount = 1;     /* index 0 means "no expression" */
    if (Asm->ExprCount >= UINT32_MAX ||
        BoxAsmGrowArray((void **)&Asm->Exprs, &Asm->ExprCapacity, Asm->ExprCount,
                        sizeof(BoxAsmExpr)) != 0)
        return -ERR_NO_MEMORY;
    Asm->Exprs[Asm->ExprCount] = *Node;
    *Out = (uint32_t)Asm->ExprCount++;
    return 0;
}

int BoxAsmSymbolClaim(BoxAsmAssembler *Asm, const char *Name, size_t Length,
                      const BoxAsmLocation *Where, uint32_t *Out)
{
    char *Copy = BoxAsmCopyText(Name, Length);
    uint32_t *Position;
    uint8_t Fresh;

    if (!Copy || BoxAsmNameIndexClaim(&Asm->SymbolIndex, Copy, &Position, &Fresh) != 0) {
        free(Copy);
        return -ERR_NO_MEMORY;
    }
    if (!Fresh) {
        free(Copy);
        *Out = *Position;
        return 0;
    }
    if (Asm->SymbolCount >= UINT32_MAX ||
        BoxAsmGrowArray((void **)&Asm->Symbols, &Asm->SymbolCapacity, Asm->SymbolCount,
                        sizeof(BoxAsmSymbol)) != 0)
        return -ERR_NO_MEMORY;
    memset(&Asm->Symbols[Asm->SymbolCount], 0, sizeof(BoxAsmSymbol));
    Asm->Symbols[Asm->SymbolCount].Name = Copy;
    Asm->Symbols[Asm->SymbolCount].Used = *Where;
    *Position = (uint32_t)Asm->SymbolCount;
    *Out = (uint32_t)Asm->SymbolCount++;
    return 0;
}

int BoxAsmLabelName(BoxAsmAssembler *Asm, const BoxAsmToken *Token, char **Out)
{
    const char *Scope;
    size_t ScopeLength;

    /* A macro's own label, ..@N.name, is no local one: it belongs to its call. */
    if (Token->Text[0] != '.' || (Token->Length > 3 && memcmp(Token->Text, "..@", 3) == 0)) {
        *Out = BoxAsmCopyText(Token->Text, Token->Length);
        return *Out ? 0 : -ERR_NO_MEMORY;
    }
    if (Asm->Scope == 0)
        return BoxAsmFail(Asm, &Token->Where,
                          "a local label needs a plain label before it", NULL);
    Scope = Asm->Symbols[Asm->Scope].Name;
    ScopeLength = strlen(Scope);
    if (ScopeLength > SIZE_MAX - Token->Length - 1u)
        return -ERR_NO_MEMORY;
    *Out = (char *)malloc(ScopeLength + Token->Length + 1u);
    if (!*Out)
        return -ERR_NO_MEMORY;
    memcpy(*Out, Scope, ScopeLength);
    memcpy(*Out + ScopeLength, Token->Text, Token->Length);
    (*Out)[ScopeLength + Token->Length] = '\0';
    return 0;
}

/* ---- Parsing ------------------------------------------------------------ */

static int BoxAsmParseLevel(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                            BoxAsmLevel Level, uint32_t *Out);

static int BoxAsmStringNumber(BoxAsmAssembler *Asm, const BoxAsmToken *Token, uint64_t *Out)
{
    BoxAsmBuffer Bytes = { 0 };
    uint64_t Value = 0;
    uint64_t Index;
    int Rc = BoxAsmLexStringBytes(Token, &Bytes);

    if (Rc == 0 && Bytes.Size > 8u)
        Rc = BoxAsmFail(Asm, &Token->Where,
                        "a string used as a number holds at most 8 bytes", NULL);
    for (Index = 0; Rc == 0 && Index < Bytes.Size; Index++)
        Value |= (uint64_t)Bytes.Data[Index] << (8u * Index);
    BoxAsmBufferRelease(&Bytes);
    *Out = Value;
    return Rc;
}

static int BoxAsmParseSymbol(BoxAsmAssembler *Asm, const BoxAsmToken *Token, uint32_t *Out)
{
    BoxAsmExpr Node;
    char *Name;
    int Rc = BoxAsmLabelName(Asm, Token, &Name);

    if (Rc != 0)
        return Rc;
    memset(&Node, 0, sizeof(Node));
    Node.Kind = BOXASM_EXPR_SYMBOL;
    Node.Where = Token->Where;
    Rc = BoxAsmSymbolClaim(Asm, Name, strlen(Name), &Token->Where, &Node.Symbol);
    free(Name);
    return Rc != 0 ? Rc : BoxAsmMakeExpr(Asm, &Node, Out);
}

static int BoxAsmParsePrimary(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                              uint32_t *Out)
{
    BoxAsmExpr Node;
    int Rc;

    memset(&Node, 0, sizeof(Node));
    Node.Where = Token->Where;
    if (BoxAsmTooDeep(Asm, Asm->StackBase))
        return BoxAsmFail(Asm, &Token->Where, "the expression nests too deep", NULL);
    if (BoxAsmTokenIsPunct(Token, '-') || BoxAsmTokenIsPunct(Token, '~') ||
        BoxAsmTokenIsPunct(Token, '+')) {
        uint16_t Operator = Token->Punct;
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0 ||
            (Rc = BoxAsmParsePrimary(Asm, Lexer, Token, &Node.Left)) != 0)
            return Rc;
        if (Operator == '+') {
            *Out = Node.Left;
            return 0;
        }
        Node.Kind = Operator == '-' ? BOXASM_EXPR_NEGATE : BOXASM_EXPR_COMPLEMENT;
        return BoxAsmMakeExpr(Asm, &Node, Out);
    }
    if (BoxAsmTokenIsPunct(Token, '(')) {
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0 ||
            (Rc = BoxAsmParseLevel(Asm, Lexer, Token, BOXASM_LEVEL_OR, Out)) != 0)
            return Rc;
        if (!BoxAsmTokenIsPunct(Token, ')'))
            return BoxAsmFail(Asm, &Token->Where, "expected ')'", NULL);
        return BoxAsmNextToken(Asm, Lexer, Token);
    }
    if (Token->Kind == BOXASM_TOKEN_NUMBER) {
        Node.Kind = BOXASM_EXPR_NUMBER;
        Node.Number = Token->Number;
    } else if (Token->Kind == BOXASM_TOKEN_STRING) {
        Node.Kind = BOXASM_EXPR_NUMBER;
        if ((Rc = BoxAsmStringNumber(Asm, Token, &Node.Number)) != 0)
            return Rc;
    } else if (BoxAsmTokenIsPunct(Token, '$')) {
        Node.Kind = BOXASM_EXPR_HERE;
    } else if (BoxAsmTokenIsPunct(Token, BOXASM_PUNCT_SECTION)) {
        Node.Kind = BOXASM_EXPR_SECTION_START;
    } else if (Token->Kind == BOXASM_TOKEN_IDENTIFIER) {
        if ((Rc = BoxAsmParseSymbol(Asm, Token, Out)) != 0)
            return Rc;
        return BoxAsmNextToken(Asm, Lexer, Token);
    } else if (Token->Kind == BOXASM_TOKEN_FLOAT) {
        return BoxAsmFail(Asm, &Token->Where,
                          "a floating-point number stands only in dd or dq data: "
                          "put it there and load it from memory", NULL);
    } else {
        return BoxAsmFail(Asm, &Token->Where, "expected a number, a name or '('", NULL);
    }
    if ((Rc = BoxAsmMakeExpr(Asm, &Node, Out)) != 0)
        return Rc;
    return BoxAsmNextToken(Asm, Lexer, Token);
}

static int BoxAsmOperatorAt(const BoxAsmToken *Token, BoxAsmLevel Level)
{
    if (Token->Kind != BOXASM_TOKEN_PUNCT)
        return 0;
    switch (Level) {
    case BOXASM_LEVEL_OR:      return Token->Punct == '|';
    case BOXASM_LEVEL_XOR:     return Token->Punct == '^';
    case BOXASM_LEVEL_AND:     return Token->Punct == '&';
    case BOXASM_LEVEL_SHIFT:   return Token->Punct == BOXASM_PUNCT_SHIFT_LEFT ||
                                      Token->Punct == BOXASM_PUNCT_SHIFT_RIGHT;
    case BOXASM_LEVEL_SUM:     return Token->Punct == '+' || Token->Punct == '-';
    case BOXASM_LEVEL_PRODUCT: return Token->Punct == '*' || Token->Punct == '/' ||
                                      Token->Punct == '%';
    default:                   return 0;
    }
}

static int BoxAsmParseLevel(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                            BoxAsmLevel Level, uint32_t *Out)
{
    int Rc = Level == BOXASM_LEVEL_PRODUCT
             ? BoxAsmParsePrimary(Asm, Lexer, Token, Out)
             : BoxAsmParseLevel(Asm, Lexer, Token, (BoxAsmLevel)(Level + 1), Out);

    while (Rc == 0 && BoxAsmOperatorAt(Token, Level)) {
        BoxAsmExpr Node;

        memset(&Node, 0, sizeof(Node));
        Node.Kind = BOXASM_EXPR_BINARY;
        Node.Operator = Token->Punct;
        Node.Where = Token->Where;
        Node.Left = *Out;
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
        Rc = Level == BOXASM_LEVEL_PRODUCT
             ? BoxAsmParsePrimary(Asm, Lexer, Token, &Node.Right)
             : BoxAsmParseLevel(Asm, Lexer, Token, (BoxAsmLevel)(Level + 1), &Node.Right);
        if (Rc == 0)
            Rc = BoxAsmMakeExpr(Asm, &Node, Out);
    }
    return Rc;
}

int BoxAsmParseExpression(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer,
                          BoxAsmToken *Token, uint32_t *Out)
{
    return BoxAsmParseLevel(Asm, Lexer, Token, BOXASM_LEVEL_OR, Out);
}

int BoxAsmParseTerm(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer,
                    BoxAsmToken *Token, uint32_t *Out)
{
    return BoxAsmParseLevel(Asm, Lexer, Token, BOXASM_LEVEL_PRODUCT, Out);
}

/* ---- Evaluation --------------------------------------------------------- */

/* Where a symbol-based value is anchored: its section and offset there, or —
 * for an outside name — the name itself. */
static int BoxAsmAnchor(const BoxAsmAssembler *Asm, const BoxAsmValue *Value,
                        uint32_t *Base, uint64_t *Offset)
{
    const BoxAsmSymbol *Symbol = &Asm->Symbols[Value->Symbol];

    if (Symbol->Kind == BOXASM_SYMBOL_LABEL) {
        *Base = BoxAsmSectionSymbol((BoxAsmSection)Symbol->Section);
        *Offset = Symbol->Offset;
        return 1;
    }
    *Base = Value->Symbol;
    *Offset = 0;
    return Symbol->Kind == BOXASM_SYMBOL_SECTION;
}

static int BoxAsmCombine(BoxAsmAssembler *Asm, const BoxAsmExpr *Node, const BoxAsmValue *Left,
                         const BoxAsmValue *Right, BoxAsmValue *Out)
{
    uint64_t A = (uint64_t)Left->Constant;
    uint64_t B = (uint64_t)Right->Constant;

    Out->Symbol = 0;
    if (Node->Operator == '+' && (Left->Symbol == 0 || Right->Symbol == 0)) {
        Out->Symbol = Left->Symbol ? Left->Symbol : Right->Symbol;
        Out->Constant = (int64_t)(A + B);
        return 0;
    }
    if (Node->Operator == '-' && Right->Symbol == 0) {
        Out->Symbol = Left->Symbol;
        Out->Constant = (int64_t)(A - B);
        return 0;
    }
    if (Node->Operator == '-' && Left->Symbol != 0) {
        uint32_t LeftBase, RightBase;
        uint64_t LeftOffset, RightOffset;

        BoxAsmAnchor(Asm, Left, &LeftBase, &LeftOffset);
        BoxAsmAnchor(Asm, Right, &RightBase, &RightOffset);
        if (LeftBase != RightBase)
            return BoxAsmFail(Asm, &Node->Where,
                              "two addresses from different places do not subtract to a number", NULL);
        Out->Constant = (int64_t)((LeftOffset + A) - (RightOffset + B));
        return 0;
    }
    if (Left->Symbol != 0 || Right->Symbol != 0)
        return BoxAsmFail(Asm, &Node->Where,
                          "an address takes only + and - of a number", NULL);
    switch (Node->Operator) {
    case '+': Out->Constant = (int64_t)(A + B); return 0;
    case '-': Out->Constant = (int64_t)(A - B); return 0;
    case '*': Out->Constant = (int64_t)(A * B); return 0;
    case '&': Out->Constant = (int64_t)(A & B); return 0;
    case '|': Out->Constant = (int64_t)(A | B); return 0;
    case '^': Out->Constant = (int64_t)(A ^ B); return 0;
    case '/':
    case '%':
        if (B == 0)
            return BoxAsmFail(Asm, &Node->Where, "division by zero", NULL);
        if (Right->Constant == -1)
            Out->Constant = Node->Operator == '/' ? (int64_t)(0 - A) : 0;
        else
            Out->Constant = Node->Operator == '/' ? Left->Constant / Right->Constant
                                                  : Left->Constant % Right->Constant;
        return 0;
    default:
        if (Right->Constant < 0 || Right->Constant > 63)
            return BoxAsmFail(Asm, &Node->Where, "a shift count is 0 to 63", NULL);
        Out->Constant = Node->Operator == BOXASM_PUNCT_SHIFT_LEFT
                        ? (int64_t)(A << B) : (Left->Constant >> B);
        return 0;
    }
}

static int BoxAsmEvaluateSymbol(BoxAsmAssembler *Asm, const BoxAsmExpr *Node, BoxAsmValue *Out)
{
    BoxAsmSymbol *Symbol = &Asm->Symbols[Node->Symbol];
    int Rc;

    switch (Symbol->Kind) {
    case BOXASM_SYMBOL_CONSTANT:
        if (Symbol->Evaluating)
            return BoxAsmFail(Asm, &Node->Where, "is defined in terms of itself", Symbol->Name);
        Symbol->Evaluating = 1;
        Rc = BoxAsmEvaluate(Asm, Symbol->Expr, Symbol->Offset, Symbol->Section, Out);
        Symbol->Evaluating = 0;
        return Rc;
    case BOXASM_SYMBOL_UNDEFINED:
        return BoxAsmFail(Asm, &Node->Where, "is not defined", Symbol->Name);
    default:
        Out->Symbol = Node->Symbol;
        Out->Constant = 0;
        return 0;
    }
}

int BoxAsmEvaluate(BoxAsmAssembler *Asm, uint32_t Expr, uint64_t Here,
                   uint8_t Section, BoxAsmValue *Out)
{
    const BoxAsmExpr *Node = &Asm->Exprs[Expr];
    BoxAsmValue Left, Right;
    int Rc;

    memset(Out, 0, sizeof(*Out));
    if (BoxAsmTooDeep(Asm, Asm->StackBase))
        return BoxAsmFail(Asm, &Node->Where,
                          "the expression is too deep to work out: too many operations, "
                          "and names defined by names, inside one another", NULL);
    switch (Node->Kind) {
    case BOXASM_EXPR_NUMBER:
        Out->Constant = (int64_t)Node->Number;
        return 0;
    case BOXASM_EXPR_SYMBOL:
        return BoxAsmEvaluateSymbol(Asm, Node, Out);
    case BOXASM_EXPR_HERE:
        Out->Symbol = BoxAsmSectionSymbol((BoxAsmSection)Section);
        Out->Constant = (int64_t)Here;
        return 0;
    case BOXASM_EXPR_SECTION_START:
        Out->Symbol = BoxAsmSectionSymbol((BoxAsmSection)Section);
        return 0;
    case BOXASM_EXPR_NEGATE:
    case BOXASM_EXPR_COMPLEMENT:
        if ((Rc = BoxAsmEvaluate(Asm, Node->Left, Here, Section, &Left)) != 0)
            return Rc;
        if (Left.Symbol != 0)
            return BoxAsmFail(Asm, &Node->Where, "an address cannot be negated", NULL);
        Out->Constant = Node->Kind == BOXASM_EXPR_NEGATE ? (int64_t)(0 - (uint64_t)Left.Constant)
                                                         : ~Left.Constant;
        return 0;
    default:
        if ((Rc = BoxAsmEvaluate(Asm, Node->Left, Here, Section, &Left)) != 0 ||
            (Rc = BoxAsmEvaluate(Asm, Node->Right, Here, Section, &Right)) != 0)
            return Rc;
        return BoxAsmCombine(Asm, Node, &Left, &Right, Out);
    }
}

int BoxAsmEvaluateConstant(BoxAsmAssembler *Asm, uint32_t Expr, uint64_t Here,
                           uint8_t Section, int64_t *Out)
{
    BoxAsmValue Value;
    int Rc = BoxAsmEvaluate(Asm, Expr, Here, Section, &Value);

    if (Rc != 0)
        return Rc;
    if (Value.Symbol != 0)
        return BoxAsmFail(Asm, &Asm->Exprs[Expr].Where, "a number is needed here, not an address",
                          NULL);
    *Out = Value.Constant;
    return 0;
}
