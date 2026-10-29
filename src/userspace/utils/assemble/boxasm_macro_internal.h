#ifndef BOXASM_MACRO_INTERNAL_H
#define BOXASM_MACRO_INTERNAL_H

#include "boxasm_macro.h"

/* How far the preprocessor goes before it says the source runs away: bytes
 * of expanded source, and lines walked (a %rep without output still walks).
 * How deep macros nest is the stack's to say (BoxAsmTooDeep). */
#define BOXASM_MACRO_OUTPUT    (16u << 20)
#define BOXASM_MACRO_WALKED    50000000u
#define BOXASM_MACRO_NAME      255u
#define BOXASM_MACRO_ANY       0xFFFFFFFFu   /* a parameter count of '*' */

/* A line as the preprocessor walks it: its text, NUL-ended, the source line
 * it stands for, and the macro call it was written in. */
typedef struct {
    const char *Text;
    uint32_t    Line;
    uint32_t    Frame;
} BoxAsmPpLine;

/* A single-line macro: %define, %xdefine or %assign. Overloads of one name
 * by their number of parameters chain through Next (an index + 1). */
typedef struct {
    char     *Name;
    int32_t   ParamCount;      /* -1: written without parentheses */
    char    **Params;
    char     *Body;
    uint32_t  Next;
    uint8_t   Live;
} BoxAsmSingleMacro;

/* A multi-line macro, overloaded by the counts of parameters it takes. */
typedef struct {
    char         *Name;
    uint32_t      Min;
    uint32_t      Max;         /* BOXASM_MACRO_ANY for '*' */
    uint8_t       Greedy;      /* '+': the last parameter takes the rest */
    char        **Defaults;    /* for parameters Min + 1 .. Min + DefaultCount */
    uint32_t      DefaultCount;
    BoxAsmPpLine *Body;
    uint32_t      BodyCount;
    uint32_t      Defined;
    uint32_t      Next;
    uint8_t       Live;
} BoxAsmMultiMacro;

/* The parameters of the macro call a line is expanded in. */
typedef struct {
    char   **Args;
    uint32_t Count;
    uint32_t Rotation;         /* %rotate */
    uint32_t Unique;           /* the number of its %%labels */
    uint32_t Reps;             /* %rep loops open in it, for %exitrep */
} BoxAsmCall;

/* One open %if: whether its branch is taken, and what it knows of the rest. */
typedef struct {
    uint8_t  Active;           /* the lines now are assembled */
    uint8_t  Done;             /* a branch was taken, or none can be */
    uint8_t  Else;             /* %else was seen */
    uint32_t Line;
    uint32_t Frame;
    char    *Text;             /* its line, to show when it is never closed */
} BoxAsmCondition;

typedef struct {
    BoxAsmAssembler   *Asm;
    BoxAsmExpansion   *Out;
    BoxAsmNameIndex    SingleIndex;
    BoxAsmSingleMacro *Singles;
    size_t             SingleCount;
    size_t             SingleCapacity;
    BoxAsmNameIndex    MultiIndex;
    BoxAsmMultiMacro  *Multis;
    size_t             MultiCount;
    size_t             MultiCapacity;
    BoxAsmCondition   *Conditions;
    size_t             ConditionCount;
    size_t             ConditionCapacity;
    BoxAsmCall        *Call;       /* the call being expanded, NULL outside one */
    uint32_t           Unique;     /* the last %%label number given */
    uint32_t           OuterReps;  /* %rep loops open outside any call */
    uint8_t            ExitRep;    /* %exitrep: the innermost %rep stops */
    uint64_t           Walked;
} BoxAsmPp;

/* ---- boxasm_macro_text.c ------------------------------------------------ */

typedef enum {
    BOXASM_PP_END = 0,         /* the end of the line, or its comment */
    BOXASM_PP_SPACE,
    BOXASM_PP_WORD,
    BOXASM_PP_NUMBER,
    BOXASM_PP_STRING,
    BOXASM_PP_OTHER            /* one character */
} BoxAsmPpKind;

typedef struct {
    uint8_t     Kind;
    const char *Text;
    size_t      Length;
} BoxAsmPpToken;

/* The token at *At, *At moved past it. */
void BoxAsmPpNext(const char *Line, size_t *At, BoxAsmPpToken *Out);
int  BoxAsmPpWordIs(const BoxAsmPpToken *Token, const char *Word);

/* A refusal at a column of a line, the line shown and the calls it came by. */
int  BoxAsmPpFail(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *At, const char *Text,
                  const char *Name);
void BoxAsmPpFailBegin(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *At,
                       BoxAsmMessage *Message);
int  BoxAsmPpFailEnd(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *At,
                     BoxAsmMessage *Message);

/* A macro call's line with %1, %{1}, %0, %%label and %+ replaced. */
int BoxAsmPpSubstitute(BoxAsmPp *Pp, const BoxAsmPpLine *Line, BoxAsmBuffer *Out);

/* The line with its single-line macros expanded, and expanded again in what
 * they expand to, never a macro inside its own expansion. */
int BoxAsmPpExpand(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                   BoxAsmBuffer *Out);

/* The value of a preprocessor expression: numbers, characters, and C's
 * operators with NASM's precedence, = <> and ^^ among them. */
int BoxAsmPpEvaluate(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                     int64_t *Out);

/* The live single-line macro called Name with Params parameters (-1: none);
 * 0 when there is none. Returns its index + 1. */
uint32_t BoxAsmPpFindSingle(const BoxAsmPp *Pp, const char *Name, size_t Length,
                            int32_t Params, int *AnyLive);

/* Text, the spaces around it dropped, as a heap string. */
char *BoxAsmPpTrimmed(const char *Text, size_t Length);

/* Parameters split at the commas outside strings and braces; a parameter
 * wholly in braces loses them. At most Limit (the rest joined into the last
 * when Greedy). Returns how many, or a negative error. */
int BoxAsmPpSplit(const char *Text, uint32_t Limit, int Greedy, char ***Out,
                  uint32_t *Count);
void BoxAsmPpFreeList(char **List, uint32_t Count);

/* ---- boxasm_macro_text.c: a directive's words --------------------------- */

const char *BoxAsmPpSpaces(const char *Text);
/* The directive a line is — the word after its leading '%' — and the text
 * after that word; 0 when the line is no directive. */
int BoxAsmPpDirective(const char *Text, BoxAsmPpToken *Name, const char **Rest);
int BoxAsmPpName(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text, const char *What,
                 BoxAsmPpToken *Name);
int BoxAsmPpNothingMore(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text);
/* The directive's line with its rest expanded, *After at the expansion in it. */
int BoxAsmPpExpandRest(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest,
                       BoxAsmBuffer *Out, BoxAsmPpLine *Working, const char **After);
int BoxAsmPpRestValue(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest, int64_t *Value);
/* Whether a text is one token of a kind, spaces aside. */
int BoxAsmPpOneToken(const char *Text, BoxAsmPpKind Kind);

/* ---- boxasm_macro_if.c --------------------------------------------------- */

int  BoxAsmPpActive(const BoxAsmPp *Pp);
void BoxAsmPpCloseConditions(BoxAsmPp *Pp, size_t Count);
/* %if*, %elif*, %else and %endif; *Handled 0 for any other directive. */
int  BoxAsmPpCondition(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const BoxAsmPpToken *Word,
                       const char *Rest, size_t Base, int *Handled);

/* ---- boxasm_macro_define.c ------------------------------------------------ */

typedef enum {
    BOXASM_PP_COLLECT_NONE = 0,
    BOXASM_PP_COLLECT_MACRO,
    BOXASM_PP_COLLECT_REP
} BoxAsmPpCollectKind;

/* The body of a %macro or a %rep being gathered, up to its end. */
typedef struct {
    uint8_t           Kind;
    uint32_t          Depth;         /* inner ones of the same kind */
    BoxAsmPpLine      Start;         /* its own line, the text a copy */
    BoxAsmPpLine     *Lines;
    size_t            Count;
    size_t            Capacity;
    BoxAsmMultiMacro  Macro;         /* MACRO: what its header said */
    int64_t           Times;         /* REP */
} BoxAsmPpCollect;

void     BoxAsmPpDropSingle(BoxAsmSingleMacro *Macro);
int      BoxAsmPpParseDefine(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest, int Expand);
int      BoxAsmPpParseAssign(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);
int      BoxAsmPpUndefine(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);
void     BoxAsmPpDropBody(BoxAsmPpLine *Lines, size_t Count);
int      BoxAsmPpParseMacro(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest,
                            BoxAsmMultiMacro *Macro);
void     BoxAsmPpDropMulti(BoxAsmMultiMacro *Macro);
int      BoxAsmPpRegister(BoxAsmPp *Pp, BoxAsmPpCollect *Collect);
int      BoxAsmPpUnmacro(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);
uint32_t BoxAsmPpFindMulti(const BoxAsmPp *Pp, const char *Name, size_t Length);
int      BoxAsmPpTakes(const BoxAsmPp *Pp, uint32_t Index, uint32_t Count);

#endif
