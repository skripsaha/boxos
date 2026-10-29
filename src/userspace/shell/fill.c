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
#include "them.h"
#include "box/form.h"
#include "box/file.h"
#include "box/system.h"
#include "box/print.h"
#include "box/memory.h"
#include "box/error.h"
#include "box/debug.h"

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

/* The child's end is waited for as a STATE (process_gone), not an event: the
 * kernel answers from the child's own record, so there is no window between
 * asking and being registered and nothing that can be dropped. */
static void WaitGone(const char *who, uint32_t pid, uint32_t generation)
{
    int32_t exit_code = 0;
    int     rc = process_gone(pid, generation, &exit_code);
    if (rc != 0)
        kdbg_print("[shell] '%s' (pid %u gen %u): process.gone refused - rc=%d",
                   who, pid, generation, rc);
}

/* What the command gave becomes `them`. When its screen went into a file
 * through the arrow, that file is what it gave, whatever the program gave
 * back; otherwise the program's Souvenir decides, when its Form says it gives
 * things. What it queued is read off either way so no stale gift is met later. */
static void KeepGift(const BoxForm *form, uint32_t pid, file_id_t screen)
{
    file_id_t *given = NULL;
    size_t     count = 0;
    bool       collected = souvenir_collect(pid, &given, &count) == 0;

    if (screen != 0) {
        free(given);
        given = malloc(sizeof(file_id_t));
        if (!given) return;
        given[0] = screen;
        ThemKeep(given, 1);
        return;
    }
    if (collected && form->Gives == BOX_FORM_GIVES_THINGS) {
        ThemKeep(given, count);
        return;
    }
    free(given);
}

static int Launch(BoxFormFiller *filler, const BoxForm *form, file_id_t program, const char *who)
{
    file_id_t screen = 0;
    int       rc = ArrowScreen(filler, who, &screen);
    if (rc == 0) rc = form_fill_program(filler, who);

    char  *bytes = NULL;
    size_t length = 0;
    if (rc == 0) rc = form_fill_luggage(filler, screen, &bytes, &length);
    if (rc != 0) return rc;

    uint32_t generation = 0;
    int      pid = proc_exec_file(program, bytes, length, NULL, &generation);
    free(bytes);
    if (pid <= 0) {
        SayFailed(who, "is there and would not start", pid == 0 ? -ERR_SPAWN_FAILED : pid);
        return FILL_STOPPED;
    }

    WaitGone(who, (uint32_t)pid, generation);
    KeepGift(form, (uint32_t)pid, screen);
    return 0;
}

void FillAndRun(file_id_t program, const char *who, const char *line)
{
    BoxForm form;
    int     rc = form_of_file(program, &form);
    if (rc != 0) {
        /* The program's own file cannot be read — its medium gone, most often:
         * it is there, and it would not start, which is what the person is
         * told when the start itself is refused. */
        SayFailed(who, "is there and would not start", rc);
        return;
    }

    BoxFormLookup  lookup;
    BoxFormFiller *filler = NULL;
    form_lookup_volume(&lookup);
    ThemShow(&lookup);

    rc = form_fill_begin(&form, line, &lookup, &filler);
    if (rc == 0) rc = FillLine(filler, &form, who);
    if (rc == 0) rc = Launch(filler, &form, program, who);
    if (rc < 0) SayFailed(who, "could not be started", rc);

    if (filler) form_fill_end(filler);
    form_release(&form);
}
