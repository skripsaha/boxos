/* takes and gives: the Form a program shows the shell, as box/form.h defines
 * it. The directives are written in the text of the Form as they come, after
 * its first line, `form 1`; the object carries that text in .boxos.form. */

#include "boxasm_asm.h"

#include "box/error.h"
#include "box/string.h"

#define BOXASM_FORM_FIELDS_MAX 16u

static const char *const BoxAsmFormKinds[] = {
    "thing", "things", "name", "words", "text", "into"
};
#define BOXASM_FORM_KIND_COUNT (sizeof(BoxAsmFormKinds) / sizeof(BoxAsmFormKinds[0]))
#define BOXASM_FORM_KIND_INTO  5u

static const char *const BoxAsmFormGives[] = { "things", "text" };

static int BoxAsmFormAppend(BoxAsmAssembler *Asm, const char *Text, size_t Length)
{
    if (Asm->Form.Text.Size == 0 && BoxAsmBufferAppend(&Asm->Form.Text, "form 1\n", 7u) != 0)
        return -ERR_NO_MEMORY;
    return BoxAsmBufferAppend(&Asm->Form.Text, Text, Length);
}

static int BoxAsmFormFail(BoxAsmAssembler *Asm, const BoxAsmToken *Token, const char *Before,
                          const char *After)
{
    BoxAsmMessage Message;

    BoxAsmFailBegin(Asm, &Token->Where, &Message);
    BoxAsmMessageText(&Message, Before);
    BoxAsmMessageSpan(&Message, Token->Text, Token->Length);
    BoxAsmMessageText(&Message, After);
    return BoxAsmFailEnd(Asm, &Token->Where, &Message);
}

static int BoxAsmFormWord(const BoxAsmToken *Token, const char *const *Words, size_t Count,
                          size_t *Out)
{
    size_t Index;

    for (Index = 0; Index < Count; Index++) {
        if (BoxAsmTokenIs(Token, Words[Index])) {
            *Out = Index;
            return 1;
        }
    }
    return 0;
}

/* The words a field shows when the shell asks for it, as they stand between
 * the quotes of its line. */
static int BoxAsmFormWhat(BoxAsmAssembler *Asm, const BoxAsmToken *Token, BoxAsmBuffer *What)
{
    size_t Index;
    int Rc;

    if (Token->Kind != BOXASM_TOKEN_STRING)
        return BoxAsmFail(Asm, &Token->Where,
                          "takes says what the field is in quotes: takes things \"the files to tag\"",
                          NULL);
    if ((Rc = BoxAsmLexStringBytes(Token, What)) != 0)
        return Rc;
    if (What->Size == 0)
        return BoxAsmFail(Asm, &Token->Where, "a field says what it is in a few words, not none",
                          NULL);
    for (Index = 0; Index < What->Size; Index++)
        if (What->Data[Index] == '\0' || What->Data[Index] == '\n' || What->Data[Index] == '"')
            return BoxAsmFail(Asm, &Token->Where,
                              "what a field is holds no newline, NUL or double quote", NULL);
    return 0;
}

static int BoxAsmFormTakesLine(BoxAsmAssembler *Asm, uint8_t Optional, size_t Kind,
                               const BoxAsmBuffer *What)
{
    const char *Name = BoxAsmFormKinds[Kind];
    int Rc = BoxAsmFormAppend(Asm, "takes ", 6u);

    if (Rc == 0 && Optional)
        Rc = BoxAsmFormAppend(Asm, "optional ", 9u);
    if (Rc == 0)
        Rc = BoxAsmFormAppend(Asm, Name, strlen(Name));
    if (Rc == 0)
        Rc = BoxAsmFormAppend(Asm, " \"", 2u);
    if (Rc == 0)
        Rc = BoxAsmFormAppend(Asm, (const char *)What->Data, What->Size);
    if (Rc == 0)
        Rc = BoxAsmFormAppend(Asm, "\"\n", 2u);
    return Rc;
}

/* takes [optional] KIND "what it is" */
static int BoxAsmParseTakes(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                            const BoxAsmToken *Word)
{
    BoxAsmBuffer What = { 0 };
    uint8_t Optional = 0;
    size_t Kind;
    int Rc;

    if (Asm->Form.Takes == BOXASM_FORM_FIELDS_MAX)
        return BoxAsmFail(Asm, &Word->Where, "a Form takes at most 16 fields", NULL);
    if (BoxAsmTokenIs(Token, "optional")) {
        Optional = 1;
        if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
            return Rc;
    }
    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER)
        return BoxAsmFail(Asm, &Token->Where,
                          "takes says what it takes: thing, things, name, words, text or into", NULL);
    if (!BoxAsmFormWord(Token, BoxAsmFormKinds, BOXASM_FORM_KIND_COUNT, &Kind))
        return BoxAsmFormFail(Asm, Token, "'",
                              "' is no kind a Form takes: thing, things, name, words, text or into");
    if (Kind == BOXASM_FORM_KIND_INTO && Asm->Form.Into)
        return BoxAsmFail(Asm, &Token->Where,
                          "a Form takes into once: the name the arrow points at is one", NULL);
    if ((Rc = BoxAsmNextToken(Asm, Lexer, Token)) != 0)
        return Rc;
    if ((Rc = BoxAsmFormWhat(Asm, Token, &What)) == 0 &&
        (Rc = BoxAsmFormTakesLine(Asm, Optional, Kind, &What)) == 0) {
        Asm->Form.Takes++;
        Asm->Form.Into |= Kind == BOXASM_FORM_KIND_INTO;
        Rc = BoxAsmNextToken(Asm, Lexer, Token);
    }
    BoxAsmBufferRelease(&What);
    return Rc;
}

/* gives things | text */
static int BoxAsmParseGives(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                            const BoxAsmToken *Word)
{
    const char *Name;
    size_t Kind;
    int Rc;

    if (Asm->Form.Gives)
        return BoxAsmFail(Asm, &Word->Where, "a Form gives once: things or text", NULL);
    if (Token->Kind != BOXASM_TOKEN_IDENTIFIER)
        return BoxAsmFail(Asm, &Token->Where, "gives says what it gives: things or text", NULL);
    if (!BoxAsmFormWord(Token, BoxAsmFormGives, 2u, &Kind))
        return BoxAsmFormFail(Asm, Token, "'", "' is not what a Form gives: things or text");
    Name = BoxAsmFormGives[Kind];
    if ((Rc = BoxAsmFormAppend(Asm, "gives ", 6u)) != 0 ||
        (Rc = BoxAsmFormAppend(Asm, Name, strlen(Name))) != 0 ||
        (Rc = BoxAsmFormAppend(Asm, "\n", 1u)) != 0)
        return Rc;
    Asm->Form.Gives = 1;
    return BoxAsmNextToken(Asm, Lexer, Token);
}

int BoxAsmParseForm(BoxAsmAssembler *Asm, BoxAsmLexer *Lexer, BoxAsmToken *Token,
                    const BoxAsmToken *Word)
{
    if (Asm->Struc.Open || Asm->Struc.Instance)
        return BoxAsmFormFail(Asm, Word, "", " stands outside struc and istruc: it is the program's, "
                                            "not a layout's");
    if (BoxAsmTokenIs(Word, "takes"))
        return BoxAsmParseTakes(Asm, Lexer, Token, Word);
    return BoxAsmParseGives(Asm, Lexer, Token, Word);
}
