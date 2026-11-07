/* %push, %pop and %repl: the preprocessor's stack of contexts, as NASM keeps
 * it. A name written %$name is the top context's own, %$$name the one below
 * it, and so on: each is written ..@N.name, N the context's number, taken
 * from the count the %%labels of macro calls are numbered by, so the two
 * never meet. The name is found where it is used — a %define's body that
 * holds one names the context it is expanded in — and a directive's own
 * name (%define %$x) where the directive stands. The stack is the whole
 * source's: a context opened in a macro call or a file may be closed outside
 * it, and one left open at the end is refused. */

#include "boxasm_macro_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* The name a context directive gives: an empty one when it writes none and
 * none is Needed. */
static int BoxAsmPpContextWord(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest,
                               BoxAsmPpToken *Name, int Needed)
{
    Rest = BoxAsmPpSpaces(Rest);
    memset(Name, 0, sizeof(*Name));
    if (!Needed && (*Rest == '\0' || *Rest == ';')) {
        Name->Text = Rest;
        return 0;
    }
    return BoxAsmPpName(Pp, Line, Rest, "expected the name of a context", Name);
}

int BoxAsmPpPush(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmPpContext *Context;
    BoxAsmPpToken Name;
    int Rc;

    if ((Rc = BoxAsmPpContextWord(Pp, Line, Rest, &Name, 0)) != 0 ||
        (Rc = BoxAsmPpNothingMore(Pp, Line, Name.Text + Name.Length)) != 0)
        return Rc;
    if (BoxAsmGrowArray((void **)&Pp->Contexts, &Pp->ContextCapacity, Pp->ContextCount,
                        sizeof(BoxAsmPpContext)) != 0)
        return -ERR_NO_MEMORY;
    Context = &Pp->Contexts[Pp->ContextCount];
    memset(Context, 0, sizeof(*Context));
    if (!(Context->Name = BoxAsmCopyText(Name.Text, Name.Length)) ||
        !(Context->Text = BoxAsmCopyText(Line->Text, strlen(Line->Text)))) {
        free(Context->Name);
        return -ERR_NO_MEMORY;
    }
    Context->Number = ++Pp->Unique;
    Context->File = Line->File;
    Context->Line = Line->Line;
    Context->Frame = Line->Frame;
    Pp->ContextCount++;
    return 0;
}

static void BoxAsmPpDropContext(BoxAsmPpContext *Context)
{
    free(Context->Name);
    free(Context->Text);
    memset(Context, 0, sizeof(*Context));
}

/* %pop, or %pop name: the top context must be called so. */
int BoxAsmPpPop(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmPpContext *Top;
    BoxAsmPpToken Name;
    BoxAsmMessage Message;
    int Rc;

    if ((Rc = BoxAsmPpContextWord(Pp, Line, Rest, &Name, 0)) != 0 ||
        (Rc = BoxAsmPpNothingMore(Pp, Line, Name.Text + Name.Length)) != 0)
        return Rc;
    if (Pp->ContextCount == 0)
        return BoxAsmPpFail(Pp, Line, NULL, "%pop has no %push before it", NULL);
    Top = &Pp->Contexts[Pp->ContextCount - 1u];
    if (Name.Length != 0 &&
        (strlen(Top->Name) != Name.Length || memcmp(Top->Name, Name.Text, Name.Length) != 0)) {
        BoxAsmPpFailBegin(Pp, Line, Name.Text, &Message);
        BoxAsmMessageText(&Message, "the context on top is '");
        BoxAsmMessageText(&Message, Top->Name);
        BoxAsmMessageText(&Message, "', not '");
        BoxAsmMessageSpan(&Message, Name.Text, Name.Length);
        BoxAsmMessageText(&Message, "'");
        return BoxAsmPpFailEnd(Pp, Line, Name.Text, &Message);
    }
    BoxAsmPpDropContext(Top);
    Pp->ContextCount--;
    return 0;
}

/* %repl name: the top context called so from now on. */
int BoxAsmPpReplace(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest)
{
    BoxAsmPpToken Name;
    char *Copy;
    int Rc;

    if ((Rc = BoxAsmPpContextWord(Pp, Line, Rest, &Name, 1)) != 0 ||
        (Rc = BoxAsmPpNothingMore(Pp, Line, Name.Text + Name.Length)) != 0)
        return Rc;
    if (Pp->ContextCount == 0)
        return BoxAsmPpFail(Pp, Line, NULL, "%repl renames the context on top, and no %push opened one", NULL);
    if (!(Copy = BoxAsmCopyText(Name.Text, Name.Length)))
        return -ERR_NO_MEMORY;
    free(Pp->Contexts[Pp->ContextCount - 1u].Name);
    Pp->Contexts[Pp->ContextCount - 1u].Name = Copy;
    return 0;
}

int BoxAsmPpContextIs(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Rest, int *Truth)
{
    const char *Top = Pp->ContextCount != 0 ? Pp->Contexts[Pp->ContextCount - 1u].Name : NULL;
    BoxAsmPpToken Name;
    int Rc;

    *Truth = 0;
    Rest = BoxAsmPpSpaces(Rest);
    if (*Rest == '\0' || *Rest == ';')
        return BoxAsmPpFail(Pp, Line, Rest, "expected the name of a context", NULL);
    while (*Rest != '\0' && *Rest != ';') {
        if ((Rc = BoxAsmPpName(Pp, Line, Rest, "expected the name of a context", &Name)) != 0)
            return Rc;
        if (Top && strlen(Top) == Name.Length && memcmp(Top, Name.Text, Name.Length) == 0)
            *Truth = 1;
        Rest = BoxAsmPpSpaces(Name.Text + Name.Length);
        if (*Rest == ',')
            Rest = BoxAsmPpSpaces(Rest + 1);
    }
    return 0;
}

/* The context-local name at Text[*At] ('%', then '$'s, then the name)
 * appended to Out as the context makes it, *At past it. */
static int BoxAsmPpContextName(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                               size_t *At, BoxAsmBuffer *Out)
{
    const char *Start = Text + *At;
    size_t Dollars = 0;
    size_t Begin;
    size_t End;
    char Digits[12];
    size_t Count = 0;
    uint32_t Number;
    BoxAsmMessage Message;

    while (Start[1u + Dollars] == '$')
        Dollars++;
    Begin = *At + 1u + Dollars;
    End = Begin;
    while (BoxAsmPpPart(Text[End]))
        End++;
    if (End == Begin)
        return BoxAsmPpFail(Pp, Line, Start, "%$ is followed by the name local to the context", NULL);
    if (Dollars > Pp->ContextCount) {
        BoxAsmPpFailBegin(Pp, Line, Start, &Message);
        BoxAsmMessageText(&Message, "'");
        BoxAsmMessageSpan(&Message, Start, End - *At);
        if (Pp->ContextCount == 0) {
            BoxAsmMessageText(&Message, "' is local to a context, and no %push has opened one");
        } else {
            BoxAsmMessageText(&Message, "' names the context ");
            BoxAsmMessageDecimal(&Message, (int64_t)Dollars - 1);
            BoxAsmMessageText(&Message, " below the top, and only ");
            BoxAsmMessageDecimal(&Message, (int64_t)Pp->ContextCount);
            BoxAsmMessageText(&Message, Pp->ContextCount == 1 ? " is open" : " are open");
        }
        return BoxAsmPpFailEnd(Pp, Line, Start, &Message);
    }
    Number = Pp->Contexts[Pp->ContextCount - Dollars].Number;
    do {
        Digits[sizeof(Digits) - 1u - Count++] = (char)('0' + Number % 10u);
        Number /= 10u;
    } while (Number != 0);
    *At = End;
    if (BoxAsmBufferAppend(Out, "..@", 3) != 0 ||
        BoxAsmBufferAppend(Out, Digits + sizeof(Digits) - Count, Count) != 0 ||
        BoxAsmBufferAppend(Out, ".", 1) != 0 ||
        BoxAsmBufferAppend(Out, Text + Begin, End - Begin) != 0)
        return -ERR_NO_MEMORY;
    return 0;
}

int BoxAsmPpContextText(BoxAsmPp *Pp, const BoxAsmPpLine *Line, const char *Text,
                        BoxAsmBuffer *Buffer, const char **Out)
{
    size_t At = 0;
    size_t Copied = 0;
    int Rc;

    *Out = Text;
    if (!strstr(Text, "%$"))
        return 0;
    Buffer->Size = 0;
    for (;;) {
        BoxAsmPpToken Token;
        size_t Here = At;

        BoxAsmPpNext(Text, &At, &Token);
        if (Token.Kind == BOXASM_PP_END)
            break;
        if (Token.Kind != BOXASM_PP_OTHER || *Token.Text != '%' || Token.Text[1] != '$')
            continue;
        if (BoxAsmBufferAppend(Buffer, Text + Copied, Here - Copied) != 0)
            return -ERR_NO_MEMORY;
        At = Here;
        if ((Rc = BoxAsmPpContextName(Pp, Line, Text, &At, Buffer)) != 0)
            return Rc;
        Copied = At;
    }
    if (BoxAsmBufferAppend(Buffer, Text + Copied, strlen(Text + Copied) + 1u) != 0)
        return -ERR_NO_MEMORY;
    Buffer->Size--;
    *Out = (const char *)Buffer->Data;
    return 0;
}

int BoxAsmPpContextLead(BoxAsmPp *Pp, BoxAsmPpLine *Working, const char **Rest,
                        BoxAsmBuffer *Buffer)
{
    const char *Text = Working->Text;
    const char *Lead = BoxAsmPpSpaces(*Rest);
    size_t At = (size_t)(Lead - Text);
    size_t Prefix = At;
    int Rc;

    if (Lead[0] != '%' || Lead[1] != '$')
        return 0;
    Buffer->Size = 0;
    if (BoxAsmBufferAppend(Buffer, Text, Prefix) != 0)
        return -ERR_NO_MEMORY;
    if ((Rc = BoxAsmPpContextName(Pp, Working, Text, &At, Buffer)) != 0)
        return Rc;
    if (BoxAsmBufferAppend(Buffer, Text + At, strlen(Text + At) + 1u) != 0)
        return -ERR_NO_MEMORY;
    Buffer->Size--;
    Working->Text = (const char *)Buffer->Data;
    *Rest = Working->Text + Prefix;
    return 0;
}

int BoxAsmPpContextsClosed(BoxAsmPp *Pp)
{
    BoxAsmPpContext *Open;
    BoxAsmPpLine Line;

    if (Pp->ContextCount == 0)
        return 0;
    Open = &Pp->Contexts[Pp->ContextCount - 1u];
    Line = (BoxAsmPpLine){ Open->Text, Open->File, Open->Line, Open->Frame };
    return BoxAsmPpFail(Pp, &Line, NULL, "this %push opened a context no %pop closed", NULL);
}

void BoxAsmPpDropContexts(BoxAsmPp *Pp)
{
    while (Pp->ContextCount > 0)
        BoxAsmPpDropContext(&Pp->Contexts[--Pp->ContextCount]);
    free(Pp->Contexts);
    Pp->Contexts = NULL;
    Pp->ContextCapacity = 0;
}
