/*
 * fill.c — fill a program's Form from the typed line, then start the program.
 *
 * BoxLib's filler sorts the words by what each one is and says when it needs
 * the person: a name that means several files, a field left empty, `all` with
 * no Use Context. The shell only asks those questions and hands the answers
 * back. Every program goes through here, with a Form or without one: a
 * program with none still gets the arrow and `filled 1`.
 */

#include "fill.h"
#include "ask.h"
#include "remake.h"
#include "them.h"
#include "box/form.h"
#include "box/file.h"
#include "box/system.h"
#include "box/print.h"
#include "box/memory.h"
#include "box/error.h"
#include "box/debug.h"
#include "box/string.h"

/* The command was stopped and the person told why. */
#define FILL_STOPPED  1

static void SayFailed(const char *who, const char *what, int rc)
{
    printf("%s: %s (error %d)\n", who, what, (int)box_errno_of(rc));
}

/* The answer to an ASK_FIELD: the field and, if the last words named no
 * file, why it is asked again. */
static AskResult AnswerField(BoxFormFiller *filler, const BoxForm *form, const char *who,
                             const BoxFormQuestion *question, int *rc)
{
    if (question->Word) printf("%s: %s\n", who, question->Word);
    printf("%s: %s?\n", who, form->Fields[question->Field].What);

    char     *words;
    AskResult asked = AskLine("> ", &words);
    if (asked == ASK_ANSWERED) {
        *rc = form_fill_words(filler, words);
        free(words);
    }
    return asked;
}

static AskResult AnswerWhich(BoxFormFiller *filler, const BoxFormQuestion *question, int *rc)
{
    file_id_t chosen;
    AskResult asked = AskWhich(question->Word, question->Choices, question->ChoiceCount, &chosen);
    if (asked == ASK_ANSWERED) *rc = form_fill_choose(filler, chosen);
    return asked;
}

static AskResult AnswerSure(BoxFormFiller *filler, const BoxFormQuestion *question, int *rc)
{
    bool yes = false;
    AskResult asked = AskSure(question->Count, &yes);
    if (asked == ASK_ANSWERED) *rc = form_fill_sure(filler, yes);
    return asked;
}

/* One question put and its answer given to the filler: 0, FILL_STOPPED when
 * the person did not answer, or a negative -error_t. */
static int Answer(BoxFormFiller *filler, const BoxForm *form, const char *who,
                  const BoxFormQuestion *question)
{
    int       rc = 0;
    AskResult asked;

    if (question->Step == BOX_FORM_ASK_WHICH)
        asked = AnswerWhich(filler, question, &rc);
    else if (question->Step == BOX_FORM_ASK_FIELD)
        asked = AnswerField(filler, form, who, question, &rc);
    else
        asked = AnswerSure(filler, question, &rc);

    if (asked != ASK_ANSWERED) {
        AskSayCancelled(who, asked);
        return FILL_STOPPED;
    }
    return rc;
}

/* Step the filler until the line is filled: 0, FILL_STOPPED (refused or not
 * answered, said), or a negative -error_t. */
static int FillLine(BoxFormFiller *filler, const BoxForm *form, const char *who)
{
    for (;;) {
        BoxFormQuestion question;
        int rc = form_fill_step(filler, &question);
        if (rc < 0) return rc;
        if (question.Step == BOX_FORM_DONE) return 0;
        if (question.Step == BOX_FORM_REFUSED) {
            printf("%s: %s\n", who, question.Word);
            return FILL_STOPPED;
        }
        rc = Answer(filler, form, who, &question);
        if (rc != 0) return rc;
    }
}

/* The file the program's screen goes to: made here when the arrow names a
 * file that is not there yet. 0 for none. */
static int ArrowScreen(BoxFormFiller *filler, const char *who, file_id_t *screen)
{
    const char *make = NULL;
    int rc = form_fill_arrow(filler, screen, &make);
    if (rc < 0) return rc;
    if (!make) return 0;

    int64_t made = create(make, NULL);
    if (made <= 0) {
        SayFailed(who, "cannot make the file after ->", (int)made);
        return FILL_STOPPED;
    }
    *screen = (file_id_t)made;
    return 0;
}

/* How a started program ended and what it gave back. */
typedef struct {
    file_id_t  Screen;         /* the arrow's file, 0 for none */
    bool       Asked;          /* its Souvenir was asked for */
    bool       Ended;          /* its end was seen: ExitCode is its own */
    int32_t    ExitCode;
    bool       Collected;      /* what it queued was read off */
    file_id_t *Given;          /* its Souvenir, heap; NULL for none */
    size_t     Count;
} FillEnd;

/* The child's end is waited for as a STATE (process_gone), not an event: the
 * kernel answers from the child's own record, so there is no window between
 * asking and being registered and nothing that can be dropped. What it
 * queued is read off whether it was asked for or not, so no stale gift is
 * met later. */
static void WaitGone(const char *who, uint32_t pid, uint32_t generation, FillEnd *end)
{
    int rc = process_gone(pid, generation, &end->ExitCode);
    end->Ended = rc == 0;
    if (rc != 0)
        kdbg_print("[shell] '%s' (pid %u gen %u): process.gone refused - rc=%d",
                   who, pid, generation, rc);
    end->Collected = souvenir_collect(pid, &end->Given, &end->Count) == 0;
}

/* What the command gave becomes `them`: the arrow's file when its screen
 * went into one, otherwise the Souvenir the shell asked for. */
static void KeepGift(FillEnd *end)
{
    if (end->Screen != 0) {
        file_id_t *screen = malloc(sizeof(file_id_t));
        if (screen) {
            screen[0] = end->Screen;
            ThemKeep(screen, 1);
        }
    } else if (end->Collected && end->Asked) {
        ThemKeep(end->Given, end->Count);
        end->Given = NULL;
    }
    free(end->Given);
    end->Given = NULL;
}

/* Start the program with its Form filled and wait until it is gone: 0 with
 * *end filled, FILL_STOPPED (said), or a negative -error_t. */
static int Launch(BoxFormFiller *filler, const BoxForm *form, file_id_t program, const char *who,
                  FillEnd *end)
{
    memset(end, 0, sizeof(*end));
    int rc = ArrowScreen(filler, who, &end->Screen);
    if (rc == 0) rc = form_fill_program(filler, who);

    /* The Souvenir is asked for only when it is kept: the Form gives things
     * and the arrow did not make the screen a file. */
    end->Asked = end->Screen == 0 && form->Gives == BOX_FORM_GIVES_THINGS;
    if (rc == 0 && end->Asked) rc = form_fill_souvenir(filler);

    char  *bytes = NULL;
    size_t length = 0;
    if (rc == 0) rc = form_fill_luggage(filler, end->Screen, &bytes, &length);
    if (rc != 0) return rc;

    uint32_t generation = 0;
    int      pid = proc_exec_file(program, bytes, length, NULL, &generation);
    free(bytes);
    if (pid <= 0) {
        SayFailed(who, "is there and would not start", pid == 0 ? -ERR_SPAWN_FAILED : pid);
        return FILL_STOPPED;
    }

    WaitGone(who, (uint32_t)pid, generation, end);
    return 0;
}

/* The program's Form, read from its file: 0, or FILL_STOPPED (said). */
static int ReadForm(file_id_t program, const char *who, BoxForm *form)
{
    int rc = form_of_file(program, form);
    if (rc == 0) return 0;
    /* The program's own file cannot be read — its medium gone, most often:
     * it is there, and it would not start, which is what the person is told
     * when the start itself is refused. */
    SayFailed(who, "is there and would not start", rc);
    return FILL_STOPPED;
}

void FillAndRun(file_id_t program, const char *who, const char *line)
{
    if (!RemakeWhenChanged(&program, who)) return;

    BoxForm form;
    if (ReadForm(program, who, &form) != 0) return;

    BoxFormLookup  lookup;
    BoxFormFiller *filler = NULL;
    FillEnd        end;
    form_lookup_volume(&lookup);
    ThemShow(&lookup);

    int rc = form_fill_begin(&form, line, &lookup, &filler);
    if (rc == 0) rc = FillLine(filler, &form, who);
    if (rc == 0) rc = Launch(filler, &form, program, who, &end);
    if (rc == 0) KeepGift(&end);
    if (rc < 0) SayFailed(who, "could not be started", rc);

    if (filler) form_fill_end(filler);
    form_release(&form);
}

bool FillAndRunOn(file_id_t program, const char *who, file_id_t thing, file_id_t *given)
{
    BoxForm form;
    *given = 0;
    if (ReadForm(program, who, &form) != 0) return false;

    /* `it` is the one file handed over: the lookup knows it and no other
     * `it`, so the person's own `them` is neither read nor changed. */
    BoxFormLookup   lookup;
    BoxFormFiller  *filler = NULL;
    BoxFormQuestion question;
    FillEnd         end;
    form_lookup_volume(&lookup);
    lookup.Them      = &thing;
    lookup.ThemCount = 1;
    lookup.ThemKnown = true;

    int rc = form_fill_begin(&form, "it", &lookup, &filler);
    if (rc == 0) rc = form_fill_step(filler, &question);
    if (rc == 0 && question.Step == BOX_FORM_REFUSED) {
        printf("%s: %s\n", who, question.Word);
        rc = FILL_STOPPED;
    } else if (rc == 0 && question.Step != BOX_FORM_DONE) {
        printf("%s: it would have to ask what it takes, and no one is asked here\n", who);
        rc = FILL_STOPPED;
    }
    if (rc == 0) rc = Launch(filler, &form, program, who, &end);
    if (rc < 0) SayFailed(who, "could not be started", rc);

    bool ended_well = rc == 0 && end.Ended && end.ExitCode == 0;
    if (ended_well && end.Asked && end.Collected && end.Count > 0) *given = end.Given[0];
    if (rc == 0) free(end.Given);
    if (filler) form_fill_end(filler);
    form_release(&form);
    return ended_well;
}
