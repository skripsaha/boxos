/* %if and its kin: the conditions of the preprocessor. A body (a file, a
 * macro's call, a %rep's pass) closes the conditions it opens. */

#include "boxasm_macro_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

int BoxAsmPpActive(const BoxAsmPp *Pp)
{
    return Pp->ConditionCount == 0 || Pp->Conditions[Pp->ConditionCount - 1u].Active;
}

/* The conditions past Count closed. */
void BoxAsmPpCloseConditions(BoxAsmPp *Pp, size_t Count)
{
    while (Pp->ConditionCount > Count)
        free(Pp->Conditions[--Pp->ConditionCount].Text);
}

typedef enum {
    BOXASM_PP_IF_VALUE = 0, BOXASM_PP_IF_DEF, BOXASM_PP_IF_IDN, BOXASM_PP_IF_IDNI,
    BOXASM_PP_IF_NUM, BOXASM_PP_IF_STR, BOXASM_PP_IF_ID, BOXASM_PP_IF_MACRO, BOXASM_PP_IF_CTX
} BoxAsmPpTest;

static const char *const BoxAsmPpTests[] = {
    "", "def", "idn", "idni", "num", "str", "id", "macro", "ctx"
};

/* Which test a %if or %elif spelling asks: if, ifn, ifdef, ifndef, ifidn and
 * the rest. Returns 1 for %if*, 2 for %elif*, 0 for neither. */
static int BoxAsmPpConditionWord(const BoxAsmPpToken *Word, BoxAsmPpTest *Test, int *Negate)
{
    char Lower[16];
    size_t Index;
    size_t Skip;
    int Kind;

    if (Word->Length >= sizeof(Lower))
        return 0;
    for (Index = 0; Index < Word->Length; Index++) {
        char Ch = Word->Text[Index];

        Lower[Index] = Ch >= 'A' && Ch <= 'Z' ? (char)(Ch - 'A' + 'a') : Ch;
    }
    Lower[Word->Length] = '\0';
    if (strncmp(Lower, "elif", 4) == 0) {
        Kind = 2;
        Skip = 4;
    } else if (strncmp(Lower, "if", 2) == 0) {
        Kind = 1;
        Skip = 2;
    } else {
        return 0;
    }
    for (Index = 0; Index < sizeof(BoxAsmPpTests) / sizeof(BoxAsmPpTests[0]); Index++) {
        if (strcmp(Lower + Skip, BoxAsmPpTests[Index]) == 0) {
            *Test = (BoxAsmPpTest)Index;
            *Negate = 0;
            return Kind;
        }
        if (Lower[Skip] == 'n' && strcmp(Lower + Skip + 1u, BoxAsmPpTests[Index]) == 0) {
            *Test = (BoxAsmPpTest)Index;
            *Negate = 1;
            return Kind;
        }
    }
    return 0;
}

/* Whether two texts are the same tokens, spaces aside. */
static int BoxAsmPpSameTokens(const char *A, const char *B, int AnyCase)
{
    size_t AtA = 0;
    size_t AtB = 0;

    for (;;) {
        BoxAsmPpToken Left;
        BoxAsmPpToken Right;
        size_t Index;

        do BoxAsmPpNext(A, &AtA, &Left); while (Left.Kind == BOXASM_PP_SPACE);
        do BoxAsmPpNext(B, &AtB, &Right); while (Right.Kind == BOXASM_PP_SPACE);
        if (Left.Kind != Right.Kind || Left.Length != Right.Length)
            return 0;
        if (Left.Kind == BOXASM_PP_END)
            return 1;
        for (Index = 0; Index < Left.Length; Index++) {
            char L = Left.Text[Index];
            char R = Right.Text[Index];

            if (AnyCase) {
                L = L >= 'A' && L <= 'Z' ? (char)(L - 'A' + 'a') : L;
                R = R >= 'A' && R <= 'Z' ? (char)(R - 'A' + 'a') : R;
            }
            if (L != R)
                return 0;
        }
    }
}

/* The truth of one %if or %elif. */
static int BoxAsmPpTestOf(BoxAsmPp *Pp, const BoxAsmPpLine *Line, BoxAsmPpTest Test,
                          const char *Rest, int *Truth)
{
    BoxAsmBuffer Expanded = { 0 };
    BoxAsmPpLine Working;
    BoxAsmPpToken Name;
    const char *Text;
    int64_t Value = 0;
    int AnyLive;
    int Rc;

    if (Test == BOXASM_PP_IF_CTX)
        return BoxAsmPpContextIs(Pp, Line, Rest, Truth);
    if (Test == BOXASM_PP_IF_DEF || Test == BOXASM_PP_IF_MACRO) {
        const char *After;

        Rest = BoxAsmPpSpaces(Rest);
        if ((Rc = BoxAsmPpName(Pp, Line, Rest, "expected the name of a macro", &Name)) != 0)
            return Rc;
        After = BoxAsmPpSpaces(Name.Text + Name.Length);
        if (Test == BOXASM_PP_IF_DEF) {
            (void)BoxAsmPpFindSingle(Pp, Name.Text, Name.Length, -1, &AnyLive);
            *Truth = AnyLive || (Name.Length == 8 && (memcmp(Name.Text, "__LINE__", 8) == 0 ||
                                                      memcmp(Name.Text, "__FILE__", 8) == 0));
            return BoxAsmPpNothingMore(Pp, Line, After);
        }
        *Truth = BoxAsmPpFindMulti(Pp, Name.Text, Name.Length) != 0;
        if (*Truth && *After != '\0' && *After != ';') {
            if ((Rc = BoxAsmPpRestValue(Pp, Line, After, &Value)) != 0)
                return Rc;
            *Truth = Value >= 0 && BoxAsmPpTakes(Pp, BoxAsmPpFindMulti(Pp, Name.Text, Name.Length),
                                                 (uint32_t)Value);
        }
        return 0;
    }
    if ((Rc = BoxAsmPpExpandRest(Pp, Line, Rest, &Expanded, &Working, &Text)) != 0) {
        BoxAsmBufferRelease(&Expanded);
        return Rc;
    }
    switch (Test) {
    case BOXASM_PP_IF_VALUE:
        Rc = BoxAsmPpEvaluate(Pp, &Working, Text, &Value);
        *Truth = Rc == 0 && Value != 0;
        break;
    case BOXASM_PP_IF_IDN:
    case BOXASM_PP_IF_IDNI: {
        char **Sides = NULL;
        uint32_t Count = 0;

        Rc = BoxAsmPpSplit(Text, 2, 1, &Sides, &Count);
        if (Rc == 0 && Count != 2)
            Rc = BoxAsmPpFail(Pp, Line, Rest, "compares two texts written with a comma between", NULL);
        if (Rc == 0)
            *Truth = BoxAsmPpSameTokens(Sides[0], Sides[1], Test == BOXASM_PP_IF_IDNI);
        BoxAsmPpFreeList(Sides, Count);
        break;
    }
    case BOXASM_PP_IF_NUM:
        *Truth = BoxAsmPpOneToken(Text, BOXASM_PP_NUMBER);
        break;
    case BOXASM_PP_IF_STR:
        *Truth = BoxAsmPpOneToken(Text, BOXASM_PP_STRING);
        break;
    default:
        *Truth = BoxAsmPpOneToken(Text, BOXASM_PP_WORD);
        break;
    }
    BoxAsmBufferRelease(&Expanded);
    return Rc;
}

static int BoxAsmPpOpenCondition(BoxAsmPp *Pp, const BoxAsmPpLine *Line, int Active, int Done)
{
    BoxAsmCondition *Condition;

    if (BoxAsmGrowArray((void **)&Pp->Conditions, &Pp->ConditionCapacity, Pp->ConditionCount,
                        sizeof(BoxAsmCondition)) != 0)
        return -ERR_NO_MEMORY;
    Condition = &Pp->Conditions[Pp->ConditionCount];
    if (!(Condition->Text = BoxAsmCopyText(Line->Text, strlen(Line->Text))))
        return -ERR_NO_MEMORY;
    Condition->Active = (uint8_t)Active;
    Condition->Done = (uint8_t)Done;
    Condition->Else = 0;
    Condition->File = Line->File;
    Condition->Line = Line->Line;
    Condition->Frame = Line->Frame;
    Pp->ConditionCount++;
    return 0;
}

/* %if*, %elif*, %else and %endif, in a taken branch or not. Base is the
 * first condition of this body: one opened outside it is not its own. */
int BoxAsmPpCondition(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const BoxAsmPpToken *Word,
                             const char *Rest, size_t Base, int *Handled)
{
    BoxAsmCondition *Top = Pp->ConditionCount > Base ? &Pp->Conditions[Pp->ConditionCount - 1u] : NULL;
    BoxAsmBuffer Substituted = { 0 };
    BoxAsmPpLine Working = *Line;
    BoxAsmBuffer Lead = { 0 };
    BoxAsmPpTest Test;
    int Negate;
    int Kind = BoxAsmPpConditionWord(Word, &Test, &Negate);
    int Truth = 0;
    int Rc = 0;

    *Handled = 1;
    if (Kind == 0 && BoxAsmPpWordIs(Word, "else")) {
        if (!Top)
            return BoxAsmPpFail(Pp, Line, Word->Text - 1, "%else has no %if before it", NULL);
        if (Top->Else)
            return BoxAsmPpFail(Pp, Line, Word->Text - 1, "a %if takes one %else", NULL);
        Top->Else = 1;
        Top->Active = !Top->Done;
        Top->Done = 1;
        return BoxAsmPpNothingMore(Pp, Line, Rest);
    }
    if (Kind == 0 && BoxAsmPpWordIs(Word, "endif")) {
        if (!Top)
            return BoxAsmPpFail(Pp, Line, Word->Text - 1, "%endif has no %if before it", NULL);
        BoxAsmPpCloseConditions(Pp, Pp->ConditionCount - 1u);
        return BoxAsmPpNothingMore(Pp, Line, Rest);
    }
    if (Kind == 0) {
        *Handled = 0;
        return 0;
    }
    if (Kind == 2 && !Top)
        return BoxAsmPpFail(Pp, Line, Word->Text - 1, "%elif has no %if before it", NULL);
    if (Kind == 2 && Top->Else)
        return BoxAsmPpFail(Pp, Line, Word->Text - 1, "%elif comes before %else, not after", NULL);
    /* The test is taken only where its branch could be: under a taken one. */
    if ((Kind == 1 && BoxAsmPpActive(Pp)) || (Kind == 2 && !Top->Done)) {
        if (Pp->Call) {
            BoxAsmPpToken Again;

            if ((Rc = BoxAsmPpSubstitute(Pp, Line, &Substituted)) != 0) {
                BoxAsmBufferRelease(&Substituted);
                return Rc;
            }
            Working.Text = (const char *)Substituted.Data;
            (void)BoxAsmPpDirective(Working.Text, &Again, &Rest);
        }
        /* %ifdef %$name asks of the context's own name. */
        if (Test == BOXASM_PP_IF_DEF || Test == BOXASM_PP_IF_MACRO)
            Rc = BoxAsmPpContextLead(Pp, &Working, &Rest, &Lead);
        if (Rc == 0)
            Rc = BoxAsmPpTestOf(Pp, &Working, Test, Rest, &Truth);
        Truth ^= Negate;
    }
    BoxAsmBufferRelease(&Substituted);
    BoxAsmBufferRelease(&Lead);
    if (Rc != 0)
        return Rc;
    if (Kind == 1)
        return BoxAsmPpOpenCondition(Pp, Line, BoxAsmPpActive(Pp) && Truth,
                                     !BoxAsmPpActive(Pp) || Truth);
    Top->Active = !Top->Done && Truth;
    Top->Done |= (uint8_t)Truth;
    return 0;
}
