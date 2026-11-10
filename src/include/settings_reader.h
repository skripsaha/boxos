#ifndef SETTINGS_READER_H
#define SETTINGS_READER_H

/*
 * The settings of a BoxOS program, read by one reader wherever they are read:
 * BoxLib in a program and in the utility `settings`, tools/pack and BoxASM
 * when they carry a declaration, inside when it says one, the stands on the
 * build machine. Written with settings_pen.h.
 *
 * Two knowledges, two owners — and both are trees: things, and what each
 * thing has.
 *
 * WHAT CAN BE SET the program knows, and says once, in its declaration: the
 * part `settings` of its box (in an ELF image the section .boxos.settings),
 * put there by BOX_SETTINGS or BoxASM's `setting`. It is the one source of
 * what each setting is, takes and starts as.
 *
 *     settings 1
 *     greeting on-or-off default on "the line said when the program starts"
 *     cursor "the mark where what you type goes"
 *         shape one-of block underline bar off default underline "its shape"
 *         color color default light-gray "its color"
 *         blink number from 100 to 5000 ms or off default off "how often it blinks"
 *     sign word default ~ "what stands before what you type"
 *         color color default light-gray "its color"
 *     history on-or-off default on next-start "keep what is typed from one start to the next"
 *
 *   NAME "what it holds"                                   a thing that only holds others
 *   NAME KIND [what the kind takes] default VALUE [next-start] "what it does"
 *
 * A line indented under another is inside it: what it has. Siblings stand at
 * one indent, in spaces; a thing that only holds others holds at least one.
 * Any setting may hold others too — the sign has a value, and a color.
 *
 *   NAME    a small letter, then small letters, digits and '-', not ending in
 *           '-'; at most SETTING_NAME_MAX bytes; not `settings` nor `default`;
 *           one sibling's only
 *   KIND    on-or-off                 on or off
 *           number from A to B [UNIT] [or off]
 *                                     a whole number from A to B, A no more
 *                                     than B; UNIT a word that says what it
 *                                     counts, which a value may repeat; `or
 *                                     off` lets the word off stand for 0, so
 *                                     A is then 1 or more
 *           word                      one word: no blank, no '"'
 *           text                      any text on one line, "" for none
 *           one-of W1 W2 ...          one of two or more different words
 *           color                     a color's name (SettingColorNames) or
 *                                     #RRGGBB
 *           key                       a key held with ctrl or alt, as a person
 *                                     spells it: ctrl+alt+q (key_chord.h)
 *   VALUE   a word, or text between double quotes — the only way to write a
 *           text with blanks at its ends, an empty one, or the word default
 *   next-start   the setting takes effect when the program starts next; one
 *           without it takes effect at once
 *
 * Things inside things go SETTINGS_DEPTH_MAX deep. The reader reads without
 * memory of its own, one line at a time, so what is open above a line is
 * held in a stack of that many levels; a declaration deeper than that is
 * refused where it is made — and a person reading a tree of settings would
 * be lost long before.
 *
 * The declaration is a part written as text (box_reader.h's rules: lines,
 * no byte below a blank but the newline). A line whose first byte past
 * blanks is '#' is a comment. A line this reader cannot read is a verdict the
 * caller says — a program passes over that setting, and all inside it, and
 * keeps the rest; a tool that writes the declaration refuses it.
 *
 * WHAT THE PERSON CHOSE the person knows: the file <program>.settings on the
 * volume, which holds only what they changed — read by settings_chosen.h, in
 * the names, values and verdicts said here.
 *
 * No allocation and no library: every span points into the bytes it was read
 * from. Integer types, bool and size_t come from the includer (see
 * box_format.h).
 */

#include "key_chord.h"

#define SETTING_NAME_MAX      32u
#define SETTINGS_DEPTH_MAX    8u
#define SETTINGS_TAB_STOP     4u
#define SETTINGS_VERSION_WORD "1"

typedef enum {
    SETTING_GROUP = 0,          /* holds others, and has no value of its own */
    SETTING_ON_OR_OFF,
    SETTING_NUMBER,
    SETTING_WORD,
    SETTING_TEXT,
    SETTING_ONE_OF,
    SETTING_COLOR,
    SETTING_KEY,
    SETTING_KIND_COUNT
} SettingKind;

typedef struct {
    const uint8_t *At;
    uint64_t       Length;
} SettingSpan;

/* One setting as its declaration says it. */
typedef struct {
    SettingSpan Name;
    SettingKind Kind;
    int64_t     From, To;       /* number */
    bool        Off;            /* number: `or off`, the word off read as 0 */
    SettingSpan Unit;           /* number: empty for none */
    SettingSpan Choices;        /* one-of: the words, as the line has them */
    SettingSpan Default;        /* the value, its quotes taken off; empty for a group */
    SettingSpan What;           /* what it does, or holds */
    bool        NextStart;
    uint32_t    Line;           /* its line in the declaration, from 1 */
    uint32_t    Parent;         /* the line of what it is inside; 0 at the top */
    uint32_t    Depth;          /* how many it is inside: 0 at the top */
    uint32_t    Indent;         /* its indent, in columns */
} Setting;

/* A value read for a setting: what the kind makes of it. */
typedef struct {
    bool        On;             /* on-or-off */
    int64_t     Number;         /* number: 0 for off */
    uint32_t    Color;          /* color: 0xRRGGBB */
    KeyChord    Key;            /* key */
    SettingSpan Text;           /* the value itself: a number's digits, or off */
} SettingValue;

typedef enum {
    SETTINGS_OK = 0,
    /* the declaration as a whole */
    SETTINGS_NOT_TEXT,          /* a byte below a blank but the newline, or DEL */
    SETTINGS_NO_HEAD,           /* the first line is not "settings 1" */
    /* where a line stands */
    SETTING_INDENT_BAD,         /* the declaration: indented unlike its siblings */
    SETTING_TOO_DEEP,           /* inside more than SETTINGS_DEPTH_MAX levels */
    SETTING_INSIDE_UNTAKEN,     /* inside a line that is not taken */
    SETTING_NOT_THERE,          /* the person's file: a name nothing declares where it stands */
    SETTING_CONTEXT_BAD,        /* a [ ] line that does not read as tags */
    SETTING_CONTEXT_INSIDE,     /* a [ ] line inside another line */
    SETTING_CONTEXT_EMPTY,      /* a [ ] line with nothing indented under it */
    /* a line of the declaration */
    SETTING_LINE_UNREAD,        /* an unclosed quote, or a quote not ended by a blank */
    SETTING_NAME_BAD,
    SETTING_NAME_TWICE,
    SETTING_HOLDS_NOTHING,      /* a thing that only holds others, with nothing inside */
    SETTING_KIND_UNKNOWN,
    SETTING_LIMITS_BAD,
    SETTING_UNIT_BAD,
    SETTING_CHOICES_BAD,
    SETTING_NO_DEFAULT,
    SETTING_DEFAULT_BAD,
    SETTING_WHAT_MISSING,
    SETTING_WORDS_AFTER,
    /* a value */
    SETTING_VALUE_EMPTY,
    SETTING_VALUE_NOT_ON_OR_OFF,
    SETTING_VALUE_NOT_NUMBER,
    SETTING_VALUE_OUTSIDE,
    SETTING_VALUE_NOT_WORD,
    SETTING_VALUE_NOT_TEXT,
    SETTING_VALUE_NOT_CHOICE,
    SETTING_VALUE_NOT_COLOR,
    SETTING_VALUE_NOT_KEY
} SettingsVerdict;

/* ── spans ───────────────────────────────────────────────────────────────── */

static inline SettingSpan SettingSpanOf(const uint8_t *at, uint64_t length)
{
    SettingSpan span;
    span.At     = at;
    span.Length = length;
    return span;
}

static inline bool SettingSpanIs(SettingSpan span, const char *word)
{
    uint64_t i = 0;
    for (; i < span.Length; i++)
        if (word[i] == '\0' || (uint8_t)word[i] != span.At[i]) return false;
    return word[i] == '\0';
}

static inline bool SettingSpanSame(SettingSpan a, SettingSpan b)
{
    if (a.Length != b.Length) return false;
    for (uint64_t i = 0; i < a.Length; i++)
        if (a.At[i] != b.At[i]) return false;
    return true;
}

static inline bool SettingBlank(uint8_t c)
{
    return c == ' ' || c == '\t';
}

/* A span with its blanks at both ends taken off. */
static inline SettingSpan SettingTrimmed(SettingSpan span)
{
    while (span.Length > 0 && SettingBlank(span.At[0])) {
        span.At++;
        span.Length--;
    }
    while (span.Length > 0 && SettingBlank(span.At[span.Length - 1])) span.Length--;
    return span;
}

/* ── words and lines ─────────────────────────────────────────────────────── */

/* The next word of a line from *at on: a run of bytes up to a blank, or text
 * between double quotes, the quotes taken off (*quoted). A quote must close
 * before the line ends and be followed by a blank or the end. False when the
 * line has no more words; SETTING_LINE_UNREAD in *verdict when it cannot be
 * read. */
static inline bool SettingWord(SettingSpan line, uint64_t *at, SettingSpan *word, bool *quoted,
                               SettingsVerdict *verdict)
{
    while (*at < line.Length && SettingBlank(line.At[*at])) (*at)++;
    if (*at == line.Length) return false;
    *quoted = line.At[*at] == '"';
    if (*quoted) {
        uint64_t start = ++(*at);
        while (*at < line.Length && line.At[*at] != '"') (*at)++;
        if (*at == line.Length || (*at + 1 < line.Length && !SettingBlank(line.At[*at + 1]))) {
            *verdict = SETTING_LINE_UNREAD;
            return false;
        }
        *word = SettingSpanOf(line.At + start, *at - start);
        (*at)++;
        return true;
    }
    uint64_t start = *at;
    while (*at < line.Length && !SettingBlank(line.At[*at])) (*at)++;
    *word = SettingSpanOf(line.At + start, *at - start);
    return true;
}

/* The next line from *at on, its CR at the end taken off; *number counts
 * every line, from 1. */
static inline bool SettingLine(const uint8_t *bytes, uint64_t size, uint64_t *at, SettingSpan *line,
                               uint32_t *number)
{
    if (*at >= size) return false;
    uint64_t start = *at, end = start;
    while (end < size && bytes[end] != '\n') end++;
    *at   = end < size ? end + 1 : end;
    *line = SettingSpanOf(bytes + start, end - start);
    if (line->Length > 0 && line->At[line->Length - 1] == '\r') line->Length--;
    (*number)++;
    return true;
}

/* A line's indent: *columns as wide as its blanks before the first word —
 * a tab reaching the next multiple of SETTINGS_TAB_STOP — and *bytes their
 * count. */
static inline void SettingIndent(SettingSpan line, uint32_t *columns, uint64_t *bytes)
{
    uint32_t width = 0;
    uint64_t at    = 0;
    while (at < line.Length && SettingBlank(line.At[at])) {
        width = line.At[at] == '\t' ? (width / SETTINGS_TAB_STOP + 1u) * SETTINGS_TAB_STOP : width + 1u;
        at++;
    }
    *columns = width;
    *bytes   = at;
}

/* Whether a line says something: not blank, not a comment. */
static inline bool SettingLineSays(SettingSpan line, uint64_t lead)
{
    return lead < line.Length && line.At[lead] != '#';
}

/* The indent of the next line from `at` on that says something; false when
 * none is left. */
static inline bool SettingNextIndent(const uint8_t *bytes, uint64_t size, uint64_t at, uint32_t *indent)
{
    SettingSpan line;
    uint32_t    number = 0;
    uint64_t    lead;
    while (SettingLine(bytes, size, &at, &line, &number)) {
        SettingIndent(line, indent, &lead);
        if (SettingLineSays(line, lead)) return true;
    }
    return false;
}

/* ── names and values ────────────────────────────────────────────────────── */

static inline bool SettingNameUsable(SettingSpan name)
{
    if (name.Length == 0 || name.Length > SETTING_NAME_MAX) return false;
    if (name.At[0] < 'a' || name.At[0] > 'z' || name.At[name.Length - 1] == '-') return false;
    for (uint64_t i = 1; i < name.Length; i++) {
        uint8_t c = name.At[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return !SettingSpanIs(name, "settings") && !SettingSpanIs(name, "default");
}

/* Text on one line: no byte below a blank but a tab, and no DEL. */
static inline bool SettingTextUsable(SettingSpan text)
{
    for (uint64_t i = 0; i < text.Length; i++)
        if ((text.At[i] < 0x20u && text.At[i] != '\t') || text.At[i] == 0x7Fu) return false;
    return true;
}

static inline bool SettingWordUsable(SettingSpan word)
{
    if (word.Length == 0 || !SettingTextUsable(word)) return false;
    for (uint64_t i = 0; i < word.Length; i++)
        if (SettingBlank(word.At[i]) || word.At[i] == '"') return false;
    return true;
}

/* A whole number: an optional '-', then digits with no leading zero but in
 * "0" itself, no "-0", within 64 bits. */
static inline bool SettingNumber(SettingSpan text, int64_t *out)
{
    uint64_t at = 0, limit = (uint64_t)INT64_MAX;
    bool     negative = text.Length > 0 && text.At[0] == '-';
    if (negative) {
        at    = 1;
        limit = (uint64_t)INT64_MAX + 1u;
    }
    uint64_t digits = text.Length - at;
    if (digits == 0 || (digits > 1 && text.At[at] == '0') || (negative && text.At[at] == '0')) return false;
    uint64_t value = 0;
    for (; at < text.Length; at++) {
        uint8_t c = text.At[at];
        if (c < '0' || c > '9') return false;
        uint64_t digit = (uint64_t)(c - '0');
        if (value > (limit - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = negative ? (int64_t)(0u - value) : (int64_t)value;
    return true;
}

static inline int SettingHex(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The colors a person may call by name, and what each is. */
typedef struct {
    const char *Name;
    uint32_t    Rgb;
} SettingColorName;

#define SETTING_COLOR_NAMES 15u

static inline const SettingColorName *SettingColorNames(void)
{
    static const SettingColorName names[SETTING_COLOR_NAMES] = {
        { "black", 0x000000u },  { "dark-gray", 0x555555u }, { "gray", 0x808080u },
        { "light-gray", 0xAAAAAAu }, { "white", 0xFFFFFFu }, { "red", 0xE04040u },
        { "green", 0x40C040u },  { "blue", 0x4060E0u },      { "cyan", 0x40C0C0u },
        { "yellow", 0xE0C040u }, { "amber", 0xFFB040u },     { "orange", 0xFF8020u },
        { "purple", 0xA060E0u }, { "pink", 0xFF80C0u },      { "brown", 0xA06030u },
    };
    return names;
}

/* A color as a person writes it: a name, or #RRGGBB in either case. */
static inline bool SettingColorRead(SettingSpan text, uint32_t *rgb)
{
    if (text.Length == 7 && text.At[0] == '#') {
        uint32_t value = 0;
        for (uint64_t i = 1; i < 7; i++) {
            int digit = SettingHex(text.At[i]);
            if (digit < 0) return false;
            value = value << 4 | (uint32_t)digit;
        }
        *rgb = value;
        return true;
    }
    const SettingColorName *names = SettingColorNames();
    for (unsigned i = 0; i < SETTING_COLOR_NAMES; i++) {
        if (SettingSpanIs(text, names[i].Name)) {
            *rgb = names[i].Rgb;
            return true;
        }
    }
    return false;
}

/* Whether a word is one of a one-of's choices. */
static inline bool SettingChoiceIs(SettingSpan choices, SettingSpan word)
{
    uint64_t        at = 0;
    SettingSpan     choice;
    bool            quoted;
    SettingsVerdict ignored;
    while (SettingWord(choices, &at, &choice, &quoted, &ignored))
        if (SettingSpanSame(choice, word)) return true;
    return false;
}

/* `value` read for the setting: SETTINGS_OK and what it is in *out, or the
 * verdict on why it does not fit. A number may be followed by the unit its
 * setting counts in: "500 ms". A thing that only holds others takes none. */
static inline SettingsVerdict SettingValueRead(const Setting *setting, SettingSpan value, SettingValue *out)
{
    const KeyChord none = { 0, 0, 0, 0 };
    out->On     = false;
    out->Number = 0;
    out->Color  = 0;
    out->Key    = none;
    out->Text   = value;
    if (!SettingTextUsable(value)) return SETTING_VALUE_NOT_TEXT;
    if (value.Length == 0 && setting->Kind != SETTING_TEXT) return SETTING_VALUE_EMPTY;

    switch (setting->Kind) {
    case SETTING_ON_OR_OFF:
        if (SettingSpanIs(value, "on")) out->On = true;
        else if (!SettingSpanIs(value, "off")) return SETTING_VALUE_NOT_ON_OR_OFF;
        return SETTINGS_OK;
    case SETTING_NUMBER: {
        if (setting->Off && SettingSpanIs(value, "off")) return SETTINGS_OK;
        SettingSpan number = value;
        uint64_t    end    = 0;
        while (end < value.Length && !SettingBlank(value.At[end])) end++;
        if (end < value.Length) {
            uint64_t unit_at = end;
            while (unit_at < value.Length && SettingBlank(value.At[unit_at])) unit_at++;
            SettingSpan unit = SettingSpanOf(value.At + unit_at, value.Length - unit_at);
            if (setting->Unit.Length == 0 || !SettingSpanSame(unit, setting->Unit)) return SETTING_VALUE_NOT_NUMBER;
            number.Length = end;
        }
        if (!SettingNumber(number, &out->Number)) return SETTING_VALUE_NOT_NUMBER;
        if (out->Number < setting->From || out->Number > setting->To) return SETTING_VALUE_OUTSIDE;
        out->Text = number;
        return SETTINGS_OK;
    }
    case SETTING_WORD:
        return SettingWordUsable(value) ? SETTINGS_OK : SETTING_VALUE_NOT_WORD;
    case SETTING_TEXT:
        return SETTINGS_OK;
    case SETTING_ONE_OF:
        return SettingChoiceIs(setting->Choices, value) ? SETTINGS_OK : SETTING_VALUE_NOT_CHOICE;
    case SETTING_COLOR:
        return SettingColorRead(value, &out->Color) ? SETTINGS_OK : SETTING_VALUE_NOT_COLOR;
    case SETTING_KEY:
        return KeyChordRead(value.At, value.Length, &out->Key) ? SETTINGS_OK : SETTING_VALUE_NOT_KEY;
    default:
        return SETTING_KIND_UNKNOWN;
    }
}

/* ── one line of the declaration ─────────────────────────────────────────── */

static inline const char *SettingKindWord(SettingKind kind)
{
    static const char *const words[SETTING_KIND_COUNT] = {
        "", "on-or-off", "number", "word", "text", "one-of", "color", "key"
    };
    return (unsigned)kind < SETTING_KIND_COUNT ? words[kind] : "";
}

/* The kind a word names; a thing that only holds others is named by none. */
static inline bool SettingKindOf(SettingSpan word, SettingKind *kind)
{
    for (unsigned i = SETTING_ON_OR_OFF; i < SETTING_KIND_COUNT; i++) {
        if (SettingSpanIs(word, SettingKindWord((SettingKind)i))) {
            *kind = (SettingKind)i;
            return true;
        }
    }
    return false;
}

/* What a number or one-of takes, after its kind: from A to B [UNIT] [or off],
 * or the choices up to `default`. *at is left on the word after them. */
static inline SettingsVerdict SettingKindWords(SettingSpan line, uint64_t *at, Setting *out)
{
    SettingSpan     word;
    bool            quoted;
    SettingsVerdict verdict = SETTINGS_OK;

    if (out->Kind == SETTING_NUMBER) {
        SettingSpan from, to;
        if (!SettingWord(line, at, &word, &quoted, &verdict) || quoted || !SettingSpanIs(word, "from") ||
            !SettingWord(line, at, &from, &quoted, &verdict) || quoted || !SettingNumber(from, &out->From) ||
            !SettingWord(line, at, &word, &quoted, &verdict) || quoted || !SettingSpanIs(word, "to") ||
            !SettingWord(line, at, &to, &quoted, &verdict) || quoted || !SettingNumber(to, &out->To) ||
            out->From > out->To)
            return verdict != SETTINGS_OK ? verdict : SETTING_LIMITS_BAD;
        uint64_t look = *at;
        if (SettingWord(line, &look, &word, &quoted, &verdict) && !quoted && !SettingSpanIs(word, "default") &&
            !SettingSpanIs(word, "or")) {
            for (uint64_t i = 0; i < word.Length; i++)
                if (!((word.At[i] >= 'a' && word.At[i] <= 'z') || (word.At[i] >= 'A' && word.At[i] <= 'Z')))
                    return SETTING_UNIT_BAD;
            out->Unit = word;
            *at       = look;
        }
        look = *at;
        if (SettingWord(line, &look, &word, &quoted, &verdict) && !quoted && SettingSpanIs(word, "or")) {
            if (!SettingWord(line, &look, &word, &quoted, &verdict) || quoted || !SettingSpanIs(word, "off") ||
                out->From < 1)
                return verdict != SETTINGS_OK ? verdict : SETTING_LIMITS_BAD;
            out->Off = true;
            *at      = look;
        }
        return verdict;
    }

    if (out->Kind == SETTING_ONE_OF) {
        uint64_t count = 0, start = *at, end = *at, look = *at;
        /* The choices are bare words: a quoted one ends them, as `default` does. */
        while (SettingWord(line, &look, &word, &quoted, &verdict) && !quoted && !SettingSpanIs(word, "default")) {
            SettingSpan before = SettingSpanOf(line.At + start, end - start);
            if (!SettingWordUsable(word) || SettingSpanIs(word, "next-start") || SettingChoiceIs(before, word))
                return SETTING_CHOICES_BAD;
            count++;
            end = look;
            *at = look;
        }
        if (verdict != SETTINGS_OK) return verdict;
        if (count < 2) return SETTING_CHOICES_BAD;
        while (start < end && SettingBlank(line.At[start])) start++;
        out->Choices = SettingSpanOf(line.At + start, end - start);
    }
    return SETTINGS_OK;
}

/* The setting a line declares, from its name at `at` on — what stands in
 * the tree round it is the caller's (SettingsDeclaredNext). */
static inline SettingsVerdict SettingRead(SettingSpan line, uint64_t at, uint32_t number, Setting *out)
{
    SettingSpan     word;
    bool            quoted;
    SettingsVerdict verdict = SETTINGS_OK;
    SettingSpan     none    = SettingSpanOf(line.At, 0);

    out->Name = out->Unit = out->Choices = out->Default = out->What = none;
    out->Kind      = SETTING_KIND_COUNT;
    out->From      = out->To = 0;
    out->Off       = false;
    out->NextStart = false;
    out->Line      = number;
    out->Parent    = out->Depth = out->Indent = 0;
    if (!SettingWord(line, &at, &out->Name, &quoted, &verdict) || quoted || !SettingNameUsable(out->Name))
        return verdict != SETTINGS_OK ? verdict : SETTING_NAME_BAD;
    if (!SettingWord(line, &at, &word, &quoted, &verdict))
        return verdict != SETTINGS_OK ? verdict : SETTING_KIND_UNKNOWN;

    if (quoted) {
        /* A thing that only holds others: its name and what it holds. */
        out->Kind = SETTING_GROUP;
        if (word.Length == 0 || !SettingTextUsable(word)) return SETTING_WHAT_MISSING;
        out->What = word;
        if (SettingWord(line, &at, &word, &quoted, &verdict) || verdict != SETTINGS_OK)
            return verdict != SETTINGS_OK ? verdict : SETTING_WORDS_AFTER;
        return SETTINGS_OK;
    }
    if (!SettingKindOf(word, &out->Kind)) return SETTING_KIND_UNKNOWN;
    if ((verdict = SettingKindWords(line, &at, out)) != SETTINGS_OK) return verdict;

    if (!SettingWord(line, &at, &word, &quoted, &verdict) || quoted || !SettingSpanIs(word, "default") ||
        !SettingWord(line, &at, &out->Default, &quoted, &verdict))
        return verdict != SETTINGS_OK ? verdict : SETTING_NO_DEFAULT;
    if (!quoted && SettingSpanIs(out->Default, "default")) return SETTING_DEFAULT_BAD;

    if (!SettingWord(line, &at, &word, &quoted, &verdict))
        return verdict != SETTINGS_OK ? verdict : SETTING_WHAT_MISSING;
    if (!quoted && SettingSpanIs(word, "next-start")) {
        out->NextStart = true;
        if (!SettingWord(line, &at, &word, &quoted, &verdict))
            return verdict != SETTINGS_OK ? verdict : SETTING_WHAT_MISSING;
    }
    if (!quoted || word.Length == 0 || !SettingTextUsable(word)) return SETTING_WHAT_MISSING;
    out->What = word;
    if (SettingWord(line, &at, &word, &quoted, &verdict) || verdict != SETTINGS_OK)
        return verdict != SETTINGS_OK ? verdict : SETTING_WORDS_AFTER;

    SettingValue value;
    return SettingValueRead(out, out->Default, &value) == SETTINGS_OK ? SETTINGS_OK : SETTING_DEFAULT_BAD;
}

/* ── the declaration ─────────────────────────────────────────────────────── */

/* A line of the declaration open above the one being read. */
typedef struct {
    uint32_t Indent;
    uint32_t Line;
    uint32_t ChildIndent;       /* where its first line inside stood */
    bool     HasChild;
    bool     Held;              /* it holds: what is inside it may be read */
} SettingsDeclaredLevel;

typedef struct {
    const uint8_t        *Bytes;
    uint64_t              Size;
    uint64_t              At;           /* where the next line starts */
    uint32_t              Line;         /* the line read last */
    SettingsDeclaredLevel Open[SETTINGS_DEPTH_MAX];
    uint32_t              Opened;
    uint32_t              TopIndent;    /* where the first line at the top stood */
    bool                  TopSeen;
    bool                  Deep;         /* a line was too deep: */
    uint32_t              DeepIndent;   /* what is indented past it is too */
} SettingsDeclared;

/* The declaration's `size` bytes at `bytes`, opened past its first line,
 * which must be "settings 1". */
static inline SettingsVerdict SettingsDeclaredBegin(const uint8_t *bytes, uint64_t size, SettingsDeclared *out)
{
    out->Bytes   = bytes;
    out->Size    = size;
    out->At      = 0;
    out->Line    = 0;
    out->Opened  = 0;
    out->TopSeen = false;
    out->Deep    = false;
    for (uint64_t i = 0; i < size; i++)
        if ((bytes[i] < 0x20u && bytes[i] != '\n') || bytes[i] == 0x7Fu) return SETTINGS_NOT_TEXT;

    SettingSpan     line, word;
    uint64_t        at = 0;
    bool            quoted;
    SettingsVerdict verdict = SETTINGS_OK;
    while (SettingLine(bytes, size, &out->At, &line, &out->Line)) {
        at = 0;
        if (!SettingWord(line, &at, &word, &quoted, &verdict)) continue;
        if (quoted || !SettingSpanIs(word, "settings")) return SETTINGS_NO_HEAD;
        if (!SettingWord(line, &at, &word, &quoted, &verdict) || quoted ||
            !SettingSpanIs(word, SETTINGS_VERSION_WORD))
            return SETTINGS_NO_HEAD;
        return SettingWord(line, &at, &word, &quoted, &verdict) ? SETTINGS_NO_HEAD : SETTINGS_OK;
    }
    return SETTINGS_NO_HEAD;
}

static inline bool SettingsDeclaredStep(SettingsDeclared *declared, Setting *out, SettingsVerdict *verdict,
                                        bool siblings);

/* Whether a line before `before`, inside the line `parent` (0: the top),
 * declares `name` — by where lines stand, every line read as it is. */
static inline bool SettingsDeclaredSiblingBefore(const uint8_t *bytes, uint64_t size, uint32_t parent,
                                                 SettingSpan name, uint32_t before)
{
    SettingsDeclared declared;
    Setting          setting;
    SettingsVerdict  verdict;
    if (SettingsDeclaredBegin(bytes, size, &declared) != SETTINGS_OK) return false;
    while (SettingsDeclaredStep(&declared, &setting, &verdict, false) && setting.Line < before)
        if (verdict == SETTINGS_OK && setting.Parent == parent && SettingSpanSame(setting.Name, name)) return true;
    return false;
}

/* The next line that says something, read as a setting where it stands:
 * true with *verdict, and out->Line the line; false when none is left.
 * `siblings` asks whether an earlier sibling had the name — a lookup by name
 * needs not ask: it takes the first, which is the one that holds. */
static inline bool SettingsDeclaredStep(SettingsDeclared *declared, Setting *out, SettingsVerdict *verdict,
                                        bool siblings)
{
    SettingSpan line;
    while (SettingLine(declared->Bytes, declared->Size, &declared->At, &line, &declared->Line)) {
        uint32_t indent;
        uint64_t lead;
        SettingIndent(line, &indent, &lead);
        if (!SettingLineSays(line, lead)) continue;

        *verdict      = SettingRead(line, lead, declared->Line, out);
        out->Indent   = indent;
        if (declared->Deep && indent > declared->DeepIndent) {
            *verdict = SETTING_TOO_DEEP;
            return true;
        }
        declared->Deep = false;
        while (declared->Opened > 0 && declared->Open[declared->Opened - 1].Indent >= indent) declared->Opened--;

        SettingsDeclaredLevel *parent = declared->Opened ? &declared->Open[declared->Opened - 1] : (SettingsDeclaredLevel *)0;
        bool                  *seen   = parent ? &parent->HasChild : &declared->TopSeen;
        uint32_t              *at     = parent ? &parent->ChildIndent : &declared->TopIndent;
        out->Parent = parent ? parent->Line : 0;
        out->Depth  = declared->Opened;

        if (declared->Opened == SETTINGS_DEPTH_MAX) {
            declared->Deep       = true;
            declared->DeepIndent = indent;
            *verdict             = SETTING_TOO_DEEP;
            return true;
        }
        if (parent && !parent->Held) {
            *verdict = SETTING_INSIDE_UNTAKEN;
        } else if (*seen && indent != *at) {
            *verdict = SETTING_INDENT_BAD;
        } else if (*verdict == SETTINGS_OK && siblings &&
                   SettingsDeclaredSiblingBefore(declared->Bytes, declared->Size, out->Parent, out->Name,
                                                 out->Line)) {
            *verdict = SETTING_NAME_TWICE;
        } else if (*verdict == SETTINGS_OK && out->Kind == SETTING_GROUP) {
            uint32_t next;
            if (!SettingNextIndent(declared->Bytes, declared->Size, declared->At, &next) || next <= indent)
                *verdict = SETTING_HOLDS_NOTHING;
        }
        if (!*seen) {
            *seen = true;
            *at   = indent;
        }
        SettingsDeclaredLevel *level = &declared->Open[declared->Opened++];
        level->Indent      = indent;
        level->Line        = out->Line;
        level->ChildIndent = 0;
        level->HasChild    = false;
        level->Held        = *verdict == SETTINGS_OK;
        return true;
    }
    return false;
}

/* The next line of the declaration that says something: true with
 * SETTINGS_OK and the setting in *out, or with the verdict on a line that is
 * not taken — out->Line says which, and out->Name is its name when that much
 * was read. False when no line is left. */
static inline bool SettingsDeclaredNext(SettingsDeclared *declared, Setting *out, SettingsVerdict *verdict)
{
    return SettingsDeclaredStep(declared, out, verdict, true);
}

/* The setting declared on line `line`, when it is taken there. */
static inline bool SettingsDeclaredAt(const uint8_t *bytes, uint64_t size, uint32_t line, Setting *out)
{
    SettingsDeclared declared;
    SettingsVerdict  verdict;
    if (line == 0 || SettingsDeclaredBegin(bytes, size, &declared) != SETTINGS_OK) return false;
    while (SettingsDeclaredStep(&declared, out, &verdict, false)) {
        if (out->Line == line) return verdict == SETTINGS_OK;
        if (out->Line > line) return false;
    }
    return false;
}

/* The setting called `name` inside the line `parent` (0: at the top). */
static inline bool SettingsDeclaredChild(const uint8_t *bytes, uint64_t size, uint32_t parent, SettingSpan name,
                                         Setting *out)
{
    SettingsDeclared declared;
    SettingsVerdict  verdict;
    if (SettingsDeclaredBegin(bytes, size, &declared) != SETTINGS_OK) return false;
    while (SettingsDeclaredStep(&declared, out, &verdict, false))
        if (verdict == SETTINGS_OK && out->Parent == parent && SettingSpanSame(out->Name, name)) return true;
    return false;
}

/* Whether the line `line` holds a setting inside it. */
static inline bool SettingsDeclaredHolds(const uint8_t *bytes, uint64_t size, uint32_t line)
{
    SettingsDeclared declared;
    SettingsVerdict  verdict;
    Setting          setting;
    if (SettingsDeclaredBegin(bytes, size, &declared) != SETTINGS_OK) return false;
    while (SettingsDeclaredStep(&declared, &setting, &verdict, false))
        if (verdict == SETTINGS_OK && setting.Parent == line) return true;
    return false;
}

/* The setting a path names — names apart by blanks, from the top down:
 * "cursor blink". */
static inline bool SettingsDeclaredPath(const uint8_t *bytes, uint64_t size, SettingSpan path, Setting *out)
{
    uint64_t        at     = 0;
    uint32_t        parent = 0;
    SettingSpan     name;
    bool            quoted, named = false;
    SettingsVerdict ignored = SETTINGS_OK;
    while (SettingWord(path, &at, &name, &quoted, &ignored)) {
        if (quoted || !SettingsDeclaredChild(bytes, size, parent, name, out)) return false;
        parent = out->Line;
        named  = true;
    }
    return named && ignored == SETTINGS_OK;
}

/* ── names near each other ───────────────────────────────────────────────── */

/* How many edits — a byte put in, taken out, changed, or two side by side
 * swapped — make one name the other; past SETTING_NAME_MAX bytes, far. */
static inline uint32_t SettingDistance(SettingSpan a, SettingSpan b)
{
    if (a.Length > SETTING_NAME_MAX || b.Length > SETTING_NAME_MAX) return ~0u;
    uint32_t before[SETTING_NAME_MAX + 1], previous[SETTING_NAME_MAX + 1], current[SETTING_NAME_MAX + 1];
    for (uint64_t j = 0; j <= b.Length; j++) previous[j] = before[j] = (uint32_t)j;
    for (uint64_t i = 1; i <= a.Length; i++) {
        current[0] = (uint32_t)i;
        for (uint64_t j = 1; j <= b.Length; j++) {
            uint32_t cost = a.At[i - 1] == b.At[j - 1] ? 0u : 1u;
            uint32_t best = previous[j] + 1;
            if (current[j - 1] + 1 < best) best = current[j - 1] + 1;
            if (previous[j - 1] + cost < best) best = previous[j - 1] + cost;
            if (i > 1 && j > 1 && a.At[i - 1] == b.At[j - 2] && a.At[i - 2] == b.At[j - 1] &&
                before[j - 2] + 1 < best)
                best = before[j - 2] + 1;
            current[j] = best;
        }
        for (uint64_t j = 0; j <= b.Length; j++) {
            before[j]   = previous[j];
            previous[j] = current[j];
        }
    }
    return previous[b.Length];
}

/* The setting inside `parent` (0: the top) whose name is nearest `name`,
 * when one is near enough to be what was meant: at most two edits, and fewer
 * than the name is long. */
static inline bool SettingsDeclaredNearest(const uint8_t *bytes, uint64_t size, uint32_t parent, SettingSpan name,
                                           Setting *out)
{
    SettingsDeclared declared;
    SettingsVerdict  verdict;
    Setting          setting;
    uint32_t         best  = 3;
    bool             found = false;
    if (SettingsDeclaredBegin(bytes, size, &declared) != SETTINGS_OK) return false;
    while (SettingsDeclaredStep(&declared, &setting, &verdict, false)) {
        if (verdict != SETTINGS_OK || setting.Parent != parent) continue;
        uint32_t distance = SettingDistance(name, setting.Name);
        if (distance < best && distance < name.Length) {
            best  = distance;
            *out  = setting;
            found = true;
        }
    }
    return found;
}

/* The first setting called `name` anywhere in the tree: where a line meant
 * for it belongs when it stands where nothing has that name. */
static inline bool SettingsDeclaredSomewhere(const uint8_t *bytes, uint64_t size, SettingSpan name, Setting *out)
{
    SettingsDeclared declared;
    SettingsVerdict  verdict;
    if (SettingsDeclaredBegin(bytes, size, &declared) != SETTINGS_OK) return false;
    while (SettingsDeclaredStep(&declared, out, &verdict, false))
        if (verdict == SETTINGS_OK && SettingSpanSame(out->Name, name)) return true;
    return false;
}

/* ── words for a verdict ─────────────────────────────────────────────────── */

/* Why a line of the declaration is refused, said so it follows "the setting
 * NAME" or "line N"; why a line of the person's file is not taken, said so
 * it follows "line N"; a value's verdict, said so it follows the value. */
static inline const char *SettingsVerdictText(SettingsVerdict verdict)
{
    switch (verdict) {
    case SETTINGS_OK:                 return "it holds";
    case SETTINGS_NOT_TEXT:           return "is not text: it holds a byte below a blank other than a newline";
    case SETTINGS_NO_HEAD:            return "does not begin with the line \"settings 1\"";
    case SETTING_INDENT_BAD:          return "is indented unlike the lines beside it";
    case SETTING_TOO_DEEP:            return "stands deeper than 8 levels of things inside things";
    case SETTING_INSIDE_UNTAKEN:      return "stands inside a line that is not taken";
    case SETTING_NOT_THERE:           return "names a setting there is none of there";
    case SETTING_CONTEXT_BAD:         return "is no context: tags between [ and ], at least one, none twice, "
                                             "nothing after the ]";
    case SETTING_CONTEXT_INSIDE:      return "is a context inside another line: a [ ] line stands at the top";
    case SETTING_CONTEXT_EMPTY:       return "holds nothing: the lines for that context go below it, indented";
    case SETTING_LINE_UNREAD:         return "cannot be read: a quote is not closed, or not followed by a blank";
    case SETTING_NAME_BAD:            return "has a name a setting cannot have: a small letter, then small "
                                             "letters, digits and -, at most 32, not settings or default";
    case SETTING_NAME_TWICE:          return "is declared twice in one place";
    case SETTING_HOLDS_NOTHING:       return "holds nothing: what it holds goes below it, indented";
    case SETTING_KIND_UNKNOWN:        return "is of a kind this reader does not know";
    case SETTING_LIMITS_BAD:          return "does not say from and to as two whole numbers, the smaller first "
                                             "(and from 1 or more with or off)";
    case SETTING_UNIT_BAD:            return "has a unit that is not a word of letters";
    case SETTING_CHOICES_BAD:         return "does not have two or more different words to choose from";
    case SETTING_NO_DEFAULT:          return "has no default";
    case SETTING_DEFAULT_BAD:         return "has a default that does not fit it";
    case SETTING_WHAT_MISSING:        return "does not say what it does, in double quotes, last";
    case SETTING_WORDS_AFTER:         return "has words after what it does";
    case SETTING_VALUE_EMPTY:         return "is empty";
    case SETTING_VALUE_NOT_ON_OR_OFF: return "is not on or off";
    case SETTING_VALUE_NOT_NUMBER:    return "is not a whole number";
    case SETTING_VALUE_OUTSIDE:       return "is outside its limits";
    case SETTING_VALUE_NOT_WORD:      return "is not one word";
    case SETTING_VALUE_NOT_TEXT:      return "is not text";
    case SETTING_VALUE_NOT_CHOICE:    return "is not one of its choices";
    case SETTING_VALUE_NOT_COLOR:     return "is not a color: a name such as amber, or #RRGGBB";
    case SETTING_VALUE_NOT_KEY:       return "is not a key held with ctrl or alt, such as ctrl+alt+q";
    }
    return "is refused";
}

#endif /* SETTINGS_READER_H */
