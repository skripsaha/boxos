#ifndef SETTINGS_PEN_H
#define SETTINGS_PEN_H

#include "settings_chosen.h"

/*
 * The pen the person's settings file (settings_chosen.h) is changed with —
 * by the utility settings and by a program changing its own settings at the
 * person's word. A change touches the lines of the one setting it is about,
 * and the lines that held it when nothing is left in them, and no other
 * byte: comments, the order of the lines, the way the person wrote them —
 * blocks or whole paths, spaces or tabs — and every line that names nothing
 * this program declares (another version's) stay as they were.
 *
 * A setting is the declared line its names lead to (Named.Line), in the
 * block of a context — its tags, apart by blanks or commas; empty for
 * everywhere.
 *
 *   set     the last line for it gets the value where it stands, and every
 *           other line for it goes — or, holding lines inside it, keeps only
 *           its names. With none, a line is made inside the deepest block of
 *           what holds it in that context, indented as the lines already
 *           inside it, or by four spaces more than the block; with no such
 *           block, the block is made — at the end of what is set everywhere,
 *           or at the end of the context's block; with no block for the
 *           context, one is made at the end of the file.
 *   erase   every line for it goes, or keeps only its names while it holds
 *           lines inside it; then every line that held it and now holds
 *           nothing — no value of its own, nothing under it that stays —
 *           goes too, a context's line with them, and the blank lines right
 *           before that; a file left with nothing but blank lines is left
 *           empty.
 *   clear   every line that names a setting the program declares goes, as
 *           erase says it, in every context: everything as the program came.
 *
 * Each change is one function called twice: with no `out` it measures the
 * file it makes into *bytes, with `out` (that much room) it writes it. The
 * value is the caller's to have read with SettingValueRead first: the pen
 * writes what it is given. A line the pen writes ends in LF.
 *
 * No allocation and no library. Integer types come from the includer.
 */

static inline void SettingsPenPut(uint8_t *out, uint64_t *at, const uint8_t *bytes, uint64_t count)
{
    if (out)
        for (uint64_t i = 0; i < count; i++) out[*at + i] = bytes[i];
    *at += count;
}

static inline void SettingsPenText(uint8_t *out, uint64_t *at, const char *text)
{
    uint64_t length = 0;
    while (text[length] != '\0') length++;
    SettingsPenPut(out, at, (const uint8_t *)text, length);
}

/* Whether a value must go between double quotes to be read back as it is:
 * empty, a blank at an end, the word default, or quotes at both ends. */
static inline bool SettingsPenQuotes(SettingSpan value)
{
    return value.Length == 0 || SettingBlank(value.At[0]) || SettingBlank(value.At[value.Length - 1]) ||
           SettingSpanIs(value, "default") ||
           (value.Length >= 2 && value.At[0] == '"' && value.At[value.Length - 1] == '"');
}

/* " VALUE" after a line's names: quoted as SettingsPenQuotes says, and also
 * when its first word names what the setting holds and another word follows
 * — read bare, those words would lead down the tree. */
static inline void SettingsPenValue(uint8_t *out, uint64_t *at, const uint8_t *declared, uint64_t declared_size,
                                    uint32_t node, SettingSpan value)
{
    uint64_t        look = 0;
    SettingSpan     first, second;
    bool            quoted;
    SettingsVerdict ignored = SETTINGS_OK;
    Setting         inside;
    bool quotes = SettingsPenQuotes(value) ||
                  (SettingWord(value, &look, &first, &quoted, &ignored) && !quoted &&
                   SettingWord(value, &look, &second, &quoted, &ignored) &&
                   SettingsDeclaredChild(declared, declared_size, node, first, &inside));
    SettingsPenText(out, at, quotes ? " \"" : " ");
    SettingsPenPut(out, at, value.At, value.Length);
    SettingsPenText(out, at, quotes ? "\"\n" : "\n");
}

/* A line's names as it wrote them — its indent with them — and LF. */
static inline void SettingsPenNames(uint8_t *out, uint64_t *at, const SettingsLine *line)
{
    SettingsPenPut(out, at, line->Whole.At, line->NamesEnd);
    SettingsPenText(out, at, "\n");
}

/* What is under a line of the file: every line after it until one that says
 * something stands no deeper. */
typedef struct {
    bool        Inside;         /* a line that says something stands under it */
    uint64_t    End;            /* past the last such line; the line's own end when none */
    SettingSpan FirstLead;      /* the indent of the first */
    bool        Matched;        /* a line the change is about stands under it */
    bool        Stays;          /* a line that stays after the change stands under it */
} SettingsPenUnder;

/* Whether a change of `all` settings, or of `node` in the block of
 * `context`, is about this line. */
static inline bool SettingsPenAbout(const SettingsLine *line, bool all, SettingSpan context, uint32_t node)
{
    if (line->Kind != SETTINGS_LINE_ENTRY || line->Named.Line == 0) return false;
    return all || (line->Named.Line == node && SettingTagsSame(line->Context, context));
}

/* Whether the pen may take the line out: one that names a declared setting,
 * or a context's line that reads. Any other line it never touches. */
static inline bool SettingsPenOwns(const SettingsLine *line)
{
    if (line->Kind == SETTINGS_LINE_ENTRY) return line->Named.Line != 0;
    return line->Kind == SETTINGS_LINE_CONTEXT &&
           (line->Verdict == SETTINGS_OK || line->Verdict == SETTING_CONTEXT_EMPTY);
}

/* A line under the one looked at, open while the lines under it are read. */
typedef struct {
    uint32_t Indent;
    bool     Owned, About, HasValue;
    bool     Matched;           /* a line the change is about stands under it */
    bool     Stays;             /* a line that stays stands under it */
} SettingsPenOpen;

/* Whether a line stays after the change: one the pen does not own always;
 * one the change is about while something under it stays (keeping its
 * names); one that holds a line the change is about while it has a value of
 * its own or something under it stays; any other untouched. */
static inline bool SettingsPenStays(const SettingsPenOpen *open)
{
    if (!open->Owned) return true;
    if (open->About) return open->Stays;
    return !open->Matched || open->HasValue || open->Stays;
}

/* `open` closed into what holds it. */
static inline void SettingsPenClose(const SettingsPenOpen *open, SettingsPenOpen *into)
{
    into->Stays   = into->Stays || SettingsPenStays(open);
    into->Matched = into->Matched || open->About || open->Matched;
}

/* `chosen` is the reader just past `line`. Each line under it is weighed
 * from the deepest up, a stack of the lines open between — the tree is no
 * deeper than the reader's own, and a line past that is one the pen does not
 * own, which stays. */
static inline void SettingsPenLook(const SettingsChosen *chosen, const SettingsLine *line, bool all,
                                   SettingSpan context, uint32_t node, SettingsPenUnder *under)
{
    SettingsChosen  scan = *chosen;
    SettingsLine    next;
    SettingsPenOpen open[SETTINGS_DEPTH_MAX + 2], top = { line->Indent, true, false, false, false, false };
    uint32_t        opened = 0;
    under->Inside    = false;
    under->End       = line->End;
    under->FirstLead = SettingSpanOf(line->Whole.At, 0);
    while (SettingsChosenNext(&scan, &next)) {
        if (next.Kind != SETTINGS_LINE_ENTRY && next.Kind != SETTINGS_LINE_CONTEXT) continue;
        if (next.Indent <= line->Indent) break;
        if (!under->Inside) under->FirstLead = SettingSpanOf(next.Whole.At, next.Lead);
        under->Inside = true;
        under->End    = next.End;
        while (opened > 0 && open[opened - 1].Indent >= next.Indent) {
            opened--;
            SettingsPenClose(&open[opened], opened > 0 ? &open[opened - 1] : &top);
        }
        SettingsPenOpen one = { next.Indent, SettingsPenOwns(&next), SettingsPenAbout(&next, all, context, node),
                                next.HasValue, false, false };
        if (opened < SETTINGS_DEPTH_MAX + 2u) open[opened++] = one;
        else SettingsPenClose(&one, &open[opened - 1]);
    }
    while (opened > 0) {
        opened--;
        SettingsPenClose(&open[opened], opened > 0 ? &open[opened - 1] : &top);
    }
    under->Matched = top.Matched;
    under->Stays   = top.Stays;
}

/* ── set ─────────────────────────────────────────────────────────────────── */

/* The lines from the declared line chain[from] down to chain[to], the last
 * with the value, each four spaces deeper than the one before, the first at
 * `lead` and four spaces more for each `deeper`. */
static inline void SettingsPenChain(uint8_t *out, uint64_t *at, const uint8_t *declared, uint64_t declared_size,
                                    const uint32_t *chain, uint32_t from, uint32_t to, SettingSpan lead,
                                    uint32_t deeper, SettingSpan value)
{
    for (uint32_t i = from; i <= to; i++) {
        Setting setting;
        SettingsDeclaredAt(declared, declared_size, chain[i], &setting);
        SettingsPenPut(out, at, lead.At, lead.Length);
        for (uint32_t d = 0; d < deeper + (i - from); d++) SettingsPenText(out, at, "    ");
        SettingsPenPut(out, at, setting.Name.At, setting.Name.Length);
        if (i == to) SettingsPenValue(out, at, declared, declared_size, chain[i], value);
        else SettingsPenText(out, at, "\n");
    }
}

/* The setting set where no line names it: where the new lines go, and the
 * lines. */
static inline void SettingsPenAdd(const uint8_t *declared, uint64_t declared_size, const uint8_t *file,
                                  uint64_t size, SettingSpan context, uint32_t node, SettingSpan value,
                                  uint8_t *out, uint64_t *bytes)
{
    /* What holds it, from the top down: chain[0] at the top, chain[depth] itself. */
    uint32_t chain[SETTINGS_DEPTH_MAX];
    Setting  setting;
    uint32_t depth = 0;
    if (!SettingsDeclaredAt(declared, declared_size, node, &setting) || setting.Depth >= SETTINGS_DEPTH_MAX) {
        *bytes = 0;
        SettingsPenPut(out, bytes, file, size);
        return;
    }
    depth = setting.Depth;
    for (uint32_t i = depth + 1; i-- > 0;) {
        chain[i] = setting.Line;
        if (i > 0) SettingsDeclaredAt(declared, declared_size, setting.Parent, &setting);
    }

    /* The block the lines go into: the deepest of what holds it with a line
     * in the context's block, the last such line; else the context's block,
     * the last one for those tags; else what is set everywhere. */
    SettingsChosen   chosen;
    SettingsLine     line;
    SettingsPenUnder under, block_under, top_under;
    uint32_t         held_at = 0, block = 0, last_top = 0;
    bool             held = false, first_context = false;
    uint64_t         insert = size, held_end = 0, block_end = 0, top_end = 0, context_start = size;
    SettingSpan      held_lead = SettingSpanOf(file, 0), block_lead = SettingSpanOf(file, 0);
    SettingSpan      top_lead = SettingSpanOf(file, 0), context_lead = SettingSpanOf(file, 0);
    SettingSpan      held_own_lead = SettingSpanOf(file, 0);
    bool             held_inside = false, block_inside = false;
    SettingSpan      block_own_lead = SettingSpanOf(file, 0);

    SettingsChosenBegin(declared, declared_size, file, size, &chosen);
    while (SettingsChosenNext(&chosen, &line)) {
        if (line.Kind == SETTINGS_LINE_CONTEXT && line.ContextLine != 0 && !first_context) {
            first_context = true;
            context_start = line.Start;
            context_lead  = SettingSpanOf(line.Whole.At, line.Lead);
        }
        if (line.Kind == SETTINGS_LINE_CONTEXT && context.Length > 0 &&
            (line.Verdict == SETTINGS_OK || line.Verdict == SETTING_CONTEXT_EMPTY) &&
            SettingTagsSame(line.Context, context)) {
            SettingsPenLook(&chosen, &line, false, context, 0, &block_under);
            block          = line.Line;
            block_end      = block_under.End;
            block_inside   = block_under.Inside;
            block_lead     = block_under.FirstLead;
            block_own_lead = SettingSpanOf(line.Whole.At, line.Lead);
        }
        if (line.Kind != SETTINGS_LINE_ENTRY || line.Named.Line == 0) continue;
        if (context.Length == 0 && line.ContextLine == 0 && line.Parent == 0) {
            SettingsPenLook(&chosen, &line, false, context, 0, &top_under);
            last_top = line.Line;
            top_end  = top_under.End;
            top_lead = SettingSpanOf(line.Whole.At, line.Lead);
        }
        if (!SettingTagsSame(line.Context, context)) continue;
        for (uint32_t i = 0; i < depth; i++) {
            if (line.Named.Line != chain[i] || (held && i < held_at)) continue;
            SettingsPenLook(&chosen, &line, false, context, 0, &under);
            held          = true;
            held_at       = i;
            held_end      = under.End;
            held_inside   = under.Inside;
            held_lead     = under.FirstLead;
            held_own_lead = SettingSpanOf(line.Whole.At, line.Lead);
        }
    }

    uint64_t at = 0;
    bool     make_context = false;
    if (held) {
        insert = held_end;
    } else if (context.Length > 0) {
        if (block) insert = block_end;
        else make_context = true;
    } else {
        insert = last_top ? top_end : context_start;
    }
    if (make_context) insert = size;

    SettingsPenPut(out, &at, file, insert);
    if (insert == size && size > 0 && file[size - 1] != '\n') SettingsPenText(out, &at, "\n");
    if (held) {
        SettingSpan lead = held_inside ? held_lead : held_own_lead;
        SettingsPenChain(out, &at, declared, declared_size, chain, held_at + 1, depth, lead, held_inside ? 0 : 1,
                         value);
    } else if (block) {
        SettingSpan lead = block_inside ? block_lead : block_own_lead;
        SettingsPenChain(out, &at, declared, declared_size, chain, 0, depth, lead, block_inside ? 0 : 1, value);
    } else if (make_context) {
        /* A blank line before the new block, unless the file ends in one. */
        if (size > 0 && !(size >= 2 && file[size - 1] == '\n' && file[size - 2] == '\n'))
            SettingsPenText(out, &at, "\n");
        SettingsPenText(out, &at, "[");
        SettingsPenPut(out, &at, context.At, context.Length);
        SettingsPenText(out, &at, "]\n");
        SettingsPenChain(out, &at, declared, declared_size, chain, 0, depth, SettingSpanOf(file, 0), 1, value);
    } else {
        /* At the top, indented as the line it follows or the one it goes before — so neither stands inside it. */
        SettingsPenChain(out, &at, declared, declared_size, chain, 0, depth, last_top ? top_lead : context_lead, 0,
                         value);
    }
    SettingsPenPut(out, &at, file + insert, size - insert);
    *bytes = at;
}

/* The file with the setting on declared line `node`, in the block of
 * `context`, set to `value`. */
static inline void SettingsPenSet(const uint8_t *declared, uint64_t declared_size, const uint8_t *file,
                                  uint64_t size, SettingSpan context, uint32_t node, SettingSpan value,
                                  uint8_t *out, uint64_t *bytes)
{
    SettingsLine last;
    if (!SettingsChosenFind(declared, declared_size, file, size, context, node, &last)) {
        SettingsPenAdd(declared, declared_size, file, size, context, node, value, out, bytes);
        return;
    }
    SettingsChosen   chosen;
    SettingsLine     line;
    SettingsPenUnder under;
    uint64_t         at = 0;
    SettingsChosenBegin(declared, declared_size, file, size, &chosen);
    while (SettingsChosenNext(&chosen, &line)) {
        if (line.Line == last.Line) {
            SettingsPenPut(out, &at, line.Whole.At, line.NamesEnd);
            SettingsPenValue(out, &at, declared, declared_size, node, value);
        } else if (SettingsPenAbout(&line, false, context, node)) {
            SettingsPenLook(&chosen, &line, false, context, node, &under);
            if (under.Inside) SettingsPenNames(out, &at, &line);
        } else {
            SettingsPenPut(out, &at, file + line.Start, line.End - line.Start);
        }
    }
    *bytes = at;
}

/* ── erase and clear ─────────────────────────────────────────────────────── */

/* Whether a line stays after the change, and whether only its names do. */
static inline bool SettingsPenKeeps(const SettingsChosen *chosen, const SettingsLine *line, bool all,
                                    SettingSpan context, uint32_t node, bool *names)
{
    SettingsPenUnder under;
    SettingsPenOpen  self = { line->Indent, SettingsPenOwns(line), SettingsPenAbout(line, all, context, node),
                              line->HasValue, false, false };
    if (self.Owned) {
        SettingsPenLook(chosen, line, all, context, node, &under);
        self.Matched = under.Matched;
        self.Stays   = under.Stays;
    }
    *names = self.About && self.HasValue;
    return SettingsPenStays(&self);
}

static inline void SettingsPenRemove(const uint8_t *declared, uint64_t declared_size, const uint8_t *file,
                                     uint64_t size, bool all, SettingSpan context, uint32_t node, uint8_t *out,
                                     uint64_t *bytes)
{
    SettingsChosen chosen;
    SettingsLine   line;
    uint64_t       at = 0, blank_start = 0, blank_end = 0;
    bool           names, said = false;

    /* Whether anything but blank lines stays: a file left with nothing else
     * is left empty. */
    SettingsChosenBegin(declared, declared_size, file, size, &chosen);
    while (!said && SettingsChosenNext(&chosen, &line))
        said = line.Kind != SETTINGS_LINE_BLANK && SettingsPenKeeps(&chosen, &line, all, context, node, &names);

    /* A run of blank lines waits for the line after it: it goes with a
     * context's line that goes — the pen put one before each block it made —
     * and stays before any other. */
    SettingsChosenBegin(declared, declared_size, file, size, &chosen);
    while (SettingsChosenNext(&chosen, &line)) {
        if (line.Kind == SETTINGS_LINE_BLANK) {
            if (blank_end == blank_start) blank_start = line.Start;
            blank_end = line.End;
            continue;
        }
        bool keeps = SettingsPenKeeps(&chosen, &line, all, context, node, &names);
        if (said && (keeps || line.Kind != SETTINGS_LINE_CONTEXT))
            SettingsPenPut(out, &at, file + blank_start, blank_end - blank_start);
        blank_start = blank_end = 0;
        if (!keeps) continue;
        if (names) SettingsPenNames(out, &at, &line);
        else SettingsPenPut(out, &at, file + line.Start, line.End - line.Start);
    }
    if (said) SettingsPenPut(out, &at, file + blank_start, blank_end - blank_start);
    *bytes = at;
}

/* The file with every line for the setting on declared line `node`, in the
 * block of `context`, taken out: the program's own value there. */
static inline void SettingsPenErase(const uint8_t *declared, uint64_t declared_size, const uint8_t *file,
                                    uint64_t size, SettingSpan context, uint32_t node, uint8_t *out,
                                    uint64_t *bytes)
{
    SettingsPenRemove(declared, declared_size, file, size, false, context, node, out, bytes);
}

/* The file with every line for a setting the declaration declares taken out,
 * in every context: everything as the program came. Comments and names it
 * does not declare stay. */
static inline void SettingsPenClear(const uint8_t *declared, uint64_t declared_size, const uint8_t *file,
                                    uint64_t size, uint8_t *out, uint64_t *bytes)
{
    SettingsPenRemove(declared, declared_size, file, size, true, SettingSpanOf(file, 0), 0, out, bytes);
}

#endif /* SETTINGS_PEN_H */
