/* settings_stand — the one reader and the one pen of a program's settings
 * (src/include/settings_reader.h, settings_pen.h), built for the host and
 * held to exact outcomes:
 *
 *     tools/settings_stand.sh
 *
 * a tree of a declaration of every kind read field for field, where each line
 * stands in it; each way a declaration or one of its lines can be wrong, to
 * its verdict; every kind's values, good and bad; settings found by name, by
 * path, and the names a typo is near; the person's file line by line —
 * blocks, whole paths, tabs, contexts, comments, quotes, CR LF, the bare word
 * default, and every way a line stands where nothing is; the pen's changes
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
    return SettingSpanOf((const uint8_t *)text, strlen(text));
}

static bool Is(SettingSpan span, const char *text)
{
    return span.Length == strlen(text) && memcmp(span.At, text, span.Length) == 0;
}

/* ── the declaration ─────────────────────────────────────────────────────── */

/* Its lines, numbered: what the stand's cases name. */
enum {
    L_GREETING = 3, L_CURSOR, L_SHAPE, L_CURSOR_COLOR, L_BLINK, L_SIGN, L_SIGN_COLOR, L_MESSAGES, L_BLANK,
    L_MESSAGES_COLOR, L_TIMING, L_CRASHES, L_WIDTH, L_SHIFT, L_NOTE, L_TITLE, L_TITLE_COLOR
};

static const char g_declaration[] =
    "settings\n"
    "# a comment in the declaration\n"
    "greeting on-or-off default on next-start \"the line said at start\"\n"
    "cursor \"the mark where what you type goes\"\n"
    "    shape one-of block underline bar off default underline \"its shape\"\n"
    "    color color default light-gray \"its color\"\n"
    "    blink number from 100 to 5000 ms or off default off \"how often it blinks\"\n"
    "sign word default ~ \"what stands before what you type\"\n"
    "    color color default #AAAAAA \"its color\"\n"
    "messages \"what is said of programs\"\n"
    "\n"
    "    color color default amber \"their color\"\n"
    "    timing number from 1 to 86400 seconds or off default 10 \"how long a command runs before it is said\"\n"
    "    crashes on-or-off default on \"say a crash\"\n"
    "width number from 24 to 200 cells default 72 \"how wide the column is\"\n"
    "shift number from -10 to 10 default -3 \"how far it moves\"\n"
    "note text default \"\" \"what it says when nothing is wrong\"\n"
    "title text default \"BoxOS Hub\" \"the line said at start\"\n"
    "    color color default white \"its color\"";

static const uint8_t *D(void)
{
    return (const uint8_t *)g_declaration;
}

static uint64_t DSize(void)
{
    return strlen(g_declaration);
}

/* The setting declared on line `line`. */
static Setting At(uint32_t line)
{
    Setting setting;
    if (!SettingsDeclaredAt(D(), DSize(), line, &setting)) {
        printf("FAIL the stand's declaration has nothing on line %u\n", line);
        exit(2);
    }
    return setting;
}

static void TestDeclared(void)
{
    SettingsDeclared declared;
    SettingsVerdict  verdict;
    Setting          s[20];
    unsigned         count = 0;
    Check(SettingsDeclaredBegin(D(), DSize(), &declared) == SETTINGS_OK, "a declaration of every kind opens");
    while (count < 20 && SettingsDeclaredNext(&declared, &s[count], &verdict)) {
        Check(verdict == SETTINGS_OK, "every line of the declaration is taken");
        count++;
    }
    Check(count == 16, "sixteen settings, the comment and the blank line passed over");

    Check(Is(s[0].Name, "greeting") && s[0].Kind == SETTING_ON_OR_OFF && Is(s[0].Default, "on") && s[0].NextStart &&
              s[0].Line == 3 && s[0].Parent == 0 && s[0].Depth == 0,
          "on-or-off, next-start, at the top");
    Check(Is(s[1].Name, "cursor") && s[1].Kind == SETTING_GROUP && Is(s[1].What, "the mark where what you type goes") &&
              s[1].Default.Length == 0 && s[1].Line == 4,
          "a thing that only holds others: its name and what it holds");
    Check(Is(s[2].Name, "shape") && s[2].Kind == SETTING_ONE_OF && Is(s[2].Choices, "block underline bar off") &&
              s[2].Parent == 4 && s[2].Depth == 1 && s[2].Indent == 4,
          "one-of inside cursor, its choices");
    Check(Is(s[3].Name, "color") && s[3].Kind == SETTING_COLOR && Is(s[3].Default, "light-gray") && s[3].Parent == 4,
          "a color by its name");
    Check(Is(s[4].Name, "blink") && s[4].Kind == SETTING_NUMBER && s[4].From == 100 && s[4].To == 5000 &&
              Is(s[4].Unit, "ms") && Is(s[4].Zero, "off") && Is(s[4].Default, "off"),
          "a number with a unit and or off, off its default");
    Check(Is(s[5].Name, "sign") && s[5].Kind == SETTING_WORD && s[5].Parent == 0 && s[5].Depth == 0,
          "back at the top after cursor's lines");
    Check(Is(s[6].Name, "color") && s[6].Parent == 8 && Is(s[6].Default, "#AAAAAA"),
          "a setting with a value of its own holds another: the same name as one inside cursor");
    Check(Is(s[8].Name, "color") && s[8].Parent == 10 && s[8].Line == 12, "a blank line inside messages passed over");
    Check(s[9].From == 1 && Is(s[9].Zero, "off") && Is(s[9].Unit, "seconds") && Is(s[9].Default, "10"), "or off with a number default");
    Check(s[12].From == -10 && s[12].To == 10 && Is(s[12].Default, "-3") && s[12].Unit.Length == 0 && s[12].Zero.Length == 0,
          "a number below zero, no unit");
    Check(Is(s[13].Name, "note") && s[13].Kind == SETTING_TEXT && s[13].Default.Length == 0, "an empty text default");
    Check(Is(s[14].Name, "title") && Is(s[14].Default, "BoxOS Hub") && Is(s[15].Name, "color") && s[15].Parent == 18 &&
              s[15].Line == 19,
          "a text holding a color, the last line unended");
}

/* The verdict on line 2 of "settings\n" + text. */
static void LineVerdict(const char *line, SettingsVerdict want, const char *what)
{
    char             text[512];
    SettingsDeclared declared;
    SettingsVerdict  verdict = SETTINGS_OK;
    Setting          setting;
    snprintf(text, sizeof(text), "settings\n%s\n", line);
    bool opened = SettingsDeclaredBegin((const uint8_t *)text, strlen(text), &declared) == SETTINGS_OK;
    bool read   = opened && SettingsDeclaredNext(&declared, &setting, &verdict);
    char label[600];
    snprintf(label, sizeof(label), "%s: %s (got %d)", what, line, (int)verdict);
    Check(read && verdict == want, label);
}

/* Every line's verdict of a declaration, in order, against `want`
 * (terminated by -1). */
static void Verdicts(const char *text, const int *want, const char *what)
{
    SettingsDeclared declared;
    SettingsVerdict  verdict;
    Setting          setting;
    unsigned         i = 0;
    bool             same = SettingsDeclaredBegin((const uint8_t *)text, strlen(text), &declared) == SETTINGS_OK;
    char             got[200] = "";
    while (same && SettingsDeclaredNext(&declared, &setting, &verdict)) {
        size_t used = strlen(got);
        snprintf(got + used, sizeof(got) - used, " %d", (int)verdict);
        if (want[i] < 0 || (int)verdict != want[i]) same = false;
        i++;
    }
    same = same && want[i] < 0;
    char label[400];
    snprintf(label, sizeof(label), "%s (got%s)", what, got);
    Check(same, label);
}

/* `or all`: the word all read as 0 — every one there is, as off is a thing
 * turned off — and nothing but off or all after `or`. */
static void TestZeroWord(void)
{
    static const char text[] = "settings\ncores number from 1 to 256 or all default all \"how many\"\n";
    SettingsDeclared  declared;
    SettingsVerdict   verdict = SETTINGS_OK;
    Setting           s;
    SettingValue      v;
    bool read = SettingsDeclaredBegin((const uint8_t *)text, strlen(text), &declared) == SETTINGS_OK &&
                SettingsDeclaredNext(&declared, &s, &verdict) && verdict == SETTINGS_OK;
    Check(read && Is(s.Zero, "all") && Is(s.Default, "all") && s.From == 1 && s.To == 256,
          "or all: the word all stands for 0, and may be the default");
    Check(read && SettingValueRead(&s, Span("all"), &v) == SETTINGS_OK && v.Number == 0 && Is(v.Text, "all"),
          "all read as 0");
    Check(read && SettingValueRead(&s, Span("4"), &v) == SETTINGS_OK && v.Number == 4, "a number all the same");
    Check(read && SettingValueRead(&s, Span("off"), &v) == SETTING_VALUE_NOT_NUMBER,
          "off is no value of a setting whose 0 is all");
    LineVerdict("cores number from 0 to 8 or all default all \"x\"", SETTING_LIMITS_BAD, "or all from 0");
    LineVerdict("cores number from 1 to 8 or some default 1 \"x\"", SETTING_LIMITS_BAD, "or a word neither off nor all");
}

static void TestDeclaredWrong(void)
{
    SettingsDeclared declared;
    struct { const char *text; SettingsVerdict want; const char *what; } heads[] = {
        { "", SETTINGS_NO_HEAD, "no bytes" },
        { "a word default b \"c\"\n", SETTINGS_NO_HEAD, "no head line" },
        { "settings 1\n", SETTINGS_NO_HEAD, "a word after the head" },
        { "settings more words\n", SETTINGS_NO_HEAD, "words after the head" },
        { "\"settings\" 2\n", SETTINGS_NO_HEAD, "a quoted head" },
        { "settings\n\ta word default b \"c\"\n", SETTINGS_NOT_TEXT, "a tab: a declaration is indented in spaces" },
        { "settings\r\n", SETTINGS_NOT_TEXT, "a CR" },
        { "settings\na word default b \"c\x7f\"\n", SETTINGS_NOT_TEXT, "a DEL" },
    };
    for (unsigned i = 0; i < sizeof(heads) / sizeof(heads[0]); i++)
        Check(SettingsDeclaredBegin((const uint8_t *)heads[i].text, strlen(heads[i].text), &declared) ==
                  heads[i].want,
              heads[i].what);
    Check(SettingsDeclaredBegin((const uint8_t *)"\n\n  settings\n", 13, &declared) == SETTINGS_OK,
          "blank lines and blanks before the head");

    LineVerdict("Prompt word default ~ \"x\"", SETTING_NAME_BAD, "a capital in the name");
    LineVerdict("2nd word default ~ \"x\"", SETTING_NAME_BAD, "a name beginning with a digit");
    LineVerdict("prompt- word default ~ \"x\"", SETTING_NAME_BAD, "a name ending in -");
    LineVerdict("prompt_x word default ~ \"x\"", SETTING_NAME_BAD, "an underscore in the name");
    LineVerdict("settings word default ~ \"x\"", SETTING_NAME_BAD, "the name settings");
    LineVerdict("default word default ~ \"x\"", SETTING_NAME_BAD, "the name default");
    LineVerdict("when word default ~ \"x\"", SETTINGS_OK, "when is a name like any other now");
    LineVerdict("abcdefghijklmnopqrstuvwxyzabcdefg word default ~ \"x\"", SETTING_NAME_BAD, "a name of 33 bytes");
    LineVerdict("abcdefghijklmnopqrstuvwxyzabcdef word default ~ \"x\"", SETTINGS_OK, "a name of 32 bytes");
    LineVerdict("\"prompt\" word default ~ \"x\"", SETTING_NAME_BAD, "a quoted name");
    LineVerdict("prompt gradient default ~ \"x\"", SETTING_KIND_UNKNOWN, "a kind this reader does not know");
    LineVerdict("prompt yes-or-no default yes \"x\"", SETTING_KIND_UNKNOWN, "yes-or-no is on-or-off now");
    LineVerdict("prompt", SETTING_KIND_UNKNOWN, "no kind and nothing it holds");
    LineVerdict("cursor \"\"", SETTING_WHAT_MISSING, "a thing that holds others, said in empty quotes");
    LineVerdict("cursor \"x\" more", SETTING_WORDS_AFTER, "words after what a thing holds");
    LineVerdict("cursor \"x\"", SETTING_HOLDS_NOTHING, "a thing that holds others with nothing inside");
    LineVerdict("w number from 9 to 3 default 5 \"x\"", SETTING_LIMITS_BAD, "from above to");
    LineVerdict("w number from 3 default 5 \"x\"", SETTING_LIMITS_BAD, "no to");
    LineVerdict("w number default 5 \"x\"", SETTING_LIMITS_BAD, "no limits");
    LineVerdict("w number from 03 to 9 default 5 \"x\"", SETTING_LIMITS_BAD, "a leading zero");
    LineVerdict("w number from -0 to 9 default 5 \"x\"", SETTING_LIMITS_BAD, "-0");
    LineVerdict("w number from 1 to 99999999999999999999 default 5 \"x\"", SETTING_LIMITS_BAD, "a limit past 64 bits");
    LineVerdict("w number from -9223372036854775808 to 9223372036854775807 default 0 \"x\"", SETTINGS_OK,
                "the widest limits");
    LineVerdict("w number from 1 to 9 ms2 default 5 \"x\"", SETTING_UNIT_BAD, "a unit with a digit");
    LineVerdict("w number from 5 to 5 default 5 \"x\"", SETTINGS_OK, "one number to choose");
    LineVerdict("w number from 0 to 9 or off default 5 \"x\"", SETTING_LIMITS_BAD, "or off from 0: off would be 0");
    LineVerdict("w number from 1 to 9 or never default 5 \"x\"", SETTING_LIMITS_BAD, "or and not off");
    LineVerdict("w number from 1 to 9 ms or off default off \"x\"", SETTINGS_OK, "a unit, or off, off the default");
    LineVerdict("w number from 1 to 9 or off default off \"x\"", SETTINGS_OK, "or off with no unit");
    LineVerdict("w number from 1 to 9 default off \"x\"", SETTING_DEFAULT_BAD, "off as a default with no or off");
    LineVerdict("l one-of dark default dark \"x\"", SETTING_CHOICES_BAD, "one choice");
    LineVerdict("l one-of dark dark default dark \"x\"", SETTING_CHOICES_BAD, "a choice twice");
    LineVerdict("l one-of dark \"light\" default dark \"x\"", SETTING_CHOICES_BAD, "a quoted choice");
    LineVerdict("l one-of dark next-start default dark \"x\"", SETTING_CHOICES_BAD, "next-start as a choice");
    LineVerdict("l one-of dark light", SETTING_NO_DEFAULT, "choices and no default after them");
    LineVerdict("p word \"x\"", SETTING_NO_DEFAULT, "a kind and no default");
    LineVerdict("p word default", SETTING_NO_DEFAULT, "default and no value");
    LineVerdict("p word default default \"x\"", SETTING_DEFAULT_BAD, "the bare word default as a default");
    LineVerdict("p word default \"default\" \"x\"", SETTINGS_OK, "a quoted default of the word default");
    LineVerdict("w number from 1 to 9 default 10 \"x\"", SETTING_DEFAULT_BAD, "a default outside the limits");
    LineVerdict("l one-of dark light default dim \"x\"", SETTING_DEFAULT_BAD, "a default not a choice");
    LineVerdict("c color default #FFB02 \"x\"", SETTING_DEFAULT_BAD, "a color of five digits");
    LineVerdict("c color default mauve \"x\"", SETTING_DEFAULT_BAD, "a color by a name it does not know");
    LineVerdict("c color default Amber \"x\"", SETTING_DEFAULT_BAD, "a color's name with a capital");
    LineVerdict("p word default \"two words\" \"x\"", SETTING_DEFAULT_BAD, "a word default with a blank");
    LineVerdict("h on-or-off default yes \"x\"", SETTING_DEFAULT_BAD, "yes for on or off");
    LineVerdict("p word default ~", SETTING_WHAT_MISSING, "no description");
    LineVerdict("p word default ~ next-start", SETTING_WHAT_MISSING, "next-start and no description");
    LineVerdict("p word default ~ what", SETTING_WHAT_MISSING, "a description not in quotes");
    LineVerdict("p word default ~ \"x\" more", SETTING_WORDS_AFTER, "words after the description");
    LineVerdict("p word default ~ \"what", SETTING_LINE_UNREAD, "an unclosed quote");
    LineVerdict("p word default ~ \"what\"s", SETTING_LINE_UNREAD, "a quote not ended by a blank");
    LineVerdict("p word default ~ next-start \"x\"", SETTINGS_OK, "next-start before the description");

    static const int twice[] = { SETTINGS_OK, SETTING_NAME_TWICE, -1 };
    Verdicts("settings\np word default ~ \"x\"\np word default ~ \"y\"\n", twice,
             "a name twice at the top: the first holds, the second is refused");
    static const int apart[] = { SETTINGS_OK, SETTINGS_OK, SETTINGS_OK, SETTINGS_OK, -1 };
    Verdicts("settings\na \"x\"\n  c color default red \"x\"\nb \"y\"\n  c color default red \"y\"\n", apart,
             "one name inside two things: each holds");
    static const int twice_inside[] = { SETTINGS_OK, SETTINGS_OK, SETTING_NAME_TWICE, -1 };
    Verdicts("settings\na \"x\"\n  c color default red \"x\"\n  c color default red \"y\"\n", twice_inside,
             "a name twice inside one thing");
    static const int indent[] = { SETTINGS_OK, SETTINGS_OK, SETTING_INDENT_BAD, SETTING_INSIDE_UNTAKEN, -1 };
    Verdicts("settings\na \"x\"\n    b word default ~ \"x\"\n  c word default ~ \"x\"\n      d word default ~ \"x\"\n",
             indent, "a sibling indented unlike the first, and what is inside it");
    static const int top[] = { SETTINGS_OK, SETTING_INDENT_BAD, -1 };
    Verdicts("settings\n a word default ~ \"x\"\nb word default ~ \"x\"\n", top,
             "the top's lines stand where the first stood");
    static const int untaken[] = { SETTING_KIND_UNKNOWN, SETTING_INSIDE_UNTAKEN, SETTING_INSIDE_UNTAKEN, SETTINGS_OK, -1 };
    Verdicts("settings\na gradient \"x\"\n  b word default ~ \"x\"\n    c word default ~ \"x\"\nd word default ~ \"x\"\n",
             untaken, "what is inside a line not taken is not taken, and the next at the top is");
    static const int deep[] = { SETTINGS_OK, SETTINGS_OK, SETTINGS_OK, SETTINGS_OK, SETTINGS_OK, SETTINGS_OK,
                                SETTINGS_OK, SETTINGS_OK, SETTING_TOO_DEEP, SETTING_TOO_DEEP, SETTINGS_OK, -1 };
    Verdicts("settings\na \"x\"\n b \"x\"\n  c \"x\"\n   d \"x\"\n    e \"x\"\n     f \"x\"\n      g \"x\"\n"
             "       h word default ~ \"x\"\n        i word default ~ \"x\"\n         j word default ~ \"x\"\n"
             "z word default ~ \"x\"\n",
             deep, "eight levels hold, the ninth and what is under it do not, the top again does");
    static const int holds[] = { SETTING_HOLDS_NOTHING, SETTINGS_OK, -1 };
    Verdicts("settings\na \"x\"\nb word default ~ \"x\"\n", holds, "a thing with nothing indented under it");
    static const int with_value[] = { SETTINGS_OK, SETTINGS_OK, SETTINGS_OK, -1 };
    Verdicts("settings\n# a comment\nsign word default ~ \"x\"\n  # a comment inside\n  color color default red "
             "\"x\"\n    size number from 1 to 3 default 1 \"x\"\n",
             with_value, "comments anywhere, and a setting with a value holding others two deep");
}

/* ── finding ─────────────────────────────────────────────────────────────── */

static void TestFind(void)
{
    Setting setting;
    Check(SettingsDeclaredChild(D(), DSize(), 0, Span("cursor"), &setting) && setting.Line == L_CURSOR,
          "a thing at the top by name");
    Check(SettingsDeclaredChild(D(), DSize(), L_CURSOR, Span("color"), &setting) && setting.Line == L_CURSOR_COLOR,
          "color inside cursor");
    Check(SettingsDeclaredChild(D(), DSize(), L_SIGN, Span("color"), &setting) && setting.Line == L_SIGN_COLOR,
          "color inside sign");
    Check(!SettingsDeclaredChild(D(), DSize(), 0, Span("color"), &setting), "no color at the top");
    Check(SettingsDeclaredHolds(D(), DSize(), L_CURSOR) && SettingsDeclaredHolds(D(), DSize(), L_SIGN) &&
              !SettingsDeclaredHolds(D(), DSize(), L_SHAPE) && !SettingsDeclaredHolds(D(), DSize(), L_GREETING),
          "what holds others, and what holds none");
    Check(SettingsDeclaredPath(D(), DSize(), Span("cursor blink"), &setting) && setting.Line == L_BLINK,
          "a path of two names");
    Check(SettingsDeclaredPath(D(), DSize(), Span("  messages\ttiming "), &setting) && setting.Line == L_TIMING,
          "a path with blanks round its names");
    Check(SettingsDeclaredPath(D(), DSize(), Span("title color"), &setting) && setting.Line == L_TITLE_COLOR,
          "the last line of the declaration by path");
    Check(!SettingsDeclaredPath(D(), DSize(), Span(""), &setting) &&
              !SettingsDeclaredPath(D(), DSize(), Span("cursor nope"), &setting) &&
              !SettingsDeclaredPath(D(), DSize(), Span("color"), &setting) &&
              !SettingsDeclaredPath(D(), DSize(), Span("\"cursor\""), &setting),
          "no setting for an empty path, a name not inside, a name not at the top, a quoted name");
    Check(SettingsDeclaredAt(D(), DSize(), L_SHAPE, &setting) && Is(setting.Name, "shape") &&
              !SettingsDeclaredAt(D(), DSize(), 2, &setting) && !SettingsDeclaredAt(D(), DSize(), 0, &setting),
          "a setting by its line; none on the comment's, none on line 0");
    Check(SettingsDeclaredSomewhere(D(), DSize(), Span("blink"), &setting) && setting.Line == L_BLINK &&
              SettingsDeclaredSomewhere(D(), DSize(), Span("color"), &setting) && setting.Line == L_CURSOR_COLOR,
          "a name anywhere in the tree, the first of two");

    struct { uint32_t parent; const char *typed, *meant; } near[] = {
        { 0, "cursr", "cursor" }, { 0, "sgin", "sign" }, { L_CURSOR, "colr", "color" }, { L_CURSOR, "shpe", "shape" },
        { L_CURSOR, "blnik", "blink" }, { L_MESSAGES, "timeing", "timing" }, { 0, "widht", "width" },
    };
    for (unsigned i = 0; i < sizeof(near) / sizeof(near[0]); i++) {
        char label[100];
        snprintf(label, sizeof(label), "%s is near %s", near[i].typed, near[i].meant);
        Check(SettingsDeclaredNearest(D(), DSize(), near[i].parent, Span(near[i].typed), &setting) &&
                  Is(setting.Name, near[i].meant),
              label);
    }
    Check(!SettingsDeclaredNearest(D(), DSize(), 0, Span("colr"), &setting) &&
              !SettingsDeclaredNearest(D(), DSize(), L_SHAPE, Span("colr"), &setting) &&
              !SettingsDeclaredNearest(D(), DSize(), 0, Span("x"), &setting),
          "near nothing: color is not at the top, shape holds nothing, one letter is near no name");
    Check(SettingDistance(Span("ab"), Span("ba")) == 1 && SettingDistance(Span("abc"), Span("abc")) == 0 &&
              SettingDistance(Span(""), Span("abc")) == 3 && SettingDistance(Span("kitten"), Span("sitting")) == 3,
          "distances");
}

/* ── values ──────────────────────────────────────────────────────────────── */

static void ValueIs(uint32_t line, const char *value, SettingsVerdict want, const char *what)
{
    Setting      setting = At(line);
    SettingValue read;
    char         label[300];
    SettingsVerdict got = SettingValueRead(&setting, Span(value), &read);
    snprintf(label, sizeof(label), "%s: %.*s '%s' (got %d)", what, (int)setting.Name.Length,
             (const char *)setting.Name.At, value, (int)got);
    Check(got == want, label);
}

static void TestValues(void)
{
    Setting      setting;
    SettingValue value;

    setting = At(L_GREETING);
    Check(SettingValueRead(&setting, Span("on"), &value) == SETTINGS_OK && value.On, "on");
    Check(SettingValueRead(&setting, Span("off"), &value) == SETTINGS_OK && !value.On, "off");
    ValueIs(L_GREETING, "On", SETTING_VALUE_NOT_ON_OR_OFF, "a capital");
    ValueIs(L_GREETING, "yes", SETTING_VALUE_NOT_ON_OR_OFF, "yes");
    ValueIs(L_GREETING, "", SETTING_VALUE_EMPTY, "nothing");

    setting = At(L_WIDTH);
    Check(SettingValueRead(&setting, Span("72"), &value) == SETTINGS_OK && value.Number == 72 && Is(value.Text, "72"),
          "a number");
    Check(SettingValueRead(&setting, Span("24 cells"), &value) == SETTINGS_OK && value.Number == 24 &&
              Is(value.Text, "24"),
          "a number with its unit, the lower limit");
    Check(SettingValueRead(&setting, Span("200\t cells"), &value) == SETTINGS_OK && value.Number == 200,
          "the upper limit, blanks before the unit");
    ValueIs(L_WIDTH, "23", SETTING_VALUE_OUTSIDE, "below the limits");
    ValueIs(L_WIDTH, "201", SETTING_VALUE_OUTSIDE, "above the limits");
    ValueIs(L_WIDTH, "72 rows", SETTING_VALUE_NOT_NUMBER, "another unit");
    ValueIs(L_WIDTH, "72cells", SETTING_VALUE_NOT_NUMBER, "a unit not apart");
    ValueIs(L_WIDTH, "072", SETTING_VALUE_NOT_NUMBER, "a leading zero");
    ValueIs(L_WIDTH, "7.5", SETTING_VALUE_NOT_NUMBER, "a fraction");
    ValueIs(L_WIDTH, "off", SETTING_VALUE_NOT_NUMBER, "off where it cannot be off");
    setting = At(L_BLINK);
    Check(SettingValueRead(&setting, Span("off"), &value) == SETTINGS_OK && value.Number == 0 && Is(value.Text, "off"),
          "off, read as 0");
    Check(SettingValueRead(&setting, Span("500 ms"), &value) == SETTINGS_OK && value.Number == 500 &&
              Is(value.Text, "500"),
          "500 ms");
    ValueIs(L_BLINK, "0", SETTING_VALUE_OUTSIDE, "0 written as a number, below the limits");
    ValueIs(L_BLINK, "Off", SETTING_VALUE_NOT_NUMBER, "Off with a capital");
    ValueIs(L_BLINK, "off ms", SETTING_VALUE_NOT_NUMBER, "off with a unit");
    setting = At(L_SHIFT);
    Check(SettingValueRead(&setting, Span("-10"), &value) == SETTINGS_OK && value.Number == -10, "below zero");
    ValueIs(L_SHIFT, "-0", SETTING_VALUE_NOT_NUMBER, "-0");

    ValueIs(L_SIGN, "»", SETTINGS_OK, "a word of UTF-8");
    ValueIs(L_SIGN, "two words", SETTING_VALUE_NOT_WORD, "two words");
    ValueIs(L_SIGN, "a\"b", SETTING_VALUE_NOT_WORD, "a quote in a word");
    ValueIs(L_SIGN, "a\x01", SETTING_VALUE_NOT_TEXT, "a control byte");

    ValueIs(L_NOTE, "", SETTINGS_OK, "an empty text");
    ValueIs(L_NOTE, " Hello there ", SETTINGS_OK, "a text with blanks");
    ValueIs(L_NOTE, "tab\there", SETTINGS_OK, "a text with a tab");
    ValueIs(L_NOTE, "line\nbreak", SETTING_VALUE_NOT_TEXT, "a text with a newline");

    ValueIs(L_SHAPE, "bar", SETTINGS_OK, "a choice");
    ValueIs(L_SHAPE, "off", SETTINGS_OK, "the word off as a choice");
    ValueIs(L_SHAPE, "Bar", SETTING_VALUE_NOT_CHOICE, "a choice with a capital");
    ValueIs(L_SHAPE, "block bar", SETTING_VALUE_NOT_CHOICE, "two choices");

    setting = At(L_CURSOR_COLOR);
    Check(SettingValueRead(&setting, Span("#7cd68A"), &value) == SETTINGS_OK && value.Color == 0x7CD68Au &&
              Is(value.Text, "#7cd68A"),
          "a color in both cases, kept as written");
    Check(SettingValueRead(&setting, Span("amber"), &value) == SETTINGS_OK && value.Color == 0xFFB040u &&
              Is(value.Text, "amber"),
          "a color by name");
    const SettingColorName *names = SettingColorNames();
    unsigned                held = 0;
    for (unsigned i = 0; i < SETTING_COLOR_NAMES; i++)
        held += SettingValueRead(&setting, Span(names[i].Name), &value) == SETTINGS_OK && value.Color == names[i].Rgb;
    Check(held == SETTING_COLOR_NAMES, "every color's name");
    Check(SettingValueRead(&setting, Span("black"), &value) == SETTINGS_OK && value.Color == 0 &&
              SettingValueRead(&setting, Span("light-gray"), &value) == SETTINGS_OK && value.Color == 0xAAAAAAu,
          "black, and light-gray as the text was");
    ValueIs(L_CURSOR_COLOR, "7CD68A", SETTING_VALUE_NOT_COLOR, "no #");
    ValueIs(L_CURSOR_COLOR, "#7CD68", SETTING_VALUE_NOT_COLOR, "five digits");
    ValueIs(L_CURSOR_COLOR, "#GGGGGG", SETTING_VALUE_NOT_COLOR, "not hex");
    ValueIs(L_CURSOR_COLOR, "grey", SETTING_VALUE_NOT_COLOR, "a name it does not know");
    ValueIs(L_CURSOR_COLOR, "light gray", SETTING_VALUE_NOT_COLOR, "a name with a blank");

    setting = At(L_CURSOR);
    Check(SettingValueRead(&setting, Span("x"), &value) == SETTING_KIND_UNKNOWN,
          "a thing that only holds others reads no value");
}

/* ── keys ────────────────────────────────────────────────────────────────── */

/* `text` read as a chord: whether it is one, and what — Mods, Extended and
 * Code — when `want` says it is. */
static void ChordIs(const char *text, bool want, uint8_t mods, uint8_t extended, uint8_t code)
{
    KeyChord chord;
    char     label[200];
    bool     read = KeyChordRead((const uint8_t *)text, strlen(text), &chord);
    snprintf(label, sizeof(label), "the chord '%s' %s", text, want ? "reads" : "is refused");
    Check(read == want && (!want || (chord.Mods == mods && chord.Extended == extended && chord.Code == code)), label);
}

static void SpelledIs(const char *text, const char *spelled)
{
    KeyChord chord;
    char     said[KEY_CHORD_SPELLED_MAX], label[200];
    bool     read = KeyChordRead((const uint8_t *)text, strlen(text), &chord);
    if (read) KeyChordSpell(&chord, said, sizeof(said));
    snprintf(label, sizeof(label), "'%s' is said '%s' (said '%s')", text, spelled, read ? said : "nothing");
    Check(read && strcmp(said, spelled) == 0, label);
}

static void TestKeys(void)
{
    const uint8_t CA = KB_MOD_CTRL | KB_MOD_ALT;
    ChordIs("ctrl+alt+q", true, CA, 0, 'q');
    ChordIs("Alt+Ctrl+Q", true, CA, 0, 'q');
    ChordIs("ctrl+shift+left", true, KB_MOD_CTRL | KB_MOD_SHIFT, 1, KEY_LEFT);
    ChordIs("alt+tab", true, KB_MOD_ALT, 0, KEY_TAB);
    ChordIs("ctrl+alt+space", true, CA, 0, ' ');
    ChordIs("ctrl+alt+page-down", true, CA, 1, KEY_PAGEDN);
    ChordIs("ctrl+alt+shift+b", true, CA | KB_MOD_SHIFT, 0, 'b');
    ChordIs("ctrl+/", true, KB_MOD_CTRL, 0, '/');
    ChordIs("ctrl+alt+7", true, CA, 0, '7');
    ChordIs("q", false, 0, 0, 0);
    ChordIs("shift+q", false, 0, 0, 0);
    ChordIs("shift+left", false, 0, 0, 0);
    ChordIs("ctrl+", false, 0, 0, 0);
    ChordIs("+q", false, 0, 0, 0);
    ChordIs("ctrl+alt", false, 0, 0, 0);
    ChordIs("ctrl+ctrl+q", false, 0, 0, 0);
    ChordIs("ctrl+alt+qq", false, 0, 0, 0);
    ChordIs("ctrl+alt+f1", false, 0, 0, 0);
    ChordIs("ctrl+shift+1", false, 0, 0, 0);
    ChordIs("ctrl+alt+q+w", false, 0, 0, 0);
    ChordIs("ctrl alt q", false, 0, 0, 0);
    ChordIs("ctrl+alt+!", true, CA | KB_MOD_SHIFT, 0, '!');
    ChordIs("ctrl+?", true, KB_MOD_CTRL | KB_MOD_SHIFT, 0, '?');
    ChordIs("ctrl+shift+?", false, 0, 0, 0);
    ChordIs("ctrl++", false, 0, 0, 0);
    ChordIs("ctrl+\"", false, 0, 0, 0);
    ChordIs("", false, 0, 0, 0);

    SpelledIs("Alt+Shift+Ctrl+A", "ctrl+alt+shift+a");
    SpelledIs("ctrl+alt+PAGE-UP", "ctrl+alt+page-up");
    SpelledIs("ctrl+?", "ctrl+?");
    SpelledIs("alt+escape", "alt+escape");
    const KeyChordName *names = KeyChordNames();
    unsigned            held  = 0;
    for (unsigned i = 0; i < KEY_CHORD_NAMES; i++) {
        char text[40], said[KEY_CHORD_SPELLED_MAX];
        KeyChord chord;
        snprintf(text, sizeof(text), "ctrl+%s", names[i].Name);
        if (!KeyChordRead((const uint8_t *)text, strlen(text), &chord)) continue;
        KeyChordSpell(&chord, said, sizeof(said));
        held += strcmp(said, text) == 0;
    }
    Check(held == KEY_CHORD_NAMES, "every key's name read and said back as it was");

    KeyChord chord, other;
    KeyChordRead((const uint8_t *)"ctrl+alt+q", 10, &chord);
    KeyChordRead((const uint8_t *)"alt+ctrl+q", 10, &other);
    const kb_event_t typed = { .cp = 'q', .scancode = 0x10, .mods = KB_MOD_CTRL | KB_MOD_ALT };
    const kb_event_t shifted = { .cp = 'Q', .scancode = 0x10, .mods = KB_MOD_CTRL | KB_MOD_ALT | KB_MOD_SHIFT };
    const kb_event_t alone = { .cp = 'q', .scancode = 0x10, .mods = KB_MOD_CTRL };
    Check(KeyChordSame(&chord, &other) && KeyChordMatches(&chord, &typed) && !KeyChordMatches(&chord, &shifted) &&
              !KeyChordMatches(&chord, &alone),
          "ctrl+alt+q is the key q with ctrl and alt held, and with nothing more or less");
    KeyChordRead((const uint8_t *)"ctrl+alt+shift+q", 16, &chord);
    Check(KeyChordMatches(&chord, &shifted), "with shift held the letter comes as a capital, and matches");
    KeyChordRead((const uint8_t *)"ctrl+left", 9, &chord);
    const kb_event_t left = { .cp = 0, .scancode = KEY_LEFT, .mods = KB_MOD_CTRL | KB_MOD_EXTENDED };
    const kb_event_t four = { .cp = '4', .scancode = KEY_LEFT, .mods = KB_MOD_CTRL };
    Check(KeyChordMatches(&chord, &left) && !KeyChordMatches(&chord, &four),
          "left is the key of the 0xE0 set, not the keypad's 4 with the same scancode");
    KeyChordRead((const uint8_t *)"ctrl+?", 6, &chord);
    const kb_event_t asked = { .cp = '?', .scancode = 0x35, .mods = KB_MOD_CTRL | KB_MOD_SHIFT };
    const kb_event_t slash = { .cp = '/', .scancode = 0x35, .mods = KB_MOD_CTRL };
    Check(KeyChordMatches(&chord, &asked) && !KeyChordMatches(&chord, &slash),
          "ctrl+? is the key under ? with ctrl and shift held, as the keyboard says it, not ctrl+/");

    SettingsDeclared declared;
    Setting          setting;
    SettingsVerdict  verdict = SETTINGS_OK;
    const char      *text    = "settings\nstop key default ctrl+alt+q \"end the program on the screen\"\n";
    Check(SettingsDeclaredBegin((const uint8_t *)text, strlen(text), &declared) == SETTINGS_OK &&
              SettingsDeclaredNext(&declared, &setting, &verdict) && verdict == SETTINGS_OK &&
              setting.Kind == SETTING_KEY && Is(setting.Default, "ctrl+alt+q"),
          "a key declared, its default read");
    LineVerdict("stop key default q \"x\"", SETTING_DEFAULT_BAD, "a key's default that is no chord");
    SettingValue value;
    Check(SettingValueRead(&setting, Span("Ctrl+Alt+B"), &value) == SETTINGS_OK && value.Key.Code == 'b' &&
              value.Key.Mods == CA && Is(value.Text, "Ctrl+Alt+B"),
          "a key's value read, kept as written");
    Check(SettingValueRead(&setting, Span("ctrl+b"), &value) == SETTINGS_OK && value.Key.Mods == KB_MOD_CTRL,
          "ctrl alone is held");
    Check(SettingValueRead(&setting, Span("b"), &value) == SETTING_VALUE_NOT_KEY &&
              SettingValueRead(&setting, Span(""), &value) == SETTING_VALUE_EMPTY,
          "a letter alone is no key, and nothing is empty");
}

/* ── the person's file ───────────────────────────────────────────────────── */

static unsigned Lines(const char *file, SettingsLine *lines, unsigned room)
{
    SettingsChosen chosen;
    unsigned       count = 0;
    SettingsChosenBegin(D(), DSize(), (const uint8_t *)file, strlen(file), &chosen);
    while (count < room && SettingsChosenNext(&chosen, &lines[count])) count++;
    return count;
}

static void TestChosenTree(void)
{
    SettingsLine l[16];
    static const char tree[] = "# the cursor I like\ncursor\n    shape  block\n\tcolor amber\ncursor blink 500 ms\n";
    Check(Lines(tree, l, 16) == 5, "a block, a tab and a path: five lines");
    Check(l[0].Kind == SETTINGS_LINE_COMMENT, "a comment");
    Check(l[1].Kind == SETTINGS_LINE_ENTRY && l[1].Named.Line == L_CURSOR && !l[1].HasValue &&
              l[1].Verdict == SETTINGS_OK && l[1].Parent == 0,
          "a line that only opens what is under it");
    Check(l[2].Named.Line == L_SHAPE && Is(l[2].Value, "block") && l[2].Verdict == SETTINGS_OK && l[2].Parent == 2 &&
              l[2].Indent == 4 && l[2].Lead == 4,
          "a value inside the block, its blanks before it taken off");
    Check(l[3].Named.Line == L_CURSOR_COLOR && Is(l[3].Value, "amber") && l[3].Parent == 2 && l[3].Indent == 4 &&
              l[3].Lead == 1,
          "a tab as deep as four spaces: the same block");
    Check(l[4].Named.Line == L_BLINK && Is(l[4].Value, "500 ms") && l[4].Parent == 0 &&
              l[4].NamesEnd == strlen("cursor blink"),
          "a whole path on one line, where its names end");

    static const char values[] = "sign  »  \nnote \"  two edges  \"\r\nwidth\nshape block\nsign default\n"
                                 "note \"default\"\nsign #7CD68A\nbad\x01line\ngreeting off";
    Check(Lines(values, l, 16) == 9, "nine lines, the last unended");
    Check(Is(l[0].Value, "»") && !l[0].Quoted && !l[0].Default, "a value, its blanks at both ends taken off");
    Check(Is(l[1].Value, "  two edges  ") && l[1].Quoted && Is(l[1].Whole, "note \"  two edges  \""),
          "a quoted value as it stands, the CR taken off the line");
    Check(l[2].Named.Line == L_WIDTH && !l[2].HasValue && l[2].Verdict == SETTING_VALUE_EMPTY,
          "a setting named with no value, holding nothing");
    Check(l[3].Verdict == SETTING_NOT_THERE && Is(l[3].Unknown, "shape") && l[3].UnknownIn == 0 &&
              l[3].Named.Line == 0,
          "shape at the top, where nothing has that name");
    Check(l[4].Default && l[4].Verdict == SETTINGS_OK, "the bare word default");
    Check(Is(l[5].Value, "default") && l[5].Quoted && !l[5].Default, "the word default quoted");
    Check(Is(l[6].Value, "#7CD68A") && l[6].Verdict == SETTINGS_OK, "a value beginning with #");
    Check(l[7].Kind == SETTINGS_LINE_NOT_TEXT && l[7].Line == 8, "a line that is not text");
    Check(l[8].End == strlen(values) && l[8].Verdict == SETTINGS_OK, "the last line, unended");

    static const char names[] = "cursor blok\ncursor\nsign color\nsign color red\nsign\n    color red\n"
                                "cursor shape round\ntitle color red\ntitle \"color red\"\ntitle colorful\n";
    Check(Lines(names, l, 16) == 10, "names and values: ten lines");
    Check(l[0].Verdict == SETTING_NOT_THERE && Is(l[0].Unknown, "blok") && l[0].UnknownIn == L_CURSOR,
          "after what only holds others, a word names what it holds: blok is none of them");
    Check(l[1].Verdict == SETTINGS_OK && !l[1].HasValue, "cursor alone");
    Check(l[2].Named.Line == L_SIGN && Is(l[2].Value, "color") && l[2].Verdict == SETTINGS_OK,
          "the last word is the value: the sign is the word color");
    Check(l[3].Named.Line == L_SIGN_COLOR && Is(l[3].Value, "red") && l[3].NamesEnd == strlen("sign color"),
          "with a word after it, color names what the sign holds");
    Check(l[4].Named.Line == L_SIGN && !l[4].HasValue && l[4].Verdict == SETTINGS_OK && l[5].Parent == 5 &&
              l[5].Named.Line == L_SIGN_COLOR,
          "the sign opened without a value, its color inside");
    Check(l[6].Named.Line == L_SHAPE && l[6].Verdict == SETTING_VALUE_NOT_CHOICE && l[6].HasValue,
          "a value that does not fit: the setting it is for is still known");
    Check(l[7].Named.Line == L_TITLE_COLOR && Is(l[7].Value, "red"), "a text's color by path");
    Check(l[8].Named.Line == L_TITLE && Is(l[8].Value, "color red") && l[8].Quoted,
          "a quoted value names nothing: the title is the text color red");
    Check(l[9].Named.Line == L_TITLE && Is(l[9].Value, "colorful"), "a word that only begins with a name");

    static const char nowhere[] = "frob\n    sign a\ncursor\n    shape bar\n        color red\n  \"cursor\" bar\n";
    Check(Lines(nowhere, l, 16) == 6, "lines that stand where nothing is: six");
    Check(l[0].Verdict == SETTING_NOT_THERE && Is(l[0].Unknown, "frob"), "a name nothing has");
    Check(l[1].Verdict == SETTING_INSIDE_UNTAKEN && l[1].Named.Line == 0, "a line inside it is not taken");
    Check(l[3].Verdict == SETTINGS_OK && l[4].Verdict == SETTING_NOT_THERE && Is(l[4].Unknown, "color") &&
              l[4].UnknownIn == L_SHAPE,
          "a line indented under shape, which holds nothing");
    Check(l[5].Verdict == SETTING_NOT_THERE && l[5].Parent == 3, "a quoted name is none");
}

static void TestChosenContext(void)
{
    SettingsLine l[16];
    static const char ctx[] = "[work, home]\n    sign w»\n        color green\ngreeting off\n[work]\n  cursor shape bar\n";
    Check(Lines(ctx, l, 16) == 6, "contexts: six lines");
    Check(l[0].Kind == SETTINGS_LINE_CONTEXT && l[0].Verdict == SETTINGS_OK && Is(l[0].Context, "work, home") &&
              l[0].ContextLine == 1,
          "a context of two tags, apart by a comma and a blank");
    Check(l[1].Named.Line == L_SIGN && Is(l[1].Context, "work, home") && l[1].ContextLine == 1 && l[1].Parent == 1,
          "a line in it");
    Check(l[2].Named.Line == L_SIGN_COLOR && l[2].ContextLine == 1 && l[2].Parent == 2, "a line deeper in it");
    Check(l[3].ContextLine == 0 && l[3].Context.Length == 0 && l[3].Verdict == SETTINGS_OK,
          "a line at the top again is everywhere");
    Check(l[5].ContextLine == 5 && Is(l[5].Context, "work") && l[5].Named.Line == L_SHAPE, "a second context");

    static const char bad[] = "[bad\n    sign a\n[]\n[a a]\n[x] y\n[ok]\ngreeting on\ncursor\n    [in]\n        shape bar\n"
                              "[a\"b]\n";
    Check(Lines(bad, l, 16) == 11, "contexts that do not read: eleven lines");
    Check(l[0].Verdict == SETTING_CONTEXT_BAD && l[1].Verdict == SETTING_INSIDE_UNTAKEN,
          "an unclosed bracket, and the line under it");
    Check(l[2].Verdict == SETTING_CONTEXT_BAD && l[3].Verdict == SETTING_CONTEXT_BAD && l[4].Verdict == SETTING_CONTEXT_BAD,
          "no tag, a tag twice, words after the bracket");
    Check(l[5].Verdict == SETTING_CONTEXT_EMPTY && l[6].ContextLine == 0 && l[6].Verdict == SETTINGS_OK,
          "a context with nothing indented under it, and the line after it everywhere");
    Check(l[8].Verdict == SETTING_CONTEXT_INSIDE && l[9].Verdict == SETTING_INSIDE_UNTAKEN,
          "a context inside a block, and what is under it");
    Check(l[10].Verdict == SETTING_CONTEXT_BAD, "a quote in a tag");

    Check(SettingTagsSame(Span("work home"), Span("home,work")) && SettingTagsSame(Span(""), Span(" ")) &&
              !SettingTagsSame(Span("work"), Span("work home")) && !SettingTagsSame(Span("work"), Span("")) &&
              !SettingTagsSame(Span("work"), Span("Work")),
          "the same tags in any order and apart by anything; everywhere is no tag");

    SettingsLine found;
    static const char last[] = "sign a\n[work]\n    sign b\nsign c\n[home work]\n    sign d\n";
    Check(SettingsChosenFind(D(), DSize(), (const uint8_t *)last, strlen(last), Span(""), L_SIGN, &found) &&
              found.Line == 4 && Is(found.Value, "c"),
          "the last line for a setting everywhere");
    Check(SettingsChosenFind(D(), DSize(), (const uint8_t *)last, strlen(last), Span("work"), L_SIGN, &found) &&
              found.Line == 3,
          "the line in the context's block");
    Check(SettingsChosenFind(D(), DSize(), (const uint8_t *)last, strlen(last), Span("work home"), L_SIGN, &found) &&
              found.Line == 6,
          "the line in the block of the same tags in another order");
    Check(!SettingsChosenFind(D(), DSize(), (const uint8_t *)last, strlen(last), Span("home"), L_SIGN, &found) &&
              !SettingsChosenFind(D(), DSize(), (const uint8_t *)last, strlen(last), Span(""), L_SHAPE, &found) &&
              !SettingsChosenFind(D(), DSize(), (const uint8_t *)last, strlen(last), Span(""), 0, &found),
          "no line for tags it has no block of, for a setting it names nowhere, for line 0");

    char deep[400] = "";
    for (unsigned i = 0; i < 11; i++) {
        size_t used = strlen(deep);
        snprintf(deep + used, sizeof(deep) - used, "%*sfrob\n", (int)i, "");
    }
    strcat(deep, "greeting on\n");
    Check(Lines(deep, l, 16) == 12, "a file eleven deep");
    bool untaken = true;
    for (unsigned i = 1; i <= 8; i++) untaken = untaken && l[i].Verdict == SETTING_INSIDE_UNTAKEN;
    Check(l[0].Verdict == SETTING_NOT_THERE && untaken && l[9].Verdict == SETTING_TOO_DEEP &&
              l[10].Verdict == SETTING_TOO_DEEP && l[11].Verdict == SETTINGS_OK,
          "nine levels open, the tenth and under it too deep, the top again read");

    SettingsChosen chosen;
    SettingsChosenBegin(D(), DSize(), (const uint8_t *)"", 0, &chosen);
    Check(!SettingsChosenNext(&chosen, &l[0]), "no file, no lines");
}

/* ── the pen ─────────────────────────────────────────────────────────────── */

typedef enum { PEN_SET, PEN_ERASE, PEN_CLEAR } PenWhat;

static void PenCall(PenWhat what, const char *file, const char *context, uint32_t node, const char *value,
                    uint8_t *out, uint64_t *bytes)
{
    const uint8_t *f    = (const uint8_t *)file;
    uint64_t       size = strlen(file);
    if (what == PEN_SET) SettingsPenSet(D(), DSize(), f, size, Span(context), node, Span(value), out, bytes);
    else if (what == PEN_ERASE) SettingsPenErase(D(), DSize(), f, size, Span(context), node, out, bytes);
    else SettingsPenClear(D(), DSize(), f, size, out, bytes);
}

/* The file a change makes, measured and written. */
static char *Pen(PenWhat what, const char *file, const char *context, uint32_t node, const char *value)
{
    uint64_t measured, written;
    PenCall(what, file, context, node, value, NULL, &measured);
    char *out = malloc(measured + 1);
    if (!out) exit(2);
    PenCall(what, file, context, node, value, (uint8_t *)out, &written);
    out[written] = '\0';
    if (written != measured) {
        printf("FAIL the pen measured %llu bytes and wrote %llu\n", (unsigned long long)measured,
               (unsigned long long)written);
        g_failed++;
    }
    return out;
}

static void PenIs(PenWhat what, const char *file, const char *context, uint32_t node, const char *value,
                  const char *want, const char *label)
{
    char *got = Pen(what, file, context, node, value);
    if (strcmp(got, want) == 0) {
        g_passed++;
    } else {
        g_failed++;
        printf("FAIL %s:\n--- got\n%s\n--- wanted\n%s\n---\n", label, got, want);
    }
    free(got);
}

static void TestPenSet(void)
{
    PenIs(PEN_SET, "", "", L_SHAPE, "block", "cursor\n    shape block\n", "the first setting makes the file, its block");
    PenIs(PEN_SET, "", "", L_GREETING, "off", "greeting off\n", "a setting at the top");
    PenIs(PEN_SET, "# mine\ncursor\n    shape bar\n    color amber\n", "", L_SHAPE, "block",
          "# mine\ncursor\n    shape block\n    color amber\n", "a value rewritten where it stands");
    PenIs(PEN_SET, "cursor shape bar   \n", "", L_SHAPE, "underline", "cursor shape underline\n",
          "a whole path rewritten as the person wrote it");
    PenIs(PEN_SET, "cursor\n    color amber\nwidth 30\n", "", L_BLINK, "500 ms",
          "cursor\n    color amber\n    blink 500 ms\nwidth 30\n", "a line added in its block, indented as the lines in it");
    PenIs(PEN_SET, "cursor\n\tcolor amber\n", "", L_SHAPE, "block", "cursor\n\tcolor amber\n\tshape block\n",
          "a block indented by tabs is added to with a tab");
    PenIs(PEN_SET, "cursor\nwidth 30\n", "", L_SHAPE, "block", "cursor\n    shape block\nwidth 30\n",
          "a block with nothing in it: four spaces deeper");
    PenIs(PEN_SET, "  cursor\n  width 30\n", "", L_SHAPE, "block", "  cursor\n      shape block\n  width 30\n",
          "four spaces deeper than an indented block");
    PenIs(PEN_SET, "width 30\n[work]\n    sign w»\n", "", L_CURSOR_COLOR, "green",
          "width 30\ncursor\n    color green\n[work]\n    sign w»\n", "a new block at the end of what is set everywhere");
    PenIs(PEN_SET, "# c\n[work]\n    sign w»\n", "", L_GREETING, "off", "# c\ngreeting off\n[work]\n    sign w»\n",
          "nothing set everywhere: before the first context");
    PenIs(PEN_SET, "  width 30\n [work]\n    sign w\n", "", L_GREETING, "off",
          "  width 30\n  greeting off\n [work]\n    sign w\n", "indented as the line it follows, so no context is inside it");
    PenIs(PEN_SET, " [work]\n    sign w\n", "", L_GREETING, "off", " greeting off\n [work]\n    sign w\n",
          "indented as the context it goes before");
    PenIs(PEN_SET, "[work]\n    sign w»\n", "work", L_SIGN_COLOR, "green", "[work]\n    sign w»\n        color green\n",
          "in a context's block, under what holds it");
    PenIs(PEN_SET, "[work]\n    sign w»\n", "work", L_GREETING, "off", "[work]\n    sign w»\n    greeting off\n",
          "in a context's block, at its end");
    PenIs(PEN_SET, "[work]\n# empty\nwidth 30\n", "work", L_GREETING, "off",
          "[work]\n    greeting off\n# empty\nwidth 30\n", "a context's block with nothing in it: four spaces under it");
    PenIs(PEN_SET, "width 30\n", "home games", L_GREETING, "off", "width 30\n\n[home games]\n    greeting off\n",
          "no block for the context: one made at the end, a blank line before it");
    PenIs(PEN_SET, "width 30\n\n", "work", L_GREETING, "off", "width 30\n\n[work]\n    greeting off\n",
          "a file ending in a blank line gets no second one");
    PenIs(PEN_SET, "", "work", L_SHAPE, "bar", "[work]\n    cursor\n        shape bar\n",
          "a context and a block in an empty file");
    PenIs(PEN_SET, "[home work]\n    sign w\n", "work, home", L_SIGN, "x", "[home work]\n    sign x\n",
          "the block of the same tags in another order");
    PenIs(PEN_SET, "width 30", "", L_GREETING, "off", "width 30\ngreeting off\n", "after a last line with no newline");
    PenIs(PEN_SET, "width 30\r\nsign a\r\n", "", L_SIGN, "b", "width 30\r\nsign b\n",
          "CR LF lines kept, the changed one written with LF");
    PenIs(PEN_SET, "sign a\n# keep\nsign b\nwidth 30\n", "", L_SIGN, "c", "# keep\nsign c\nwidth 30\n",
          "two lines for one setting: the last rewritten, the other gone");
    PenIs(PEN_SET, "sign a\n    color red\nsign b\n", "", L_SIGN, "c", "sign\n    color red\nsign c\n",
          "the other line holds lines: only its names stay");
    PenIs(PEN_SET, "sign\n    color red\n", "", L_SIGN, "»", "sign »\n    color red\n",
          "a line that only opened its block gets the value");
    PenIs(PEN_SET, "sign a\n[work]\n    sign w\n", "", L_SIGN, "b", "sign b\n[work]\n    sign w\n",
          "everywhere's line, not the context's");
    PenIs(PEN_SET, "sign a\n", "work", L_SIGN, "w", "sign a\n\n[work]\n    sign w\n", "the context's own, apart");
    PenIs(PEN_SET, "frob 5\n    sign a\n", "", L_SIGN, "b", "frob 5\n    sign a\nsign b\n",
          "a line inside one not taken is no line for the setting");
    PenIs(PEN_SET, "", "", L_NOTE, "", "note \"\"\n", "an empty text quoted");
    PenIs(PEN_SET, "", "", L_NOTE, " hi ", "note \" hi \"\n", "blanks at the ends quoted");
    PenIs(PEN_SET, "", "", L_SIGN, "default", "sign \"default\"\n", "the word default quoted");
    PenIs(PEN_SET, "", "", L_NOTE, "\"hi\"", "note \"\"hi\"\"\n", "quotes at both ends quoted");
    PenIs(PEN_SET, "", "", L_NOTE, "say \"hi\" now", "note say \"hi\" now\n", "quotes inside left as they are");
    PenIs(PEN_SET, "", "", L_TITLE, "color red", "title \"color red\"\n",
          "a value whose first word names what the setting holds, quoted");
    PenIs(PEN_SET, "", "", L_TITLE, "colorful day", "title colorful day\n", "a value that only begins like a name");
    PenIs(PEN_SET, "", "", L_TITLE, "color", "title color\n", "one word that is a name: the last word is the value");
}

static void TestPenErase(void)
{
    PenIs(PEN_ERASE, "sign a\n# keep\nsign b\nwidth 30\n", "", L_SIGN, NULL, "# keep\nwidth 30\n",
          "every line for a setting erased");
    PenIs(PEN_ERASE, "width 30\n", "", L_SIGN, NULL, "width 30\n", "erasing a setting with no line changes nothing");
    PenIs(PEN_ERASE, "cursor\n    shape block\nwidth 30\n", "", L_SHAPE, NULL, "width 30\n",
          "the block that held it goes when nothing is left in it");
    PenIs(PEN_ERASE, "cursor\n    shape block\n    color amber\n", "", L_SHAPE, NULL, "cursor\n    color amber\n",
          "the block stays while something stays in it");
    PenIs(PEN_ERASE, "sign »\n    color gray\n", "", L_SIGN, NULL, "sign\n    color gray\n",
          "a line that holds lines keeps its names");
    PenIs(PEN_ERASE, "sign »\n    color gray\n", "", L_SIGN_COLOR, NULL, "sign »\n",
          "a setting with a value of its own stays when what it held goes");
    PenIs(PEN_ERASE, "width 30\n[work]\n    sign w\n", "work", L_SIGN, NULL, "width 30\n",
          "a context's line goes with the last line in it");
    PenIs(PEN_ERASE, "[work]\n    # mine\n    sign w\n", "work", L_SIGN, NULL, "    # mine\n",
          "a comment stays where it was");
    PenIs(PEN_ERASE, "width 30\n\n[work]\n    sign w\n", "work", L_SIGN, NULL, "width 30\n",
          "the blank line before a context's line goes with it");
    PenIs(PEN_ERASE, "width 30\n\nsign a\n\n", "", L_SIGN, NULL, "width 30\n\n\n",
          "the person's blank lines stay with what stays");
    PenIs(PEN_ERASE, "\nsign a\n\n", "", L_SIGN, NULL, "", "a file left with nothing but blank lines is left empty");
    PenIs(PEN_ERASE, "cursor\n    blnk 5\n    shape block\n", "", L_SHAPE, NULL, "cursor\n    blnk 5\n",
          "a line the pen does not know keeps its block");
    PenIs(PEN_ERASE, "messages\n    color red\n    timing\n", "", L_MESSAGES_COLOR, NULL, "messages\n    timing\n",
          "a line it does not erase, with nothing in it, keeps its block too");
    PenIs(PEN_ERASE, "sign a\n[work]\n    sign w\n", "", L_SIGN, NULL, "[work]\n    sign w\n",
          "everywhere's line erased, the context's kept");
    PenIs(PEN_ERASE, "cursor\n    shape bar\n[work]\n    cursor\n        shape block\n", "work", L_SHAPE, NULL,
          "cursor\n    shape bar\n", "a context's block of blocks, all gone");
    PenIs(PEN_CLEAR, "# mine\ncursor\n    shape bar\nother-version x\n[work]\n    sign w\nwidth 30", "", 0, NULL,
          "# mine\nother-version x\n",
          "everything as it came: comments and names not declared stay, every context's lines go");
    PenIs(PEN_CLEAR, "sign a\n\n[work]\n    sign w\n\n[home]\n    width 30\n", "", 0, NULL, "",
          "everything as it came: the blocks the pen made, and the blank lines before them, all gone");
    PenIs(PEN_CLEAR, "[bad\n    sign w\nsign a\n", "", 0, NULL, "[bad\n    sign w\n",
          "a context that does not read, and what is under it, are not the pen's");
}

/* Every value the pen writes is read back as it was given. */
static void TestPenReadsBack(void)
{
    const char *values[] = { "»", "", " ", "  edges  ", "default", "\"", "\"\"", "\"a\"", "a\"", "\"a", "#7CD68A",
                             "two words", "tab\tinside", "\tlead", "trail\t", "x", "color", "color red", "color  red",
                             "\"color\" red", "# not a comment" };
    struct { const char *file; uint32_t node; const char *context; } places[] = {
        { "# c\ntitle old\n", L_TITLE, "" },
        { "", L_TITLE, "work" },
        { "title\n    color red\n", L_TITLE, "" },
    };
    unsigned held = 0, tried = 0;
    for (unsigned p = 0; p < sizeof(places) / sizeof(places[0]); p++) {
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            char        *file = Pen(PEN_SET, places[p].file, places[p].context, places[p].node, values[i]);
            SettingsLine line;
            bool ok = SettingsChosenFind(D(), DSize(), (const uint8_t *)file, strlen(file), Span(places[p].context),
                                         places[p].node, &line) &&
                      Is(line.Value, values[i]) && !line.Default && line.HasValue;
            tried++;
            if (ok) held++;
            else printf("FAIL the value '%s' is read back as '%.*s' from:\n%s\n", values[i], (int)line.Value.Length,
                        (const char *)line.Value.At, file);
            free(file);
        }
    }
    char label[100];
    snprintf(label, sizeof(label), "every value written is read back as it was (%u of %u)", held, tried);
    Check(held == tried, label);
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

static uint64_t Shuffled(uint8_t *bytes, uint64_t room)
{
    static const char *const pieces[] = {
        "settings\n", "sign ", "cursor ", "shape ", "color ", "blink ", "title ", "word ", "number ", "from ",
        "to ", "-3 ", "200 ", "one-of ", "block ", "bar ", "default ", "\"", "next-start ", "#FFB020 ", "amber ",
        "text ", "on-or-off ", "on ", "off ", "or ", "ms ", "\n", " ", "    ", "\t", "\r\n", "#", "[", "]", "work ",
        ",", "x", "\x01", "\x7f", "\"\"", "\xc2\xbb", "\"what\" "
    };
    uint64_t size  = 0;
    unsigned count = (unsigned)(Random() % 40);
    for (unsigned i = 0; i < count; i++) {
        const char *piece  = pieces[Random() % (sizeof(pieces) / sizeof(pieces[0]))];
        size_t      length = strlen(piece);
        if (size + length > room) break;
        memcpy(bytes + size, piece, length);
        size += length;
    }
    return size;
}

/* Bytes made of the declaration's and the file's own words, cut and shuffled,
 * with stray bytes: read as a declaration, and as a person's file the pen
 * changes — every span inside them, every change measured as written, a
 * setting set read back, one erased gone. */
static void TestRandom(void)
{
    unsigned runs = 20000, inside = 0;
    for (unsigned run = 0; run < runs; run++) {
        uint8_t  bytes[400];
        uint64_t size = Shuffled(bytes, sizeof(bytes));
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
                fine = fine && (setting.Kind == SETTING_GROUP ||
                                SettingValueRead(&setting, setting.Default, &value) == SETTINGS_OK) &&
                       setting.Name.At >= copy && setting.What.At + setting.What.Length <= copy + size &&
                       setting.Depth < SETTINGS_DEPTH_MAX;
            }

        SettingsChosen chosen;
        SettingsLine   line;
        SettingsChosenBegin(D(), DSize(), copy, size, &chosen);
        while (SettingsChosenNext(&chosen, &line))
            fine = fine && line.Start <= line.End && line.End <= size &&
                   line.Value.At + line.Value.Length <= copy + size && line.NamesEnd <= line.Whole.Length;

        static const char *const contexts[] = { "", "work", "home work" };
        const char *context = contexts[run % 3];
        uint64_t    measured, written;
        SettingsPenSet(D(), DSize(), copy, size, Span(context), L_SIGN_COLOR, Span("amber"), NULL, &measured);
        uint8_t *out = malloc(measured ? measured : 1);
        SettingsPenSet(D(), DSize(), copy, size, Span(context), L_SIGN_COLOR, Span("amber"), out, &written);
        SettingsLine back;
        fine = fine && written == measured &&
               SettingsChosenFind(D(), DSize(), out, written, Span(context), L_SIGN_COLOR, &back) &&
               Is(back.Value, "amber");

        SettingsPenErase(D(), DSize(), out, written, Span(context), L_SIGN_COLOR, NULL, &measured);
        uint8_t *erased = malloc(measured ? measured : 1);
        uint64_t erased_size;
        SettingsPenErase(D(), DSize(), out, written, Span(context), L_SIGN_COLOR, erased, &erased_size);
        fine = fine && erased_size == measured &&
               (!SettingsChosenFind(D(), DSize(), erased, erased_size, Span(context), L_SIGN_COLOR, &back) ||
                !back.HasValue);

        SettingsPenClear(D(), DSize(), copy, size, NULL, &measured);
        uint8_t *cleared = malloc(measured ? measured : 1);
        uint64_t cleared_size;
        SettingsPenClear(D(), DSize(), copy, size, cleared, &cleared_size);
        fine = fine && cleared_size == measured;
        SettingsChosenBegin(D(), DSize(), cleared, cleared_size, &chosen);
        while (SettingsChosenNext(&chosen, &line))
            fine = fine && !(line.Kind == SETTINGS_LINE_ENTRY && line.Named.Line != 0 && line.HasValue);

        free(cleared);
        free(erased);
        free(out);
        free(copy);
        if (fine) inside++;
        else if (run - inside < 3) printf("FAIL at random: run %u\n", run);
    }
    char label[100];
    snprintf(label, sizeof(label), "%u inputs made at random read inside their bounds (%u held)", runs, inside);
    Check(inside == runs, label);
}

int main(void)
{
    TestDeclared();
    TestZeroWord();
    TestDeclaredWrong();
    TestFind();
    TestValues();
    TestKeys();
    TestChosenTree();
    TestChosenContext();
    TestPenSet();
    TestPenErase();
    TestPenReadsBack();
    TestRandom();
    printf("settings: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
