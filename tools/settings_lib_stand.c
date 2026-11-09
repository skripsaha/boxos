/* settings_lib_stand — BoxLib's settings put together (src/settings_read.c)
 * on the host, held to exact outcomes:
 *
 *     tools/settings_stand.sh
 *
 * Only BoxLib's own headers are seen here, so the source is built as it is
 * in BoxOS. A case is a declaration and a person's file; its outcome is each
 * setting's value and whether the person chose it, and every sentence said
 * of a line not taken, word for word, in order. */

#include "box/defs.h"
#include "box/types.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/string.h"
#include "box/core/settings_internal.h"

static int g_passed;
static int g_failed;

static void Check(bool ok, const char *what)
{
    if (ok) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s\n", what);
}

static const char g_clock[] =
    "settings 1\n"
    "setting hours one-of 24 12 default 24 \"how the hours are counted\"\n"
    "setting seconds yes-or-no default yes \"whether the seconds are shown\"\n"
    "setting color color default #E6E6E6 \"the colour of the digits\"\n"
    "setting width number from 5 to 40 cells default 8 next-start \"how wide it is\"\n"
    "setting label word default ~ \"what stands before the time\"\n"
    "setting note text default \"\" \"said under the time\"\n";

static char *Copy(const char *text)
{
    char *copy = malloc(strlen(text) + 1);
    memcpy(copy, text, strlen(text) + 1);
    return copy;
}

/* The settings of a program called clock with this declaration and file. */
static void Assemble(BoxSettings *out, const char *declared, const char *chosen)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->Program, "clock", 6);
    out->Declared     = Copy(declared);
    out->DeclaredSize = strlen(declared);
    if (chosen) {
        out->Chosen     = Copy(chosen);
        out->ChosenSize = strlen(chosen);
    }
    if (settings_assemble(out) != 0) {
        printf("FAIL the settings could not be put together\n");
        g_failed++;
    }
}

static void ValueIs(const BoxSettings *settings, const char *name, const char *value, bool chosen)
{
    char              label[160];
    const BoxSetting *setting = settings_find(settings, name);
    snprintf(label, sizeof(label), "%s is '%s'%s (it is '%s'%s)", name, value, chosen ? ", chosen" : "",
             setting ? setting->Value : "none", setting && setting->Chosen ? ", chosen" : "");
    Check(setting && strcmp(setting->Value, value) == 0 && setting->Chosen == chosen, label);
}

static void ProblemsAre(const BoxSettings *settings, const char *const *want, size_t count, const char *what)
{
    bool same = settings->ProblemCount == count;
    for (size_t i = 0; same && i < count; i++) same = strcmp(settings->Problems[i], want[i]) == 0;
    if (same) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s — said:\n", what);
    for (size_t i = 0; i < settings->ProblemCount; i++) printf("    %s\n", settings->Problems[i]);
    printf("  wanted:\n");
    for (size_t i = 0; i < count; i++) printf("    %s\n", want[i]);
}

static void TestNoFile(void)
{
    BoxSettings s;
    Assemble(&s, g_clock, NULL);
    Check(s.Count == 6, "six settings declared");
    ValueIs(&s, "hours", "24", false);
    ValueIs(&s, "note", "", false);
    const BoxSetting *seconds = settings_find(&s, "seconds");
    const BoxSetting *color = settings_find(&s, "color");
    const BoxSetting *width = settings_find(&s, "width");
    Check(seconds && seconds->Yes && color && color->Color == 0xE6E6E6u && width && width->Number == 8 &&
              width->NextStart && strcmp(width->Unit, "cells") == 0 && strcmp(width->What, "how wide it is") == 0,
          "each default read as its kind");
    Check(strcmp(settings_find(&s, "hours")->Choices, "24 12") == 0, "the choices kept");
    Check(s.ProblemCount == 0, "nothing said with no file");
    Check(settings_find(&s, "hour") == NULL, "no setting for a name not declared");
    settings_release(&s);
}

static void TestFile(void)
{
    BoxSettings s;
    Assemble(&s, g_clock,
             "settings 1\n"
             "# twelve hours, like the old one\n"
             "hours 12\n"
             "seconds no\n"
             "color #7cd68a\n"
             "width 30 cells\n"
             "label \"  \"\n"
             "note \"default\"\n"
             "when work hours 24\n");
    ValueIs(&s, "hours", "12", true);
    ValueIs(&s, "seconds", "no", true);
    ValueIs(&s, "color", "#7cd68a", true);
    ValueIs(&s, "width", "30", true);
    ValueIs(&s, "label", "~", false);
    ValueIs(&s, "note", "default", true);
    Check(!settings_find(&s, "seconds")->Yes && settings_find(&s, "color")->Color == 0x7CD68Au &&
              settings_find(&s, "width")->Number == 30,
          "each value read as its kind");
    ProblemsAre(&s,
                (const char *const[]){ "clock.settings, line 7: label wants one word; ~ is kept" }, 1,
                "a word of blanks is no word");
    settings_release(&s);
}

static void TestWrongLines(void)
{
    BoxSettings s;
    Assemble(&s, g_clock,
             "settings 2\n"
             "hours 13\n"
             "hourz 12\n"
             "brightness 3\n"
             "seconds maybe\n"
             "seconds no\n"
             "color default\n"
             "width\n"
             "note bad\x01\n"
             "width 50\n"
             "settings 1\n"
             "note\n");
    ValueIs(&s, "hours", "24", false);
    ValueIs(&s, "seconds", "no", true);
    ValueIs(&s, "color", "#E6E6E6", false);
    ValueIs(&s, "width", "8", false);
    ValueIs(&s, "note", "", false);
    static const char *const want[] = {
        "clock.settings says settings 2; it is read as settings 1",
        "clock.settings, line 2: hours wants one of 24, 12; 24 is kept",
        "clock.settings, line 3: clock has no setting hourz — did you mean hours?",
        "clock.settings, line 4: clock has no setting brightness; the line is left as it is",
        "clock.settings, line 5: seconds wants yes or no; yes is kept",
        "clock.settings, line 6: seconds is set before, on line 5; this line is taken",
        "clock.settings, line 8: width wants a whole number from 5 to 40 cells; 8 is kept",
        "clock.settings, line 9 is not text; it is passed over",
        "clock.settings, line 10: width is set before, on line 8; this line is taken",
        "clock.settings, line 10: width wants a whole number from 5 to 40 cells; 8 is kept",
        "clock.settings, line 12: note wants text on one line; \"\" is kept",
    };
    ProblemsAre(&s, want, sizeof(want) / sizeof(want[0]), "every line not taken named, in order");
    settings_release(&s);
}

static void TestDeclarationWrong(void)
{
    BoxSettings s;
    Assemble(&s,
             "settings 1\n"
             "setting look gradient default x \"a later kind\"\n"
             "setting hours one-of 24 default 24 \"one choice\"\n"
             "setting seconds yes-or-no default yes \"whether the seconds are shown\"\n",
             "seconds no\nlook x\n");
    Check(s.Count == 1, "only the setting it can read is declared");
    ValueIs(&s, "seconds", "no", true);
    static const char *const want[] = {
        "clock declares a setting it cannot have, line 2: the setting look is of a kind this reader does not "
        "know; it is passed over",
        "clock declares a setting it cannot have, line 3: the setting hours does not have two or more different "
        "words to choose from; it is passed over",
        "clock.settings, line 2: clock has no setting look; the line is left as it is",
    };
    ProblemsAre(&s, want, sizeof(want) / sizeof(want[0]), "a declaration made elsewhere, lines it cannot read");
    settings_release(&s);

    Assemble(&s, "setting a word default b \"c\"\n", "a x\n");
    static const char *const headless[] = {
        "clock's settings do not begin with the line \"settings 1\"; it has none",
        "clock.settings, line 1: clock has no setting a; the line is left as it is",
    };
    ProblemsAre(&s, headless, 2, "a declaration without its first line");
    settings_release(&s);

    Assemble(&s, "", "hours 12\n");
    Check(s.Count == 0, "a program that declares none");
    settings_release(&s);
}

static void TestCheck(void)
{
    BoxSettings s;
    Assemble(&s, g_clock, NULL);
    Check(settings_check(&s, "hours", "12") == 0, "12 fits hours");
    Check(settings_check(&s, "width", "12 cells") == 0, "a number with its unit fits");
    struct { const char *name, *value, *why; } refused[] = {
        { "hours", "13", "hours wants one of 24, 12" },
        { "seconds", "", "seconds wants yes or no" },
        { "color", "green", "color wants a colour written #RRGGBB" },
        { "width", "41", "width wants a whole number from 5 to 40 cells" },
        { "label", "a b", "label wants one word" },
        { "note", "a\nb", "note wants text on one line" },
        { "hourz", "12", "clock has no setting hourz — did you mean hours?" },
        { "volume", "3", "clock has no setting volume" },
    };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        char label[200];
        int  rc = settings_check(&s, refused[i].name, refused[i].value);
        snprintf(label, sizeof(label), "%s '%s' refused: %s (said %s)", refused[i].name, refused[i].value,
                 refused[i].why, s.Why ? s.Why : "nothing");
        Check(rc == -ERR_INVALID_ARGUMENT && s.Why && strcmp(s.Why, refused[i].why) == 0, label);
    }
    settings_release(&s);
}

static void TestReassemble(void)
{
    BoxSettings s;
    Assemble(&s, g_clock, "hours 12\n");
    ValueIs(&s, "hours", "12", true);
    free(s.Chosen);
    s.Chosen     = Copy("seconds no\nhours 99\n");
    s.ChosenSize = strlen(s.Chosen);
    Check(settings_reassemble(&s) == 0, "put together again");
    ValueIs(&s, "hours", "24", false);
    ValueIs(&s, "seconds", "no", true);
    ProblemsAre(&s, (const char *const[]){ "clock.settings, line 2: hours wants one of 24, 12; 24 is kept" }, 1,
                "the problems are the file's as it is now");
    settings_release(&s);
}

int main(void)
{
    TestNoFile();
    TestFile();
    TestWrongLines();
    TestDeclarationWrong();
    TestCheck();
    TestReassemble();
    printf("settings, put together: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
