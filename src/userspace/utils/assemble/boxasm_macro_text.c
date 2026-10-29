/* The text of one line, as the preprocessor works on it: its tokens, the
 * parameters of the call it is in, its single-line macros, the value of an
 * expression in it, and the words of a directive it is. */

#include "boxasm_macro_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* ---- Tokens ------------------------------------------------------------- */

static int BoxAsmPpLetter(char Ch)
{
    return (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || Ch == '_';
}

static int BoxAsmPpDigit(char Ch)
{
    return Ch >= '0' && Ch <= '9';
}

static int BoxAsmPpPart(char Ch)
{
    return BoxAsmPpLetter(Ch) || BoxAsmPpDigit(Ch) || Ch == '.' || Ch == '@';
}

void BoxAsmPpNext(const char *Line, size_t *At, BoxAsmPpToken *Out)
{
    const char *Start = Line + *At;
    const char *End = Start;

    Out->Text = Start;
    if (*Start == '\0' || *Start == ';' || *Start == '\n' || *Start == '\r') {
        Out->Kind = BOXASM_PP_END;
        Out->Length = 0;
        return;
    }
    if (*Start == ' ' || *Start == '\t') {
        while (*End == ' ' || *End == '\t')
            End++;
        Out->Kind = BOXASM_PP_SPACE;
    } else if (BoxAsmPpLetter(*Start) || *Start == '.') {
        while (BoxAsmPpPart(*End))
            End++;
        Out->Kind = BOXASM_PP_WORD;
    } else if (BoxAsmPpDigit(*Start)) {
        int Point = 0;

        /* A number, a float's exponent sign among it: 1.5e-3. */
        while (BoxAsmPpPart(*End) ||
               ((*End == '+' || *End == '-') && Point && (End[-1] == 'e' || End[-1] == 'E'))) {
            Point |= *End == '.';
            End++;
        }
        Out->Kind = BOXASM_PP_NUMBER;
    } else if (*Start == '"' || *Start == '\'') {
        End++;
        while (*End != '\0' && *End != '\n' && *End != *Start) {
            if (*End == '\\' && End[1] != '\0' && End[1] != '\n')
                End++;
            End++;
        }
        if (*End == *Start)
            End++;
        Out->Kind = BOXASM_PP_STRING;
    } else {
        End++;
        Out->Kind = BOXASM_PP_OTHER;
    }
    Out->Length = (size_t)(End - Start);
    *At += Out->Length;
}

int BoxAsmPpWordIs(const BoxAsmPpToken *Token, const char *Word)
{
    size_t Index;

    if (Token->Kind != BOXASM_PP_WORD || strlen(Word) != Token->Length)
        return 0;
    for (Index = 0; Index < Token->Length; Index++) {
        char Ch = Token->Text[Index];

        if (Ch >= 'A' && Ch <= 'Z')
            Ch = (char)(Ch - 'A' + 'a');
        if (Ch != Word[Index])
            return 0;
    }
    return 1;
}

char *BoxAsmPpTrimmed(const char *Text, size_t Length)
{
    while (Length != 0 && (*Text == ' ' || *Text == '\t')) {
        Text++;
        Length--;
    }
    while (Length != 0 && (Text[Length - 1u] == ' ' || Text[Length - 1u] == '\t' ||
                           Text[Length - 1u] == '\r' || Text[Length - 1u] == '\n'))
        Length--;
    return BoxAsmCopyText(Text, Length);
}

void BoxAsmPpFreeList(char **List, uint32_t Count)
{
    uint32_t Index;

    for (Index = 0; List && Index < Count; Index++)
        free(List[Index]);
    free(List);
}

/* A parameter, its spaces dropped and, when it is wholly in braces, them. */
static char *BoxAsmPpParameter(const char *Text, size_t Length)
{
    char *Trimmed = BoxAsmPpTrimmed(Text, Length);
    size_t Size;

    if (!Trimmed)
        return NULL;
    Size = strlen(Trimmed);
    if (Size >= 2 && Trimmed[0] == '{' && Trimmed[Size - 1u] == '}') {
        memmove(Trimmed, Trimmed + 1, Size - 2u);
        Trimmed[Size - 2u] = '\0';
    }
    return Trimmed;
}

int BoxAsmPpSplit(const char *Text, uint32_t Limit, int Greedy, char ***Out, uint32_t *Count)
{
    char **List = NULL;
    size_t Capacity = 0;
    size_t At = 0;
    size_t Begin = 0;
    uint32_t Braces = 0;
    BoxAsmPpToken Token;

    *Out = NULL;
    *Count = 0;
    BoxAsmPpNext(Text, &At, &Token);
    while (Token.Kind == BOXASM_PP_SPACE)
        BoxAsmPpNext(Text, &At, &Token);
    if (Token.Kind == BOXASM_PP_END)
        return 0;
    At = 0;
    for (;;) {
        size_t Here = At;
        int Ends;

        BoxAsmPpNext(Text, &At, &Token);
        Ends = Token.Kind == BOXASM_PP_END;
        if (Token.Kind == BOXASM_PP_OTHER && *Token.Text == '{')
            Braces++;
        else if (Token.Kind == BOXASM_PP_OTHER && *Token.Text == '}' && Braces != 0)
            Braces--;
        if (Ends || (Token.Kind == BOXASM_PP_OTHER && *Token.Text == ',' && Braces == 0 &&
                     !(Greedy && *Count + 1u == Limit))) {
            if (BoxAsmGrowArray((void **)&List, &Capacity, *Count, sizeof(char *)) != 0 ||
                !(List[*Count] = BoxAsmPpParameter(Text + Begin, Here - Begin))) {
                BoxAsmPpFreeList(List, *Count);
                return -ERR_NO_MEMORY;
            }
            (*Count)++;
            Begin = At;
            if (Ends)
                break;
        }
    }
    *Out = List;
    return 0;
}

/* ---- Refusals ----------------------------------------------------------- */

/* At's column in the line, when At is in it: a token of some macro's body,
 * met while expanding, stands at the start of the line it was reached from. */
static BoxAsmLocation BoxAsmPpWhere(const BoxAsmPpLine *Line, const char *At)
{
    BoxAsmLocation Where;

    Where.Line = Line->Line;
    Where.Column = 1u;
    if (At && At >= Line->Text && At <= Line->Text + strlen(Line->Text))
        Where.Column = (uint32_t)(At - Line->Text) + 1u;
    Where.LineStart = Line->Text;
    return Where;
}

void BoxAsmPpFailBegin(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *At,
                       BoxAsmMessage *Message)
{
    BoxAsmLocation Where = BoxAsmPpWhere(Line, At);

    Pp->Asm->FailFile = Line->File;
    Pp->Asm->FailFrame = Line->Frame;
    BoxAsmFailBegin(Pp->Asm, &Where, Message);
}

int BoxAsmPpFailEnd(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *At,
                    BoxAsmMessage *Message)
{
    BoxAsmLocation Where = BoxAsmPpWhere(Line, At);

    return BoxAsmFailEnd(Pp->Asm, &Where, Message);
}

int BoxAsmPpFail(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *At, const char *Text,
                 const char *Name)
{
    BoxAsmMessage Message;

    BoxAsmPpFailBegin(Pp, Line, At, &Message);
    if (Name) {
        BoxAsmMessageName(&Message, Name);
        BoxAsmMessageText(&Message, " ");
    }
    BoxAsmMessageText(&Message, Text);
    return BoxAsmPpFailEnd(Pp, Line, At, &Message);
}

/* -ERR_OUT_OF_RANGE past BOXASM_MACRO_OUTPUT: no other refusal says it. */
static int BoxAsmPpAppend(BoxAsmBuffer *Out, const char *Text, size_t Length)
{
    if (Out->Size + Length > BOXASM_MACRO_OUTPUT)
        return -ERR_OUT_OF_RANGE;
    return BoxAsmBufferAppend(Out, Text, Length);
}

static int BoxAsmPpTooLarge(BoxAsmPp *Pp, const BoxAsmPpLine *Line, int Rc)
{
    if (Rc == -ERR_OUT_OF_RANGE)
        return BoxAsmPpFail(Pp, Line, NULL, "the macros make the line larger than 16 MiB", NULL);
    return Rc;
}

/* The buffer ended with a NUL that is not counted, so it reads as text. */
static int BoxAsmPpEndText(BoxAsmBuffer *Out)
{
    if (BoxAsmBufferAppend(Out, "", 1) != 0)
        return -ERR_NO_MEMORY;
    Out->Size--;
    return 0;
}

/* Spaces at the end of what is written dropped: %+ joins its neighbours. */
static void BoxAsmPpTrimEnd(BoxAsmBuffer *Out)
{
    while (Out->Size != 0 && (Out->Data[Out->Size - 1u] == ' ' || Out->Data[Out->Size - 1u] == '\t'))
        Out->Size--;
}

/* ---- Parameters of a call ----------------------------------------------- */

static int BoxAsmPpDecimal(BoxAsmBuffer *Out, uint64_t Value)
{
    char Digits[24];
    size_t Count = 0;

    do {
        Digits[sizeof(Digits) - 1u - Count++] = (char)('0' + Value % 10u);
        Value /= 10u;
    } while (Value != 0);
    return BoxAsmPpAppend(Out, Digits + sizeof(Digits) - Count, Count);
}

/* Parameter Number (1-based) of the call, its rotation counted; nothing past
 * the last. */
static int BoxAsmPpArgument(const BoxAsmCall *Call, uint64_t Number, BoxAsmBuffer *Out)
{
    const char *Text;

    if (Number == 0 || Number > Call->Count)
        return 0;
    Text = Call->Args[(Number - 1u + Call->Rotation) % Call->Count];
    return BoxAsmPpAppend(Out, Text, strlen(Text));
}

int BoxAsmPpSubstitute(BoxAsmPp *Pp, const BoxAsmPpLine *Line, BoxAsmBuffer *Out)
{
    const BoxAsmCall *Call = Pp->Call;
    const char *Text = Line->Text;
    size_t At = 0;
    int Rc = 0;

    Out->Size = 0;
    for (;;) {
        BoxAsmPpToken Token;
        const char *Next;

        BoxAsmPpNext(Text, &At, &Token);
        if (Token.Kind == BOXASM_PP_END) {
            Rc = BoxAsmPpAppend(Out, Token.Text, strlen(Token.Text));
            break;
        }
        Next = Token.Text + 1;
        if (Token.Kind != BOXASM_PP_OTHER || *Token.Text != '%' ||
            !(*Next == '%' || BoxAsmPpDigit(*Next) || *Next == '{' || *Next == '+')) {
            if ((Rc = BoxAsmPpAppend(Out, Token.Text, Token.Length)) != 0)
                break;
            continue;
        }
        if (*Next == '+') {                    /* %+ joins what stands on either side */
            BoxAsmPpTrimEnd(Out);
            At++;
            while (Text[At] == ' ' || Text[At] == '\t')
                At++;
            continue;
        }
        if (!Call) {
            BoxAsmMessage Message;
            size_t End = At + 1u;

            while (BoxAsmPpPart(Text[End]) || Text[End] == '{' || Text[End] == '}')
                End++;
            BoxAsmPpFailBegin(Pp, Line, Token.Text, &Message);
            BoxAsmMessageText(&Message, "'");
            BoxAsmMessageSpan(&Message, Token.Text, End - (size_t)(Token.Text - Text));
            BoxAsmMessageText(&Message, *Next == '%' ? "' is a label of a macro's own"
                                                     : "' is a parameter of a macro");
            BoxAsmMessageText(&Message, ", and this line is in none");
            return BoxAsmPpFailEnd(Pp, Line, Token.Text, &Message);
        }
        if (*Next == '%') {                    /* %%label: one of each call's own */
            size_t Begin = At + 1u;
            size_t End = Begin;

            while (BoxAsmPpPart(Text[End]))
                End++;
            if (End == Begin)
                return BoxAsmPpFail(Pp, Line, Token.Text, "%% is followed by the label's name", NULL);
            if ((Rc = BoxAsmPpAppend(Out, "..@", 3)) != 0 ||
                (Rc = BoxAsmPpDecimal(Out, Call->Unique)) != 0 ||
                (Rc = BoxAsmPpAppend(Out, ".", 1)) != 0 ||
                (Rc = BoxAsmPpAppend(Out, Text + Begin, End - Begin)) != 0)
                break;
            At = End;
            continue;
        }
        {
            int Braced = *Next == '{';
            size_t End = At + (size_t)Braced;
            uint64_t Number = 0;

            while (BoxAsmPpDigit(Text[End]) && Number < 0xFFFFFFFFu)
                Number = Number * 10u + (uint64_t)(Text[End++] - '0');
            if (Braced && (End == At + 1u || Text[End] != '}'))
                return BoxAsmPpFail(Pp, Line, Token.Text, "%{ holds a parameter's number: %{1}", NULL);
            At = End + (size_t)Braced;
            Rc = Number == 0 ? BoxAsmPpDecimal(Out, Call->Count) : BoxAsmPpArgument(Call, Number, Out);
            if (Rc != 0)
                break;
        }
    }
    if (Rc == 0)
        Rc = BoxAsmPpEndText(Out);
    return BoxAsmPpTooLarge(Pp, Line, Rc);
}

/* ---- Single-line macros -------------------------------------------------- */

uint32_t BoxAsmPpFindSingle(const BoxAsmPp *Pp, const char *Name, size_t Length,
                            int32_t Params, int *AnyLive)
{
    char Key[BOXASM_MACRO_NAME + 1u];
    uint32_t Index;

    *AnyLive = 0;
    if (Length > BOXASM_MACRO_NAME)
        return 0;
    memcpy(Key, Name, Length);
    Key[Length] = '\0';
    if (!BoxAsmNameIndexFind(&Pp->SingleIndex, Key, &Index))
        return 0;
    for (; Index != 0; Index = Pp->Singles[Index - 1u].Next) {
        const BoxAsmSingleMacro *Macro = &Pp->Singles[Index - 1u];

        if (!Macro->Live)
            continue;
        *AnyLive = 1;
        if (Macro->ParamCount == Params)
            return Index;
    }
    return 0;
}

/* The macros an expansion is inside, which it does not expand again. */
typedef struct BoxAsmPpOff {
    uint32_t                   Macro;
    const struct BoxAsmPpOff *Up;
} BoxAsmPpOff;

static int BoxAsmPpIsOff(const BoxAsmPpOff *Off, uint32_t Macro)
{
    for (; Off; Off = Off->Up)
        if (Off->Macro == Macro)
            return 1;
    return 0;
}

static int BoxAsmPpExpandText(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                              BoxAsmBuffer *Out, const BoxAsmPpOff *Off, uintptr_t Start);

/* A %define's body with its parameters replaced by the arguments, and %+
 * joining what stands beside it, NUL-ended in Out. */
static int BoxAsmPpBind(const BoxAsmSingleMacro *Macro, char **Args, BoxAsmBuffer *Out)
{
    size_t At = 0;
    int Rc = 0;

    Out->Size = 0;
    for (;;) {
        BoxAsmPpToken Token;
        int32_t Param;

        BoxAsmPpNext(Macro->Body, &At, &Token);
        if (Token.Kind == BOXASM_PP_END)
            break;
        if (Token.Kind == BOXASM_PP_OTHER && *Token.Text == '%' && Token.Text[1] == '+') {
            BoxAsmPpTrimEnd(Out);
            At++;
            while (Macro->Body[At] == ' ' || Macro->Body[At] == '\t')
                At++;
            continue;
        }
        for (Param = 0; Token.Kind == BOXASM_PP_WORD && Param < Macro->ParamCount; Param++)
            if (strlen(Macro->Params[Param]) == Token.Length &&
                memcmp(Macro->Params[Param], Token.Text, Token.Length) == 0)
                break;
        if (Token.Kind == BOXASM_PP_WORD && Param < Macro->ParamCount)
            Rc = BoxAsmPpAppend(Out, Args[Param], strlen(Args[Param]));
        else
            Rc = BoxAsmPpAppend(Out, Token.Text, Token.Length);
        if (Rc != 0)
            return Rc;
    }
    return BoxAsmPpEndText(Out);
}

/* The arguments of a call of a %define with parameters, from just after its
 * '(' to its ')', commas inside inner parentheses kept. *After is past ')'. */
static int BoxAsmPpCallArguments(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                                 size_t Open, size_t *After, char ***Args, uint32_t *Count)
{
    size_t At = Open + 1u;
    size_t Begin = At;
    size_t Capacity = 0;
    uint32_t Depth = 0;
    int Blank = 1;

    *Args = NULL;
    *Count = 0;
    for (;;) {
        BoxAsmPpToken Token;
        size_t Here = At;

        BoxAsmPpNext(Text, &At, &Token);
        if (Token.Kind == BOXASM_PP_END) {
            BoxAsmPpFreeList(*Args, *Count);
            *Args = NULL;
            return BoxAsmPpFail(Pp, Line, Text + Open, "is not closed by ')'", "(");
        }
        if (Token.Kind != BOXASM_PP_SPACE)
            Blank &= Token.Kind == BOXASM_PP_OTHER && *Token.Text == ')' && Depth == 0;
        if (Token.Kind != BOXASM_PP_OTHER)
            continue;
        if (*Token.Text == '(') {
            Depth++;
        } else if (*Token.Text == ')' && Depth != 0) {
            Depth--;
        } else if ((*Token.Text == ',' && Depth == 0) || *Token.Text == ')') {
            if (!(*Token.Text == ')' && Blank && *Count == 0)) {
                if (BoxAsmGrowArray((void **)Args, &Capacity, *Count, sizeof(char *)) != 0 ||
                    !((*Args)[*Count] = BoxAsmPpTrimmed(Text + Begin, Here - Begin))) {
                    BoxAsmPpFreeList(*Args, *Count);
                    *Args = NULL;
                    return -ERR_NO_MEMORY;
                }
                (*Count)++;
            }
            Begin = At;
            if (*Token.Text == ')') {
                *After = At;
                return 0;
            }
        }
    }
}

/* __LINE__ and __FILE__, unless the program defined them itself. */
static int BoxAsmPpBuiltin(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const BoxAsmPpToken *Token,
                           BoxAsmBuffer *Out, int *Done)
{
    const char *Name;

    *Done = 0;
    if (Token->Length == 8 && memcmp(Token->Text, "__LINE__", 8) == 0) {
        *Done = 1;
        return BoxAsmPpDecimal(Out, Line->Line);
    }
    if (Token->Length != 8 || memcmp(Token->Text, "__FILE__", 8) != 0)
        return 0;
    *Done = 1;
    if (BoxAsmPpAppend(Out, "\"", 1) != 0)
        return -ERR_OUT_OF_RANGE;
    for (Name = BoxAsmExpansionFile(Pp->Out, Line->File); *Name != '\0'; Name++)
        if (((*Name == '"' || *Name == '\\') && BoxAsmPpAppend(Out, "\\", 1) != 0) ||
            BoxAsmPpAppend(Out, Name, 1) != 0)
            return -ERR_OUT_OF_RANGE;
    return BoxAsmPpAppend(Out, "\"", 1);
}

/* One word of the text: a macro's expansion when it names a live one that
 * is not already being expanded, the word itself otherwise. *At moves past
 * the arguments a macro with parameters takes. */
static int BoxAsmPpExpandWord(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                              size_t *At, const BoxAsmPpToken *Token, BoxAsmBuffer *Out,
                              const BoxAsmPpOff *Off, uintptr_t Start)
{
    BoxAsmBuffer Bound = { 0 };
    BoxAsmPpOff Inner;
    char **Args = NULL;
    uint32_t Count = 0;
    uint32_t Macro;
    size_t Look = *At;
    int AnyLive;
    int Done;
    int Rc;

    Macro = BoxAsmPpFindSingle(Pp, Token->Text, Token->Length, -1, &AnyLive);
    if (Macro == 0 && !AnyLive) {
        if ((Rc = BoxAsmPpBuiltin(Pp, Line, Token, Out, &Done)) != 0 || Done)
            return Rc;
        return BoxAsmPpAppend(Out, Token->Text, Token->Length);
    }
    if (Macro == 0) {                          /* only with parameters: a '(' must follow */
        while (Text[Look] == ' ' || Text[Look] == '\t')
            Look++;
        if (Text[Look] != '(')
            return BoxAsmPpAppend(Out, Token->Text, Token->Length);
        if ((Rc = BoxAsmPpCallArguments(Pp, Line, Text, Look, At, &Args, &Count)) != 0)
            return Rc;
        Macro = BoxAsmPpFindSingle(Pp, Token->Text, Token->Length, (int32_t)Count, &AnyLive);
        if (Macro == 0) {
            BoxAsmMessage Message;

            BoxAsmPpFreeList(Args, Count);
            BoxAsmPpFailBegin(Pp, Line, Token->Text, &Message);
            BoxAsmMessageText(&Message, "no %define of '");
            BoxAsmMessageSpan(&Message, Token->Text, Token->Length);
            BoxAsmMessageText(&Message, "' takes ");
            BoxAsmMessageDecimal(&Message, Count);
            BoxAsmMessageText(&Message, Count == 1 ? " argument" : " arguments");
            return BoxAsmPpFailEnd(Pp, Line, Token->Text, &Message);
        }
    }
    if (BoxAsmPpIsOff(Off, Macro)) {               /* inside its own expansion: left as written */
        BoxAsmPpFreeList(Args, Count);
        return BoxAsmPpAppend(Out, Token->Text, (size_t)(Text + *At - Token->Text));
    }
    Rc = BoxAsmPpBind(&Pp->Singles[Macro - 1u], Args, &Bound);
    BoxAsmPpFreeList(Args, Count);
    Inner.Macro = Macro;
    Inner.Up = Off;
    if (Rc == 0)
        Rc = BoxAsmPpExpandText(Pp, Line, (const char *)Bound.Data, Out, &Inner, Start);
    BoxAsmBufferRelease(&Bound);
    return Rc;
}

static int BoxAsmPpExpandText(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                              BoxAsmBuffer *Out, const BoxAsmPpOff *Off, uintptr_t Start)
{
    size_t At = 0;
    int Rc = 0;

    if (BoxAsmTooDeep(Pp->Asm, Start))
        return BoxAsmPpFail(Pp, Line, NULL, "the %defines expand inside one another too deep", NULL);
    for (;;) {
        BoxAsmPpToken Token;

        BoxAsmPpNext(Text, &At, &Token);
        if (Token.Kind == BOXASM_PP_END)
            return 0;
        if (Token.Kind == BOXASM_PP_WORD)
            Rc = BoxAsmPpExpandWord(Pp, Line, Text, &At, &Token, Out, Off, Start);
        else
            Rc = BoxAsmPpAppend(Out, Token.Text, Token.Length);
        if (Rc != 0)
            return Rc;
    }
}

int BoxAsmPpExpand(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text, BoxAsmBuffer *Out)
{
    size_t At = 0;
    BoxAsmPpToken Token;
    int Rc;

    Out->Size = 0;
    Rc = BoxAsmPpExpandText(Pp, Line, Text, Out, NULL, (uintptr_t)__builtin_frame_address(0));
    if (Rc == 0) {                             /* the comment goes along as it was */
        do
            BoxAsmPpNext(Text, &At, &Token);
        while (Token.Kind != BOXASM_PP_END);
        Rc = BoxAsmPpAppend(Out, Token.Text, strlen(Token.Text));
    }
    if (Rc == 0)
        Rc = BoxAsmPpEndText(Out);
    return BoxAsmPpTooLarge(Pp, Line, Rc);
}

/* ---- Expressions --------------------------------------------------------- */

typedef struct {
    BoxAsmPp           *Pp;
    const BoxAsmPpLine *Line;
    const char         *Text;
    size_t              At;
    uintptr_t           Start;     /* the stack where the expression began */
} BoxAsmPpEval;

typedef enum {
    BOXASM_PP_OR = 0, BOXASM_PP_XOR, BOXASM_PP_AND, BOXASM_PP_RELATION,
    BOXASM_PP_BIT_OR, BOXASM_PP_BIT_XOR, BOXASM_PP_BIT_AND, BOXASM_PP_SHIFT,
    BOXASM_PP_SUM, BOXASM_PP_PRODUCT, BOXASM_PP_UNARY
} BoxAsmPpLevel;

static void BoxAsmPpSkip(BoxAsmPpEval *Eval)
{
    while (Eval->Text[Eval->At] == ' ' || Eval->Text[Eval->At] == '\t')
        Eval->At++;
}

static int BoxAsmPpIs(BoxAsmPpEval *Eval, const char *Operator)
{
    size_t Length = strlen(Operator);

    BoxAsmPpSkip(Eval);
    return strncmp(Eval->Text + Eval->At, Operator, Length) == 0;
}

/* The operator of Level at the cursor, taken; 0 when there is none. Longer
 * spellings are tried first, and one that belongs to a tighter level is
 * left alone: '<' is not a relation in "<<", '&' not a bit-and in "&&". */
static int BoxAsmPpOperator(BoxAsmPpEval *Eval, BoxAsmPpLevel Level, const char **Taken)
{
    static const char *const Operators[][7] = {
        [BOXASM_PP_OR]       = { "||" },
        [BOXASM_PP_XOR]      = { "^^" },
        [BOXASM_PP_AND]      = { "&&" },
        [BOXASM_PP_RELATION] = { "==", "!=", "<>", "<=", ">=", "<", ">" },
        [BOXASM_PP_BIT_OR]   = { "|" },
        [BOXASM_PP_BIT_XOR]  = { "^" },
        [BOXASM_PP_BIT_AND]  = { "&" },
        [BOXASM_PP_SHIFT]    = { "<<", ">>" },
        [BOXASM_PP_SUM]      = { "+", "-" },
        [BOXASM_PP_PRODUCT]  = { "*", "/", "%" },
    };
    const char *At;
    size_t Index;

    BoxAsmPpSkip(Eval);
    At = Eval->Text + Eval->At;
    if ((Level == BOXASM_PP_RELATION && (strncmp(At, "<<", 2) == 0 || strncmp(At, ">>", 2) == 0)) ||
        (Level == BOXASM_PP_BIT_OR && strncmp(At, "||", 2) == 0) ||
        (Level == BOXASM_PP_BIT_XOR && strncmp(At, "^^", 2) == 0) ||
        (Level == BOXASM_PP_BIT_AND && strncmp(At, "&&", 2) == 0))
        return 0;
    if (Level == BOXASM_PP_RELATION && *At == '=' && At[1] != '=') {
        *Taken = "==";                          /* NASM's = is == */
        Eval->At++;
        return 1;
    }
    for (Index = 0; Index < 7 && Operators[Level][Index]; Index++) {
        size_t Length = strlen(Operators[Level][Index]);

        if (strncmp(At, Operators[Level][Index], Length) == 0) {
            *Taken = Operators[Level][Index];
            Eval->At += Length;
            return 1;
        }
    }
    return 0;
}

static int BoxAsmPpLevelOf(BoxAsmPpEval *Eval, BoxAsmPpLevel Level, int64_t *Out);

/* A number, a character constant, or a parenthesised expression. */
static int BoxAsmPpPrimary(BoxAsmPpEval *Eval, int64_t *Out)
{
    const char *At;
    BoxAsmLexer Lexer;
    BoxAsmToken Token;
    const char *Why = NULL;
    int Rc;

    BoxAsmPpSkip(Eval);
    At = Eval->Text + Eval->At;
    if (*At == '(') {
        Eval->At++;
        if ((Rc = BoxAsmPpLevelOf(Eval, BOXASM_PP_OR, Out)) != 0)
            return Rc;
        if (!BoxAsmPpIs(Eval, ")"))
            return BoxAsmPpFail(Eval->Pp, Eval->Line, Eval->Text + Eval->At, "expected ')'", NULL);
        Eval->At++;
        return 0;
    }
    if (BoxAsmPpDigit(*At) || *At == '"' || *At == '\'') {
        BoxAsmLexBegin(&Lexer, At, strlen(At));
        if (BoxAsmLexNext(&Lexer, &Token, &Why) != 0)
            return BoxAsmPpFail(Eval->Pp, Eval->Line, At, Why, NULL);
        if (Token.Kind == BOXASM_TOKEN_FLOAT)
            return BoxAsmPpFail(Eval->Pp, Eval->Line, At,
                                "a floating-point number has no value in the preprocessor", NULL);
        if (Token.Kind == BOXASM_TOKEN_STRING) {
            BoxAsmBuffer Bytes = { 0 };
            uint64_t Value = 0;
            uint64_t Index;

            if ((Rc = BoxAsmLexStringBytes(&Token, &Bytes)) != 0)
                return Rc;
            if (Bytes.Size > 8) {
                BoxAsmBufferRelease(&Bytes);
                return BoxAsmPpFail(Eval->Pp, Eval->Line, At,
                                    "a string used as a number holds at most 8 bytes", NULL);
            }
            for (Index = Bytes.Size; Index-- > 0;)
                Value = (Value << 8) | Bytes.Data[Index];
            BoxAsmBufferRelease(&Bytes);
            *Out = (int64_t)Value;
        } else {
            *Out = (int64_t)Token.Number;
        }
        Eval->At += Token.Length;
        return 0;
    }
    if (BoxAsmPpLetter(*At) || *At == '.') {
        size_t Length = 0;
        BoxAsmMessage Message;

        while (BoxAsmPpPart(At[Length]))
            Length++;
        BoxAsmPpFailBegin(Eval->Pp, Eval->Line, At, &Message);
        BoxAsmMessageText(&Message, "'");
        BoxAsmMessageSpan(&Message, At, Length);
        BoxAsmMessageText(&Message, "' has no value before assembly: the preprocessor "
                                    "computes with numbers and the macros that stand for them");
        return BoxAsmPpFailEnd(Eval->Pp, Eval->Line, At, &Message);
    }
    return BoxAsmPpFail(Eval->Pp, Eval->Line, At, "expected a number, a character or '('", NULL);
}

static int BoxAsmPpUnary(BoxAsmPpEval *Eval, int64_t *Out)
{
    char Operator;
    int Rc;

    BoxAsmPpSkip(Eval);
    if (BoxAsmTooDeep(Eval->Pp->Asm, Eval->Start))
        return BoxAsmPpFail(Eval->Pp, Eval->Line, Eval->Text + Eval->At,
                            "the expression nests too deep", NULL);
    Operator = Eval->Text[Eval->At];
    if (Operator != '-' && Operator != '+' && Operator != '~' && Operator != '!')
        return BoxAsmPpPrimary(Eval, Out);
    if (Operator == '!' && Eval->Text[Eval->At + 1u] == '=')
        return BoxAsmPpPrimary(Eval, Out);
    Eval->At++;
    if ((Rc = BoxAsmPpUnary(Eval, Out)) != 0)
        return Rc;
    if (Operator == '-')
        *Out = (int64_t)(0 - (uint64_t)*Out);
    else if (Operator == '~')
        *Out = ~*Out;
    else if (Operator == '!')
        *Out = *Out == 0;
    return 0;
}

/* Left op Right, with the assembler's own arithmetic: wrapping, / % and >>
 * signed, shifts by 0 to 63. */
static int BoxAsmPpApply(BoxAsmPpEval *Eval, const char *Operator, const char *At, int64_t Left,
                         int64_t Right, int64_t *Out)
{
    uint64_t A = (uint64_t)Left;
    uint64_t B = (uint64_t)Right;

    switch (Operator[0]) {
    case '|': *Out = Operator[1] == '|' ? (Left || Right) : (int64_t)(A | B); return 0;
    case '^': *Out = Operator[1] == '^' ? (!Left != !Right) : (int64_t)(A ^ B); return 0;
    case '&': *Out = Operator[1] == '&' ? (Left && Right) : (int64_t)(A & B); return 0;
    case '=': *Out = Left == Right; return 0;
    case '!': *Out = Left != Right; return 0;
    case '+': *Out = (int64_t)(A + B); return 0;
    case '-': *Out = (int64_t)(A - B); return 0;
    case '*': *Out = (int64_t)(A * B); return 0;
    case '/':
    case '%':
        if (Right == 0)
            return BoxAsmPpFail(Eval->Pp, Eval->Line, At, "division by zero", NULL);
        if (Right == -1)
            *Out = Operator[0] == '/' ? (int64_t)(0 - A) : 0;
        else
            *Out = Operator[0] == '/' ? Left / Right : Left % Right;
        return 0;
    default:
        break;
    }
    if (Operator[0] == '<' && Operator[1] == '>') {
        *Out = Left != Right;
        return 0;
    }
    if ((Operator[0] == '<' && Operator[1] == '<') || (Operator[0] == '>' && Operator[1] == '>')) {
        if (Right < 0 || Right > 63)
            return BoxAsmPpFail(Eval->Pp, Eval->Line, At, "a shift count is 0 to 63", NULL);
        *Out = Operator[0] == '<' ? (int64_t)(A << B) : Left >> B;
        return 0;
    }
    if (Operator[0] == '<')
        *Out = Operator[1] == '=' ? Left <= Right : Left < Right;
    else
        *Out = Operator[1] == '=' ? Left >= Right : Left > Right;
    return 0;
}

static int BoxAsmPpLevelOf(BoxAsmPpEval *Eval, BoxAsmPpLevel Level, int64_t *Out)
{
    const char *Operator;
    int Rc;

    if (Level == BOXASM_PP_UNARY)
        return BoxAsmPpUnary(Eval, Out);
    if ((Rc = BoxAsmPpLevelOf(Eval, (BoxAsmPpLevel)(Level + 1), Out)) != 0)
        return Rc;
    for (;;) {
        const char *At;
        int64_t Right;

        BoxAsmPpSkip(Eval);
        At = Eval->Text + Eval->At;
        if (!BoxAsmPpOperator(Eval, Level, &Operator))
            return 0;
        if ((Rc = BoxAsmPpLevelOf(Eval, (BoxAsmPpLevel)(Level + 1), &Right)) != 0 ||
            (Rc = BoxAsmPpApply(Eval, Operator, At, *Out, Right, Out)) != 0)
            return Rc;
    }
}

int BoxAsmPpEvaluate(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text, int64_t *Out)
{
    BoxAsmPpEval Eval = { Pp, Line, Text, 0, (uintptr_t)__builtin_frame_address(0) };
    int Rc;

    BoxAsmPpSkip(&Eval);
    if (Text[Eval.At] == '\0' || Text[Eval.At] == ';')
        return BoxAsmPpFail(Pp, Line, Text + Eval.At, "expected an expression", NULL);
    if ((Rc = BoxAsmPpLevelOf(&Eval, BOXASM_PP_OR, Out)) != 0)
        return Rc;
    BoxAsmPpSkip(&Eval);
    if (Text[Eval.At] != '\0' && Text[Eval.At] != ';')
        return BoxAsmPpFail(Pp, Line, Text + Eval.At,
                            "expected an operator or the end of the expression", NULL);
    return 0;
}

/* ---- Words of a directive ------------------------------------------------- */

const char *BoxAsmPpSpaces(const char *Text)
{
    while (*Text == ' ' || *Text == '\t')
        Text++;
    return Text;
}

/* The directive a line is — the word after its leading '%' — and the text
 * after that word; 0 when the line is no directive. */
int BoxAsmPpDirective(const char *Text, BoxAsmPpToken *Name, const char **Rest)
{
    size_t At;

    Text = BoxAsmPpSpaces(Text);
    if (*Text != '%')
        return 0;
    At = 1;
    BoxAsmPpNext(Text, &At, Name);
    if (Name->Kind != BOXASM_PP_WORD)
        return 0;
    *Rest = Text + At;
    return 1;
}

/* A name at the start of Text: a word of at most BOXASM_MACRO_NAME bytes. */
int BoxAsmPpName(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                        const char *What, BoxAsmPpToken *Name)
{
    size_t At = 0;

    BoxAsmPpNext(Text, &At, Name);
    if (Name->Kind != BOXASM_PP_WORD)
        return BoxAsmPpFail(Pp, Line, Text, What, NULL);
    if (Name->Length > BOXASM_MACRO_NAME)
        return BoxAsmPpFail(Pp, Line, Text, "a macro's name is at most 255 characters", NULL);
    return 0;
}

int BoxAsmPpNothingMore(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text)
{
    Text = BoxAsmPpSpaces(Text);
    if (*Text != '\0' && *Text != ';')
        return BoxAsmPpFail(Pp, Line, Text, "is more than the directive takes", NULL);
    return 0;
}

/* The directive's line with its rest expanded: the text up to Rest as
 * written, then Rest's expansion, NUL-ended in Out. *Working is that line,
 * and *After points at the expansion in it, so what is said of it points
 * at its own column. */
int BoxAsmPpExpandRest(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest,
                              BoxAsmBuffer *Out, BoxAsmPpLine *Working, const char **After)
{
    BoxAsmBuffer Expanded = { 0 };
    size_t Prefix = (size_t)(Rest - Line->Text);
    int Rc = BoxAsmPpExpand(Pp, Line, Rest, &Expanded);

    Out->Size = 0;
    if (Rc == 0 && (BoxAsmBufferAppend(Out, Line->Text, Prefix) != 0 ||
                    BoxAsmBufferAppend(Out, Expanded.Data, Expanded.Size + 1u) != 0))
        Rc = -ERR_NO_MEMORY;
    BoxAsmBufferRelease(&Expanded);
    if (Rc != 0)
        return Rc;
    Out->Size--;                               /* its NUL uncounted */
    *Working = *Line;
    Working->Text = (const char *)Out->Data;
    *After = Working->Text + Prefix;
    return 0;
}

/* The value of the expression a directive's rest is, its macros expanded. */
int BoxAsmPpRestValue(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest,
                             int64_t *Value)
{
    BoxAsmBuffer Text = { 0 };
    BoxAsmPpLine Working;
    const char *After;
    int Rc = BoxAsmPpExpandRest(Pp, Line, Rest, &Text, &Working, &After);

    if (Rc == 0)
        Rc = BoxAsmPpEvaluate(Pp, &Working, After, Value);
    BoxAsmBufferRelease(&Text);
    return Rc;
}

/* Whether a text is one token of a kind, spaces aside. */
int BoxAsmPpOneToken(const char *Text, BoxAsmPpKind Kind)
{
    size_t At = 0;
    BoxAsmPpToken Token;

    do BoxAsmPpNext(Text, &At, &Token); while (Token.Kind == BOXASM_PP_SPACE);
    if (Token.Kind != Kind)
        return 0;
    do BoxAsmPpNext(Text, &At, &Token); while (Token.Kind == BOXASM_PP_SPACE);
    return Token.Kind == BOXASM_PP_END;
}
