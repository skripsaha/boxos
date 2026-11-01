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

/* A line as the preprocessor walks it: its text, NUL-ended, the file and
 * the line of it it stands for, and the macro call or %include it was
 * reached through. */
typedef struct {
    const char *Text;
    uint32_t    File;
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
    uint8_t   Blind;           /* %idefine: its name matches in any case */
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
    uint8_t       Blind;       /* %imacro: its name matches in any case */
} BoxAsmMultiMacro;

/* The parameters of the macro call a line is expanded in. */
typedef struct {
    char   **Args;
    uint32_t Count;
    uint32_t Rotation;         /* %rotate */
    uint32_t Unique;           /* the number of its %%labels */
    uint32_t Reps;             /* %rep loops open in it, for %exitrep */
} BoxAsmCall;

/* One context of %push: its name ("" when it was given none), the number
 * its local names are made unique by, and the line that opened it. */
typedef struct {
    char    *Name;
    uint32_t Number;
    uint32_t File;
    uint32_t Line;
    uint32_t Frame;
    char    *Text;
} BoxAsmPpContext;

/* One open %if: whether its branch is taken, and what it knows of the rest. */
typedef struct {
    uint8_t  Active;           /* the lines now are assembled */
    uint8_t  Done;             /* a branch was taken, or none can be */
    uint8_t  Else;             /* %else was seen */
    uint32_t File;
    uint32_t Line;
    uint32_t Frame;
    char    *Text;             /* its line, to show when it is never closed */
} BoxAsmCondition;

typedef struct {
    BoxAsmAssembler   *Asm;
    BoxAsmExpansion   *Out;
    BoxAsmFiles       *Files;      /* where %includes are found; NULL: nowhere */
    BoxAsmBuffer      *Said;       /* where %warning's lines go; NULL: nowhere */
    uint64_t          *Open;       /* the files being read, by id, the source first */
    size_t             OpenCount;
    size_t             OpenCapacity;
    char             **Homes;      /* each file's home, by its index in Out->Files */
    size_t             HomeCapacity;
    BoxAsmNameIndex    SingleIndex;
    BoxAsmNameIndex    SingleBlindIndex; /* %idefine'd names, in lower case */
    BoxAsmSingleMacro *Singles;
    size_t             SingleCount;
    size_t             SingleCapacity;
    BoxAsmNameIndex    MultiIndex;
    BoxAsmNameIndex    MultiBlindIndex;  /* %imacro'd names, in lower case */
    BoxAsmMultiMacro  *Multis;
    size_t             MultiCount;
    size_t             MultiCapacity;
    BoxAsmCondition   *Conditions;
    size_t             ConditionCount;
    size_t             ConditionCapacity;
    BoxAsmPpContext   *Contexts;   /* %push's stack, its top last */
    size_t             ContextCount;
    size_t             ContextCapacity;
    BoxAsmCall        *Call;       /* the call being expanded, NULL outside one */
    uint32_t           Unique;     /* the last %%label number given */
    uint32_t           OuterReps;  /* %rep loops open outside any call */
    uint8_t            ExitRep;    /* %exitrep: the innermost %rep stops */
    uint64_t           Walked;
} BoxAsmPp;

/* ---- boxasm_macro.c ----------------------------------------------------- */

/* A file's text cut into its lines in place, each ended by a NUL where its
 * newline was: file File, reached through Frame. *Lines is the caller's to
 * free. A NUL in the text is refused where it stands: a source is text. */
int BoxAsmPpCut(BoxAsmPp *Pp, char *Text, size_t Size, uint32_t File, uint32_t Frame,
                BoxAsmPpLine **Lines, size_t *Count);
/* A new frame: a macro called, or a file included, from Line. */
int BoxAsmPpFrame(BoxAsmPp *Pp, uint8_t Kind, const char *Name, const BoxAsmPpLine *Line,
                  uint32_t *Frame);
/* The lines of one body: a file, a macro's call, a %rep's pass. */
int BoxAsmPpLines(BoxAsmPp *Pp, const BoxAsmPpLine *Lines, size_t Count);

/* ---- boxasm_macro_context.c --------------------------------------------- */

/* %push [name], %pop [name] and %repl name. */
int  BoxAsmPpPush(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);
int  BoxAsmPpPop(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);
int  BoxAsmPpReplace(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);
/* %ifctx name...: whether the top context is called one of the names. */
int  BoxAsmPpContextIs(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest, int *Truth);
/* Text with each %$name, %$$name and deeper written as the name the context
 * gives it, ..@N.name, as it stands now; Out is Text itself (*Out == Text)
 * when it names none. */
int  BoxAsmPpContextText(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                         BoxAsmBuffer *Buffer, const char **Out);
/* A directive whose first word, at *Rest, is %$name: the line with that word
 * the context's own, in Buffer, *Working and *Rest moved into it. */
int  BoxAsmPpContextLead(BoxAsmPp *Pp, BoxAsmPpLine *Working, const char **Rest,
                         BoxAsmBuffer *Buffer);
/* A context left open at the end of the source is refused at its %push. */
int  BoxAsmPpContextsClosed(BoxAsmPp *Pp);
void BoxAsmPpDropContexts(BoxAsmPp *Pp);

/* ---- boxasm_macro_string.c ----------------------------------------------- */

/* %strlen NAME string, %substr NAME string, start[, length], and %strcat
 * NAME string, string...: NAME defined as the length or the string made. */
int BoxAsmPpStrlen(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);
int BoxAsmPpSubstr(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);
int BoxAsmPpStrcat(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);

/* ---- boxasm_macro_include.c --------------------------------------------- */

/* %include "name" tags... [everywhere]: the one file it means, read here. */
int BoxAsmPpInclude(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);
/* An ordinary line about to be put out: when it is an incbin, the one file
 * it means read, its bytes kept for the line. */
int BoxAsmPpIncbin(BoxAsmPp *Pp, const BoxAsmPpLine *Line);

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
/* Whether a name may hold the character past its first. */
int  BoxAsmPpPart(char Ch);
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

/* The live single-line macro called Name with Params parameters (-1: none),
 * defined in that case or with %idefine in any; 0 when there is none.
 * Returns its index + 1. */
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
/* Name made to stand for Body (Params parameters, -1 none), which it owns;
 * Blind, in any case. */
int      BoxAsmPpDefine(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const BoxAsmPpToken *Name,
                        int32_t ParamCount, char **Params, char *Body, int Blind);
int      BoxAsmPpParseDefine(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest, int Expand,
                             int Blind);
int      BoxAsmPpParseAssign(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest, int Blind);
int      BoxAsmPpUndefine(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest);
void     BoxAsmPpDropBody(BoxAsmPpLine *Lines, size_t Count);
int      BoxAsmPpParseMacro(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest,
                            BoxAsmMultiMacro *Macro, int Blind);
void     BoxAsmPpDropMulti(BoxAsmMultiMacro *Macro);
int      BoxAsmPpRegister(BoxAsmPp *Pp, BoxAsmPpCollect *Collect);
int      BoxAsmPpUnmacro(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest, int Blind);
uint32_t BoxAsmPpFindMulti(const BoxAsmPp *Pp, const char *Name, size_t Length);
int      BoxAsmPpTakes(const BoxAsmPp *Pp, uint32_t Index, uint32_t Count);

#endif
