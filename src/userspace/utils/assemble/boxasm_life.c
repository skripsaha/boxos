/* life: how the program lives, as box_reader.h's part `life` says it —
 *
 *     life stays            it lives on by itself: whoever starts it does not wait
 *     life serves rise      it serves others, and the system raises it when it falls
 *
 * The words are the program's, not the assembler's: task, stays and serves,
 * one of them at most, and rise at most once. They go into the program's
 * .boxos.life a word a line after "life 1", and a word the one reader of life
 * would pass over is refused here, where it was written, rather than lost on
 * the way to whoever starts the program. */

#include "boxasm_asm.h"

#include "box/error.h"
#include "box/string.h"

static int BoxAsmIsBlank(char Character)
{
    return Character == ' ' || Character == '\t' || Character == '\r';
}

static int BoxAsmLifeKind(const char *Word, size_t Length)
{
    static const char *const Kinds[] = { "task", "stays", "serves" };
    size_t Index;

    for (Index = 0; Index < sizeof(Kinds) / sizeof(Kinds[0]); Index++)
        if (Length == strlen(Kinds[Index]) && memcmp(Word, Kinds[Index], Length) == 0)
            return 1;
    return 0;
}

static int BoxAsmLifeIsRise(const char *Word, size_t Length)
{
    return Length == 4 && memcmp(Word, "rise", 4) == 0;
}

/* Whether the life written so far already says a word of this sort: a way of
 * living (Kind), or rise. */
static int BoxAsmLifeSays(const BoxAsmBuffer *Life, int Kind)
{
    size_t At = 0;

    while (At < Life->Size) {
        const char *Line = (const char *)Life->Data + At;
        size_t Length = 0;

        while (At + Length < Life->Size && Line[Length] != '\n')
            Length++;
        if (Kind ? BoxAsmLifeKind(Line, Length) : BoxAsmLifeIsRise(Line, Length))
            return 1;
        At += Length + 1;
    }
    return 0;
}

static int BoxAsmLifeFail(BoxAsmAssembler *Asm, const BoxAsmToken *Word, const char *Said, size_t Length,
                          const char *Why)
{
    BoxAsmMessage Message;

    BoxAsmFailBegin(Asm, &Word->Where, &Message);
    if (Said) {
        BoxAsmMessageSpan(&Message, Said, Length);
        BoxAsmMessageText(&Message, " ");
    }
    BoxAsmMessageText(&Message, Why);
    return BoxAsmFailEnd(Asm, &Word->Where, &Message);
}

/* One word of the program's life, written after what it says already. */
static int BoxAsmLifeWord(BoxAsmAssembler *Asm, const BoxAsmToken *Word, const char *Said, size_t Length)
{
    int Kind = BoxAsmLifeKind(Said, Length);

    if (!Kind && !BoxAsmLifeIsRise(Said, Length))
        return BoxAsmLifeFail(Asm, Word, Said, Length,
                              "is no word of how a program lives: task, stays, serves or rise");
    if (BoxAsmLifeSays(&Asm->Life, Kind))
        return BoxAsmLifeFail(Asm, Word, NULL, 0,
                              Kind ? "a program lives one way: it says task, stays or serves once"
                                   : "rise is said once");
    if (Asm->Life.Size == 0 && BoxAsmBufferAppend(&Asm->Life, "life 1\n", 7u) != 0)
        return -ERR_NO_MEMORY;
    if (BoxAsmBufferAppend(&Asm->Life, Said, Length) != 0 || BoxAsmBufferAppend(&Asm->Life, "\n", 1u) != 0)
        return -ERR_NO_MEMORY;
    return 0;
}

/* life WORD... */
int BoxAsmParseProgramLife(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                           const BoxAsmToken *Word)
{
    const char *At = Word->Text + Word->Length;
    const char *Limit = Lexer->Source + Lexer->Size;
    int Words = 0;
    int Rc;

    if (Asm->Struc.Open || Asm->Struc.Instance)
        return BoxAsmFail(Asm, &Word->Where,
                          "life stands outside struc and istruc: it is the program's, not a layout's", NULL);
    for (;;) {
        const char *Start;

        while (At < Limit && BoxAsmIsBlank(*At))
            At++;
        if (At == Limit || *At == '\n' || *At == ';')
            break;
        Start = At;
        while (At < Limit && !BoxAsmIsBlank(*At) && *At != '\n' && *At != ';')
            At++;
        if ((Rc = BoxAsmLifeWord(Asm, Word, Start, (size_t)(At - Start))) != 0)
            return Rc;
        Words++;
    }
    if (Words == 0)
        return BoxAsmFail(Asm, &Word->Where, "life says how the program lives: task, stays, serves or rise",
                          NULL);
    Lexer->Position = (size_t)(At - Lexer->Source);
    return BoxAsmNextToken(Asm, Lexer, Token);
}
