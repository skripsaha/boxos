/* The preprocessor: NASM's %define, %xdefine, %assign, %undef, %macro,
 * %unmacro, %rotate, %if and its kin, %rep, %exitrep and %error, carried out
 * line by line before the assembler reads a word.
 *
 * Every line it writes remembers where it came from — the source line, and
 * the macro call it was written in — so a refusal anywhere later points at
 * the line of the source and names the calls it was reached through. A body
 * (of a macro, of a %rep, of the file) is a source of its own: a %if or a
 * %macro opened in it is closed in it. */

#include "boxasm_macro_internal.h"

#include "boxasm_isa.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

static int BoxAsmPpLines(BoxAsmPp *Pp, const BoxAsmPpLine *Lines, size_t Count);

/* ---- Output ------------------------------------------------------------- */

static int BoxAsmPpEmit(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text, size_t Length)
{
    BoxAsmExpansion *Out = Pp->Out;

    if (Out->Text.Size + Length + 1u > BOXASM_MACRO_OUTPUT)
        return BoxAsmPpFail(Pp, Line, NULL, "the macros make the source larger than 16 MiB", NULL);
    if (BoxAsmGrowArray((void **)&Out->Lines, &Out->LineCapacity, Out->LineCount,
                        sizeof(BoxAsmOrigin)) != 0 ||
        BoxAsmBufferAppend(&Out->Text, Text, Length) != 0 ||
        BoxAsmBufferAppend(&Out->Text, "\n", 1) != 0)
        return -ERR_NO_MEMORY;
    Out->Lines[Out->LineCount].Line = Line->Line;
    Out->Lines[Out->LineCount].Frame = Line->Frame;
    Out->LineCount++;
    return 0;
}

/* A new call of Name from Line: its frame. */
static int BoxAsmPpFrame(BoxAsmPp *Pp, const char *Name, const BoxAsmPpLine *Line, uint32_t *Frame)
{
    BoxAsmExpansion *Out = Pp->Out;

    if (Out->FrameCount >= 0xFFFFFFFFu || Out->Names.Size >= 0xFFFFFFFFu ||
        BoxAsmGrowArray((void **)&Out->Frames, &Out->FrameCapacity, Out->FrameCount,
                        sizeof(BoxAsmFrame)) != 0)
        return -ERR_NO_MEMORY;
    Out->Frames[Out->FrameCount].Name = (uint32_t)Out->Names.Size;
    Out->Frames[Out->FrameCount].Line = Line->Line;
    Out->Frames[Out->FrameCount].Parent = Line->Frame;
    if (BoxAsmBufferAppend(&Out->Names, Name, strlen(Name) + 1u) != 0)
        return -ERR_NO_MEMORY;
    *Frame = (uint32_t)Out->FrameCount++;
    return 0;
}

/* ---- Multi-line macros: calls --------------------------------------------- */

/* The overload of the macro that takes Count parameters, and how many it
 * takes when its last one is greedy (0: none takes Count). */
static uint32_t BoxAsmPpOverload(const BoxAsmPp *Pp, uint32_t Index, uint32_t Count,
                                 uint32_t *Limit)
{
    uint32_t Greedy = 0;

    for (; Index != 0; Index = Pp->Multis[Index - 1u].Next) {
        const BoxAsmMultiMacro *Macro = &Pp->Multis[Index - 1u];

        if (!Macro->Live)
            continue;
        if (Count >= Macro->Min && Count <= Macro->Max) {
            *Limit = Macro->Greedy ? Macro->Max : 0;
            return Index;
        }
        if (Macro->Greedy && Count > Macro->Max && Greedy == 0)
            Greedy = Index;
    }
    if (Greedy != 0)
        *Limit = Pp->Multis[Greedy - 1u].Max;
    return Greedy;
}

static int BoxAsmPpCountMismatch(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *At,
                                 uint32_t Index, uint32_t Count)
{
    const BoxAsmMultiMacro *Macro = &Pp->Multis[Index - 1u];
    BoxAsmMessage Message;

    BoxAsmPpFailBegin(Pp, Line, At, &Message);
    BoxAsmMessageName(&Message, Macro->Name);
    BoxAsmMessageText(&Message, " takes ");
    for (; Index != 0; Index = Pp->Multis[Index - 1u].Next) {
        const BoxAsmMultiMacro *Each = &Pp->Multis[Index - 1u];

        if (!Each->Live)
            continue;
        if (Each != Macro)
            BoxAsmMessageText(&Message, " or ");
        BoxAsmMessageDecimal(&Message, Each->Min);
        if (Each->Max == BOXASM_MACRO_ANY || Each->Greedy) {
            BoxAsmMessageText(&Message, " or more");
        } else if (Each->Max != Each->Min) {
            BoxAsmMessageText(&Message, " to ");
            BoxAsmMessageDecimal(&Message, Each->Max);
        }
    }
    BoxAsmMessageText(&Message, " parameters, and ");
    BoxAsmMessageDecimal(&Message, Count);
    BoxAsmMessageText(&Message, Count == 1 ? " is given here" : " are given here");
    return BoxAsmPpFailEnd(Pp, Line, At, &Message);
}

/* A call of the macro named at Name, its parameters the rest of the line. */
static int BoxAsmPpInvoke(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const BoxAsmPpToken *Name,
                          uint32_t First)
{
    const char *Rest = Name->Text + Name->Length;
    BoxAsmPpLine *Body = NULL;
    BoxAsmCall Call;
    BoxAsmCall *Outer;
    const BoxAsmMultiMacro *Macro;
    char **Args = NULL;
    uint32_t Count = 0;
    uint32_t Limit = 0;
    uint32_t Index;
    uint32_t Frame;
    uint32_t Each;
    int Rc;

    if ((Rc = BoxAsmPpSplit(Rest, 0, 0, &Args, &Count)) != 0)
        return Rc;
    Index = BoxAsmPpOverload(Pp, First, Count, &Limit);
    if (Index != 0 && Limit != 0 && Count > Limit) {
        BoxAsmPpFreeList(Args, Count);
        if ((Rc = BoxAsmPpSplit(Rest, Limit, 1, &Args, &Count)) != 0)
            return Rc;
    }
    if (Index == 0) {
        BoxAsmPpFreeList(Args, Count);
        return BoxAsmPpCountMismatch(Pp, Line, Name->Text, First, Count);
    }
    Macro = &Pp->Multis[Index - 1u];
    if (Pp->Calls >= BOXASM_MACRO_CALLS) {
        BoxAsmPpFreeList(Args, Count);
        return BoxAsmPpFail(Pp, Line, Name->Text,
                            "is called inside macro calls 256 deep: a macro may be calling itself",
                            Macro->Name);
    }
    /* The parameters left out that have defaults take them, and %0 counts
     * them as NASM does; one without a default is empty, and not counted. */
    if (Count < Macro->Min + Macro->DefaultCount) {
        uint32_t Filled = Macro->Min + Macro->DefaultCount;
        char **Grown = (char **)realloc(Args, Filled * sizeof(char *));

        if (!Grown) {
            BoxAsmPpFreeList(Args, Count);
            return -ERR_NO_MEMORY;
        }
        Args = Grown;
        for (; Count < Filled; Count++) {
            const char *Text = Macro->Defaults[Count - Macro->Min];

            if (!(Args[Count] = BoxAsmCopyText(Text, strlen(Text)))) {
                BoxAsmPpFreeList(Args, Count);
                return -ERR_NO_MEMORY;
            }
        }
    }
    if ((Rc = BoxAsmPpFrame(Pp, Macro->Name, Line, &Frame)) != 0 ||
        !(Body = (BoxAsmPpLine *)malloc((Macro->BodyCount ? Macro->BodyCount : 1u) *
                                        sizeof(BoxAsmPpLine)))) {
        BoxAsmPpFreeList(Args, Count);
        return Rc != 0 ? Rc : -ERR_NO_MEMORY;
    }
    for (Each = 0; Each < Macro->BodyCount; Each++) {
        Body[Each] = Macro->Body[Each];
        Body[Each].Frame = Frame;
    }
    Call.Args = Args;
    Call.Count = Count;
    Call.Rotation = 0;
    Call.Unique = ++Pp->Unique;
    Call.Reps = 0;
    Outer = Pp->Call;
    Pp->Call = &Call;
    Pp->Calls++;
    Rc = BoxAsmPpLines(Pp, Body, Macro->BodyCount);
    Pp->Calls--;
    Pp->Call = Outer;
    free(Body);
    BoxAsmPpFreeList(Args, Count);
    return Rc;
}

/* An ordinary line: its single-line macros expanded, then a call of a
 * multi-line macro carried out — after a label of its own, when the line
 * starts with one — or the line written out. */
static int BoxAsmPpOrdinary(BoxAsmPp *Pp, const BoxAsmPpLine *Line)
{
    BoxAsmBuffer Expanded = { 0 };
    BoxAsmPpLine Working = *Line;
    BoxAsmPpToken First;
    BoxAsmPpToken Colon;
    BoxAsmPpToken Second;
    uint32_t Macro = 0;
    size_t At = 0;
    int Rc;

    if ((Rc = BoxAsmPpExpand(Pp, Line, Line->Text, &Expanded)) != 0) {
        BoxAsmBufferRelease(&Expanded);
        return Rc;
    }
    Working.Text = (const char *)Expanded.Data;
    do BoxAsmPpNext(Working.Text, &At, &First); while (First.Kind == BOXASM_PP_SPACE);
    if (First.Kind == BOXASM_PP_WORD)
        Macro = BoxAsmPpFindMulti(Pp, First.Text, First.Length);
    if (Macro != 0) {
        uint32_t Limit;
        char **Args = NULL;
        uint32_t Count = 0;
        uint8_t Instruction;

        /* A macro named like an instruction stands aside for a count it does
         * not take, as in NASM: the instruction is meant. */
        Rc = BoxAsmPpSplit(First.Text + First.Length, 0, 0, &Args, &Count);
        BoxAsmPpFreeList(Args, Count);
        Instruction = BoxAsmFindInstruction(First.Text, First.Length, &(uint8_t){ 0 }) != NULL;
        if (Rc == 0 && Instruction && BoxAsmPpOverload(Pp, Macro, Count, &Limit) == 0)
            Rc = BoxAsmPpEmit(Pp, &Working, Working.Text, (size_t)Expanded.Size);
        else if (Rc == 0)
            Rc = BoxAsmPpInvoke(Pp, &Working, &First, Macro);
        BoxAsmBufferRelease(&Expanded);
        return Rc;
    }
    if (First.Kind == BOXASM_PP_WORD) {
        size_t After = At;

        BoxAsmPpNext(Working.Text, &After, &Colon);
        if (Colon.Kind == BOXASM_PP_OTHER && *Colon.Text == ':') {
            do BoxAsmPpNext(Working.Text, &After, &Second); while (Second.Kind == BOXASM_PP_SPACE);
            if (Second.Kind == BOXASM_PP_WORD &&
                (Macro = BoxAsmPpFindMulti(Pp, Second.Text, Second.Length)) != 0) {
                Rc = BoxAsmPpEmit(Pp, &Working, Working.Text, (size_t)(Colon.Text + 1 - Working.Text));
                if (Rc == 0)
                    Rc = BoxAsmPpInvoke(Pp, &Working, &Second, Macro);
                BoxAsmBufferRelease(&Expanded);
                return Rc;
            }
        }
    }
    Rc = BoxAsmPpEmit(Pp, &Working, Working.Text, (size_t)Expanded.Size);
    BoxAsmBufferRelease(&Expanded);
    return Rc;
}

/* ---- Directives ---------------------------------------------------------- */

static int BoxAsmPpRotate(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    int64_t Value;
    int Rc;

    if (!Pp->Call)
        return BoxAsmPpFail(Pp, Line, NULL, "%rotate turns a macro's parameters, and this line is in no macro", NULL);
    if ((Rc = BoxAsmPpRestValue(Pp, Line, Rest, &Value)) != 0 || Pp->Call->Count == 0)
        return Rc;
    Value %= (int64_t)Pp->Call->Count;
    if (Value < 0)
        Value += (int64_t)Pp->Call->Count;
    Pp->Call->Rotation = (uint32_t)((Pp->Call->Rotation + (uint64_t)Value) % Pp->Call->Count);
    return 0;
}

/* %error and %fatal: the text, or the string, as the refusal. */
static int BoxAsmPpError(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmBuffer Expanded = { 0 };
    BoxAsmMessage Message;
    char *Text;
    int Rc;

    if ((Rc = BoxAsmPpExpand(Pp, Line, Rest, &Expanded)) != 0) {
        BoxAsmBufferRelease(&Expanded);
        return Rc;
    }
    {
        size_t At = 0;
        BoxAsmPpToken Token;

        do BoxAsmPpNext((const char *)Expanded.Data, &At, &Token); while (Token.Kind != BOXASM_PP_END);
        Text = BoxAsmPpTrimmed((const char *)Expanded.Data, (size_t)(Token.Text - (const char *)Expanded.Data));
    }
    BoxAsmBufferRelease(&Expanded);
    if (!Text)
        return -ERR_NO_MEMORY;
    BoxAsmPpFailBegin(Pp, Line, NULL, &Message);
    if (BoxAsmPpOneToken(Text, BOXASM_PP_STRING) && strlen(Text) >= 2)
        BoxAsmMessageSpan(&Message, Text + 1, strlen(Text) - 2u);
    else
        BoxAsmMessageText(&Message, Text[0] ? Text : "%error");
    free(Text);
    return BoxAsmPpFailEnd(Pp, Line, NULL, &Message);
}

static int BoxAsmPpStartCollect(BoxAsmPp *Pp, const BoxAsmPpLine *Line, BoxAsmPpCollect *Collect,
                                uint8_t Kind)
{
    (void)Pp;
    Collect->Kind = Kind;
    Collect->Depth = 0;
    Collect->Start = *Line;
    Collect->Start.Text = BoxAsmCopyText(Line->Text, strlen(Line->Text));
    return Collect->Start.Text ? 0 : -ERR_NO_MEMORY;
}

/* The other directives, on a line of a taken branch. */
static int BoxAsmPpOtherDirective(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const BoxAsmPpToken *Word,
                                  const char *Rest, BoxAsmPpCollect *Collect)
{
    int Rc;

    if (BoxAsmPpWordIs(Word, "define"))
        return BoxAsmPpParseDefine(Pp, Line, Rest, 0);
    if (BoxAsmPpWordIs(Word, "xdefine"))
        return BoxAsmPpParseDefine(Pp, Line, Rest, 1);
    if (BoxAsmPpWordIs(Word, "assign"))
        return BoxAsmPpParseAssign(Pp, Line, Rest);
    if (BoxAsmPpWordIs(Word, "undef"))
        return BoxAsmPpUndefine(Pp, Line, Rest);
    if (BoxAsmPpWordIs(Word, "macro")) {
        if ((Rc = BoxAsmPpParseMacro(Pp, Line, Rest, &Collect->Macro)) != 0)
            return Rc;
        return BoxAsmPpStartCollect(Pp, Line, Collect, BOXASM_PP_COLLECT_MACRO);
    }
    if (BoxAsmPpWordIs(Word, "unmacro"))
        return BoxAsmPpUnmacro(Pp, Line, Rest);
    if (BoxAsmPpWordIs(Word, "rep")) {
        Rc = BoxAsmPpRestValue(Pp, Line, Rest, &Collect->Times);
        if (Rc == 0 && Collect->Times < 0)
            Rc = BoxAsmPpFail(Pp, Line, Rest, "%rep repeats 0 or more times", NULL);
        if (Rc != 0)
            return Rc;
        return BoxAsmPpStartCollect(Pp, Line, Collect, BOXASM_PP_COLLECT_REP);
    }
    if (BoxAsmPpWordIs(Word, "rotate"))
        return BoxAsmPpRotate(Pp, Line, Rest);
    if (BoxAsmPpWordIs(Word, "exitrep")) {
        if ((Pp->Call ? Pp->Call->Reps : Pp->OuterReps) == 0)
            return BoxAsmPpFail(Pp, Line, Word->Text - 1, "%exitrep stands inside a %rep", NULL);
        Pp->ExitRep = 1;
        return BoxAsmPpNothingMore(Pp, Line, Rest);
    }
    if (BoxAsmPpWordIs(Word, "error") || BoxAsmPpWordIs(Word, "fatal"))
        return BoxAsmPpError(Pp, Line, Rest);
    if (BoxAsmPpWordIs(Word, "endmacro"))
        return BoxAsmPpFail(Pp, Line, Word->Text - 1, "%endmacro has no %macro before it", NULL);
    if (BoxAsmPpWordIs(Word, "endrep"))
        return BoxAsmPpFail(Pp, Line, Word->Text - 1, "%endrep has no %rep before it", NULL);
    {
        BoxAsmMessage Message;

        BoxAsmPpFailBegin(Pp, Line, Word->Text - 1, &Message);
        BoxAsmMessageText(&Message, "'%");
        BoxAsmMessageSpan(&Message, Word->Text, Word->Length);
        BoxAsmMessageText(&Message, "' is not a directive of the preprocessor");
        return BoxAsmPpFailEnd(Pp, Line, Word->Text - 1, &Message);
    }
}

/* ---- Bodies -------------------------------------------------------------- */

/* One line of a body being gathered; *Ended when it was the body's end. */
static int BoxAsmPpGather(BoxAsmPpCollect *Collect, const BoxAsmPpLine *Line, int *Ended)
{
    const char *Open = Collect->Kind == BOXASM_PP_COLLECT_MACRO ? "macro" : "rep";
    const char *Close = Collect->Kind == BOXASM_PP_COLLECT_MACRO ? "endmacro" : "endrep";
    BoxAsmPpToken Word;
    const char *Rest;
    BoxAsmPpLine *Copy;

    *Ended = 0;
    if (BoxAsmPpDirective(Line->Text, &Word, &Rest)) {
        if (BoxAsmPpWordIs(&Word, Open)) {
            Collect->Depth++;
        } else if (BoxAsmPpWordIs(&Word, Close)) {
            if (Collect->Depth == 0) {
                *Ended = 1;
                return 0;
            }
            Collect->Depth--;
        }
    }
    if (BoxAsmGrowArray((void **)&Collect->Lines, &Collect->Capacity, Collect->Count,
                        sizeof(BoxAsmPpLine)) != 0)
        return -ERR_NO_MEMORY;
    Copy = &Collect->Lines[Collect->Count];
    *Copy = *Line;
    if (!(Copy->Text = BoxAsmCopyText(Line->Text, strlen(Line->Text))))
        return -ERR_NO_MEMORY;
    Collect->Count++;
    return 0;
}

static void BoxAsmPpEndCollect(BoxAsmPpCollect *Collect)
{
    BoxAsmPpDropBody(Collect->Lines, Collect->Count);
    free((char *)Collect->Start.Text);
    BoxAsmPpDropMulti(&Collect->Macro);
    memset(Collect, 0, sizeof(*Collect));
}

/* A %rep's body, Times over, or until %exitrep. */
static int BoxAsmPpRepeat(BoxAsmPp *Pp, BoxAsmPpCollect *Collect)
{
    uint32_t *Reps = Pp->Call ? &Pp->Call->Reps : &Pp->OuterReps;
    int64_t Index;
    int Rc = 0;

    (*Reps)++;
    for (Index = 0; Index < Collect->Times && Rc == 0; Index++) {
        if (++Pp->Walked > BOXASM_MACRO_WALKED) {
            Rc = BoxAsmPpFail(Pp, &Collect->Start, NULL,
                              "the preprocessor has walked 50 million lines: a %rep may run away", NULL);
            break;
        }
        Rc = BoxAsmPpLines(Pp, Collect->Lines, Collect->Count);
        if (Pp->ExitRep) {
            Pp->ExitRep = 0;
            break;
        }
    }
    (*Reps)--;
    return Rc;
}

/* One line of a body that is not being gathered. */
static int BoxAsmPpLineOf(BoxAsmPp *Pp, const BoxAsmPpLine *Line, size_t Base,
                          BoxAsmPpCollect *Collect)
{
    BoxAsmBuffer Substituted = { 0 };
    BoxAsmPpLine Working = *Line;
    BoxAsmPpToken Word;
    const char *Rest;
    int Handled;
    int Rc;

    if (++Pp->Walked > BOXASM_MACRO_WALKED)
        return BoxAsmPpFail(Pp, Line, NULL,
                            "the preprocessor has walked 50 million lines: a %rep may run away", NULL);
    if (BoxAsmPpDirective(Line->Text, &Word, &Rest)) {
        if ((Rc = BoxAsmPpCondition(Pp, Line, &Word, Rest, Base, &Handled)) != 0 || Handled)
            return Rc;
    }
    if (!BoxAsmPpActive(Pp))
        return 0;
    if ((Rc = BoxAsmPpSubstitute(Pp, Line, &Substituted)) != 0) {
        BoxAsmBufferRelease(&Substituted);
        return Rc;
    }
    Working.Text = (const char *)Substituted.Data;
    if (BoxAsmPpDirective(Working.Text, &Word, &Rest))
        Rc = BoxAsmPpOtherDirective(Pp, &Working, &Word, Rest, Collect);
    else
        Rc = BoxAsmPpOrdinary(Pp, &Working);
    BoxAsmBufferRelease(&Substituted);
    return Rc;
}

/* The lines of one body: a file, a macro's call, a %rep's pass. */
static int BoxAsmPpLines(BoxAsmPp *Pp, const BoxAsmPpLine *Lines, size_t Count)
{
    BoxAsmPpCollect Collect;
    size_t Base = Pp->ConditionCount;
    size_t Index;
    int Rc = 0;

    memset(&Collect, 0, sizeof(Collect));
    for (Index = 0; Index < Count && Rc == 0 && !Pp->ExitRep; Index++) {
        int Ended;

        if (Collect.Kind == BOXASM_PP_COLLECT_NONE) {
            Rc = BoxAsmPpLineOf(Pp, &Lines[Index], Base, &Collect);
            continue;
        }
        if ((Rc = BoxAsmPpGather(&Collect, &Lines[Index], &Ended)) != 0 || !Ended)
            continue;
        Rc = Collect.Kind == BOXASM_PP_COLLECT_MACRO ? BoxAsmPpRegister(Pp, &Collect)
                                                     : BoxAsmPpRepeat(Pp, &Collect);
        BoxAsmPpEndCollect(&Collect);
    }
    if (Rc == 0 && !Pp->ExitRep && Collect.Kind != BOXASM_PP_COLLECT_NONE)
        Rc = BoxAsmPpFail(Pp, &Collect.Start, NULL,
                          Collect.Kind == BOXASM_PP_COLLECT_MACRO ? "this %macro is not closed by %endmacro"
                                                                  : "this %rep is not closed by %endrep",
                          NULL);
    if (Rc == 0 && !Pp->ExitRep && Pp->ConditionCount > Base) {
        const BoxAsmCondition *Open = &Pp->Conditions[Base];
        BoxAsmPpLine At = { Open->Text, Open->Line, Open->Frame };

        Rc = BoxAsmPpFail(Pp, &At, NULL, "this %if is not closed by %endif", NULL);
    }
    BoxAsmPpCloseConditions(Pp, Base);         /* %exitrep leaves its body's %ifs */
    BoxAsmPpEndCollect(&Collect);
    return Rc;
}

/* ---- The whole source ---------------------------------------------------- */

static void BoxAsmPpRelease(BoxAsmPp *Pp)
{
    size_t Index;

    for (Index = 0; Index < Pp->SingleCount; Index++) {
        BoxAsmPpDropSingle(&Pp->Singles[Index]);
        free(Pp->Singles[Index].Name);
    }
    for (Index = 0; Index < Pp->MultiCount; Index++)
        BoxAsmPpDropMulti(&Pp->Multis[Index]);
    free(Pp->Singles);
    free(Pp->Multis);
    BoxAsmPpCloseConditions(Pp, 0);
    free(Pp->Conditions);
    BoxAsmNameIndexRelease(&Pp->SingleIndex);
    BoxAsmNameIndexRelease(&Pp->MultiIndex);
}

void BoxAsmExpansionRelease(BoxAsmExpansion *Expansion)
{
    BoxAsmBufferRelease(&Expansion->Text);
    BoxAsmBufferRelease(&Expansion->Names);
    free(Expansion->Lines);
    free(Expansion->Frames);
    memset(Expansion, 0, sizeof(*Expansion));
}

void BoxAsmExpansionPlace(const BoxAsmExpansion *Expansion, uint32_t Line, uint32_t *Source,
                          uint32_t *Frame)
{
    *Source = Line;
    *Frame = 0;
    if (Line >= 1 && Line <= Expansion->LineCount) {
        *Source = Expansion->Lines[Line - 1u].Line;
        *Frame = Expansion->Lines[Line - 1u].Frame;
    }
}

int BoxAsmPreprocess(BoxAsmAssembler *Asm, const char *Source, size_t Size, BoxAsmExpansion *Out)
{
    BoxAsmPpLine *Lines = NULL;
    size_t Count = 0;
    size_t Capacity = 0;
    char *Text;
    char *At;
    BoxAsmPp Pp;
    int Rc = 0;

    memset(Out, 0, sizeof(*Out));
    memset(&Pp, 0, sizeof(Pp));
    Pp.Asm = Asm;
    Pp.Out = Out;
    if (!(Text = BoxAsmCopyText(Source, Size)) ||
        BoxAsmGrowArray((void **)&Out->Frames, &Out->FrameCapacity, 0, sizeof(BoxAsmFrame)) != 0 ||
        BoxAsmBufferAppend(&Out->Names, "", 1) != 0) {
        free(Text);
        return -ERR_NO_MEMORY;
    }
    memset(&Out->Frames[0], 0, sizeof(BoxAsmFrame));
    Out->FrameCount = 1;
    Asm->Expansion = Out;
    /* The source cut into its lines, each ended by a NUL where its newline was. */
    for (At = Text; Rc == 0 && (size_t)(At - Text) <= Size;) {
        char *End = At;

        while ((size_t)(End - Text) < Size && *End != '\n')
            End++;
        if ((size_t)(End - Text) == Size && End == At && Count != 0)
            break;
        *End = '\0';
        if (End > At && End[-1] == '\r')
            End[-1] = '\0';
        if (Count >= 0xFFFFFFFEu ||
            BoxAsmGrowArray((void **)&Lines, &Capacity, Count, sizeof(BoxAsmPpLine)) != 0) {
            Rc = -ERR_NO_MEMORY;
            break;
        }
        Lines[Count].Text = At;
        Lines[Count].Line = (uint32_t)(Count + 1u);
        Lines[Count].Frame = 0;
        Count++;
        At = End + 1;
    }
    if (Rc == 0)
        Rc = BoxAsmPpLines(&Pp, Lines, Count);
    if (Rc == 0 && (BoxAsmBufferAppend(&Out->Text, "", 1) != 0))
        Rc = -ERR_NO_MEMORY;
    if (Rc == 0)
        Out->Text.Size--;                      /* the NUL ends the text, uncounted */
    BoxAsmPpRelease(&Pp);
    free(Lines);
    free(Text);
    return Rc;
}
