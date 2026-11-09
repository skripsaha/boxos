/* setting: a line of the settings a program declares, as box/settings.h and
 * src/include/settings_reader.h define them. Its words are the declaration's,
 * not the assembler's — yes-or-no, #FFB020, -3, » stay as written — so the
 * line is taken from the source whole, read by the one reader of settings,
 * and written into the program's declaration, .boxos.settings, its words
 * apart by single blanks. A line the reader would not read is refused here,
 * where it was written, rather than skipped by the program that has it. */

#include "boxasm_asm.h"

#include "box/error.h"
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

/* "setting" and the line's words, each apart by one blank, quoted ones in
 * their quotes. Text between quotes holds nothing below a blank: the
 * declaration is a part written as text, which holds no tab. */
static int BoxAsmSettingWrite(BoxAsmAssembler *Asm, const BoxAsmToken *Word, SettingSpan Rest,
                              SettingSpan Name)
{
    SettingSpan Piece;
    SettingsVerdict Ignored;
    uint64_t At = 0;
    bool Quoted;
    int Rc;

    if (Asm->Settings.Size == 0 && BoxAsmBufferAppend(&Asm->Settings, "settings 1\n", 11u) != 0)
        return -ERR_NO_MEMORY;
    if (BoxAsmBufferAppend(&Asm->Settings, "setting", 7u) != 0)
        return -ERR_NO_MEMORY;
    while (SettingWord(Rest, &At, &Piece, &Quoted, &Ignored)) {
        uint64_t Index;

        for (Index = 0; Quoted && Index < Piece.Length; Index++)
            if (Piece.At[Index] < 0x20u)
                return BoxAsmSettingFail(Asm, Word, Name,
                                         "holds a tab between its quotes, which a declaration cannot hold");
        Rc = BoxAsmBufferAppend(&Asm->Settings, Quoted ? " \"" : " ", Quoted ? 2u : 1u);
        if (Rc == 0)
            Rc = BoxAsmBufferAppend(&Asm->Settings, Piece.At, Piece.Length);
        if (Rc == 0 && Quoted)
            Rc = BoxAsmBufferAppend(&Asm->Settings, "\"", 1u);
        if (Rc != 0)
            return -ERR_NO_MEMORY;
    }
    return BoxAsmBufferAppend(&Asm->Settings, "\n", 1u) != 0 ? -ERR_NO_MEMORY : 0;
}

/* setting NAME KIND [what the kind takes] default VALUE [next-start] "what it does" */
int BoxAsmParseProgramSetting(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                              const BoxAsmToken *Word)
{
    SettingSpan Rest = BoxAsmSettingRest(Lexer, Word);
    Setting Declared;
    Setting Before;
    SettingsVerdict Verdict;
    int Rc;

    if (Asm->Struc.Open || Asm->Struc.Instance)
        return BoxAsmFail(Asm, &Word->Where,
                          "setting stands outside struc and istruc: it is the program's, not a layout's",
                          NULL);
    while (Rest.Length > 0 && (Rest.At[Rest.Length - 1] == ' ' || Rest.At[Rest.Length - 1] == '\t' ||
                               Rest.At[Rest.Length - 1] == '\r'))
        Rest.Length--;
    Verdict = SettingRead(Rest, 0, Word->Where.Line, &Declared);
    if (Verdict != SETTINGS_OK)
        return BoxAsmSettingFail(Asm, Word, Declared.Name, SettingsVerdictText(Verdict));
    if (SettingsDeclaredFind(Asm->Settings.Data, Asm->Settings.Size, Declared.Name, &Before))
        return BoxAsmSettingFail(Asm, Word, Declared.Name, "is declared already");
    if ((Rc = BoxAsmSettingWrite(Asm, Word, Rest, Declared.Name)) != 0)
        return Rc;
    Lexer->Position = (size_t)((const char *)Rest.At + Rest.Length - Lexer->Source);
    return BoxAsmNextToken(Asm, Lexer, Token);
}
