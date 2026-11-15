/* setting: a line of the settings a program declares, as box/settings.h and
 * src/include/settings_reader.h define them — a tree, so the line names the
 * path of what holds it first, by the names already declared: `setting
 * cursor shape one-of block bar default bar "its shape"`. Its words are the
 * declaration's, not the assembler's — on-or-off, #FFB020, -3, » stay as
 * written — so the line is taken from the source whole, read by the one
 * reader of settings, and written into the program's declaration,
 * .boxos.settings: indented four spaces for each thing that holds it, its
 * words apart by single blanks, right under the last line of what holds it.
 * A line the reader would not read is refused here, where it was written,
 * rather than skipped by the program that has it. */

#include "boxasm_asm.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#include "settings_reader.h"

static int BoxAsmSettingFail(BoxAsmAssembler *Asm, const BoxAsmToken *Word, SettingSpan Name,
                             const char *Why)
{
    BoxAsmMessage Message;

    BoxAsmFailBegin(Asm, &Word->Where, &Message);
    if (Name.Length != 0) {
        BoxAsmMessageText(&Message, "the setting ");
        BoxAsmMessageSpan(&Message, (const char *)Name.At, (size_t)Name.Length);
        BoxAsmMessageText(&Message, " ");
    } else {
        BoxAsmMessageText(&Message, "this setting ");
    }
    BoxAsmMessageText(&Message, Why);
    return BoxAsmFailEnd(Asm, &Word->Where, &Message);
}

/* The rest of the statement after `setting`: to the end of its line, or to a
 * comment outside double quotes. */
static SettingSpan BoxAsmSettingRest(const BoxAsmLexer *Lexer, const BoxAsmToken *Word)
{
    const char *Start = Word->Text + Word->Length;
    const char *End = Start;
    const char *Limit = Lexer->Source + Lexer->Size;
    int Quoted = 0;

    while (End < Limit && *End != '\n' && (Quoted || *End != ';')) {
        if (*End == '"')
            Quoted = !Quoted;
        End++;
    }
    return (SettingSpan){ (const uint8_t *)Start, (uint64_t)(End - Start) };
}

/* The setting called `Name` inside the declared line `Parent` (0: the top)
 * in the declaration so far — a thing that holds others found while it holds
 * nothing yet, as it does until the lines that go under it are assembled:
 * the linker holds the whole tree to the reader. */
static bool BoxAsmSettingChild(const BoxAsmBuffer *Settings, uint32_t Parent, SettingSpan Name, Setting *Out)
{
    SettingsDeclared Declared;
    SettingsVerdict Verdict;

    if (SettingsDeclaredBegin(Settings->Data, Settings->Size, &Declared) != SETTINGS_OK)
        return false;
    while (SettingsDeclaredStep(&Declared, Out, &Verdict, false))
        if ((Verdict == SETTINGS_OK || Verdict == SETTING_HOLDS_NOTHING) && Out->Parent == Parent &&
            SettingSpanSame(Out->Name, Name))
            return true;
    return false;
}

/* The line's words from `From` on, each apart by one blank, quoted ones in
 * their quotes, indented four spaces for each of `Depth` things that hold
 * it, into `Line`. Text between quotes holds nothing below a blank: the
 * declaration is a part written as text, which holds no tab. */
static int BoxAsmSettingLine(BoxAsmAssembler *Asm, const BoxAsmToken *Word, SettingSpan Rest, uint64_t From,
                             uint32_t Depth, SettingSpan Name, BoxAsmBuffer *Line)
{
    SettingSpan Piece;
    SettingsVerdict Ignored;
    uint64_t At = From;
    bool Quoted;
    bool First = true;
    uint32_t Level;

    for (Level = 0; Level < Depth; Level++)
        if (BoxAsmBufferAppend(Line, "    ", 4u) != 0)
            return -ERR_NO_MEMORY;
    while (SettingWord(Rest, &At, &Piece, &Quoted, &Ignored)) {
        uint64_t Index;
        int Rc;

        for (Index = 0; Quoted && Index < Piece.Length; Index++)
            if (Piece.At[Index] < 0x20u)
                return BoxAsmSettingFail(Asm, Word, Name,
                                         "holds a tab between its quotes, which a declaration cannot hold");
        Rc = First ? 0 : BoxAsmBufferAppend(Line, " ", 1u);
        if (Rc == 0 && Quoted)
            Rc = BoxAsmBufferAppend(Line, "\"", 1u);
        if (Rc == 0)
            Rc = BoxAsmBufferAppend(Line, Piece.At, Piece.Length);
        if (Rc == 0 && Quoted)
            Rc = BoxAsmBufferAppend(Line, "\"", 1u);
        if (Rc != 0)
            return -ERR_NO_MEMORY;
        First = false;
    }
    return BoxAsmBufferAppend(Line, "\n", 1u) != 0 ? -ERR_NO_MEMORY : 0;
}

/* Where a line inside the declared line `Parent` goes: past the last line
 * that stands under it — at the end, for one at the top. */
static uint64_t BoxAsmSettingPlace(const BoxAsmBuffer *Settings, uint32_t Parent)
{
    SettingsDeclared Declared;
    Setting Setting;
    SettingsVerdict Verdict;
    uint64_t End = Settings->Size;
    uint32_t Depth = 0;
    bool Inside = false;

    if (Parent == 0 || SettingsDeclaredBegin(Settings->Data, Settings->Size, &Declared) != SETTINGS_OK)
        return End;
    while (SettingsDeclaredStep(&Declared, &Setting, &Verdict, false)) {
        if (Setting.Line == Parent) {
            Inside = true;
            Depth = Setting.Depth;
            End = Declared.At;
        } else if (Inside) {
            if (Setting.Depth <= Depth)
                break;
            End = Declared.At;
        }
    }
    return End;
}

/* `Line` put into the declaration at `At`. */
static int BoxAsmSettingInsert(BoxAsmBuffer *Settings, uint64_t At, const BoxAsmBuffer *Line)
{
    uint64_t Tail = Settings->Size - At;

    if (BoxAsmBufferAppend(Settings, Line->Data, Line->Size) != 0)
        return -ERR_NO_MEMORY;
    memmove(Settings->Data + At + Line->Size, Settings->Data + At, (size_t)Tail);
    memcpy(Settings->Data + At, Line->Data, (size_t)Line->Size);
    return 0;
}

/* setting [PATH ...] NAME KIND [what the kind takes] default VALUE [next-start] "what it does"
 * setting [PATH ...] NAME "what it holds" */
int BoxAsmParseProgramSetting(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                              const BoxAsmToken *Word)
{
    SettingSpan Rest = BoxAsmSettingRest(Lexer, Word);
    uint32_t Chain[SETTINGS_DEPTH_MAX];
    uint64_t Starts[SETTINGS_DEPTH_MAX + 1];
    uint32_t Held = 0;
    uint32_t Depth;
    Setting Declared;
    Setting Before;
    SettingsVerdict Verdict;
    SettingsVerdict First = SETTINGS_OK;
    SettingSpan FirstName = { Rest.At, 0 };
    BoxAsmBuffer Line = { NULL, 0, 0 };
    int Rc;

    if (Asm->Struc.Open || Asm->Struc.Instance)
        return BoxAsmFail(Asm, &Word->Where,
                          "setting stands outside struc and istruc: it is the program's, not a layout's",
                          NULL);
    while (Rest.Length > 0 && (Rest.At[Rest.Length - 1] == ' ' || Rest.At[Rest.Length - 1] == '\t' ||
                               Rest.At[Rest.Length - 1] == '\r'))
        Rest.Length--;
    if (Asm->Settings.Size == 0 &&
        BoxAsmBufferAppend(&Asm->Settings, "settings\n", sizeof("settings\n") - 1u) != 0)
        return -ERR_NO_MEMORY;

    /* What holds it: the leading words that name, from the top down,
     * settings declared already — as many of them as leave the rest of the
     * line a setting it can read, so a name that is also a kind's word reads
     * as the one the line means. */
    Starts[0] = 0;
    while (Held < SETTINGS_DEPTH_MAX) {
        uint64_t At = Starts[Held];
        SettingSpan Name;
        bool Quoted;
        SettingsVerdict Ignored = SETTINGS_OK;

        if (!SettingWord(Rest, &At, &Name, &Quoted, &Ignored) || Quoted ||
            !BoxAsmSettingChild(&Asm->Settings, Held ? Chain[Held - 1u] : 0, Name, &Before))
            break;
        Chain[Held++] = Before.Line;
        Starts[Held] = At;
    }
    for (Depth = Held + 1u; Depth-- > 0;) {
        Verdict = SettingRead(Rest, Starts[Depth], Word->Where.Line, &Declared);
        if (Depth == Held) {
            First = Verdict;
            FirstName = Declared.Name;
        }
        if (Verdict == SETTINGS_OK)
            break;
    }
    if (Verdict != SETTINGS_OK)
        return BoxAsmSettingFail(Asm, Word, FirstName, SettingsVerdictText(First));
    if (Depth >= SETTINGS_DEPTH_MAX)
        return BoxAsmSettingFail(Asm, Word, Declared.Name, SettingsVerdictText(SETTING_TOO_DEEP));
    if (BoxAsmSettingChild(&Asm->Settings, Depth ? Chain[Depth - 1u] : 0, Declared.Name, &Before))
        return BoxAsmSettingFail(Asm, Word, Declared.Name, "is declared already");

    Rc = BoxAsmSettingLine(Asm, Word, Rest, Starts[Depth], Depth, Declared.Name, &Line);
    if (Rc == 0)
        Rc = BoxAsmSettingInsert(&Asm->Settings,
                                 BoxAsmSettingPlace(&Asm->Settings, Depth ? Chain[Depth - 1u] : 0), &Line);
    free(Line.Data);
    if (Rc != 0)
        return Rc;
    Lexer->Position = (size_t)((const char *)Rest.At + Rest.Length - Lexer->Source);
    return BoxAsmNextToken(Asm, Lexer, Token);
}
