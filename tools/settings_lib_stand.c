/* settings_lib_stand — BoxLib's settings put together (src/settings_read.c)
 * on the host, held to exact outcomes:
 *
 *     tools/settings_stand.sh
 *
 * Only BoxLib's own headers are seen here, so the source is built as it is
 * in BoxOS. A case is a declaration and a person's file; its outcome is each
 * setting's path and value and whether the person chose it, what each
 * context's block sets, and every sentence said of a line not taken, word
 * for word, in order. */

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
    "hours one-of 24 12 default 24 \"how the hours are counted\"\n"
    "seconds on-or-off default on \"whether the seconds are shown\"\n"
    "digits \"the digits of the time\"\n"
    "    color color default #E6E6E6 \"their color\"\n"
    "    width number from 5 to 40 cells default 8 next-start \"how wide they are\"\n"
    "label word default ~ \"what stands before the time\"\n"
    "    color color default gray \"its color\"\n"
    "note text default \"\" \"said under the time\"\n"
    "blink number from 100 to 2000 ms or off default off \"how often the colons blink\"\n";

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

static void ValueIs(const BoxSettings *settings, const char *path, const char *value, bool chosen)
{
    char              label[160];
    const BoxSetting *setting = settings_find(settings, path);
    snprintf(label, sizeof(label), "%s is '%s'%s (it is '%s'%s)", path, value, chosen ? ", chosen" : "",
             setting ? setting->Value.Text : "none", setting && setting->Chosen ? ", chosen" : "");
    Check(setting && strcmp(setting->Value.Text, value) == 0 && setting->Chosen == chosen, label);
}

static void InContextIs(const BoxSettings *settings, size_t at, const char *tags, const char *path, const char *value)
{
    char label[200];
    bool held = at < settings->InContextCount;
    const BoxSettingInContext *in = held ? &settings->InContext[at] : NULL;
    held = held && strcmp(in->Tags, tags) == 0 && strcmp(settings->Settings[in->Setting].Path, path) == 0 &&
           strcmp(in->Value.Text, value) == 0;
    snprintf(label, sizeof(label), "in context %zu: [%s] %s is '%s'", at, tags, path, value);
    Check(held, label);
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
    Check(s.Count == 9, "nine settings declared, the thing that holds two among them");
    ValueIs(&s, "hours", "24", false);
    ValueIs(&s, "note", "", false);
    ValueIs(&s, "blink", "off", false);
    const BoxSetting *digits = settings_find(&s, "digits");
    const BoxSetting *color  = settings_find(&s, "digits color");
    const BoxSetting *width  = settings_find(&s, "digits  width");
    const BoxSetting *label  = settings_find(&s, "label color");
    Check(digits && digits->Kind == SETTING_GROUP && digits->Depth == 0 && strcmp(digits->Value.Text, "") == 0 &&
              strcmp(digits->What, "the digits of the time") == 0,
          "the thing that only holds others, its value empty");
    Check(color && strcmp(color->Path, "digits color") == 0 && strcmp(color->Name, "color") == 0 &&
              color->Depth == 1 && color->Value.Color == 0xE6E6E6u,
          "a setting inside it: its path, its name, its depth, its value as its kind");
    Check(width && width->Value.Number == 8 && width->NextStart && strcmp(width->Unit, "cells") == 0,
          "a path found with two blanks between its names");
    Check(label && label->Value.Color == 0x808080u && strcmp(label->Value.Text, "gray") == 0,
          "a color's name for its default, as written and as its color");
    Check(settings_find(&s, "seconds")->Value.On && settings_find(&s, "blink")->Off &&
              settings_find(&s, "blink")->Value.Number == 0,
          "on, and off read as 0");
    Check(strcmp(settings_find(&s, "hours")->Choices, "24 12") == 0, "the choices kept");
    Check(s.ProblemCount == 0 && s.InContextCount == 0, "nothing said and nothing in a context with no file");
    Check(!settings_find(&s, "color") && !settings_find(&s, "hour") && !settings_find(&s, "") &&
              !settings_find(&s, "digits color width"),
          "no setting for a name not at the top, not declared, none, or a path too long");
    settings_release(&s);
}

static void TestFile(void)
{
    BoxSettings s;
    Assemble(&s, g_clock,
             "# twelve hours, like the old one\n"
             "hours 12\n"
             "seconds off\n"
             "digits\n"
             "    color amber\n"
             "\twidth 30 cells\n"
             "label \"  \"\n"
             "note \"default\"\n"
             "blink 500 ms\n"
             "label color pink\n"
             "[work]\n"
             "    hours 24\n"
             "    digits color red\n"
             "    blink default\n");
    ValueIs(&s, "hours", "12", true);
    ValueIs(&s, "seconds", "off", true);
    ValueIs(&s, "digits color", "amber", true);
    ValueIs(&s, "digits width", "30", true);
    ValueIs(&s, "label", "~", false);
    ValueIs(&s, "note", "default", true);
    ValueIs(&s, "blink", "500", true);
    ValueIs(&s, "label color", "pink", true);
    Check(!settings_find(&s, "seconds")->Value.On && settings_find(&s, "digits color")->Value.Color == 0xFFB040u &&
              settings_find(&s, "digits width")->Value.Number == 30 && settings_find(&s, "blink")->Value.Number == 500,
          "each value read as its kind");
    Check(s.InContextCount == 3, "three values for the context work");
    InContextIs(&s, 0, "work", "hours", "24");
    InContextIs(&s, 1, "work", "digits color", "red");
    InContextIs(&s, 2, "work", "blink", "off");
    Check(s.InContextCount == 3 && s.InContext[1].Value.Color == 0xE04040u && s.InContext[1].Line == 13,
          "a context's value read as its kind, its line kept");
    ProblemsAre(&s, (const char *const[]){ "clock.settings, line 7: label wants one word; ~ is kept" }, 1,
                "a word of blanks is no word");
    settings_release(&s);
}

static void TestWrongLines(void)
{
    BoxSettings s;
    Assemble(&s, g_clock,
             "hours 13\n"
             "hourz 12\n"
             "brightness 3\n"
             "seconds maybe\n"
             "seconds off\n"
             "color red\n"
             "digits colr red\n"
             "digits 5\n"
             "width\n"
             "note bad\x01\n"
             "label\n"
             "    color\n"
             "blink 0\n"
             "[work\n"
             "    hours 12\n"
             "[home]\n"
             "    hours 99\n"
             "    hours 12\n"
             "    hours 24\n"
             "[empty]\n"
             "seconds on\n"
             "digits\n"
             "    width\n"
             "        size 3\n"
             "digits pink\n");
    ValueIs(&s, "hours", "24", false);
    ValueIs(&s, "seconds", "on", true);
    ValueIs(&s, "label color", "gray", false);
    ValueIs(&s, "blink", "off", false);
    Check(s.InContextCount == 1, "one value in a context");
    InContextIs(&s, 0, "home", "hours", "24");
    static const char *const want[] = {
        "clock.settings, line 1: hours wants one of 24, 12; 24 is kept",
        "clock.settings, line 2: clock has no setting hourz — did you mean hours?",
        "clock.settings, line 3: clock has no setting brightness; the line is left as it is",
        "clock.settings, line 4: seconds wants on or off; on is kept",
        "clock.settings, line 5: seconds is set before, on line 4; this line is taken",
        "clock.settings, line 6: clock has no setting color — did you mean digits color?",
        "clock.settings, line 7: digits has no setting colr inside it — did you mean digits color?",
        "clock.settings, line 8: digits has no setting 5 inside it — did you mean digits width 5?",
        "clock.settings, line 9: clock has no setting width — did you mean digits width?",
        "clock.settings, line 10 is not text; it is passed over",
        "clock.settings, line 12: label color wants a color: a name such as amber, or #RRGGBB; gray is kept",
        "clock.settings, line 13: blink wants a whole number from 100 to 2000 ms, or off; off is kept",
        "clock.settings, line 14: the line is no context: tags between [ and ], at least one, none twice, nothing "
        "after the ]; it is passed over",
        "clock.settings, line 15: it stands inside line 14, which is not taken; it is passed over",
        "clock.settings, line 17: [home] hours wants one of 24, 12; the line is passed over",
        "clock.settings, line 19: [home] hours is set before, on line 18; this line is taken",
        "clock.settings, line 20: the line holds nothing: the lines for that context go below it, indented; it is "
        "passed over",
        "clock.settings, line 21: seconds is set before, on line 5; this line is taken",
        "clock.settings, line 23: digits width wants a whole number from 5 to 40 cells; 8 is kept",
        "clock.settings, line 24: digits width holds nothing; size is not inside it",
        "clock.settings, line 25: digits has no setting pink inside it — did you mean digits color pink?",
    };
    ProblemsAre(&s, want, sizeof(want) / sizeof(want[0]), "every line not taken named, in order");
    settings_release(&s);
}

static void TestDeclarationWrong(void)
{
    BoxSettings s;
    Assemble(&s,
             "settings 1\n"
             "look gradient default x \"a later kind\"\n"
             "    shade color default red \"inside it\"\n"
             "hours one-of 24 default 24 \"one choice\"\n"
             "seconds on-or-off default on \"whether the seconds are shown\"\n",
             "seconds off\nlook x\n");
    Check(s.Count == 1, "only the setting it can read is declared");
    ValueIs(&s, "seconds", "off", true);
    static const char *const want[] = {
        "clock declares a setting it cannot have, line 2: the setting look is of a kind this reader does not "
        "know; it is passed over",
        "clock declares a setting it cannot have, line 3: the setting shade stands inside a line that is not "
        "taken; it is passed over",
        "clock declares a setting it cannot have, line 4: the setting hours does not have two or more different "
        "words to choose from; it is passed over",
        "clock.settings, line 2: clock has no setting look; the line is left as it is",
    };
    ProblemsAre(&s, want, sizeof(want) / sizeof(want[0]), "a declaration made elsewhere, lines it cannot read");
    settings_release(&s);

    Assemble(&s, "a word default b \"c\"\n", "a x\n");
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
    Check(settings_check(&s, "digits width", "12 cells") == 0, "a number with its unit fits, by its path");
    Check(settings_check(&s, "blink", "off") == 0 && settings_check(&s, "digits color", "amber") == 0,
          "off, and a color by name");
    struct { const char *path, *value, *why; } refused[] = {
        { "hours", "13", "hours wants one of 24, 12" },
        { "seconds", "", "seconds wants on or off" },
        { "digits color", "greenish", "digits color wants a color: a name such as amber, or #RRGGBB" },
        { "digits width", "41", "digits width wants a whole number from 5 to 40 cells" },
        { "label", "a b", "label wants one word" },
        { "note", "a\nb", "note wants text on one line" },
        { "blink", "0", "blink wants a whole number from 100 to 2000 ms, or off" },
        { "digits", "x", "digits wants no value of its own: it holds others" },
        { "hourz", "12", "clock has no setting hourz — did you mean hours?" },
        { "digits colr", "x", "digits has no setting colr inside it — did you mean digits color?" },
        { "color", "red", "clock has no setting color — did you mean digits color?" },
        { "volume", "3", "clock has no setting volume" },
        { "label", "colr pink", "label has no setting colr inside it — did you mean label color?" },
        { "digits amber", "", "digits has no setting amber inside it — did you mean digits color amber?" },
        { "digits 12", "", "digits has no setting 12 inside it — did you mean digits width 12?" },
        { "digits 99", "", "digits has no setting 99 inside it" },
        { "", "x", "which setting of clock? Say its name, or its whole path" },
    };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        char label[240];
        int  rc = settings_check(&s, refused[i].path, refused[i].value);
        snprintf(label, sizeof(label), "%s '%s' refused: %s (said %s)", refused[i].path, refused[i].value,
                 refused[i].why, s.Why ? s.Why : "nothing");
        Check(rc == -ERR_INVALID_ARGUMENT && s.Why && strcmp(s.Why, refused[i].why) == 0, label);
    }
    settings_release(&s);
}

static void TestReassemble(void)
{
    BoxSettings s;
    Assemble(&s, g_clock, "hours 12\n[work]\n    hours 24\n");
    ValueIs(&s, "hours", "12", true);
    free(s.Chosen);
    s.Chosen     = Copy("seconds off\nhours 99\n");
    s.ChosenSize = strlen(s.Chosen);
    Check(settings_reassemble(&s) == 0, "put together again");
    ValueIs(&s, "hours", "24", false);
    ValueIs(&s, "seconds", "off", true);
    Check(s.InContextCount == 0, "the context's value gone with its line");
    ProblemsAre(&s, (const char *const[]){ "clock.settings, line 2: hours wants one of 24, 12; 24 is kept" }, 1,
                "the problems are the file's as it is now");
    settings_release(&s);
}

/* What holds for `path` while the Use Context is `context`: the block of
 * `tags` with `value`, or for NULL tags none — what is set everywhere. */
static void HoldsIs(const BoxSettings *settings, const char *context, const char *path, const char *tags,
                    const char *value)
{
    char                       label[200];
    const BoxSetting          *setting = settings_find(settings, path);
    const BoxSettingInContext *in      = setting ? settings_in_context(settings, (size_t)(setting - settings->Settings),
                                                                        context)
                                                 : NULL;
    bool                       held    = setting && (tags ? in && strcmp(in->Tags, tags) == 0 &&
                                                               strcmp(in->Value.Text, value) == 0
                                                          : !in && strcmp(setting->Value.Text, value) == 0);
    snprintf(label, sizeof(label), "in the Use Context '%s' %s holds %s from %s%s%s (it is %s%s%s)",
             context ? context : "(none)", path, value, tags ? "[" : "everywhere", tags ? tags : "", tags ? "]" : "",
             in ? "[" : "everywhere", in ? in->Tags : "", in ? "]" : "");
    Check(held, label);
}

/* Which block holds in which Use Context: the one whose tags the context
 * holds every one of, of those the one of the most tags, of as many the one
 * written last; "key:" is the bare key, as the kernel spells it. */
static void TestInContext(void)
{
    BoxSettings s;
    Assemble(&s, g_clock,
             "hours 12\n"
             "[work]\n"
             "    hours 24\n"
             "    blink 500 ms\n"
             "[work, late]\n"
             "    hours 12\n"
             "[home]\n"
             "    hours 24\n"
             "[project:alpha]\n"
             "    blink 1000 ms\n"
             "[late]\n"
             "    digits color red\n"
             "[work:]\n"
             "    seconds off\n"
             "[late work]\n"
             "    digits color green\n"
             "[late]\n"
             "    digits color blue\n");
    ProblemsAre(&s,
                (const char *const[]){
                    "clock.settings, line 18: [late] digits color is set before, on line 12; this line is taken" },
                1, "a setting set again in a block of the same tags: the last line counts");
    HoldsIs(&s, NULL, "hours", NULL, "12");
    HoldsIs(&s, "", "hours", NULL, "12");
    HoldsIs(&s, "other", "hours", NULL, "12");
    HoldsIs(&s, "work", "hours", "work", "24");
    HoldsIs(&s, "work", "blink", "work", "500");
    HoldsIs(&s, "work", "seconds", "work:", "off");
    HoldsIs(&s, "work", "digits color", NULL, "#E6E6E6");
    HoldsIs(&s, "work,late", "hours", "work, late", "12");
    HoldsIs(&s, "late,work", "hours", "work, late", "12");
    HoldsIs(&s, "late, work, home", "hours", "work, late", "12");
    HoldsIs(&s, "home,work", "hours", "home", "24");
    HoldsIs(&s, "work,home", "hours", "home", "24");
    HoldsIs(&s, "late", "hours", NULL, "12");
    HoldsIs(&s, "late", "digits color", "late", "blue");
    HoldsIs(&s, "late,work", "digits color", "late work", "green");
    HoldsIs(&s, "project:alpha", "blink", "project:alpha", "1000");
    HoldsIs(&s, "project", "blink", NULL, "off");
    HoldsIs(&s, "project:beta", "blink", NULL, "off");
    HoldsIs(&s, "work,project:alpha", "blink", "project:alpha", "1000");
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
    TestInContext();
    printf("settings, put together: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
