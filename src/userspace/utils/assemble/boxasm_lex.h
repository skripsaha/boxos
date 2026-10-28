#ifndef BOXASM_LEX_H
#define BOXASM_LEX_H

#include "boxasm_support.h"

/* Where a piece of source sits: 1-based line and column, and the start of
 * its line so a diagnostic can show it. */
typedef struct {
    uint32_t    Line;
    uint32_t    Column;
    const char *LineStart;
} BoxAsmLocation;

typedef enum {
    BOXASM_TOKEN_END = 0,     /* end of the source */
    BOXASM_TOKEN_NEWLINE,     /* end of a statement */
    BOXASM_TOKEN_IDENTIFIER,
    BOXASM_TOKEN_NUMBER,
    BOXASM_TOKEN_STRING,      /* "..." or '...'; a number where a number is meant */
    BOXASM_TOKEN_PUNCT        /* one of , : [ ] ( ) + - * / % & | ^ ~ $ and << >> $$ */
} BoxAsmTokenKind;

typedef struct {
    BoxAsmTokenKind Kind;
    BoxAsmLocation  Where;
    const char     *Text;     /* the token as written */
    size_t          Length;
    uint64_t        Number;   /* NUMBER: its value */
    uint16_t        Punct;    /* PUNCT: the character, or BOXASM_PUNCT_* for two */
} BoxAsmToken;

#define BOXASM_PUNCT_SHIFT_LEFT  0x100u
#define BOXASM_PUNCT_SHIFT_RIGHT 0x101u
#define BOXASM_PUNCT_SECTION     0x102u   /* $$ */

typedef struct {
    const char *Source;
    size_t      Size;
    size_t      Position;
    uint32_t    Line;
    const char *LineStart;
} BoxAsmLexer;

void BoxAsmLexBegin(BoxAsmLexer *Lexer, const char *Source, size_t Size);

/* The next token. A malformed one (an unclosed string, a bad escape, a number
 * that does not fit 64 bits, a character no token starts with) is -1 with the
 * reason in *Why and its place in Out->Where. */
int BoxAsmLexNext(BoxAsmLexer *Lexer, BoxAsmToken *Out, const char **Why);

/* The bytes a STRING token stands for, its escapes resolved. */
int BoxAsmLexStringBytes(const BoxAsmToken *Token, BoxAsmBuffer *Out);

int BoxAsmTokenIs(const BoxAsmToken *Token, const char *Word);
int BoxAsmTokenIsPunct(const BoxAsmToken *Token, uint16_t Punct);

#endif
