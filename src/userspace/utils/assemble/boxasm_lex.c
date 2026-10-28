#include "boxasm_lex.h"

#include "box/error.h"
#include "box/string.h"

static int BoxAsmIsLetter(char Ch)
{
    return (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || Ch == '_';
}

static int BoxAsmIsDigit(char Ch)
{
    return Ch >= '0' && Ch <= '9';
}

static int BoxAsmIdentifierStart(char Ch)
{
    return BoxAsmIsLetter(Ch) || Ch == '.';
}

static int BoxAsmIdentifierPart(char Ch)
{
    return BoxAsmIsLetter(Ch) || BoxAsmIsDigit(Ch) || Ch == '.';
}

static int BoxAsmDigitValue(char Ch)
{
    if (Ch >= '0' && Ch <= '9') return Ch - '0';
    if (Ch >= 'a' && Ch <= 'f') return Ch - 'a' + 10;
    if (Ch >= 'A' && Ch <= 'F') return Ch - 'A' + 10;
    return 99;
}

void BoxAsmLexBegin(BoxAsmLexer *Lexer, const char *Source, size_t Size)
{
    Lexer->Source = Source;
    Lexer->Size = Size;
    Lexer->Position = 0;
    Lexer->Line = 1;
    Lexer->LineStart = Source;
}

static char BoxAsmPeek(const BoxAsmLexer *Lexer, size_t Ahead)
{
    return Lexer->Position + Ahead < Lexer->Size ? Lexer->Source[Lexer->Position + Ahead] : '\0';
}

/* Decimal, 0x hexadecimal or 0b binary, with _ allowed between digits. A
 * letter or digit run on past the number is part of a malformed number,
 * never the start of the next token. */
static size_t BoxAsmSkipDigits(const BoxAsmLexer *Lexer, size_t At)
{
    while (At < Lexer->Size && (BoxAsmIsDigit(Lexer->Source[At]) || Lexer->Source[At] == '_'))
        At++;
    return At;
}

/* digits '.' [digits] [e|E [+|-] digits], '_' among any of the digits. */
static int BoxAsmLexFloat(BoxAsmLexer *Lexer, BoxAsmToken *Out, const char **Why)
{
    size_t At = BoxAsmSkipDigits(Lexer, Lexer->Position) + 1u;

    At = BoxAsmSkipDigits(Lexer, At);
    if (At < Lexer->Size && (Lexer->Source[At] == 'e' || Lexer->Source[At] == 'E')) {
        size_t Digits;

        At++;
        if (At < Lexer->Size && (Lexer->Source[At] == '+' || Lexer->Source[At] == '-'))
            At++;
        Digits = At;
        At = BoxAsmSkipDigits(Lexer, At);
        if (At == Digits || !BoxAsmIsDigit(Lexer->Source[Digits])) {
            Lexer->Position = At;
            *Why = "an exponent needs its digits: 1.5e3";
            return -1;
        }
    }
    Lexer->Position = At;
    if (At < Lexer->Size && BoxAsmIdentifierPart(Lexer->Source[At])) {
        *Why = "malformed number";
        return -1;
    }
    Out->Kind = BOXASM_TOKEN_FLOAT;
    return 0;
}

static int BoxAsmLexNumber(BoxAsmLexer *Lexer, BoxAsmToken *Out, const char **Why)
{
    uint64_t Base = 10;
    uint64_t Value = 0;
    size_t Digits = 0;

    if (BoxAsmPeek(Lexer, 0) == '0' &&
        (BoxAsmPeek(Lexer, 1) == 'x' || BoxAsmPeek(Lexer, 1) == 'X')) {
        Base = 16;
        Lexer->Position += 2;
    } else if (BoxAsmPeek(Lexer, 0) == '0' &&
               (BoxAsmPeek(Lexer, 1) == 'b' || BoxAsmPeek(Lexer, 1) == 'B')) {
        Base = 2;
        Lexer->Position += 2;
    } else {
        size_t End = BoxAsmSkipDigits(Lexer, Lexer->Position);

        if (End < Lexer->Size && Lexer->Source[End] == '.')
            return BoxAsmLexFloat(Lexer, Out, Why);
    }
    while (Lexer->Position < Lexer->Size) {
        char Ch = Lexer->Source[Lexer->Position];
        uint64_t Digit;

        if (Ch == '_') {
            Lexer->Position++;
            continue;
        }
        if (!BoxAsmIdentifierPart(Ch))
            break;
        Digit = (uint64_t)BoxAsmDigitValue(Ch);
        if (Digit >= Base) {
            *Why = "malformed number";
            return -1;
        }
        if (Value > (UINT64_MAX - Digit) / Base) {
            *Why = "number does not fit in 64 bits";
            return -1;
        }
        Value = Value * Base + Digit;
        Digits++;
        Lexer->Position++;
    }
    if (Digits == 0) {
        *Why = "malformed number";
        return -1;
    }
    Out->Kind = BOXASM_TOKEN_NUMBER;
    Out->Number = Value;
    return 0;
}

/* One character of a quoted string, an escape resolved; *Length is how many
 * source bytes it took. */
static int BoxAsmStringChar(const char *At, size_t Left, uint8_t *Byte, size_t *Length)
{
    if (Left == 0)
        return -1;
    if (At[0] != '\\') {
        *Byte = (uint8_t)At[0];
        *Length = 1;
        return 0;
    }
    if (Left < 2)
        return -1;
    *Length = 2;
    switch (At[1]) {
    case 'n':  *Byte = '\n'; return 0;
    case 't':  *Byte = '\t'; return 0;
    case 'r':  *Byte = '\r'; return 0;
    case '0':  *Byte = '\0'; return 0;
    case '\\': *Byte = '\\'; return 0;
    case '"':  *Byte = '"';  return 0;
    case '\'': *Byte = '\''; return 0;
    case 'x': {
        int High = Left >= 4 ? BoxAsmDigitValue(At[2]) : 99;
        int Low = Left >= 4 ? BoxAsmDigitValue(At[3]) : 99;
        if (High > 15 || Low > 15)
            return -1;
        *Byte = (uint8_t)(High * 16 + Low);
        *Length = 4;
        return 0;
    }
    default:
        return -1;
    }
}

static int BoxAsmLexString(BoxAsmLexer *Lexer, BoxAsmToken *Out, const char **Why)
{
    char Quote = Lexer->Source[Lexer->Position++];

    for (;;) {
        uint8_t Byte;
        size_t Length;
        char Ch = BoxAsmPeek(Lexer, 0);

        if (Lexer->Position >= Lexer->Size || Ch == '\n') {
            *Why = "string is not closed on its line";
            return -1;
        }
        if (Ch == Quote) {
            Lexer->Position++;
            break;
        }
        if (BoxAsmStringChar(Lexer->Source + Lexer->Position,
                             Lexer->Size - Lexer->Position, &Byte, &Length) != 0) {
            *Why = "unknown escape in string (\\n \\t \\r \\0 \\\\ \\\" \\' \\xHH)";
            return -1;
        }
        Lexer->Position += Length;
    }
    Out->Kind = BOXASM_TOKEN_STRING;
    return 0;
}

static int BoxAsmLexPunct(BoxAsmLexer *Lexer, BoxAsmToken *Out, const char **Why)
{
    static const char Singles[] = ",:[]()+-*/%&|^~$";
    char Ch = BoxAsmPeek(Lexer, 0);
    char Next = BoxAsmPeek(Lexer, 1);

    Out->Kind = BOXASM_TOKEN_PUNCT;
    if ((Ch == '<' && Next == '<') || (Ch == '>' && Next == '>')) {
        Out->Punct = Ch == '<' ? BOXASM_PUNCT_SHIFT_LEFT : BOXASM_PUNCT_SHIFT_RIGHT;
        Lexer->Position += 2;
        return 0;
    }
    if (Ch == '$' && Next == '$') {
        Out->Punct = BOXASM_PUNCT_SECTION;
        Lexer->Position += 2;
        return 0;
    }
    if (Ch != '\0' && strchr(Singles, Ch)) {
        Out->Punct = (uint8_t)Ch;
        Lexer->Position++;
        return 0;
    }
    *Why = "this character does not belong in BoxASM source";
    return -1;
}

int BoxAsmLexNext(BoxAsmLexer *Lexer, BoxAsmToken *Out, const char **Why)
{
    size_t Start;
    char Ch;
    int Rc;

    for (;;) {
        Ch = BoxAsmPeek(Lexer, 0);
        if (Lexer->Position < Lexer->Size && (Ch == ' ' || Ch == '\t' || Ch == '\r')) {
            Lexer->Position++;
            continue;
        }
        if (Lexer->Position < Lexer->Size && Ch == ';') {
            while (Lexer->Position < Lexer->Size && Lexer->Source[Lexer->Position] != '\n')
                Lexer->Position++;
            continue;
        }
        break;
    }
    memset(Out, 0, sizeof(*Out));
    Start = Lexer->Position;
    Out->Text = Lexer->Source + Start;
    Out->Where.Line = Lexer->Line;
    Out->Where.Column = (uint32_t)(Lexer->Source + Start - Lexer->LineStart) + 1u;
    Out->Where.LineStart = Lexer->LineStart;
    if (Lexer->Position >= Lexer->Size) {
        Out->Kind = BOXASM_TOKEN_END;
        return 0;
    }
    if (Ch == '\n') {
        Lexer->Position++;
        Lexer->Line++;
        Lexer->LineStart = Lexer->Source + Lexer->Position;
        Out->Kind = BOXASM_TOKEN_NEWLINE;
        Out->Length = 1;
        return 0;
    }
    if (Ch == '\0') {
        *Why = "the source contains a NUL byte";
        return -1;
    }
    if (BoxAsmIdentifierStart(Ch)) {
        while (Lexer->Position < Lexer->Size &&
               BoxAsmIdentifierPart(Lexer->Source[Lexer->Position]))
            Lexer->Position++;
        Out->Kind = BOXASM_TOKEN_IDENTIFIER;
        Rc = 0;
    } else if (BoxAsmIsDigit(Ch)) {
        Rc = BoxAsmLexNumber(Lexer, Out, Why);
    } else if (Ch == '"' || Ch == '\'') {
        Rc = BoxAsmLexString(Lexer, Out, Why);
    } else {
        Rc = BoxAsmLexPunct(Lexer, Out, Why);
    }
    Out->Length = Lexer->Position - Start;
    return Rc;
}

int BoxAsmLexStringBytes(const BoxAsmToken *Token, BoxAsmBuffer *Out)
{
    size_t At = 1;

    while (At + 1u < Token->Length) {
        uint8_t Byte;
        size_t Length;

        if (BoxAsmStringChar(Token->Text + At, Token->Length - 1u - At, &Byte, &Length) != 0)
            return -ERR_INVALID_ARGUMENT;
        if (BoxAsmBufferAppend(Out, &Byte, 1u) != 0)
            return -ERR_NO_MEMORY;
        At += Length;
    }
    return 0;
}

int BoxAsmTokenIs(const BoxAsmToken *Token, const char *Word)
{
    size_t Length = strlen(Word);
    size_t Index;

    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER || Token->Length != Length)
        return 0;
    for (Index = 0; Index < Length; Index++) {
        char Ch = Token->Text[Index];
        if (Ch >= 'A' && Ch <= 'Z')
            Ch = (char)(Ch - 'A' + 'a');
        if (Ch != Word[Index])
            return 0;
    }
    return 1;
}

int BoxAsmTokenIsPunct(const BoxAsmToken *Token, uint16_t Punct)
{
    return Token->Kind == BOXASM_TOKEN_PUNCT && Token->Punct == Punct;
}
