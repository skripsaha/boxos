/*
 * Gangway — its settings: the person's gangway.settings, read by the
 * declaration boxos_gangway_settings.h gives and by the one reader every
 * program's settings are read by (settings_reader.h, settings_chosen.h). The
 * Lectern finds the file by its name and holds it to the book; gangway_attest.c
 * hands its bytes here.
 *
 * What the person set is taken where the value fits. Every other line is named
 * in the journal in the words BoxLib names it in (boxlib settings_read.c) —
 * "gangway.settings, line 3: cores wants a whole number from 1 to 256, or all;
 * all is kept" — and what it would have set stays as Gangway came. Lines for a
 * Use Context are passed over, said once at their [ ] line: Gangway starts
 * before there is a context. What was taken is said last, in one line.
 *
 * The screen is Gangway's own (gangway_video.c reads g_settings); the rest is
 * the kernel's, handed on as stamps of the Boarding Pass (gangway_handoff.c).
 */

#include "gangway.h"
#include "boarding_pass.h"
#include "boxos_gangway_settings.h"
#include "settings_chosen.h"

GangwaySettings g_settings = { 0, 0, 0, BOARDING_STARTUP_QUIET, 0, 0 };

static const char g_declared[] = GANGWAY_SETTINGS;

/* The most lines a declaration as short as Gangway's has; a line set before is
 * looked up by its line in it. */
#define DECLARED_LINES_MOST 16u

static void PrintSpan(SettingSpan span)
{
    char     piece[65];
    uint64_t at = 0;
    while (at < span.Length) {
        uint32_t n = 0;
        while (n < sizeof(piece) - 1u && at < span.Length) piece[n++] = (char)span.At[at++];
        piece[n] = '\0';
        Print(piece);
    }
}

/* "Gangway: gangway.settings, line N: " */
static void LineHead(uint32_t line)
{
    Print("Gangway: " GANGWAY_SETTINGS_FILE ", line ");
    PrintDec(line);
    Print(": ");
}

/* What a setting of the declaration takes, as BoxLib's settings_wants says it. */
static void PrintWants(const Setting *s)
{
    switch (s->Kind) {
    case SETTING_ON_OR_OFF: Print("on or off"); return;
    case SETTING_NUMBER:
        Print("a whole number from ");
        PrintDec((uint64_t)s->From);
        Print(" to ");
        PrintDec((uint64_t)s->To);
        if (s->Unit.Length) {
            Print(" ");
            PrintSpan(s->Unit);
        }
        if (s->Zero.Length) {
            Print(", or ");
            PrintSpan(s->Zero);
        }
        return;
    case SETTING_ONE_OF: {
        SettingSpan     choice;
        SettingsVerdict ignored;
        uint64_t        at = 0;
        bool            quoted;
        const char     *between = " ";
        Print("one of");
        while (SettingWord(s->Choices, &at, &choice, &quoted, &ignored)) {
            Print(between);
            PrintSpan(choice);
            between = ", ";
        }
        return;
    }
    case SETTING_WORD: Print("one word"); return;
    default: Print("what this reader does not know"); return;
    }
}

/* "1920x1080" as a width and a height, each a whole number from 1: 1 so. */
static int ScreenSize(SettingSpan v, uint32_t *width, uint32_t *height)
{
    uint64_t w = 0, h = 0, at = 0;
    while (at < v.Length && v.At[at] >= '0' && v.At[at] <= '9' && w <= 100000u) w = w * 10u + (v.At[at++] - '0');
    if (!w || at == 0 || at >= v.Length || v.At[at++] != 'x') return 0;
    uint64_t from = at;
    while (at < v.Length && v.At[at] >= '0' && v.At[at] <= '9' && h <= 100000u) h = h * 10u + (v.At[at++] - '0');
    if (!h || at == from || at != v.Length) return 0;
    *width  = (uint32_t)w;
    *height = (uint32_t)h;
    return 1;
}

/* The value `v` — one that fits setting `s` — taken, `set` when it is the
 * person's own and not the word default: 1, or 0 when it is one Gangway
 * cannot take after all (a screen that is no size), said. */
static int Taken(const Setting *s, SettingSpan v, uint32_t line, bool set)
{
    SettingValue value;
    uint32_t     bit = 0;
    if (SettingValueRead(s, v, &value) != SETTINGS_OK) return 0;
    if (SettingSpanIs(s->Name, "screen")) {
        uint32_t w = 0, h = 0;
        bit = GANGWAY_SET_SCREEN;
        if (!SettingSpanIs(v, "auto") && !ScreenSize(v, &w, &h)) {
            LineHead(line);
            Print("screen wants auto, or as wide x as tall, 1920x1080; ");
            PrintSpan(v);
            Print(" is no size, and auto is kept\n");
            return 0;
        }
        g_settings.screen_width  = w;
        g_settings.screen_height = h;
    } else if (SettingSpanIs(s->Name, "startup")) {
        bit                = GANGWAY_SET_STARTUP;
        g_settings.startup = SettingSpanIs(v, "all") ? BOARDING_STARTUP_ALL : BOARDING_STARTUP_QUIET;
    } else if (SettingSpanIs(s->Name, "cores")) {
        bit              = GANGWAY_SET_CORES;
        g_settings.cores = (uint32_t)value.Number;
    } else if (SettingSpanIs(s->Name, "keep-log")) {
        bit                 = GANGWAY_SET_KEEP_LOG;
        g_settings.keep_log = value.On ? 1u : 0u;
    }
    g_settings.set = set ? g_settings.set | bit : g_settings.set & ~bit;
    return 1;
}

/* A line whose names lead to a setting of the declaration. */
static void Named(const SettingsLine *line, uint32_t *before)
{
    const Setting *s = &line->Named;
    if (!line->HasValue && line->Verdict == SETTINGS_OK) return;   /* it only opens what is under it */
    if (s->Line < DECLARED_LINES_MOST && before[s->Line]) {
        LineHead(line->Line);
        PrintSpan(s->Name);
        Print(" is set before, on line ");
        PrintDec(before[s->Line]);
        Print("; this line is taken\n");
    }
    if (s->Line < DECLARED_LINES_MOST) before[s->Line] = line->Line;
    if (line->Verdict == SETTINGS_OK) {
        if (Taken(s, line->Default ? s->Default : line->Value, line->Line, !line->Default)) return;
    } else {
        LineHead(line->Line);
        PrintSpan(s->Name);
        Print(" wants ");
        PrintWants(s);
        Print("; ");
        PrintSpan(s->Default);
        Print(" is kept\n");
    }
    (void)Taken(s, s->Default, line->Line, false);
}

/* A line not taken, said: its name the declaration has none of — and the
 * nearest that it has, when one is near — or what is wrong with it. */
static void Untaken(const SettingsLine *line)
{
    Setting near;
    LineHead(line->Line);
    if (line->Verdict == SETTING_NOT_THERE) {
        Print(GANGWAY_SETTINGS_PROGRAM " has no setting ");
        PrintSpan(line->Unknown);
        if (SettingsDeclaredNearest((const uint8_t *)g_declared, sizeof(g_declared) - 1u, 0, line->Unknown, &near)) {
            Print(" — did you mean ");
            PrintSpan(near.Name);
            Print("?\n");
        } else {
            Print("; the line is left as it is\n");
        }
        return;
    }
    if (line->Verdict == SETTING_INSIDE_UNTAKEN) {
        Print("it stands inside line ");
        PrintDec(line->Parent);
        Print(", which is not taken; it is passed over\n");
        return;
    }
    Print("the line ");
    Print(SettingsVerdictText(line->Verdict));
    Print("; it is passed over\n");
}

/* What the file sets, each as it is taken — "screen 1920x1080, cores 2" — or
 * that it sets nothing. */
static void TakenSaid(void)
{
    const char *between = "";
    Print("Gangway: " GANGWAY_SETTINGS_FILE " sets ");
    if (g_settings.set & GANGWAY_SET_SCREEN) {
        Print("screen ");
        if (g_settings.screen_width) {
            PrintDec(g_settings.screen_width);
            Print("x");
            PrintDec(g_settings.screen_height);
        } else {
            Print("auto");
        }
        between = ", ";
    }
    if (g_settings.set & GANGWAY_SET_STARTUP) {
        Print(between);
        Print(g_settings.startup == BOARDING_STARTUP_ALL ? "startup all" : "startup quiet");
        between = ", ";
    }
    if (g_settings.set & GANGWAY_SET_CORES) {
        Print(between);
        Print("cores ");
        if (g_settings.cores) PrintDec(g_settings.cores);
        else Print("all");
        between = ", ";
    }
    if (g_settings.set & GANGWAY_SET_KEEP_LOG) {
        Print(between);
        Print(g_settings.keep_log ? "keep-log on" : "keep-log off");
        between = ", ";
    }
    Print(between[0] ? "\n" : "nothing — everything as Gangway came\n");
}

void SettingsTake(const uint8_t *text, uint64_t bytes)
{
    SettingsChosen chosen;
    SettingsLine   line;
    uint32_t       before[DECLARED_LINES_MOST];

    MemZero(before, sizeof(before));
    SettingsChosenBegin((const uint8_t *)g_declared, sizeof(g_declared) - 1u, text, bytes, &chosen);
    while (SettingsChosenNext(&chosen, &line)) {
        if (line.Kind == SETTINGS_LINE_NOT_TEXT) {
            Print("Gangway: " GANGWAY_SETTINGS_FILE ", line ");
            PrintDec(line.Line);
            Print(" is not text; it is passed over\n");
        } else if (line.Kind == SETTINGS_LINE_CONTEXT) {
            if (line.Verdict != SETTINGS_OK) {
                Untaken(&line);
                continue;
            }
            LineHead(line.Line);
            Print("[");
            PrintSpan(line.Context);
            Print("] — Gangway starts before any Use Context; what is under it is passed over\n");
        } else if (line.Kind == SETTINGS_LINE_ENTRY && !line.ContextLine) {
            if (line.Named.Line != 0) Named(&line, before);
            else Untaken(&line);
        }
    }
    TakenSaid();
}
