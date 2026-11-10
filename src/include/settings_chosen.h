#ifndef SETTINGS_CHOSEN_H
#define SETTINGS_CHOSEN_H

#include "settings_reader.h"

/*
 * What a person chose in a program's settings, read by the one reader of
 * settings (settings_reader.h): the lines of the person's file, each read
 * down the tree of the declaration from what it stands inside.
 *
 * WHAT THE PERSON CHOSE the person knows: the file <program>.settings on the
 * volume, which holds only what they changed. There is none until they change
 * something; with none, everything is as the program came.
 *
 *     # the cursor I like
 *     cursor
 *         shape  block
 *         color  amber
 *     sign  »
 *     history off
 *     cursor blink 500 ms
 *     [work]
 *         sign  w»
 *             color  green
 *
 *   NAMES [VALUE]  the names lead down the tree from what the line stands
 *                  inside — a line indented under another stands inside it —
 *                  and the rest of the line is the value: its blanks at both
 *                  ends taken off, text between double quotes as it stands,
 *                  the bare word default for the program's own value. One
 *                  line may name a whole path, as the utility settings takes
 *                  it: `cursor blink 500 ms`. A line with no value only opens
 *                  what is under it.
 *   [TAGS]         what is indented under it is for the Use Context when it
 *                  holds every one of the tags, apart by blanks or commas —
 *                  the context as the prompt shows it; at the top only
 *   # ...          a comment: a whole line whose first byte past blanks is '#'
 *                  (a value may hold '#': a color does)
 *
 * Blanks are spaces and tabs — a tab reaching the next column that is a
 * multiple of SETTINGS_TAB_STOP — and a line may end in CR LF. A line's
 * indent is only compared with the lines above it: deeper is inside. Words
 * after a name that only holds others name what it holds; after a name with
 * a value of its own they name what it holds while another word follows, the
 * last always the value. A line that says nothing this reader knows is the
 * caller's to name, never a reason to stop.
 *
 * No allocation and no library: every span points into the bytes it was read
 * from. Integer types and bool come from the includer (see box_format.h).
 */

/* ── the person's file ───────────────────────────────────────────────────── */

typedef enum {
    SETTINGS_LINE_BLANK = 0,
    SETTINGS_LINE_COMMENT,
    SETTINGS_LINE_CONTEXT,      /* [TAGS] */
    SETTINGS_LINE_ENTRY,        /* NAMES [VALUE] */
    SETTINGS_LINE_NOT_TEXT      /* a byte below a blank but a tab, or DEL */
} SettingsLineKind;

typedef struct {
    SettingsLineKind Kind;
    SettingSpan      Whole;     /* the line as it stands, its CR and LF aside */
    uint32_t         Line;      /* from 1 */
    uint64_t         Start;     /* where the line starts in the file */
    uint64_t         End;       /* where the next one starts */
    uint32_t         Indent;    /* in columns */
    uint64_t         Lead;      /* the bytes of its indent */
    SettingsVerdict  Verdict;   /* CONTEXT, ENTRY */
    SettingSpan      Context;   /* the tags of the [ ] block it stands in — a [ ] line's own; empty: everywhere */
    uint32_t         ContextLine;   /* that [ ] line; 0: everywhere */
    uint32_t         Parent;    /* ENTRY: the line of the file it stands inside; 0 at the top */
    Setting          Named;     /* ENTRY: what its names lead to — Named.Line 0 when they lead nowhere */
    SettingSpan      Unknown;   /* SETTING_NOT_THERE: the name nothing has */
    uint32_t         UnknownIn; /* the declared line it was looked for inside; 0: the top */
    uint64_t         NamesEnd;  /* in Whole, where its names end */
    bool             HasValue;  /* ENTRY: the line gives a value */
    SettingSpan      Value;     /* its blanks at both ends and its quotes taken off */
    bool             Quoted;    /* the value was between double quotes */
    bool             Default;   /* the bare word default — the program's own value */
} SettingsLine;

/* A line of the person's file open above the one being read. */
typedef struct {
    uint32_t Indent;
    uint32_t Line;
    uint32_t Node;              /* the declared line its names lead to; 0 for a [ ] line */
    bool     Held;              /* what is inside it may be read */
} SettingsChosenLevel;

typedef struct {
    const uint8_t      *Declared;
    uint64_t            DeclaredSize;
    const uint8_t      *Bytes;
    uint64_t            Size;
    uint64_t            At;
    uint32_t            Line;
    SettingsChosenLevel Open[SETTINGS_DEPTH_MAX + 1];   /* a [ ] line, and the tree under it */
    uint32_t            Opened;
    SettingSpan         Context;
    uint32_t            ContextLine;
    bool                Deep;
    uint32_t            DeepIndent;
} SettingsChosen;

static inline void SettingsChosenBegin(const uint8_t *declared, uint64_t declared_size, const uint8_t *bytes,
                                       uint64_t size, SettingsChosen *out)
{
    out->Declared     = declared;
    out->DeclaredSize = declared_size;
    out->Bytes        = bytes;
    out->Size         = size;
    out->At           = 0;
    out->Line         = 0;
    out->Opened       = 0;
    out->Context      = SettingSpanOf(bytes, 0);
    out->ContextLine  = 0;
    out->Deep         = false;
}

/* A value as the rest of a line gives it: blanks at both ends taken off; text
 * between double quotes as it stands; the bare word default. */
static inline SettingSpan SettingsValueOf(SettingSpan rest, bool *quoted, bool *is_default)
{
    rest        = SettingTrimmed(rest);
    *is_default = SettingSpanIs(rest, "default");
    *quoted     = rest.Length >= 2 && rest.At[0] == '"' && rest.At[rest.Length - 1] == '"';
    if (*quoted) {
        rest.At++;
        rest.Length -= 2;
    }
    return rest;
}

static inline bool SettingTagSeparator(uint8_t c)
{
    return SettingBlank(c) || c == ',';
}

/* The next tag of a list from *at on: apart by blanks or commas, and spelled
 * as the kernel spells a tag — "key:" is the bare "key". */
static inline bool SettingTag(SettingSpan tags, uint64_t *at, SettingSpan *tag)
{
    while (*at < tags.Length && SettingTagSeparator(tags.At[*at])) (*at)++;
    if (*at == tags.Length) return false;
    uint64_t start = *at;
    while (*at < tags.Length && !SettingTagSeparator(tags.At[*at])) (*at)++;
    uint64_t length = *at - start;
    if (length > 1 && tags.At[start + length - 1] == ':') length--;
    *tag = SettingSpanOf(tags.At + start, length);
    return true;
}

/* Whether a list of tags holds `tag`. */
static inline bool SettingTagsHold(SettingSpan tags, SettingSpan tag)
{
    uint64_t    at = 0;
    SettingSpan one;
    while (SettingTag(tags, &at, &one))
        if (SettingSpanSame(one, tag)) return true;
    return false;
}

static inline uint32_t SettingTagsCount(SettingSpan tags)
{
    uint64_t    at    = 0;
    uint32_t    count = 0;
    SettingSpan one;
    while (SettingTag(tags, &at, &one)) count++;
    return count;
}

/* Whether `context` holds every tag of `tags`: the Use Context, comma-joined
 * as use_get says it, holding a block's tags. */
static inline bool SettingTagsWithin(SettingSpan tags, SettingSpan context)
{
    uint64_t    at = 0;
    SettingSpan one;
    while (SettingTag(tags, &at, &one))
        if (!SettingTagsHold(context, one)) return false;
    return true;
}

/* Whether two lists name the same tags, in any order: an empty one names
 * none — everywhere. */
static inline bool SettingTagsSame(SettingSpan a, SettingSpan b)
{
    uint64_t    at = 0;
    SettingSpan one;
    if (SettingTagsCount(a) != SettingTagsCount(b)) return false;
    while (SettingTag(a, &at, &one))
        if (!SettingTagsHold(b, one)) return false;
    return true;
}

/* The tags between the brackets of a [ ] line, read: at least one, none
 * twice, each with a key before any colon, without a bracket or a quote,
 * nothing after the ]. */
static inline bool SettingContextRead(SettingSpan line, uint64_t lead, SettingSpan *tags)
{
    SettingSpan rest = SettingTrimmed(SettingSpanOf(line.At + lead, line.Length - lead));
    if (rest.Length < 2 || rest.At[0] != '[' || rest.At[rest.Length - 1] != ']') return false;
    *tags = SettingTrimmed(SettingSpanOf(rest.At + 1, rest.Length - 2));
    uint64_t    at = 0;
    SettingSpan tag;
    uint32_t    count = 0;
    while (SettingTag(*tags, &at, &tag)) {
        if (tag.At[0] == ':') return false;
        for (uint64_t i = 0; i < tag.Length; i++)
            if (tag.At[i] == '[' || tag.At[i] == ']' || tag.At[i] == '"') return false;
        SettingSpan before = SettingSpanOf(tags->At, (uint64_t)(tag.At - tags->At));
        if (SettingTagsHold(before, tag)) return false;
        count++;
    }
    return count > 0;
}

/* The names of an ENTRY line followed down the tree from the declared line
 * `parent`, and the rest of it taken as its value. */
static inline void SettingsChosenNames(const SettingsChosen *chosen, uint32_t parent, SettingsLine *out)
{
    SettingSpan     line = out->Whole, word = SettingSpanOf(out->Whole.At, 0);
    uint64_t        at   = out->Lead;
    bool            quoted  = false;
    SettingsVerdict ignored = SETTINGS_OK;

    if (!SettingWord(line, &at, &word, &quoted, &ignored) || quoted || ignored != SETTINGS_OK ||
        !SettingsDeclaredChild(chosen->Declared, chosen->DeclaredSize, parent, word, &out->Named)) {
        out->Verdict      = ignored != SETTINGS_OK ? SETTING_LINE_UNREAD : SETTING_NOT_THERE;
        out->Unknown      = word;
        out->UnknownIn    = parent;
        out->Named.Line = 0;
        return;
    }
    for (;;) {
        uint64_t    look = at, after;
        SettingSpan next, last;
        Setting     inside;
        if (!SettingWord(line, &look, &next, &quoted, &ignored)) break;
        if (out->Named.Kind == SETTING_GROUP) {
            /* What only holds others has no value: a word after it names what it holds. */
            if (quoted || !SettingsDeclaredChild(chosen->Declared, chosen->DeclaredSize, out->Named.Line, next,
                                                 &inside)) {
                out->Verdict      = SETTING_NOT_THERE;
                out->Unknown      = next;
                out->UnknownIn    = out->Named.Line;
                out->Named.Line = 0;
                return;
            }
        } else {
            after = look;
            if (quoted || !SettingWord(line, &after, &last, &quoted, &ignored) ||
                !SettingsDeclaredChild(chosen->Declared, chosen->DeclaredSize, out->Named.Line, next, &inside))
                break;
        }
        out->Named = inside;
        at           = look;
    }
    out->NamesEnd = at;

    SettingSpan rest = SettingTrimmed(SettingSpanOf(line.At + at, line.Length - at));
    out->HasValue    = rest.Length > 0;
    out->Value       = SettingsValueOf(rest, &out->Quoted, &out->Default);
    if (!out->HasValue) {
        out->Verdict = SettingsDeclaredHolds(chosen->Declared, chosen->DeclaredSize, out->Named.Line)
                           ? SETTINGS_OK
                           : SETTING_VALUE_EMPTY;
    } else if (!out->Default) {
        SettingValue value;
        out->Verdict = SettingValueRead(&out->Named, out->Value, &value);
    }
}

/* The next line of the person's file, whatever it says — an ENTRY line read
 * down the tree from what it stands inside, its Verdict SETTINGS_OK when its
 * names lead to a declared setting and its value fits; out->Named is what
 * they lead to even when the value does not fit. */
static inline bool SettingsChosenNext(SettingsChosen *chosen, SettingsLine *out)
{
    uint64_t    start = chosen->At;
    SettingSpan line;
    if (!SettingLine(chosen->Bytes, chosen->Size, &chosen->At, &line, &chosen->Line)) return false;

    out->Kind         = SETTINGS_LINE_BLANK;
    out->Whole        = line;
    out->Line         = chosen->Line;
    out->Start        = start;
    out->End          = chosen->At;
    out->Verdict      = SETTINGS_OK;
    out->Context      = SettingSpanOf(line.At, 0);
    out->ContextLine  = 0;
    out->Parent       = 0;
    out->Named.Line = 0;
    out->Unknown      = SettingSpanOf(line.At, 0);
    out->UnknownIn    = 0;
    out->NamesEnd     = line.Length;
    out->HasValue     = out->Quoted = out->Default = false;
    out->Value        = SettingSpanOf(line.At + line.Length, 0);
    SettingIndent(line, &out->Indent, &out->Lead);
    if (!SettingTextUsable(line)) {
        out->Kind = SETTINGS_LINE_NOT_TEXT;
        return true;
    }
    if (out->Lead == line.Length) return true;
    if (line.At[out->Lead] == '#') {
        out->Kind = SETTINGS_LINE_COMMENT;
        return true;
    }
    bool context = line.At[out->Lead] == '[';
    out->Kind    = context ? SETTINGS_LINE_CONTEXT : SETTINGS_LINE_ENTRY;
    if (chosen->Deep && out->Indent > chosen->DeepIndent) {
        out->Verdict = SETTING_TOO_DEEP;
        return true;
    }
    chosen->Deep = false;
    while (chosen->Opened > 0 && chosen->Open[chosen->Opened - 1].Indent >= out->Indent) chosen->Opened--;
    if (chosen->Opened == 0) {
        chosen->Context     = SettingSpanOf(line.At, 0);
        chosen->ContextLine = 0;
    }

    SettingsChosenLevel *parent = chosen->Opened ? &chosen->Open[chosen->Opened - 1] : (SettingsChosenLevel *)0;
    out->Parent      = parent ? parent->Line : 0;
    out->Context     = chosen->Context;
    out->ContextLine = chosen->ContextLine;
    if (chosen->Opened == SETTINGS_DEPTH_MAX + 1u) {
        chosen->Deep       = true;
        chosen->DeepIndent = out->Indent;
        out->Verdict       = SETTING_TOO_DEEP;
        return true;
    }

    uint32_t node = 0;
    if (context) {
        SettingSpan tags;
        if (parent) {
            out->Verdict = SETTING_CONTEXT_INSIDE;
        } else if (!SettingContextRead(line, out->Lead, &tags)) {
            out->Verdict = SETTING_CONTEXT_BAD;
        } else {
            uint32_t next;
            out->Context        = tags;
            out->ContextLine    = out->Line;
            chosen->Context     = tags;
            chosen->ContextLine = out->Line;
            if (!SettingNextIndent(chosen->Bytes, chosen->Size, chosen->At, &next) || next <= out->Indent)
                out->Verdict = SETTING_CONTEXT_EMPTY;
        }
    } else if (parent && !parent->Held) {
        out->Verdict = SETTING_INSIDE_UNTAKEN;
    } else {
        SettingsChosenNames(chosen, parent ? parent->Node : 0, out);
        node = out->Named.Line;
    }

    SettingsChosenLevel *level = &chosen->Open[chosen->Opened++];
    level->Indent = out->Indent;
    level->Line   = out->Line;
    level->Node   = node;
    level->Held   = context ? out->Verdict == SETTINGS_OK || out->Verdict == SETTING_CONTEXT_EMPTY : node != 0;
    return true;
}

/* The last ENTRY line of the person's file that names the declared line
 * `node` in the block of the tags `context` (empty: everywhere) — with a
 * value or without, as the last one is what counts. With none, out->Line is
 * 0, which no line of a file is. */
static inline bool SettingsChosenFind(const uint8_t *declared, uint64_t declared_size, const uint8_t *bytes,
                                      uint64_t size, SettingSpan context, uint32_t node, SettingsLine *out)
{
    SettingsChosen chosen;
    SettingsLine   line;
    bool           found = false;
    out->Kind = SETTINGS_LINE_BLANK;
    out->Line = 0;
    if (node == 0) return false;
    SettingsChosenBegin(declared, declared_size, bytes, size, &chosen);
    while (SettingsChosenNext(&chosen, &line)) {
        if (line.Kind == SETTINGS_LINE_ENTRY && line.Named.Line == node && SettingTagsSame(line.Context, context)) {
            *out  = line;
            found = true;
        }
    }
    return found;
}

#endif /* SETTINGS_CHOSEN_H */
