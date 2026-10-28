/* The macros the preprocessor knows: %define, %xdefine, %assign and %undef,
 * and %macro and %unmacro, kept in name indexes over overloads that are
 * never freed while it runs — a macro may be defined again while it is
 * expanding. */

#include "boxasm_macro_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* ---- Single-line macros -------------------------------------------------- */

void BoxAsmPpDropSingle(BoxAsmSingleMacro *Macro)
{
    BoxAsmPpFreeList(Macro->Params, (uint32_t)(Macro->ParamCount > 0 ? Macro->ParamCount : 0));
    free(Macro->Body);
    Macro->Params = NULL;
    Macro->Body = NULL;
    Macro->Live = 0;
}

/* Name (Params parameters, -1 none) made to stand for Body, which it now owns. */
static int BoxAsmPpDefine(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const BoxAsmPpToken *Name,
                          int32_t ParamCount, char **Params, char *Body)
{
    uint32_t *Head;
    uint32_t Index;
    uint8_t Fresh;
    char *Key;
    int AnyLive;

    Index = BoxAsmPpFindSingle(Pp, Name->Text, Name->Length, ParamCount, &AnyLive);
    if (Index == 0 && AnyLive) {
        /* Live overloads, none of this count: with parameters only when this
         * is plain, a plain one when this takes parameters. */
        uint32_t Plain = BoxAsmPpFindSingle(Pp, Name->Text, Name->Length, -1, &AnyLive);

        if (ParamCount < 0 || Plain != 0) {
            BoxAsmPpFreeList(Params, (uint32_t)(ParamCount > 0 ? ParamCount : 0));
            free(Body);
            return BoxAsmPpFail(Pp, Line, Name->Text,
                                ParamCount < 0 ? "is already a %define with parameters; a name takes them or not"
                                               : "is already a %define without parameters; a name takes them or not",
                                NULL);
        }
    }
    if (Index != 0) {                          /* defined again: the newest body is the one */
        BoxAsmSingleMacro *Macro = &Pp->Singles[Index - 1u];

        BoxAsmPpDropSingle(Macro);
        Macro->Params = Params;
        Macro->Body = Body;
        Macro->Live = 1;
        return 0;
    }
    if (Pp->SingleCount >= 0xFFFFFFFEu ||
        BoxAsmGrowArray((void **)&Pp->Singles, &Pp->SingleCapacity, Pp->SingleCount,
                        sizeof(BoxAsmSingleMacro)) != 0 ||
        !(Key = BoxAsmCopyText(Name->Text, Name->Length))) {
        BoxAsmPpFreeList(Params, (uint32_t)(ParamCount > 0 ? ParamCount : 0));
        free(Body);
        return -ERR_NO_MEMORY;
    }
    if (BoxAsmNameIndexClaim(&Pp->SingleIndex, Key, &Head, &Fresh) != 0) {
        free(Key);
        BoxAsmPpFreeList(Params, (uint32_t)(ParamCount > 0 ? ParamCount : 0));
        free(Body);
        return -ERR_NO_MEMORY;
    }
    Pp->Singles[Pp->SingleCount] = (BoxAsmSingleMacro){
        Key, ParamCount, Params, Body, *Head, 1
    };
    *Head = (uint32_t)++Pp->SingleCount;
    return 0;
}

/* %define NAME body, %define NAME(a, b) body, and %xdefine, its body expanded
 * where it is written. */
int BoxAsmPpParseDefine(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest,
                               int Expand)
{
    BoxAsmPpToken Name;
    char **Params = NULL;
    uint32_t Count = 0;
    int32_t ParamCount = -1;
    const char *Body;
    char *Text;
    int Rc;

    Rest = BoxAsmPpSpaces(Rest);
    if ((Rc = BoxAsmPpName(Pp, Line, Rest, "expected the name the %define gives", &Name)) != 0)
        return Rc;
    Body = Name.Text + Name.Length;
    if (*Body == '(') {                        /* parameters only right after the name */
        const char *Close = Body;
        char *List;

        while (*Close != '\0' && *Close != ')' && *Close != ';')
            Close++;
        if (*Close != ')')
            return BoxAsmPpFail(Pp, Line, Body, "the parameters are not closed by ')'", NULL);
        if (!(List = BoxAsmCopyText(Body + 1, (size_t)(Close - Body - 1))))
            return -ERR_NO_MEMORY;
        Rc = BoxAsmPpSplit(List, 0, 0, &Params, &Count);
        free(List);
        if (Rc != 0)
            return Rc;
        for (uint32_t Index = 0; Index < Count; Index++) {
            BoxAsmPpToken Param;
            size_t At = 0;

            BoxAsmPpNext(Params[Index], &At, &Param);
            if (Param.Kind != BOXASM_PP_WORD || Param.Length != strlen(Params[Index])) {
                BoxAsmPpFreeList(Params, Count);
                return BoxAsmPpFail(Pp, Line, Body, "a parameter is a name: %define f(a, b)", NULL);
            }
        }
        ParamCount = (int32_t)Count;
        Body = Close + 1;
    }
    {
        size_t At = 0;
        BoxAsmPpToken Token;

        do BoxAsmPpNext(Body, &At, &Token); while (Token.Kind != BOXASM_PP_END);
        Text = BoxAsmPpTrimmed(Body, (size_t)(Token.Text - Body));
    }
    if (!Text) {
        BoxAsmPpFreeList(Params, Count);
        return -ERR_NO_MEMORY;
    }
    if (Expand) {
        BoxAsmBuffer Expanded = { 0 };

        Rc = BoxAsmPpExpand(Pp, Line, Text, &Expanded);
        free(Text);
        Text = Rc == 0 ? BoxAsmPpTrimmed((const char *)Expanded.Data, (size_t)Expanded.Size) : NULL;
        BoxAsmBufferRelease(&Expanded);
        if (!Text) {
            BoxAsmPpFreeList(Params, Count);
            return Rc != 0 ? Rc : -ERR_NO_MEMORY;
        }
    }
    return BoxAsmPpDefine(Pp, Line, &Name, ParamCount, Params, Text);
}

/* %assign NAME expression: the name stands for the expression's value. */
int BoxAsmPpParseAssign(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmPpToken Name;
    char Digits[24];
    size_t Count = 0;
    uint64_t Magnitude;
    int64_t Value;
    char *Text;
    int Rc;

    Rest = BoxAsmPpSpaces(Rest);
    if ((Rc = BoxAsmPpName(Pp, Line, Rest, "expected the name %assign gives", &Name)) != 0)
        return Rc;
    if ((Rc = BoxAsmPpRestValue(Pp, Line, Name.Text + Name.Length, &Value)) != 0)
        return Rc;
    Magnitude = Value < 0 ? (uint64_t)0 - (uint64_t)Value : (uint64_t)Value;
    do {
        Digits[sizeof(Digits) - 1u - Count++] = (char)('0' + Magnitude % 10u);
        Magnitude /= 10u;
    } while (Magnitude != 0);
    if (Value < 0)
        Digits[sizeof(Digits) - 1u - Count++] = '-';
    if (!(Text = BoxAsmCopyText(Digits + sizeof(Digits) - Count, Count)))
        return -ERR_NO_MEMORY;
    return BoxAsmPpDefine(Pp, Line, &Name, -1, NULL, Text);
}

int BoxAsmPpUndefine(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmPpToken Name;
    char Key[BOXASM_MACRO_NAME + 1u];
    uint32_t Index;
    int Rc;

    Rest = BoxAsmPpSpaces(Rest);
    if ((Rc = BoxAsmPpName(Pp, Line, Rest, "expected the name to %undef", &Name)) != 0)
        return Rc;
    memcpy(Key, Name.Text, Name.Length);
    Key[Name.Length] = '\0';
    if (BoxAsmNameIndexFind(&Pp->SingleIndex, Key, &Index))
        for (; Index != 0; Index = Pp->Singles[Index - 1u].Next)
            BoxAsmPpDropSingle(&Pp->Singles[Index - 1u]);
    return BoxAsmPpNothingMore(Pp, Line, Name.Text + Name.Length);
}


/* ---- Multi-line macros: definition --------------------------------------- */

void BoxAsmPpDropBody(BoxAsmPpLine *Lines, size_t Count)
{
    size_t Index;

    for (Index = 0; Lines && Index < Count; Index++)
        free((char *)Lines[Index].Text);
    free(Lines);
}

/* A count: digits, saturating past 0xFFFFFFFE. */
static const char *BoxAsmPpCount(const char *Text, uint32_t *Out)
{
    uint64_t Value = 0;

    while (*Text >= '0' && *Text <= '9') {
        Value = Value * 10u + (uint64_t)(*Text++ - '0');
        if (Value > 0xFFFFFFFEu)
            Value = 0xFFFFFFFEu;
    }
    *Out = (uint32_t)Value;
    return Text;
}

/* %macro NAME n, n-m, n-*, with '+' for a greedy last parameter, .nolist,
 * and the defaults of the parameters past n. */
int BoxAsmPpParseMacro(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest,
                              BoxAsmMultiMacro *Macro)
{
    BoxAsmPpToken Name;
    const char *At;
    int Rc;

    memset(Macro, 0, sizeof(*Macro));
    Rest = BoxAsmPpSpaces(Rest);
    if ((Rc = BoxAsmPpName(Pp, Line, Rest, "expected the name of the macro", &Name)) != 0)
        return Rc;
    At = BoxAsmPpSpaces(Name.Text + Name.Length);
    if (*At < '0' || *At > '9')
        return BoxAsmPpFail(Pp, Line, At, "expected how many parameters it takes: 2, 1-3, 0-* or 2+", NULL);
    At = BoxAsmPpCount(At, &Macro->Min);
    Macro->Max = Macro->Min;
    if (*At == '-') {
        At++;
        if (*At == '*') {
            Macro->Max = BOXASM_MACRO_ANY;
            At++;
        } else if (*At >= '0' && *At <= '9') {
            At = BoxAsmPpCount(At, &Macro->Max);
        } else {
            return BoxAsmPpFail(Pp, Line, At, "expected the most parameters, or *", NULL);
        }
        if (Macro->Max < Macro->Min)
            return BoxAsmPpFail(Pp, Line, At, "takes no fewer parameters than its fewest", NULL);
    }
    if (*At == '+') {
        Macro->Greedy = 1;
        At++;
    }
    if (strncmp(At, ".nolist", 7) == 0)
        At += 7;
    if (*At != ' ' && *At != '\t' && *At != '\0' && *At != ';')
        return BoxAsmPpFail(Pp, Line, At, "is not part of a parameter count", NULL);
    if ((Rc = BoxAsmPpSplit(At, 0, 0, &Macro->Defaults, &Macro->DefaultCount)) != 0)
        return Rc;
    if (Macro->Max != BOXASM_MACRO_ANY && Macro->DefaultCount > Macro->Max - Macro->Min) {
        BoxAsmPpFreeList(Macro->Defaults, Macro->DefaultCount);
        Macro->Defaults = NULL;
        return BoxAsmPpFail(Pp, Line, At, "has more defaults than parameters that may be left out", NULL);
    }
    if (!(Macro->Name = BoxAsmCopyText(Name.Text, Name.Length))) {
        BoxAsmPpFreeList(Macro->Defaults, Macro->DefaultCount);
        return -ERR_NO_MEMORY;
    }
    Macro->Defined = Line->Line;
    return 0;
}

void BoxAsmPpDropMulti(BoxAsmMultiMacro *Macro)
{
    BoxAsmPpFreeList(Macro->Defaults, Macro->DefaultCount);
    BoxAsmPpDropBody(Macro->Body, Macro->BodyCount);
    free(Macro->Name);
    memset(Macro, 0, sizeof(*Macro));
}

/* The gathered macro made live. A body is never freed while the
 * preprocessor runs: a macro may be defined again while it is expanding. */
int BoxAsmPpRegister(BoxAsmPp *Pp, BoxAsmPpCollect *Collect)
{
    BoxAsmMultiMacro *Macro = &Collect->Macro;
    uint32_t Index = 0;
    uint32_t *Head;
    uint8_t Fresh;
    char Key[BOXASM_MACRO_NAME + 1u];

    memcpy(Key, Macro->Name, strlen(Macro->Name) + 1u);
    if (BoxAsmNameIndexFind(&Pp->MultiIndex, Key, &Index)) {
        for (; Index != 0; Index = Pp->Multis[Index - 1u].Next) {
            BoxAsmMultiMacro *Other = &Pp->Multis[Index - 1u];
            uint64_t OtherMax = Other->Greedy ? BOXASM_MACRO_ANY : Other->Max;
            uint64_t MacroMax = Macro->Greedy ? BOXASM_MACRO_ANY : Macro->Max;

            if (!Other->Live)
                continue;
            if (Other->Min == Macro->Min && Other->Max == Macro->Max && Other->Greedy == Macro->Greedy) {
                Other->Live = 0;               /* defined again */
                continue;
            }
            if (Macro->Min <= OtherMax && Other->Min <= MacroMax)
                return BoxAsmPpFail(Pp, &Collect->Start, NULL,
                                    "takes some of the parameter counts another macro of its name takes",
                                    Macro->Name);
        }
    }
    if (Pp->MultiCount >= 0xFFFFFFFEu ||
        BoxAsmGrowArray((void **)&Pp->Multis, &Pp->MultiCapacity, Pp->MultiCount,
                        sizeof(BoxAsmMultiMacro)) != 0)
        return -ERR_NO_MEMORY;
    Macro->Body = Collect->Lines;
    Macro->BodyCount = (uint32_t)Collect->Count;
    Collect->Lines = NULL;
    Collect->Count = 0;
    if (BoxAsmNameIndexClaim(&Pp->MultiIndex, Macro->Name, &Head, &Fresh) != 0)
        return -ERR_NO_MEMORY;
    Macro->Next = *Head;
    Macro->Live = 1;
    Pp->Multis[Pp->MultiCount] = *Macro;
    memset(Macro, 0, sizeof(*Macro));
    *Head = (uint32_t)++Pp->MultiCount;
    return 0;
}

int BoxAsmPpUnmacro(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmMultiMacro Spec;
    uint32_t Index;
    int Rc = BoxAsmPpParseMacro(Pp, Line, Rest, &Spec);

    if (Rc != 0)
        return Rc;
    if (BoxAsmNameIndexFind(&Pp->MultiIndex, Spec.Name, &Index)) {
        for (; Index != 0; Index = Pp->Multis[Index - 1u].Next) {
            BoxAsmMultiMacro *Macro = &Pp->Multis[Index - 1u];

            if (Macro->Live && Macro->Min == Spec.Min && Macro->Max == Spec.Max &&
                Macro->Greedy == Spec.Greedy) {
                Macro->Live = 0;
                BoxAsmPpDropMulti(&Spec);
                return 0;
            }
        }
    }
    Rc = BoxAsmPpFail(Pp, Line, Rest, "is no macro with those parameter counts", Spec.Name);
    BoxAsmPpDropMulti(&Spec);
    return Rc;
}

uint32_t BoxAsmPpFindMulti(const BoxAsmPp *Pp, const char *Name, size_t Length)
{
    char Key[BOXASM_MACRO_NAME + 1u];
    uint32_t Index;

    if (Length > BOXASM_MACRO_NAME)
        return 0;
    memcpy(Key, Name, Length);
    Key[Length] = '\0';
    if (!BoxAsmNameIndexFind(&Pp->MultiIndex, Key, &Index))
        return 0;
    for (; Index != 0; Index = Pp->Multis[Index - 1u].Next)
        if (Pp->Multis[Index - 1u].Live)
            return Index;
    return 0;
}

/* Whether a live overload of the macro takes Count parameters. */
int BoxAsmPpTakes(const BoxAsmPp *Pp, uint32_t Index, uint32_t Count)
{
    for (; Index != 0; Index = Pp->Multis[Index - 1u].Next) {
        const BoxAsmMultiMacro *Macro = &Pp->Multis[Index - 1u];

        if (Macro->Live && Count >= Macro->Min &&
            (Count <= Macro->Max || Macro->Greedy))
            return 1;
    }
    return 0;
}
