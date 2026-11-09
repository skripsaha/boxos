/* settings_stand — the one reader and the one pen of a program's settings
 * (src/include/settings_reader.h, settings_pen.h), built for the host and
 * held to exact outcomes:
 *
 *     tools/settings_stand.sh
 *
 * a declaration of every kind read field for field; each way a declaration or
 * one of its lines can be wrong, to its verdict; every kind's values, good and
 * bad; the names a typo is near; the person's file line by line — blanks,
 * comments, quotes, CR LF, `when`, the bare word default; the pen's changes
 * byte for byte, every value it writes read back as it was; and a reader fed
 * bytes made at random, which must read every one of them inside its bounds
 * (the stand is built under the address and undefined-behaviour sanitizers).
 * The last line counts the cases that held. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_pen.h"

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

static SettingSpan Span(const char *text)
{
    return (SettingSpan){ (const uint8_t *)text, strlen(text) };
}

static bool Is(SettingSpan span, const char *text)
{
    return span.Length == strlen(text) && memcmp(span.At, text, span.Length) == 0;
}

/* ── the declaration ─────────────────────────────────────────────────────── */

static const char g_declaration[] =
    "settings 1\n"
    "setting history yes-or-no default yes next-start \"keep what is typed from one start to the next\"\n"
    "setting prompt word default ~ \"what stands before what you type\"\n"
    "later a line this reader does not know\n"
    "\n"
    "setting width number from 24 to 200 cells default 72 \"how wide the column is\"\n"
    "setting look one-of dark light dim default dark \"how the page looks\"\n"
    "setting prompt-color color default #FFB020 \"the colour of the prompt\"\n"
    "setting greeting text default \"BoxOS Hub\" \"the line said at start\"\n"
    "setting shift number from -10 to 10 default -3 \"how far it moves\"\n"
    "setting quiet text default \"\" \"what it says when nothing is wrong\"";

static void TestDeclared(void)
{
    SettingsDeclared declared;
    SettingsVerdict  verdict;
    Setting          s[8];
    unsigned         count = 0;
    Check(SettingsDeclaredBegin((const uint8_t *)g_declaration, strlen(g_declaration), &declared) == SETTINGS_OK,
          "a declaration of every kind opens");
    while (count < 8 && SettingsDeclaredNext(&declared, &s[count], &verdict)) {
        Check(verdict == SETTINGS_OK, "every setting of the declaration is read");
        count++;
    }
    Check(count == 8 && !SettingsDeclaredNext(&declared, &s[0], &verdict), "eight settings, no line taken for one");

    Check(Is(s[0].Name, "history") && s[0].Kind == SETTING_YES_OR_NO && Is(s[0].Default, "yes") &&
              s[0].NextStart && Is(s[0].What, "keep what is typed from one start to the next") && s[0].Line == 2,
          "yes-or-no, next-start, its line");
    Check(Is(s[1].Name, "prompt") && s[1].Kind == SETTING_WORD && Is(s[1].Default, "~") && !s[1].NextStart,
          "word");
    Check(Is(s[2].Name, "width") && s[2].Kind == SETTING_NUMBER && s[2].From == 24 && s[2].To == 200 &&
              Is(s[2].Unit, "cells") && Is(s[2].Default, "72") && s[2].Line == 6,
          "number from to with a unit, its line past one passed over");
    Check(Is(s[3].Name, "look") && s[3].Kind == SETTING_ONE_OF && Is(s[3].Choices, "dark light dim") &&
              Is(s[3].Default, "dark"),
          "one-of, its choices");
    Check(Is(s[4].Name, "prompt-color") && s[4].Kind == SETTING_COLOR && Is(s[4].Default, "#FFB020"), "color");
    Check(Is(s[5].Name, "greeting") && s[5].Kind == SETTING_TEXT && Is(s[5].Default, "BoxOS Hub") &&
              Is(s[5].What, "the line said at start"),
          "text with a quoted default");
    Check(s[6].From == -10 && s[6].To == 10 && Is(s[6].Default, "-3") && s[6].Unit.Length == 0,
          "number below zero, no unit");
    Check(Is(s[7].Name, "quiet") && s[7].Default.Length == 0, "text with an empty default, the last line unended");

    Setting found;
    Check(SettingsDeclaredFind((const uint8_t *)g_declaration, strlen(g_declaration), Span("look"), &found) &&
              found.Kind == SETTING_ONE_OF,
          "a setting found by name");
    Check(!SettingsDeclaredFind((const uint8_t *)g_declaration, strlen(g_declaration), Span("loo"), &found),
          "no setting for a name it does not declare");
}

/* The verdict on the one setting line of "settings 1\n" + line. */
static void LineVerdict(const char *line, SettingsVerdict want, const char *what)
{
    char             text[512];
    SettingsDeclared declared;
    SettingsVerdict  verdict = SETTINGS_OK;
    Setting          setting;
    snprintf(text, sizeof(text), "settings 1\n%s\n", line);
    bool opened = SettingsDeclaredBegin((const uint8_t *)text, strlen(text), &declared) == SETTINGS_OK;
    bool read   = opened && SettingsDeclaredNext(&declared, &setting, &verdict);
    char label[600];
    snprintf(label, sizeof(label), "%s: %s (got %d)", what, line, (int)verdict);
    Check(read && verdict == want, label);
}

static void TestDeclaredWrong(void)
{
    SettingsDeclared declared;
    struct { const char *text; SettingsVerdict want; const char *what; } heads[] = {
        { "", SETTINGS_NO_HEAD, "no bytes" },
        { "setting a word default b \"c\"\n", SETTINGS_NO_HEAD, "no head line" },
        { "settings 2\n", SETTINGS_NO_HEAD, "a later version" },
        { "settings 1 more\n", SETTINGS_NO_HEAD, "words after the version" },
        { "\"settings\" 1\n", SETTINGS_NO_HEAD, "a quoted head" },
        { "settings 1\nsetting a\tword default b \"c\"\n", SETTINGS_NOT_TEXT, "a tab" },
        { "settings 1\r\n", SETTINGS_NOT_TEXT, "a CR" },
        { "settings 1\nsetting a word default b \"c\x7f\"\n", SETTINGS_NOT_TEXT, "a DEL" },
    };
    for (unsigned i = 0; i < sizeof(heads) / sizeof(heads[0]); i++)
        Check(SettingsDeclaredBegin((const uint8_t *)heads[i].text, strlen(heads[i].text), &declared) ==
                  heads[i].want,
              heads[i].what);
    Check(SettingsDeclaredBegin((const uint8_t *)"\n\n  settings 1\n", 15, &declared) == SETTINGS_OK,
          "blank lines and blanks before the head");

    LineVerdict("setting Prompt word default ~ \"x\"", SETTING_NAME_BAD, "a capital in the name");
    LineVerdict("setting 2nd word default ~ \"x\"", SETTING_NAME_BAD, "a name beginning with a digit");
    LineVerdict("setting prompt- word default ~ \"x\"", SETTING_NAME_BAD, "a name ending in -");
    LineVerdict("setting prompt_x word default ~ \"x\"", SETTING_NAME_BAD, "an underscore in the name");
    LineVerdict("setting when word default ~ \"x\"", SETTING_NAME_BAD, "the name when");
    LineVerdict("setting settings word default ~ \"x\"", SETTING_NAME_BAD, "the name settings");
    LineVerdict("setting abcdefghijklmnopqrstuvwxyzabcdefg word default ~ \"x\"", SETTING_NAME_BAD,
                "a name of 33 bytes");
    LineVerdict("setting abcdefghijklmnopqrstuvwxyzabcdef word default ~ \"x\"", SETTINGS_OK, "a name of 32 bytes");
    LineVerdict("setting \"prompt\" word default ~ \"x\"", SETTING_NAME_BAD, "a quoted name");
    LineVerdict("setting", SETTING_NAME_BAD, "no name");
    LineVerdict("setting prompt gradient default ~ \"x\"", SETTING_KIND_UNKNOWN, "a kind this reader does not know");
    LineVerdict("setting prompt", SETTING_KIND_UNKNOWN, "no kind");
    LineVerdict("setting w number from 9 to 3 default 5 \"x\"", SETTING_LIMITS_BAD, "from above to");
    LineVerdict("setting w number from 3 default 5 \"x\"", SETTING_LIMITS_BAD, "no to");
    LineVerdict("setting w number default 5 \"x\"", SETTING_LIMITS_BAD, "no limits");
    LineVerdict("setting w number from 03 to 9 default 5 \"x\"", SETTING_LIMITS_BAD, "a leading zero");
    LineVerdict("setting w number from -0 to 9 default 5 \"x\"", SETTING_LIMITS_BAD, "-0");
    LineVerdict("setting w number from 1 to 99999999999999999999 default 5 \"x\"", SETTING_LIMITS_BAD,
                "a limit past 64 bits");
    LineVerdict("setting w number from -9223372036854775808 to 9223372036854775807 default 0 \"x\"", SETTINGS_OK,
                "the widest limits");
    LineVerdict("setting w number from 1 to 9 ms2 default 5 \"x\"", SETTING_UNIT_BAD, "a unit with a digit");
    LineVerdict("setting w number from 5 to 5 default 5 \"x\"", SETTINGS_OK, "one number to choose");
    LineVerdict("setting l one-of dark default dark \"x\"", SETTING_CHOICES_BAD, "one choice");
    LineVerdict("setting l one-of dark dark default dark \"x\"", SETTING_CHOICES_BAD, "a choice twice");
    LineVerdict("setting l one-of dark \"light\" default dark \"x\"", SETTING_CHOICES_BAD, "a quoted choice");
    LineVerdict("setting l one-of dark next-start default dark \"x\"", SETTING_CHOICES_BAD, "next-start as a choice");
    LineVerdict("setting l one-of dark light", SETTING_NO_DEFAULT, "choices and no default after them");
    LineVerdict("setting p word \"x\"", SETTING_NO_DEFAULT, "no default");
    LineVerdict("setting p word default", SETTING_NO_DEFAULT, "default and no value");
    LineVerdict("setting p word default default \"x\"", SETTING_DEFAULT_BAD, "the bare word default as a default");
    LineVerdict("setting p word default \"default\" \"x\"", SETTINGS_OK, "a quoted default of the word default");
    LineVerdict("setting w number from 1 to 9 default 10 \"x\"", SETTING_DEFAULT_BAD, "a default outside the limits");
    LineVerdict("setting l one-of dark light default dim \"x\"", SETTING_DEFAULT_BAD, "a default not a choice");
    LineVerdict("setting c color default #FFB02 \"x\"", SETTING_DEFAULT_BAD, "a colour of five digits");
    LineVerdict("setting p word default \"two words\" \"x\"", SETTING_DEFAULT_BAD, "a word default with a blank");
    LineVerdict("setting p word default \"\" \"x\"", SETTING_DEFAULT_BAD, "an empty word default");
    LineVerdict("setting h yes-or-no default true \"x\"", SETTING_DEFAULT_BAD, "true for yes or no");
    LineVerdict("setting p word default ~", SETTING_WHAT_MISSING, "no description");
    LineVerdict("setting p word default ~ next-start", SETTING_WHAT_MISSING, "next-start and no description");
    LineVerdict("setting p word default ~ \"\"", SETTING_WHAT_MISSING, "an empty description");
    LineVerdict("setting p word default ~ what", SETTING_WHAT_MISSING, "a description not in quotes");
    LineVerdict("setting p word default ~ \"x\" more", SETTING_WORDS_AFTER, "words after the description");
    LineVerdict("setting p word default ~ \"x\" \"y\"", SETTING_WORDS_AFTER, "two descriptions");
    LineVerdict("setting p word default ~ \"what", SETTING_LINE_UNREAD, "an unclosed quote");
    LineVerdict("setting p word default ~ \"what\"s", SETTING_LINE_UNREAD, "a quote not ended by a blank");
    LineVerdict("setting p word default ~ next-start \"x\"", SETTINGS_OK, "next-start before the description");

    static const char twice[] = "settings 1\nsetting p word default ~ \"x\"\nsetting p word default ~ \"y\"\n";
    Setting          first, second;
    SettingsVerdict  v1, v2;
    SettingsDeclaredBegin((const uint8_t *)twice, strlen(twice), &declared);
    Check(SettingsDeclaredNext(&declared, &first, &v1) && v1 == SETTINGS_OK &&
              SettingsDeclaredNext(&declared, &second, &v2) && v2 == SETTING_NAME_TWICE && second.Line == 3,
          "a name declared twice: the first holds, the second is refused");
    Setting found;
    Check(SettingsDeclaredFind((const uint8_t *)twice, strlen(twice), Span("p"), &found) && Is(found.What, "x"),
          "the first of two is the one found");

    static const char mixed[] =
        "settings 1\nsetting a gradient default x \"x\"\nsetting b word default ~ \"y\"\n";
    Setting          one, two;
    SettingsVerdict  u1, u2;
    SettingsDeclaredBegin((const uint8_t *)mixed, strlen(mixed), &declared);
    Check(SettingsDeclaredNext(&declared, &one, &u1) && u1 == SETTING_KIND_UNKNOWN && Is(one.Name, "a") &&
              SettingsDeclaredNext(&declared, &two, &u2) && u2 == SETTINGS_OK && Is(two.Name, "b"),
          "a setting of an unknown kind is named and the next is read");
}

/* ── values ──────────────────────────────────────────────────────────────── */

static Setting DeclaredOne(const char *name)
{
    Setting setting;
    if (!SettingsDeclaredFind((const uint8_t *)g_declaration, strlen(g_declaration), Span(name), &setting)) {
        printf("FAIL the stand's declaration has no %s\n", name);
        exit(2);
    }
    return setting;
}

static void ValueIs(const char *name, const char *value, SettingsVerdict want, const char *what)
{
    Setting      setting = DeclaredOne(name);
    SettingValue read;
    char         label[300];
    SettingsVerdict got = SettingValueRead(&setting, Span(value), &read);
    snprintf(label, sizeof(label), "%s: %s '%s' (got %d)", what, name, value, (int)got);
    Check(got == want, label);
}

static void TestValues(void)
{
    Setting      setting;
    SettingValue value;

    setting = DeclaredOne("history");
    Check(SettingValueRead(&setting, Span("yes"), &value) == SETTINGS_OK && value.Yes, "yes");
    Check(SettingValueRead(&setting, Span("no"), &value) == SETTINGS_OK && !value.Yes, "no");
    ValueIs("history", "Yes", SETTING_VALUE_NOT_YES_OR_NO, "a capital");
    ValueIs("history", "1", SETTING_VALUE_NOT_YES_OR_NO, "a digit");
    ValueIs("history", "", SETTING_VALUE_EMPTY, "nothing");

    setting = DeclaredOne("width");
    Check(SettingValueRead(&setting, Span("72"), &value) == SETTINGS_OK && value.Number == 72 &&
              Is(value.Text, "72"),
          "a number");
    Check(SettingValueRead(&setting, Span("24 cells"), &value) == SETTINGS_OK && value.Number == 24 &&
              Is(value.Text, "24"),
          "a number with its unit, the lower limit");
    Check(SettingValueRead(&setting, Span("200\t cells"), &value) == SETTINGS_OK && value.Number == 200,
          "the upper limit, blanks before the unit");
    ValueIs("width", "23", SETTING_VALUE_OUTSIDE, "below the limits");
    ValueIs("width", "201", SETTING_VALUE_OUTSIDE, "above the limits");
    ValueIs("width", "72 rows", SETTING_VALUE_NOT_NUMBER, "another unit");
    ValueIs("width", "72cells", SETTING_VALUE_NOT_NUMBER, "a unit not apart");
    ValueIs("width", "072", SETTING_VALUE_NOT_NUMBER, "a leading zero");
    ValueIs("width", "+72", SETTING_VALUE_NOT_NUMBER, "a plus");
    ValueIs("width", "7.5", SETTING_VALUE_NOT_NUMBER, "a fraction");
    ValueIs("width", "99999999999999999999", SETTING_VALUE_NOT_NUMBER, "past 64 bits");
    setting = DeclaredOne("shift");
    Check(SettingValueRead(&setting, Span("-10"), &value) == SETTINGS_OK && value.Number == -10, "below zero");
    ValueIs("shift", "-0", SETTING_VALUE_NOT_NUMBER, "-0");
    ValueIs("shift", "-", SETTING_VALUE_NOT_NUMBER, "a minus alone");
    ValueIs("shift", "-3 cells", SETTING_VALUE_NOT_NUMBER, "a unit for a number with none");

    ValueIs("prompt", "»", SETTINGS_OK, "a word of UTF-8");
    ValueIs("prompt", "w»", SETTINGS_OK, "a word");
    ValueIs("prompt", "two words", SETTING_VALUE_NOT_WORD, "two words");
    ValueIs("prompt", "a\"b", SETTING_VALUE_NOT_WORD, "a quote in a word");
    ValueIs("prompt", "a\x01", SETTING_VALUE_NOT_TEXT, "a control byte");

    ValueIs("greeting", "", SETTINGS_OK, "an empty text");
    ValueIs("greeting", " Hello there ", SETTINGS_OK, "a text with blanks");
    ValueIs("greeting", "tab\there", SETTINGS_OK, "a text with a tab");
    ValueIs("greeting", "line\nbreak", SETTING_VALUE_NOT_TEXT, "a text with a newline");

    ValueIs("look", "light", SETTINGS_OK, "a choice");
    ValueIs("look", "dim", SETTINGS_OK, "the last choice");
    ValueIs("look", "Light", SETTING_VALUE_NOT_CHOICE, "a choice with a capital");
    ValueIs("look", "dar", SETTING_VALUE_NOT_CHOICE, "the start of a choice");
    ValueIs("look", "dark light", SETTING_VALUE_NOT_CHOICE, "two choices");

    setting = DeclaredOne("prompt-color");
    Check(SettingValueRead(&setting, Span("#7cd68A"), &value) == SETTINGS_OK && value.Color == 0x7CD68Au,
          "a colour in both cases");
    Check(SettingValueRead(&setting, Span("#000000"), &value) == SETTINGS_OK && value.Color == 0, "black");
    ValueIs("prompt-color", "7CD68A", SETTING_VALUE_NOT_COLOR, "no #");
    ValueIs("prompt-color", "#7CD68", SETTING_VALUE_NOT_COLOR, "five digits");
    ValueIs("prompt-color", "#7CD68AA", SETTING_VALUE_NOT_COLOR, "seven digits");
    ValueIs("prompt-color", "#GGGGGG", SETTING_VALUE_NOT_COLOR, "not hex");
}

static void TestNear(void)
{
    struct { const char *typed, *meant; } near[] = {
        { "promt", "prompt" }, { "prmopt", "prompt" }, { "prompt-colour", "prompt-color" },
        { "widht", "width" }, { "histroy", "history" }, { "lok", "look" },
    };
    for (unsigned i = 0; i < sizeof(near) / sizeof(near[0]); i++) {
        Setting setting;
        char    label[100];
        snprintf(label, sizeof(label), "%s is near %s", near[i].typed, near[i].meant);
        Check(SettingsDeclaredNearest((const uint8_t *)g_declaration, strlen(g_declaration), Span(near[i].typed),
                                      &setting) && Is(setting.Name, near[i].meant),
              label);
    }
    const char *far[] = { "colour", "x", "zz", "lo", "abcdefghijklmnopqrstuvwxyzabcdefgh" };
    for (unsigned i = 0; i < sizeof(far) / sizeof(far[0]); i++) {
        Setting setting;
        char    label[100];
        snprintf(label, sizeof(label), "%s is near nothing", far[i]);
        Check(!SettingsDeclaredNearest((const uint8_t *)g_declaration, strlen(g_declaration), Span(far[i]),
                                       &setting),
              label);
    }
    Check(SettingDistance(Span("ab"), Span("ba")) == 1 && SettingDistance(Span("abc"), Span("abc")) == 0 &&
              SettingDistance(Span(""), Span("abc")) == 3 && SettingDistance(Span("kitten"), Span("sitting")) == 3,
          "distances");
}

/* ── the person's file ───────────────────────────────────────────────────── */

static void TestChosen(void)
{
    static const char file[] =
        "settings 1\n"
        "# the prompt I like\n"
        "   # a comment after blanks\n"
        "prompt   »  \n"
        "\n"
        "greeting \"  two edges  \"\r\n"
        "\t \n"
        "look default\n"
        "quiet \"default\"\n"
        "prompt-color #7CD68A\n"
        "when work prompt w»\n"
        "bad\x01line\n"
        "width\n"
        "prompt w»";
    SettingsChosen chosen;
    SettingsLine   l[16];
    unsigned       count = 0;
    SettingsChosenBegin((const uint8_t *)file, strlen(file), &chosen);
    while (count < 16 && SettingsChosenNext(&chosen, &l[count])) count++;
    Check(count == 14, "every line read, the last one unended");
    Check(l[0].Kind == SETTINGS_LINE_HEAD && Is(l[0].Value, "1") && l[0].Line == 1, "the head and its version");
    Check(l[1].Kind == SETTINGS_LINE_COMMENT && l[2].Kind == SETTINGS_LINE_COMMENT, "comments");
    Check(l[3].Kind == SETTINGS_LINE_SET && Is(l[3].Name, "prompt") && Is(l[3].Value, "»") && !l[3].Quoted &&
              !l[3].Default,
          "a value, its blanks at both ends taken off");
    Check(l[4].Kind == SETTINGS_LINE_BLANK && l[6].Kind == SETTINGS_LINE_BLANK, "blank lines, blanks only");
    Check(Is(l[5].Value, "  two edges  ") && l[5].Quoted && Is(l[5].Whole, "greeting \"  two edges  \""),
          "a quoted value as it stands, the CR taken off the line");
    Check(Is(l[7].Name, "look") && l[7].Default, "the bare word default");
    Check(Is(l[8].Value, "default") && l[8].Quoted && !l[8].Default, "the word default quoted");
    Check(Is(l[9].Value, "#7CD68A"), "a value beginning with #");
    Check(l[10].Kind == SETTINGS_LINE_WHEN, "a when line");
    Check(l[11].Kind == SETTINGS_LINE_NOT_TEXT && l[11].Line == 12, "a line that is not text");
    Check(Is(l[12].Name, "width") && l[12].Value.Length == 0 && !l[12].Quoted, "a name with no value");
    Check(l[13].Start + strlen("prompt w»") == strlen(file) && l[13].End == strlen(file), "where the last line is");

    SettingsLine found;
    Check(SettingsChosenFind((const uint8_t *)file, strlen(file), Span("prompt"), &found) && found.Line == 14 &&
              Is(found.Value, "w»"),
          "the last line for a name is the one that counts");
    Check(!SettingsChosenFind((const uint8_t *)file, strlen(file), Span("history"), &found), "no line for a name");
    Check(!SettingsChosenFind((const uint8_t *)file, strlen(file), Span("work"), &found),
          "a when line sets nothing");

    SettingsChosenBegin((const uint8_t *)"", 0, &chosen);
    Check(!SettingsChosenNext(&chosen, &l[0]), "no file, no lines");
}

/* ── the pen ─────────────────────────────────────────────────────────────── */

typedef enum { PEN_SET, PEN_ERASE, PEN_CLEAR } PenWhat;

/* The file a change makes, measured and written. */
static char *Pen(const char *file, PenWhat what, const char *name, const char *value)
{
    uint64_t measured, written;
    uint64_t size = strlen(file);
    if (what == PEN_SET)
        SettingsPenSet((const uint8_t *)file, size, Span(name), Span(value), NULL, &measured);
    else if (what == PEN_ERASE)
        SettingsPenErase((const uint8_t *)file, size, Span(name), NULL, &measured);
    else
        SettingsPenClear((const uint8_t *)file, size, (const uint8_t *)g_declaration, strlen(g_declaration), NULL,
                         &measured);
    char *out = malloc(measured + 1);
    if (!out) exit(2);
    if (what == PEN_SET)
        SettingsPenSet((const uint8_t *)file, size, Span(name), Span(value), (uint8_t *)out, &written);
    else if (what == PEN_ERASE)
        SettingsPenErase((const uint8_t *)file, size, Span(name), (uint8_t *)out, &written);
    else
        SettingsPenClear((const uint8_t *)file, size, (const uint8_t *)g_declaration, strlen(g_declaration),
                         (uint8_t *)out, &written);
    out[written] = '\0';
    if (written != measured) {
        printf("FAIL the pen measured %llu bytes and wrote %llu\n", (unsigned long long)measured,
               (unsigned long long)written);
        g_failed++;
    }
    return out;
}

static void PenIs(const char *file, PenWhat what, const char *name, const char *value, const char *want,
                  const char *label)
{
    char *got = Pen(file, what, name, value);
    if (strcmp(got, want) == 0) {
        g_passed++;
    } else {
        g_failed++;
        printf("FAIL %s:\n--- got\n%s\n--- wanted\n%s\n---\n", label, got, want);
    }
    free(got);
}

static void TestPen(void)
{
    PenIs("", PEN_SET, "prompt", "»", "settings 1\nprompt »\n", "the first setting makes the file");
    PenIs("settings 1\n# mine\nprompt »\nhistory no\n", PEN_SET, "prompt", "w»",
          "settings 1\n# mine\nprompt w»\nhistory no\n", "a line rewritten where it stands");
    PenIs("settings 1\nprompt a\n# keep\nprompt b\nlook dim\n", PEN_SET, "prompt", "c",
          "settings 1\n# keep\nprompt c\nlook dim\n", "two lines for one name: the last rewritten, the other gone");
    PenIs("settings 1\nhistory no\n", PEN_SET, "prompt", "»", "settings 1\nhistory no\nprompt »\n",
          "a new setting at the end");
    PenIs("settings 1\nhistory no", PEN_SET, "prompt", "»", "settings 1\nhistory no\nprompt »\n",
          "added after a last line with no newline");
    PenIs("history no\n", PEN_SET, "prompt", "»", "history no\nprompt »\n",
          "a file without a head keeps none");
    PenIs("settings 1\r\nhistory no\r\nprompt a\r\n", PEN_SET, "prompt", "b", "settings 1\r\nhistory no\r\nprompt b\n",
          "CR LF lines kept, the changed one written with LF");
    PenIs("settings 1\nwhen work prompt w»\n", PEN_SET, "prompt", "»", "settings 1\nwhen work prompt w»\nprompt »\n",
          "a when line is not the setting's line");
    PenIs("settings 1\npromptly yes\n", PEN_SET, "prompt", "»", "settings 1\npromptly yes\nprompt »\n",
          "a name that begins with the name is another name");
    PenIs("", PEN_SET, "greeting", "", "settings 1\ngreeting \"\"\n", "an empty text quoted");
    PenIs("", PEN_SET, "greeting", " hi ", "settings 1\ngreeting \" hi \"\n", "blanks at the ends quoted");
    PenIs("", PEN_SET, "prompt", "default", "settings 1\nprompt \"default\"\n", "the word default quoted");
    PenIs("", PEN_SET, "greeting", "\"hi\"", "settings 1\ngreeting \"\"hi\"\"\n", "quotes at both ends quoted");
    PenIs("", PEN_SET, "greeting", "say \"hi\" now", "settings 1\ngreeting say \"hi\" now\n",
          "quotes inside left as they are");

    PenIs("settings 1\nprompt a\n# keep\nprompt b\nlook dim\n", PEN_ERASE, "prompt", NULL,
          "settings 1\n# keep\nlook dim\n", "every line for a name erased");
    PenIs("settings 1\nlook dim\n", PEN_ERASE, "prompt", NULL, "settings 1\nlook dim\n",
          "erasing a name with no line changes nothing");
    PenIs("settings 1\n# mine\nprompt a\nother-version x\nwhen work prompt w\nlook dim\nwidth 30", PEN_CLEAR, NULL, NULL,
          "settings 1\n# mine\nother-version x\nwhen work prompt w\n",
          "everything as it came: comments, names not declared and when lines stay");
}

/* Every value the pen writes is read back as it was given. */
static void TestPenReadsBack(void)
{
    const char *values[] = { "»", "", " ", "  edges  ", "default", "\"", "\"\"", "\"a\"", "a\"", "\"a", "#7CD68A",
                             "two words", "tab\tinside", "\tlead", "trail\t", "x" };
    unsigned held = 0;
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char        *file = Pen("settings 1\n# c\ngreeting old\n", PEN_SET, "greeting", values[i]);
        SettingsLine line;
        bool ok = SettingsChosenFind((const uint8_t *)file, strlen(file), Span("greeting"), &line) &&
                  Is(line.Value, values[i]) && !line.Default;
        if (ok) held++;
        else printf("FAIL the value '%s' is read back as '%.*s'\n", values[i], (int)line.Value.Length, line.Value.At);
        free(file);
    }
    Check(held == sizeof(values) / sizeof(values[0]), "every value written is read back as it was");
}

/* ── bytes at random ─────────────────────────────────────────────────────── */

static uint64_t g_seed = 0x9E3779B97F4A7C15ull;

static uint64_t Random(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return g_seed;
}

/* Bytes made of the declaration's and the file's own words, cut and shuffled,
 * with stray bytes: the reader reads all of them, every span inside them. */
static void TestRandom(void)
{
    static const char *const pieces[] = {
        "settings 1\n", "setting ", "prompt ", "word ", "number ", "from ", "to ", "-3 ", "200 ", "one-of ",
        "dark ", "light ", "default ", "\"", "next-start ", "color ", "#FFB020 ", "text ", "yes-or-no ", "yes ",
        "\n", " ", "\t", "\r\n", "#", "when ", "x", "\x01", "\x7f", "\"\"", "cells ", "\xc2\xbb"
    };
    unsigned runs = 20000, inside = 0;
    for (unsigned run = 0; run < runs; run++) {
        uint8_t  bytes[256];
        uint64_t size = 0;
        unsigned count = (unsigned)(Random() % 24);
        for (unsigned i = 0; i < count; i++) {
            const char *piece = pieces[Random() % (sizeof(pieces) / sizeof(pieces[0]))];
            size_t      length = strlen(piece);
            if (size + length > sizeof(bytes)) break;
            memcpy(bytes + size, piece, length);
            size += length;
        }
        uint8_t *copy = malloc(size ? size : 1);   /* exactly as long: a read past it is caught */
        memcpy(copy, bytes, size);

        bool             fine = true;
        SettingsDeclared declared;
        Setting          setting;
        SettingsVerdict  verdict;
        if (SettingsDeclaredBegin(copy, size, &declared) == SETTINGS_OK)
            while (SettingsDeclaredNext(&declared, &setting, &verdict)) {
                if (verdict != SETTINGS_OK) continue;
                SettingValue value;
                fine = fine && SettingValueRead(&setting, setting.Default, &value) == SETTINGS_OK &&
                       setting.Name.At >= copy && setting.What.At + setting.What.Length <= copy + size;
            }
        SettingsChosen chosen;
        SettingsLine   line;
        SettingsChosenBegin(copy, size, &chosen);
        while (SettingsChosenNext(&chosen, &line))
            fine = fine && line.Start <= line.End && line.End <= size &&
                   line.Value.At + line.Value.Length <= copy + size;
        uint64_t measured;
        SettingsPenSet(copy, size, Span("prompt"), Span("»"), NULL, &measured);
        uint8_t *out = malloc(measured);
        uint64_t written;
        SettingsPenSet(copy, size, Span("prompt"), Span("»"), out, &written);
        SettingsLine back;
        fine = fine && written == measured && SettingsChosenFind(out, written, Span("prompt"), &back) &&
               Is(back.Value, "»");
        free(out);
        free(copy);
        if (fine) inside++;
    }
    char label[100];
    snprintf(label, sizeof(label), "%u inputs made at random read inside their bounds (%u held)", runs, inside);
    Check(inside == runs, label);
}

int main(void)
{
    TestDeclared();
    TestDeclaredWrong();
    TestValues();
    TestNear();
    TestChosen();
    TestPen();
    TestPenReadsBack();
    TestRandom();
    printf("settings: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
