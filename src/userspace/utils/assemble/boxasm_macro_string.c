/* %strlen, %substr and %strcat: strings taken apart and put together by the
 * preprocessor, their macros expanded first, as NASM's are. A string is read
 * as the assembler reads it — its escapes are its bytes — and one made is
 * written so that it reads back to the same bytes. */

#include "boxasm_macro_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* The name a string directive defines, and its expanded rest. */
static int BoxAsmPpStringStart(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest,
                               const char *What, BoxAsmPpToken *Name, BoxAsmBuffer *Text,
                               BoxAsmPpLine *Working, const char **After)
{
    int Rc;

    Rest = BoxAsmPpSpaces(Rest);
    if ((Rc = BoxAsmPpName(Pp, Line, Rest, What, Name)) != 0)
        return Rc;
    return BoxAsmPpExpandRest(Pp, Line, Name->Text + Name->Length, Text, Working, After);
}

/* The bytes of Text, which is one string, appended to Bytes. */
static int BoxAsmPpStringBytes(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                               BoxAsmBuffer *Bytes)
{
    BoxAsmPpToken Found;
    BoxAsmToken Token;
    size_t At = 0;

    if (!BoxAsmPpOneToken(Text, BOXASM_PP_STRING))
        return BoxAsmPpFail(Pp, Line, BoxAsmPpSpaces(Text), "expected a string in quotes", NULL);
    do BoxAsmPpNext(Text, &At, &Found); while (Found.Kind == BOXASM_PP_SPACE);
    memset(&Token, 0, sizeof(Token));
    Token.Kind = BOXASM_TOKEN_STRING;
    Token.Text = Found.Text;
    Token.Length = Found.Length;
    if (BoxAsmLexStringBytes(&Token, Bytes) != 0)
        return BoxAsmPpFail(Pp, Line, Found.Text, "is not a string the assembler reads", NULL);
    return 0;
}

/* Bytes written as a string in quotes, with an escape for each byte that is
 * no printable character or is the quote or the backslash. */
static char *BoxAsmPpQuoted(const uint8_t *Bytes, size_t Size)
{
    static const char Hex[] = "0123456789abcdef";
    BoxAsmBuffer Out = { 0 };
    size_t Index;
    int Rc = BoxAsmBufferAppend(&Out, "\"", 1);

    for (Index = 0; Index < Size && Rc == 0; Index++) {
        uint8_t Byte = Bytes[Index];
        char Escape[4] = { '\\', 'x', Hex[Byte >> 4], Hex[Byte & 15u] };

        if (Byte == '"' || Byte == '\\') {
            Escape[1] = (char)Byte;
            Rc = BoxAsmBufferAppend(&Out, Escape, 2);
        } else if (Byte >= 0x20 && Byte < 0x7F) {
            Rc = BoxAsmBufferAppend(&Out, &Byte, 1);
        } else {
            Rc = BoxAsmBufferAppend(&Out, Escape, 4);
        }
    }
    if (Rc == 0)
        Rc = BoxAsmBufferAppend(&Out, "\"", 2);
    if (Rc != 0) {
        BoxAsmBufferRelease(&Out);
        return NULL;
    }
    return (char *)Out.Data;
}

static int BoxAsmPpDefineString(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const BoxAsmPpToken *Name,
                                const uint8_t *Bytes, size_t Size)
{
    char *Body = BoxAsmPpQuoted(Bytes, Size);

    if (!Body)
        return -ERR_NO_MEMORY;
    return BoxAsmPpDefine(Pp, Line, Name, -1, NULL, Body, 0);
}

/* %strlen NAME string: NAME stands for how many bytes the string holds. */
int BoxAsmPpStrlen(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmBuffer Text = { 0 };
    BoxAsmBuffer Bytes = { 0 };
    BoxAsmPpLine Working;
    BoxAsmPpToken Name;
    const char *After;
    char Digits[24];
    size_t Count = 0;
    size_t Size;
    char *Body;
    int Rc;

    Rc = BoxAsmPpStringStart(Pp, Line, Rest, "expected the name %strlen gives", &Name, &Text,
                             &Working, &After);
    if (Rc == 0)
        Rc = BoxAsmPpStringBytes(Pp, &Working, After, &Bytes);
    Size = (size_t)Bytes.Size;
    BoxAsmBufferRelease(&Text);
    BoxAsmBufferRelease(&Bytes);
    if (Rc != 0)
        return Rc;
    do {
        Digits[sizeof(Digits) - 1u - Count++] = (char)('0' + Size % 10u);
        Size /= 10u;
    } while (Size != 0);
    if (!(Body = BoxAsmCopyText(Digits + sizeof(Digits) - Count, Count)))
        return -ERR_NO_MEMORY;
    return BoxAsmPpDefine(Pp, Line, &Name, -1, NULL, Body, 0);
}

/* %substr NAME string, start[, length]: the bytes from the start'th, the
 * first being 1; one byte when no length is written, and a length below 0
 * stops that many bytes short of the end and one, so -1 takes the rest. */
int BoxAsmPpSubstr(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmBuffer Text = { 0 };
    BoxAsmBuffer Bytes = { 0 };
    BoxAsmPpLine Working;
    BoxAsmPpToken Name;
    const char *After;
    char **Parts = NULL;
    uint32_t Count = 0;
    int64_t Start = 0;
    int64_t Length = 1;
    int64_t Size;
    int Rc;

    Rc = BoxAsmPpStringStart(Pp, Line, Rest, "expected the name %substr gives", &Name, &Text,
                             &Working, &After);
    if (Rc == 0)
        Rc = BoxAsmPpSplit(After, 3, 0, &Parts, &Count);
    if (Rc == 0 && (Count < 2 || Count > 3))
        Rc = BoxAsmPpFail(Pp, &Working, After, "takes a string, where it starts and how long it is, "
                                               "with commas between", NULL);
    if (Rc == 0)
        Rc = BoxAsmPpStringBytes(Pp, &Working, Parts[0], &Bytes);
    if (Rc == 0)
        Rc = BoxAsmPpEvaluate(Pp, &Working, Parts[1], &Start);
    if (Rc == 0 && Count == 3)
        Rc = BoxAsmPpEvaluate(Pp, &Working, Parts[2], &Length);
    if (Rc == 0 && Start < 1)
        Rc = BoxAsmPpFail(Pp, &Working, After, "a string's first byte is 1", NULL);
    BoxAsmPpFreeList(Parts, Count);
    if (Rc == 0) {
        Size = (int64_t)Bytes.Size;
        if (Start > Size)
            Start = Size + 1;
        if (Length < 0)
            Length = Size - Start + 1 + Length + 1;
        if (Length < 0)
            Length = 0;
        if (Length > Size - Start + 1)
            Length = Size - Start + 1;
        Rc = BoxAsmPpDefineString(Pp, Line, &Name, Length != 0 ? Bytes.Data + Start - 1 : NULL,
                                  (size_t)Length);
    }
    BoxAsmBufferRelease(&Text);
    BoxAsmBufferRelease(&Bytes);
    return Rc;
}

/* %strcat NAME string, string...: the strings one after another. */
int BoxAsmPpStrcat(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmBuffer Text = { 0 };
    BoxAsmBuffer Bytes = { 0 };
    BoxAsmPpLine Working;
    BoxAsmPpToken Name;
    const char *After;
    char **Parts = NULL;
    uint32_t Count = 0;
    uint32_t Index;
    int Rc;

    Rc = BoxAsmPpStringStart(Pp, Line, Rest, "expected the name %strcat gives", &Name, &Text,
                             &Working, &After);
    if (Rc == 0)
        Rc = BoxAsmPpSplit(After, 0, 0, &Parts, &Count);
    if (Rc == 0 && Count == 0)
        Rc = BoxAsmPpFail(Pp, &Working, After, "expected the strings to join, with commas between", NULL);
    for (Index = 0; Index < Count && Rc == 0; Index++)
        Rc = BoxAsmPpStringBytes(Pp, &Working, Parts[Index], &Bytes);
    BoxAsmPpFreeList(Parts, Count);
    if (Rc == 0)
        Rc = BoxAsmPpDefineString(Pp, Line, &Name, Bytes.Data, (size_t)Bytes.Size);
    BoxAsmBufferRelease(&Text);
    BoxAsmBufferRelease(&Bytes);
    return Rc;
}
