/* %include: another file read where the line stands, as NASM reads one, but
 * found as BoxOS finds a file — by its name and the tags it wears, never by a
 * path.
 *
 *     %include "math.asm"                       the one of that name here
 *     %include "math.asm" fast                  ... that wears fast as well
 *     %include "print.asm" mylib everywhere     past the context: the one
 *                                               of that name wearing mylib
 *
 * Here is the home of the file the line is written in: for the source and
 * what is found beside it, the context the work began in. The tags after the
 * name are the meaning the file must wear, checked whenever they are written.
 * A file reached `everywhere` looks for its own %includes among the tags it
 * was found by, so a library's neighbours are its own wherever it is used.
 * One file, or a refusal that says what is there and what to write. */

#include "boxasm_macro_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* What the line asks for: the name, the meaning as a comma list, and whether
 * it looks past the home. */
typedef struct {
    char       *Name;
    BoxAsmBuffer Tags;
    uint8_t     Everywhere;
    const char *At;            /* where the name stands, for a refusal */
} BoxAsmPpAsk;

static void BoxAsmPpAskRelease(BoxAsmPpAsk *Ask)
{
    free(Ask->Name);
    BoxAsmBufferRelease(&Ask->Tags);
}

static int BoxAsmPpWordEnds(char C)
{
    return C == '\0' || C == ' ' || C == '\t' || C == ',' || C == ';';
}

static int BoxAsmPpNoQuotes(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *At,
                            const char *Directive)
{
    BoxAsmMessage Message;

    BoxAsmPpFailBegin(Pp, Line, At, &Message);
    BoxAsmMessageText(&Message, "expected the file's name in quotes: ");
    BoxAsmMessageText(&Message, Directive);
    BoxAsmMessageText(&Message, Directive[0] == '%' ? " \"math.asm\"" : " \"data.bin\"");
    return BoxAsmPpFailEnd(Pp, Line, At, &Message);
}

/* NASM's <name> looks along the include paths, and files here have none: the
 * line to write instead is said, the same name in quotes. */
static int BoxAsmPpAngle(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *At,
                         const char *Directive)
{
    const char *Close = strchr(At + 1, '>');
    BoxAsmMessage Message;

    if (!Close || Close == At + 1 || memchr(At + 1, '"', (size_t)(Close - At - 1)))
        return BoxAsmPpNoQuotes(Pp, Line, At, Directive);
    BoxAsmPpFailBegin(Pp, Line, At, &Message);
    BoxAsmMessageText(&Message, "files are found by name and tags, not along a path: write ");
    BoxAsmMessageText(&Message, Directive);
    BoxAsmMessageText(&Message, " \"");
    BoxAsmMessageSpan(&Message, At + 1, (size_t)(Close - At - 1));
    BoxAsmMessageText(&Message, "\", and after it a tag it wears, or everywhere, to say which");
    return BoxAsmPpFailEnd(Pp, Line, At, &Message);
}

/* The name in quotes, then words up to the end or a comment — or, for
 * incbin, a comma — `everywhere`, or a tag. */
static int BoxAsmPpAskOf(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest, BoxAsmPpAsk *Ask,
                         const char *Directive)
{
    int Incbin = Directive[0] != '%';
    BoxAsmBuffer Bytes = { 0 };
    BoxAsmLexer Lexer;
    BoxAsmToken Token;
    const char *Why = NULL;
    const char *At = BoxAsmPpSpaces(Rest);
    int Rc;

    Ask->At = At;
    if (*At == '<')
        return BoxAsmPpAngle(Pp, Line, At, Directive);
    if (*At != '"' && *At != '\'' && *At != '`')
        return BoxAsmPpNoQuotes(Pp, Line, At, Directive);
    BoxAsmLexBegin(&Lexer, At, strlen(At));
    if (BoxAsmLexNext(&Lexer, &Token, &Why) != 0)
        return BoxAsmPpFail(Pp, Line, At, Why, NULL);
    if ((Rc = BoxAsmLexStringBytes(&Token, &Bytes)) != 0)
        return Rc;
    if (Bytes.Size == 0 || memchr(Bytes.Data, '\0', (size_t)Bytes.Size)) {
        BoxAsmBufferRelease(&Bytes);
        return BoxAsmPpFail(Pp, Line, At, Bytes.Size == 0 ? "the file's name is empty"
                                                          : "a file's name holds no NUL byte", NULL);
    }
    Ask->Name = BoxAsmCopyText((const char *)Bytes.Data, (size_t)Bytes.Size);
    BoxAsmBufferRelease(&Bytes);
    if (!Ask->Name)
        return -ERR_NO_MEMORY;
    for (At += Token.Length;;) {
        const char *Word;

        while (*At == ' ' || *At == '\t' || (*At == ',' && !Incbin))
            At++;
        if (*At == '\0' || *At == ';' || *At == ',')
            return 0;
        for (Word = At; !BoxAsmPpWordEnds(*At); At++) {
            if (*At == '"' || *At == '\'' || *At == '`')
                return BoxAsmPpFail(Pp, Line, At, "a tag is written as a word, without quotes", NULL);
        }
        if ((size_t)(At - Word) == 10u && memcmp(Word, "everywhere", 10u) == 0) {
            Ask->Everywhere = 1;
            continue;
        }
        if ((Ask->Tags.Size != 0 && BoxAsmBufferAppend(&Ask->Tags, ",", 1) != 0) ||
            BoxAsmBufferAppend(&Ask->Tags, Word, (uint64_t)(At - Word)) != 0)
            return -ERR_NO_MEMORY;
    }
}

/* The refusal of a lookup: its summary at the line, the line, the calls and
 * %includes it was reached through, then its details. */
static int BoxAsmPpNotFound(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const BoxAsmPpAsk *Ask, int Rc,
                            const BoxAsmBuffer *Summary, const BoxAsmBuffer *Details)
{
    BoxAsmMessage Message;

    BoxAsmPpFailBegin(Pp, Line, Ask->At, &Message);
    if (Summary->Size != 0) {
        BoxAsmMessageSpan(&Message, (const char *)Summary->Data, (size_t)Summary->Size);
    } else {
        BoxAsmMessageText(&Message, "the volume would not look for ");
        BoxAsmMessageText(&Message, Ask->Name);
        BoxAsmMessageText(&Message, ": ");
        BoxAsmMessageText(&Message, BoxAsmErrorText(Rc));
    }
    Rc = BoxAsmPpFailEnd(Pp, Line, Ask->At, &Message);
    if (Details->Size != 0) {
        const char *Text = (const char *)Details->Data;
        size_t Length = (size_t)Details->Size;

        if (*Text == '\n') {
            Text++;
            Length--;
        }
        BoxAsmMessageText(&Message, "\n");
        BoxAsmMessageSpan(&Message, Text, Length);
    }
    return Rc;
}

/* A new file of the expansion, its name and its home: its index. */
static int BoxAsmPpAddFile(BoxAsmPp *Pp, const char *Name, const char *Home, uint32_t *Index)
{
    BoxAsmExpansion *Out = Pp->Out;
    char *Copy;

    if (Out->FileCount >= 0xFFFFFFFFu || Out->Names.Size >= 0xFFFFFFFFu ||
        BoxAsmGrowArray((void **)&Out->Files, &Out->FileCapacity, Out->FileCount, sizeof(uint32_t)) != 0 ||
        BoxAsmGrowArray((void **)&Pp->Homes, &Pp->HomeCapacity, Out->FileCount, sizeof(char *)) != 0 ||
        !(Copy = BoxAsmCopyText(Home, strlen(Home))))
        return -ERR_NO_MEMORY;
    if (BoxAsmBufferAppend(&Out->Names, Name, strlen(Name) + 1u) != 0) {
        free(Copy);
        return -ERR_NO_MEMORY;
    }
    Out->Files[Out->FileCount] = (uint32_t)(Out->Names.Size - strlen(Name) - 1u);
    Pp->Homes[Out->FileCount] = Copy;
    *Index = (uint32_t)Out->FileCount++;
    return 0;
}

/* The file read as a body of its own, outside any macro call and %rep: its
 * %1 is no parameter, its %exitrep ends no %rep of the line's. */
static int BoxAsmPpReadIn(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const BoxAsmPpAsk *Ask,
                          uint64_t Id, const char *Home)
{
    BoxAsmCall *Call = Pp->Call;
    uint32_t OuterReps = Pp->OuterReps;
    BoxAsmPpLine *Lines = NULL;
    size_t Count = 0;
    uint32_t File;
    uint32_t Frame;
    char *Text = NULL;
    size_t Size = 0;
    int Rc;

    if ((Rc = Pp->Files->Read(Pp->Files, Id, BOXASM_MACRO_OUTPUT, &Text, &Size)) < 0) {
        BoxAsmMessage Message;

        BoxAsmPpFailBegin(Pp, Line, Ask->At, &Message);
        BoxAsmMessageText(&Message, Ask->Name);
        BoxAsmMessageText(&Message, Rc == -ERR_OUT_OF_RANGE ? " is larger than 16 MiB: it is not a source"
                                                            : " could not be read: ");
        if (Rc != -ERR_OUT_OF_RANGE)
            BoxAsmMessageText(&Message, BoxAsmErrorText(Rc));
        return BoxAsmPpFailEnd(Pp, Line, Ask->At, &Message);
    }
    if (memchr(Text, '\0', Size)) {             /* a file of bytes, not a source */
        const char *Nul = (const char *)memchr(Text, '\0', Size);
        const char *At;
        uint32_t Number = 1;
        BoxAsmMessage Message;

        for (At = Text; At < Nul; At++)
            Number += *At == '\n';
        free(Text);
        BoxAsmPpFailBegin(Pp, Line, Ask->At, &Message);
        BoxAsmMessageText(&Message, Ask->Name);
        BoxAsmMessageText(&Message, " holds a NUL byte in its line ");
        BoxAsmMessageDecimal(&Message, Number);
        BoxAsmMessageText(&Message, ": it is not a source");
        return BoxAsmPpFailEnd(Pp, Line, Ask->At, &Message);
    }
    if ((Rc = BoxAsmPpAddFile(Pp, Ask->Name, Home, &File)) == 0 &&
        (Rc = BoxAsmPpFrame(Pp, BOXASM_FRAME_FILE, Ask->Name, Line, &Frame)) == 0 &&
        (Rc = BoxAsmPpCut(Pp, Text, Size, File, Frame, &Lines, &Count)) == 0 &&
        (Rc = BoxAsmGrowArray((void **)&Pp->Open, &Pp->OpenCapacity, Pp->OpenCount,
                              sizeof(uint64_t))) == 0) {
        Pp->Open[Pp->OpenCount++] = Id;
        Pp->Call = NULL;
        Pp->OuterReps = 0;
        Rc = BoxAsmPpLines(Pp, Lines, Count);
        Pp->Call = Call;
        Pp->OuterReps = OuterReps;
        Pp->OpenCount--;
    }
    free(Lines);
    free(Text);
    return Rc;
}

int BoxAsmPpInclude(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmBuffer Expanded = { 0 };
    BoxAsmBuffer Summary = { 0 };
    BoxAsmBuffer Details = { 0 };
    BoxAsmPpLine Working;
    BoxAsmLookup Lookup;
    BoxAsmPpAsk Ask;
    const char *After;
    uint64_t Id = 0;
    size_t Index;
    int Rc;

    memset(&Ask, 0, sizeof(Ask));
    if ((Rc = BoxAsmPpExpandRest(Pp, Line, Rest, &Expanded, &Working, &After)) != 0 ||
        (Rc = BoxAsmPpAskOf(Pp, &Working, After, &Ask, "%include")) != 0)
        goto Done;
    if (BoxAsmBufferAppend(&Ask.Tags, "", 1) != 0) {     /* the list as text */
        Rc = -ERR_NO_MEMORY;
        goto Done;
    }
    if (!Pp->Files) {
        Rc = BoxAsmPpFail(Pp, &Working, Ask.At,
                          "this source is no file of a volume, and finds no other file to include", NULL);
        goto Done;
    }
    Lookup.Name = Ask.Name;
    Lookup.Tags = (const char *)Ask.Tags.Data;
    Lookup.Home = Pp->Homes[Working.File];
    Lookup.Everywhere = Ask.Everywhere;
    Lookup.Directive = "%include";
    if ((Rc = BoxAsmLocate(Pp->Files, &Lookup, &Id, &Summary, &Details)) != 0) {
        if (Rc != -ERR_NO_MEMORY)
            Rc = BoxAsmPpNotFound(Pp, &Working, &Ask, Rc, &Summary, &Details);
        goto Done;
    }
    for (Index = 0; Index < Pp->OpenCount; Index++) {
        if (Pp->Open[Index] != 0 && Pp->Open[Index] == Id) {
            Rc = BoxAsmPpFail(Pp, &Working, Ask.At,
                              "is already being read: the files include one another in a circle",
                              Ask.Name);
            goto Done;
        }
    }
    if (BoxAsmTooDeep(Pp->Asm, Pp->Asm->StackBase)) {
        Rc = BoxAsmPpFail(Pp, &Working, Ask.At, "is included inside files and macros too deep",
                          Ask.Name);
        goto Done;
    }
    Rc = BoxAsmPpReadIn(Pp, &Working, &Ask, Id, Ask.Everywhere ? Lookup.Tags : Lookup.Home);
Done:
    BoxAsmBufferRelease(&Expanded);
    BoxAsmBufferRelease(&Summary);
    BoxAsmBufferRelease(&Details);
    BoxAsmPpAskRelease(&Ask);
    return Rc;
}

/* incbin "name" tags... [everywhere][, skip[, count]]: the file found as
 * %include finds one, its bytes kept for the line; how many of them the
 * program takes is the assembler's to read from the rest. */
int BoxAsmPpIncbin(BoxAsmPp *Pp, const BoxAsmPpLine *Line)
{
    BoxAsmExpansion *Out = Pp->Out;
    BoxAsmBuffer Summary = { 0 };
    BoxAsmBuffer Details = { 0 };
    BoxAsmLookup Lookup;
    BoxAsmPpToken Token;
    BoxAsmPpAsk Ask;
    size_t At = 0;
    uint64_t Id = 0;
    char *Bytes = NULL;
    size_t Size = 0;
    int Rc;

    do BoxAsmPpNext(Line->Text, &At, &Token);
    while (Token.Kind != BOXASM_PP_END && !(Token.Kind == BOXASM_PP_WORD && BoxAsmPpWordIs(&Token, "incbin")));
    if (Token.Kind == BOXASM_PP_END)
        return 0;
    memset(&Ask, 0, sizeof(Ask));
    if ((Rc = BoxAsmPpAskOf(Pp, Line, Token.Text + Token.Length, &Ask, "incbin")) != 0)
        goto Done;
    if (BoxAsmBufferAppend(&Ask.Tags, "", 1) != 0) {
        Rc = -ERR_NO_MEMORY;
        goto Done;
    }
    if (!Pp->Files) {
        Rc = BoxAsmPpFail(Pp, Line, Ask.At,
                          "this source is no file of a volume, and finds no other file to read", NULL);
        goto Done;
    }
    Lookup.Name = Ask.Name;
    Lookup.Tags = (const char *)Ask.Tags.Data;
    Lookup.Home = Pp->Homes[Line->File];
    Lookup.Everywhere = Ask.Everywhere;
    Lookup.Directive = "incbin";
    if ((Rc = BoxAsmLocate(Pp->Files, &Lookup, &Id, &Summary, &Details)) != 0) {
        if (Rc != -ERR_NO_MEMORY)
            Rc = BoxAsmPpNotFound(Pp, Line, &Ask, Rc, &Summary, &Details);
        goto Done;
    }
    /* The program's bytes are counted in 32 bits: a file past 4 GiB fits none. */
    if ((Rc = Pp->Files->Read(Pp->Files, Id, UINT32_MAX, &Bytes, &Size)) < 0) {
        BoxAsmMessage Message;

        BoxAsmPpFailBegin(Pp, Line, Ask.At, &Message);
        BoxAsmMessageText(&Message, Ask.Name);
        BoxAsmMessageText(&Message, Rc == -ERR_OUT_OF_RANGE ? " is larger than 4 GiB, more than a program holds"
                                                            : " could not be read: ");
        if (Rc != -ERR_OUT_OF_RANGE)
            BoxAsmMessageText(&Message, BoxAsmErrorText(Rc));
        Rc = BoxAsmPpFailEnd(Pp, Line, Ask.At, &Message);
        goto Done;
    }
    Rc = 0;
    if (BoxAsmGrowArray((void **)&Out->Incbins, &Out->IncbinCapacity, Out->IncbinCount,
                        sizeof(BoxAsmIncbin)) != 0 ||
        BoxAsmBufferAppend(&Out->Binary, Bytes, Size) != 0) {
        Rc = -ERR_NO_MEMORY;
        goto Done;
    }
    Out->Incbins[Out->IncbinCount++] = (BoxAsmIncbin){
        (uint32_t)(Out->LineCount + 1u), Out->Binary.Size - Size, Size
    };
Done:
    free(Bytes);
    BoxAsmBufferRelease(&Summary);
    BoxAsmBufferRelease(&Details);
    BoxAsmPpAskRelease(&Ask);
    return Rc;
}
