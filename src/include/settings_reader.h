#ifndef SETTINGS_READER_H
#define SETTINGS_READER_H

/*
 * The settings of a BoxOS program, read by one reader wherever they are read:
 * BoxLib in a program and in the utility `settings`, tools/pack and BoxASM
 * when they carry a declaration, inside when it says one, the stands on the
 * build machine. Written with settings_pen.h.
 *
 * Two knowledges, two owners.
 *
 * WHAT CAN BE SET the program knows, and says once, in its declaration: the
 * part `settings` of its box (in an ELF image the section .boxos.settings),
 * put there by BOX_SETTINGS or BoxASM's `setting`. It is the one source of
 * what each setting is, takes and starts as.
 *
 *     settings 1
 *     setting history yes-or-no default yes next-start "keep what is typed from one start to the next"
 *     setting prompt word default ~ "what stands before what you type"
 *     setting width number from 24 to 200 cells default 72 "how wide the column is"
 *     setting look one-of dark light default dark "how the page looks"
 *     setting prompt-color color default #FFB020 "the colour of the prompt"
 *     setting greeting text default "BoxOS Hub" "the line said at start"
 *
 *   setting NAME KIND [what the kind takes] default VALUE [next-start] "what it does"
 *
 *   NAME    a small letter, then small letters, digits and '-', not ending in
 *           '-'; at most SETTING_NAME_MAX bytes; not `settings` nor `when`,
 *           which begin other lines of the person's file; one setting's only
 *   KIND    yes-or-no                 yes or no
 *           number from A to B [UNIT] a whole number from A to B, A no more
 *                                     than B; UNIT a word that says what it
 *                                     counts, which a value may repeat
 *           word                      one word: no blank, no '"'
 *           text                      any text on one line, "" for none
 *           one-of W1 W2 ...          one of two or more different words
 *           color                     #RRGGBB
 *   VALUE   a word, or text between double quotes — the only way to write a
 *           text with blanks at its ends, an empty one, or the word default
 *   next-start   the setting takes effect when the program starts next; one
 *           without it takes effect at once
 *
 * The declaration is a part written as text (box_reader.h's rules: lines,
 * no byte below a blank but the newline). A line that does not begin with
 * `setting` is passed over; a setting line of a kind this reader does not
 * know, or one it cannot read, is a verdict the caller says — a program skips
 * that setting and keeps the rest, a tool that writes the declaration
 * refuses it.
 *
 * WHAT THE PERSON CHOSE the person knows: the file <program>.settings on the
 * volume, which holds only what they changed. There is none until they change
 * something; with none, everything is as the program came.
 *
 *     settings 1
 *     # the prompt I like
 *     prompt »
 *     history no
 *     prompt-color #7CD68A
 *     when work prompt w»
 *
 *   settings 1     the first line, written by the library; a file without it
 *                  is read as settings 1
 *   # ...          a comment: a whole line whose first byte past blanks is '#'
 *                  (a value may hold '#': a colour does)
 *   NAME VALUE     VALUE the rest of the line, its blanks at both ends taken
 *                  off; between double quotes it is taken as it stands; the
 *                  bare word default means the program's own value
 *   when TAGS NAME VALUE   a setting for a Use Context, for later: known, and
 *                  passed over by this version
 *
 * Blanks are spaces and tabs, and a line may end in CR LF. A line that says
 * nothing this reader knows is the caller's to name, never a reason to stop.
 *
 * No allocation and no library: every span points into the bytes it was read
 * from. Integer types and bool come from the includer (see box_format.h).
 */

#define SETTING_NAME_MAX    32u
#define SETTINGS_VERSION_WORD "1"

typedef enum {
    SETTING_YES_OR_NO = 0,
    SETTING_NUMBER,
    SETTING_WORD,
    SETTING_TEXT,
    SETTING_ONE_OF,
    SETTING_COLOR,
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
    SettingSpan Unit;           /* number: empty for none */
    SettingSpan Choices;        /* one-of: the words, as the line has them */
    SettingSpan Default;        /* the value, its quotes taken off */
    SettingSpan What;           /* what it does */
    bool        NextStart;
    uint32_t    Line;           /* its line in the declaration, from 1 */
} Setting;

/* A value read for a setting: what the kind makes of it. */
typedef struct {
    bool        Yes;            /* yes-or-no */
    int64_t     Number;         /* number */
    uint32_t    Color;          /* color: 0xRRGGBB */
    SettingSpan Text;           /* word, text, one-of: the value itself */
} SettingValue;

typedef enum {
    SETTINGS_OK = 0,
    /* the declaration as a whole */
    SETTINGS_NOT_TEXT,          /* a byte below a blank but the newline, or DEL */
    SETTINGS_NO_HEAD,           /* the first line is not "settings 1" */
    /* a setting line */
    SETTING_LINE_UNREAD,        /* an unclosed quote, or a quote not ended by a blank */
    SETTING_NAME_BAD,
    SETTING_NAME_TWICE,
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
    SETTING_VALUE_NOT_YES_OR_NO,
    SETTING_VALUE_NOT_NUMBER,
    SETTING_VALUE_OUTSIDE,
    SETTING_VALUE_NOT_WORD,
    SETTING_VALUE_NOT_TEXT,
    SETTING_VALUE_NOT_CHOICE,
    SETTING_VALUE_NOT_COLOR
} SettingsVerdict;

/* ── spans ───────────────────────────────────────────────────────────────── */

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

/* ── words of a line ─────────────────────────────────────────────────────── */

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
        word->At     = line.At + start;
        word->Length = *at - start;
        (*at)++;
        return true;
    }
    uint64_t start = *at;
    while (*at < line.Length && !SettingBlank(line.At[*at])) (*at)++;
    word->At     = line.At + start;
    word->Length = *at - start;
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
    *at          = end < size ? end + 1 : end;
    line->At     = bytes + start;
    line->Length = end - start;
    if (line->Length > 0 && line->At[line->Length - 1] == '\r') line->Length--;
    (*number)++;
    return true;
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
    return !SettingSpanIs(name, "settings") && !SettingSpanIs(name, "when");
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
 * setting counts in: "72 cells". */
static inline SettingsVerdict SettingValueRead(const Setting *setting, SettingSpan value, SettingValue *out)
{
    out->Yes    = false;
    out->Number = 0;
    out->Color  = 0;
    out->Text   = value;
    if (!SettingTextUsable(value)) return SETTING_VALUE_NOT_TEXT;
    if (value.Length == 0 && setting->Kind != SETTING_TEXT) return SETTING_VALUE_EMPTY;

    switch (setting->Kind) {
    case SETTING_YES_OR_NO:
        if (SettingSpanIs(value, "yes")) out->Yes = true;
        else if (!SettingSpanIs(value, "no")) return SETTING_VALUE_NOT_YES_OR_NO;
        return SETTINGS_OK;
    case SETTING_NUMBER: {
        SettingSpan number = value;
        uint64_t    end    = 0;
        while (end < value.Length && !SettingBlank(value.At[end])) end++;
        if (end < value.Length) {
            uint64_t    unit_at = end;
            while (unit_at < value.Length && SettingBlank(value.At[unit_at])) unit_at++;
            SettingSpan unit = { value.At + unit_at, value.Length - unit_at };
            if (setting->Unit.Length == 0 || !SettingSpanSame(unit, setting->Unit))
                return SETTING_VALUE_NOT_NUMBER;
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
        if (value.Length != 7 || value.At[0] != '#') return SETTING_VALUE_NOT_COLOR;
        for (uint64_t i = 1; i < 7; i++) {
            int digit = SettingHex(value.At[i]);
            if (digit < 0) return SETTING_VALUE_NOT_COLOR;
            out->Color = out->Color << 4 | (uint32_t)digit;
        }
        return SETTINGS_OK;
    default:
        return SETTING_KIND_UNKNOWN;
    }
}

/* ── the declaration ─────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *Bytes;
    uint64_t       Size;
    uint64_t       At;          /* where the next line starts */
    uint32_t       Line;        /* the line read last */
} SettingsDeclared;

/* The declaration's `size` bytes at `bytes`, opened past its first line,
 * which must be "settings 1". */
static inline SettingsVerdict SettingsDeclaredBegin(const uint8_t *bytes, uint64_t size, SettingsDeclared *out)
{
    out->Bytes = bytes;
    out->Size  = size;
    out->At    = 0;
    out->Line  = 0;
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

/* The kind a word names. */
static inline bool SettingKindOf(SettingSpan word, SettingKind *kind)
{
    static const char *const words[SETTING_KIND_COUNT] = {
        "yes-or-no", "number", "word", "text", "one-of", "color"
    };
    for (unsigned i = 0; i < SETTING_KIND_COUNT; i++) {
        if (SettingSpanIs(word, words[i])) {
            *kind = (SettingKind)i;
            return true;
        }
    }
    return false;
}

static inline const char *SettingKindWord(SettingKind kind)
{
    static const char *const words[SETTING_KIND_COUNT] = {
        "yes-or-no", "number", "word", "text", "one-of", "color"
    };
    return (unsigned)kind < SETTING_KIND_COUNT ? words[kind] : "";
}

/* What a number or one-of takes, after its kind: from A to B [UNIT], or the
 * choices up to `default`. *at is left on the word after them. */
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
        if (SettingWord(line, &look, &word, &quoted, &verdict) && !quoted && !SettingSpanIs(word, "default")) {
            for (uint64_t i = 0; i < word.Length; i++)
                if (!((word.At[i] >= 'a' && word.At[i] <= 'z') || (word.At[i] >= 'A' && word.At[i] <= 'Z')))
                    return SETTING_UNIT_BAD;
            out->Unit = word;
            *at       = look;
        }
        return verdict;
    }

    if (out->Kind == SETTING_ONE_OF) {
        uint64_t count = 0, start = *at, end = *at, look = *at;
        /* The choices are bare words: a quoted one ends them, as `default` does. */
        while (SettingWord(line, &look, &word, &quoted, &verdict) && !quoted && !SettingSpanIs(word, "default")) {
            SettingSpan before = { line.At + start, end - start };
            if (!SettingWordUsable(word) || SettingSpanIs(word, "next-start") || SettingChoiceIs(before, word))
                return SETTING_CHOICES_BAD;
            count++;
            end = look;
            *at = look;
        }
        if (verdict != SETTINGS_OK) return verdict;
        if (count < 2) return SETTING_CHOICES_BAD;
        while (start < end && SettingBlank(line.At[start])) start++;
        out->Choices.At     = line.At + start;
        out->Choices.Length = end - start;
    }
    return SETTINGS_OK;
}

/* The setting a line declares, from its first word past `setting`. */
static inline SettingsVerdict SettingRead(SettingSpan line, uint64_t at, uint32_t number, Setting *out)
{
    SettingSpan     word;
    bool            quoted;
    SettingsVerdict verdict = SETTINGS_OK;

    *out      = (Setting){ { line.At, 0 }, SETTING_KIND_COUNT, 0, 0, { line.At, 0 }, { line.At, 0 },
                           { line.At, 0 }, { line.At, 0 }, false, number };
    if (!SettingWord(line, &at, &out->Name, &quoted, &verdict) || quoted || !SettingNameUsable(out->Name))
        return verdict != SETTINGS_OK ? verdict : SETTING_NAME_BAD;
    if (!SettingWord(line, &at, &word, &quoted, &verdict) || quoted || !SettingKindOf(word, &out->Kind))
        return verdict != SETTINGS_OK ? verdict : SETTING_KIND_UNKNOWN;
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

/* Whether a setting line before line `before` declares `name`. */
static inline bool SettingsDeclaredBefore(const SettingsDeclared *declared, SettingSpan name, uint32_t before)
{
    uint64_t        at = 0, word_at;
    uint32_t        number = 0;
    SettingSpan     line, word;
    bool            quoted;
    SettingsVerdict ignored;
    while (SettingLine(declared->Bytes, declared->Size, &at, &line, &number) && number < before) {
        word_at = 0;
        if (SettingWord(line, &word_at, &word, &quoted, &ignored) && !quoted && SettingSpanIs(word, "setting") &&
            SettingWord(line, &word_at, &word, &quoted, &ignored) && !quoted && SettingSpanSame(word, name))
            return true;
    }
    return false;
}

/* The next setting line of the declaration: true with SETTINGS_OK and the
 * setting in *out, or with the verdict on a line that cannot be read —
 * out->Line says which, and out->Name is its name when that much was read.
 * False when no setting line is left. */
static inline bool SettingsDeclaredNext(SettingsDeclared *declared, Setting *out, SettingsVerdict *verdict)
{
    SettingSpan     line, word;
    bool            quoted;
    SettingsVerdict unread = SETTINGS_OK;
    while (SettingLine(declared->Bytes, declared->Size, &declared->At, &line, &declared->Line)) {
        uint64_t at = 0;
        if (!SettingWord(line, &at, &word, &quoted, &unread) || quoted || !SettingSpanIs(word, "setting"))
            continue;
        *verdict = SettingRead(line, at, declared->Line, out);
        if (*verdict == SETTINGS_OK && SettingsDeclaredBefore(declared, out->Name, declared->Line))
            *verdict = SETTING_NAME_TWICE;
        return true;
    }
    return false;
}

/* The setting called `name`, as the declaration says it, when a line that can
 * be read declares it (the first such line). */
static inline bool SettingsDeclaredFind(const uint8_t *bytes, uint64_t size, SettingSpan name, Setting *out)
{
    SettingsDeclared declared;
    SettingsVerdict  verdict;
    if (SettingsDeclaredBegin(bytes, size, &declared) != SETTINGS_OK) return false;
    while (SettingsDeclaredNext(&declared, out, &verdict))
        if (verdict == SETTINGS_OK && SettingSpanSame(out->Name, name)) return true;
    return false;
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

/* The declared setting whose name is nearest `name`, when one is near enough
 * to be what was meant: at most two edits, and fewer than the name is long. */
static inline bool SettingsDeclaredNearest(const uint8_t *bytes, uint64_t size, SettingSpan name, Setting *out)
{
    SettingsDeclared declared;
    SettingsVerdict  verdict;
    Setting          setting;
    uint32_t         best = 3;
    bool             found = false;
    if (SettingsDeclaredBegin(bytes, size, &declared) != SETTINGS_OK) return false;
    while (SettingsDeclaredNext(&declared, &setting, &verdict)) {
        if (verdict != SETTINGS_OK) continue;
        uint32_t distance = SettingDistance(name, setting.Name);
        if (distance < best && distance < name.Length) {
            best  = distance;
            *out  = setting;
            found = true;
        }
    }
    return found;
}

/* ── the person's file ───────────────────────────────────────────────────── */

typedef enum {
    SETTINGS_LINE_BLANK = 0,
    SETTINGS_LINE_COMMENT,
    SETTINGS_LINE_HEAD,         /* settings N: Value is N */
    SETTINGS_LINE_SET,          /* NAME VALUE */
    SETTINGS_LINE_WHEN,         /* when TAGS NAME VALUE: passed over by this version */
    SETTINGS_LINE_NOT_TEXT      /* a byte below a blank but a tab, or DEL */
} SettingsLineKind;

typedef struct {
    SettingsLineKind Kind;
    SettingSpan      Whole;     /* the line as it stands, its CR and LF aside */
    SettingSpan      Name;      /* SET */
    SettingSpan      Value;     /* SET: its blanks at both ends and its quotes taken off; HEAD: N */
    bool             Quoted;    /* SET: the value was between double quotes */
    bool             Default;   /* SET: the bare word default — the program's own value */
    uint32_t         Line;      /* from 1 */
    uint64_t         Start;     /* where the line starts in the file */
    uint64_t         End;       /* where the next one starts */
} SettingsLine;

typedef struct {
    const uint8_t *Bytes;
    uint64_t       Size;
    uint64_t       At;
    uint32_t       Line;
} SettingsChosen;

static inline void SettingsChosenBegin(const uint8_t *bytes, uint64_t size, SettingsChosen *out)
{
    out->Bytes = bytes;
    out->Size  = size;
    out->At    = 0;
    out->Line  = 0;
}

/* A value as the rest of a line gives it: blanks at both ends taken off; text
 * between double quotes as it stands; the bare word default. */
static inline SettingSpan SettingsValueOf(SettingSpan rest, bool *quoted, bool *is_default)
{
    while (rest.Length > 0 && SettingBlank(rest.At[0])) {
        rest.At++;
        rest.Length--;
    }
    while (rest.Length > 0 && SettingBlank(rest.At[rest.Length - 1])) rest.Length--;
    *is_default = SettingSpanIs(rest, "default");
    *quoted     = rest.Length >= 2 && rest.At[0] == '"' && rest.At[rest.Length - 1] == '"';
    if (*quoted) {
        rest.At++;
        rest.Length -= 2;
    }
    return rest;
}

/* The next line of the person's file, whatever it says. */
static inline bool SettingsChosenNext(SettingsChosen *chosen, SettingsLine *out)
{
    uint64_t    start = chosen->At;
    SettingSpan line;
    if (!SettingLine(chosen->Bytes, chosen->Size, &chosen->At, &line, &chosen->Line)) return false;

    *out = (SettingsLine){ SETTINGS_LINE_BLANK, line, { line.At, 0 }, { line.At, 0 }, false, false, chosen->Line,
                           start, chosen->At };
    if (!SettingTextUsable(line)) {
        out->Kind = SETTINGS_LINE_NOT_TEXT;
        return true;
    }
    uint64_t at = 0;
    while (at < line.Length && SettingBlank(line.At[at])) at++;
    if (at == line.Length) return true;
    if (line.At[at] == '#') {
        out->Kind = SETTINGS_LINE_COMMENT;
        return true;
    }

    uint64_t name_at = at;
    while (at < line.Length && !SettingBlank(line.At[at])) at++;
    SettingSpan first = { line.At + name_at, at - name_at };
    SettingSpan rest  = { line.At + at, line.Length - at };
    if (SettingSpanIs(first, "settings")) {
        bool quoted, is_default;
        out->Kind  = SETTINGS_LINE_HEAD;
        out->Value = SettingsValueOf(rest, &quoted, &is_default);
    } else if (SettingSpanIs(first, "when")) {
        out->Kind = SETTINGS_LINE_WHEN;
    } else {
        out->Kind  = SETTINGS_LINE_SET;
        out->Name  = first;
        out->Value = SettingsValueOf(rest, &out->Quoted, &out->Default);
    }
    return true;
}

/* The line of the person's file that sets `name` — the last one, as the last
 * one is what counts. */
static inline bool SettingsChosenFind(const uint8_t *bytes, uint64_t size, SettingSpan name, SettingsLine *out)
{
    SettingsChosen chosen;
    SettingsLine   line;
    bool           found = false;
    SettingsChosenBegin(bytes, size, &chosen);
    while (SettingsChosenNext(&chosen, &line)) {
        if (line.Kind == SETTINGS_LINE_SET && SettingSpanSame(line.Name, name)) {
            *out  = line;
            found = true;
        }
    }
    return found;
}

/* ── words for a verdict ─────────────────────────────────────────────────── */

/* Why a declaration or a setting line is refused, said so it follows the
 * setting's name or "line N: "; a value's verdict as it follows the value. */
static inline const char *SettingsVerdictText(SettingsVerdict verdict)
{
    switch (verdict) {
    case SETTINGS_OK:                 return "it holds";
    case SETTINGS_NOT_TEXT:           return "is not text: it holds a byte below a blank other than a newline";
    case SETTINGS_NO_HEAD:            return "does not begin with the line \"settings 1\"";
    case SETTING_LINE_UNREAD:         return "cannot be read: a quote is not closed, or not followed by a blank";
    case SETTING_NAME_BAD:            return "has a name a setting cannot have: a small letter, then small "
                                             "letters, digits and -, at most 32, not settings or when";
    case SETTING_NAME_TWICE:          return "is declared twice";
    case SETTING_KIND_UNKNOWN:        return "is of a kind this reader does not know";
    case SETTING_LIMITS_BAD:          return "does not say from and to as two whole numbers, the smaller first";
    case SETTING_UNIT_BAD:            return "has a unit that is not a word of letters";
    case SETTING_CHOICES_BAD:         return "does not have two or more different words to choose from";
    case SETTING_NO_DEFAULT:          return "has no default";
    case SETTING_DEFAULT_BAD:         return "has a default that does not fit it";
    case SETTING_WHAT_MISSING:        return "does not say what it does, in double quotes, last";
    case SETTING_WORDS_AFTER:         return "has words after what it does";
    case SETTING_VALUE_EMPTY:         return "is empty";
    case SETTING_VALUE_NOT_YES_OR_NO: return "is not yes or no";
    case SETTING_VALUE_NOT_NUMBER:    return "is not a whole number";
    case SETTING_VALUE_OUTSIDE:       return "is outside its limits";
    case SETTING_VALUE_NOT_WORD:      return "is not one word";
    case SETTING_VALUE_NOT_TEXT:      return "is not text";
    case SETTING_VALUE_NOT_CHOICE:    return "is not one of its choices";
    case SETTING_VALUE_NOT_COLOR:     return "is not a colour written #RRGGBB";
    }
    return "is refused";
}

#endif /* SETTINGS_READER_H */
