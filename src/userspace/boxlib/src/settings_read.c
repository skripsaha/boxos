/*
 * settings_read.c — a program's settings put together from their bytes: what
 * its declaration says can be set, what the person's file chose — everywhere
 * and in each context's block — and in words every line of either that is
 * not taken (box/settings.h).
 *
 * Both texts are untrusted, read by the one reader (settings_reader.h,
 * settings_chosen.h); no
 * line of the person's file stops anything — the setting keeps its value and
 * the line is named. No volume here: the host stand builds this file.
 */

#include "box/core/settings_internal.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/string.h"
#include "settings_chosen.h"

static char *keep(SettingSpan span)
{
    char *copy = malloc((size_t)span.Length + 1);
    if (!copy) return NULL;
    memcpy(copy, span.At, (size_t)span.Length);
    copy[span.Length] = '\0';
    return copy;
}

static SettingSpan span_of(const char *text)
{
    return SettingSpanOf((const uint8_t *)text, text ? strlen(text) : 0);
}

int settings_say(BoxSettings *out, const char *text)
{
    char **grown = realloc(out->Problems, (out->ProblemCount + 1) * sizeof(char *));
    if (!grown) return -ERR_NO_MEMORY;
    out->Problems = grown;
    if (!(out->Problems[out->ProblemCount] = keep(span_of(text)))) return -ERR_NO_MEMORY;
    out->ProblemCount++;
    return 0;
}

int settings_refuse(BoxSettings *out, const char *why)
{
    free(out->Why);
    out->Why = keep(span_of(why));
    return out->Why ? -ERR_INVALID_ARGUMENT : -ERR_NO_MEMORY;
}

void settings_wants(const BoxSetting *setting, char *out, size_t capacity)
{
    switch (setting->Kind) {
    case SETTING_GROUP: snprintf(out, capacity, "no value of its own: it holds others"); return;
    case SETTING_ON_OR_OFF: snprintf(out, capacity, "on or off"); return;
    case SETTING_NUMBER:
        snprintf(out, capacity, "a whole number from %lld to %lld%s%s%s", (long long)setting->From,
                 (long long)setting->To, setting->Unit[0] ? " " : "", setting->Unit, setting->Off ? ", or off" : "");
        return;
    case SETTING_WORD: snprintf(out, capacity, "one word"); return;
    case SETTING_TEXT: snprintf(out, capacity, "text on one line"); return;
    case SETTING_ONE_OF: {
        SettingSpan     choices = span_of(setting->Choices), choice;
        SettingsVerdict ignored;
        uint64_t        at = 0;
        bool            quoted;
        size_t          used = (size_t)snprintf(out, capacity, "one of");
        const char     *between = " ";
        while (used < capacity && SettingWord(choices, &at, &choice, &quoted, &ignored)) {
            used += (size_t)snprintf(out + used, capacity - used, "%s%.*s", between, (int)choice.Length,
                                     (const char *)choice.At);
            between = ", ";
        }
        return;
    }
    case SETTING_COLOR: snprintf(out, capacity, "a color: a name such as amber, or #RRGGBB"); return;
    default: snprintf(out, capacity, "what this reader does not know"); return;
    }
}

/* A value as a sentence shows it: as it is, or "" for none. */
static const char *shown(const char *value)
{
    return value[0] ? value : "\"\"";
}

/* ---- the declaration ---------------------------------------------------- */

static void forget_value(BoxSettingValue *value)
{
    free((void *)value->Text);
    memset(value, 0, sizeof(*value));
}

/* `text` read for the declared setting into *value: 0, -ERR_INVALID_ARGUMENT
 * when it does not fit, or -ERR_NO_MEMORY. */
static int read_value(const Setting *declared, SettingSpan text, BoxSettingValue *value)
{
    SettingValue read;
    if (SettingValueRead(declared, text, &read) != SETTINGS_OK) return -ERR_INVALID_ARGUMENT;
    char *kept = keep(read.Text);
    if (!kept) return -ERR_NO_MEMORY;
    forget_value(value);
    value->Text   = kept;
    value->On     = read.On;
    value->Number = read.Number;
    value->Color  = read.Color;
    return 0;
}

/* The setting on declared line `line`: its index, or Count for none. */
static size_t index_of_line(const BoxSettings *settings, uint32_t line)
{
    for (size_t i = 0; i < settings->Count; i++)
        if (settings->Settings[i].Line == line) return i;
    return settings->Count;
}

static int keep_setting(BoxSettings *settings, BoxSetting *entry, const Setting *declared)
{
    memset(entry, 0, sizeof(*entry));
    entry->Kind      = declared->Kind;
    entry->Line      = declared->Line;
    entry->Depth     = declared->Depth;
    entry->From      = declared->From;
    entry->To        = declared->To;
    entry->Off       = declared->Off;
    entry->NextStart = declared->NextStart;
    entry->Name      = keep(declared->Name);
    entry->What      = keep(declared->What);
    entry->Default   = keep(declared->Default);
    entry->Unit      = keep(declared->Unit);
    entry->Choices   = keep(declared->Choices);
    if (!entry->Name || !entry->What || !entry->Default || !entry->Unit || !entry->Choices) return -ERR_NO_MEMORY;

    /* Its path: the path of what holds it — put together before it — and its name. */
    size_t      parent = index_of_line(settings, declared->Parent);
    const char *above  = declared->Parent && parent < settings->Count ? settings->Settings[parent].Path : "";
    size_t      length = strlen(above) + (above[0] ? 1 : 0) + strlen(entry->Name) + 1;
    char       *path   = malloc(length);
    if (!path) return -ERR_NO_MEMORY;
    snprintf(path, length, "%s%s%s", above, above[0] ? " " : "", entry->Name);
    entry->Path = path;
    if (declared->Kind == SETTING_GROUP) return (entry->Value.Text = keep(span_of(""))) ? 0 : -ERR_NO_MEMORY;
    return read_value(declared, declared->Default, &entry->Value);
}

/* The declaration: every setting it can say, and a sentence for each line it
 * cannot — the toolchain refuses such lines, so a program carries one only
 * when it was made elsewhere. */
static int assemble_declared(BoxSettings *out)
{
    SettingsDeclared declared;
    Setting          setting;
    SettingsVerdict  verdict;
    char             line[SETTINGS_SAYING_MAX];
    size_t           count = 0;
    int              rc;

    if (out->DeclaredSize == 0) return 0;
    if (SettingsDeclaredBegin((const uint8_t *)out->Declared, out->DeclaredSize, &declared) != SETTINGS_OK) {
        snprintf(line, sizeof(line), "%s's settings do not begin with the line \"settings %s\"; it has none",
                 out->Program, SETTINGS_VERSION_WORD);
        return settings_say(out, line);
    }
    while (SettingsDeclaredNext(&declared, &setting, &verdict)) count += verdict == SETTINGS_OK;
    if (count > 0 && !(out->Settings = calloc(count, sizeof(BoxSetting)))) return -ERR_NO_MEMORY;
    SettingsDeclaredBegin((const uint8_t *)out->Declared, out->DeclaredSize, &declared);
    while (SettingsDeclaredNext(&declared, &setting, &verdict)) {
        if (verdict == SETTINGS_OK) {
            BoxSetting *entry = &out->Settings[out->Count];
            rc                = keep_setting(out, entry, &setting);
            out->Count++;
            if (rc != 0) return rc;
            continue;
        }
        if (setting.Name.Length > 0)
            snprintf(line, sizeof(line), "%s declares a setting it cannot have, line %u: the setting %.*s %s; it "
                     "is passed over", out->Program, setting.Line, (int)setting.Name.Length,
                     (const char *)setting.Name.At, SettingsVerdictText(verdict));
        else
            snprintf(line, sizeof(line), "%s declares a setting it cannot have, line %u: the line %s; it is "
                     "passed over", out->Program, setting.Line, SettingsVerdictText(verdict));
        if ((rc = settings_say(out, line)) != 0) return rc;
    }
    return 0;
}

/* ---- paths -------------------------------------------------------------- */

/* Whether two paths name the same names, however many blanks stand apart. */
static bool path_same(SettingSpan a, SettingSpan b)
{
    uint64_t        at_a = 0, at_b = 0;
    SettingSpan     word_a, word_b;
    bool            quoted_a, quoted_b;
    SettingsVerdict ignored;
    for (;;) {
        bool more_a = SettingWord(a, &at_a, &word_a, &quoted_a, &ignored);
        bool more_b = SettingWord(b, &at_b, &word_b, &quoted_b, &ignored);
        if (!more_a || !more_b) return more_a == more_b;
        if (quoted_a || quoted_b || !SettingSpanSame(word_a, word_b)) return false;
    }
}

const BoxSetting *settings_find(const BoxSettings *settings, const char *path)
{
    if (!settings || !path) return NULL;
    for (size_t i = 0; i < settings->Count; i++)
        if (path_same(span_of(settings->Settings[i].Path), span_of(path))) return &settings->Settings[i];
    return NULL;
}

/* "clock has no setting digts — did you mean digits?"; "digits has no
 * setting colr inside it — did you mean digits color?"; "clock has no
 * setting color — did you mean digits color?" for a name that is only
 * inside something; "digits width holds nothing; color is not inside it".
 * The name was looked for inside the declared line `parent` (0: the top).
 * False when nothing near was found to say: the name may be another
 * version's. */
static bool say_not_there(const BoxSettings *settings, uint32_t parent, SettingSpan name, char *out,
                          size_t capacity)
{
    const uint8_t    *declared = (const uint8_t *)settings->Declared;
    uint64_t          size     = settings->DeclaredSize;
    size_t            index    = index_of_line(settings, parent);
    const BoxSetting *holder   = parent && index < settings->Count ? &settings->Settings[index] : NULL;
    Setting           near;
    if (holder && !SettingsDeclaredHolds(declared, size, parent)) {
        snprintf(out, capacity, "%s holds nothing; %.*s is not inside it", holder->Path, (int)name.Length,
                 (const char *)name.At);
        return true;
    }
    int written;
    if (holder) written = snprintf(out, capacity, "%s has no setting %.*s inside it", holder->Path, (int)name.Length,
                                   (const char *)name.At);
    else written = snprintf(out, capacity, "%s has no setting %.*s", settings->Program, (int)name.Length,
                            (const char *)name.At);
    if (written < 0 || (size_t)written >= capacity) return false;
    if (SettingsDeclaredNearest(declared, size, parent, name, &near) ||
        (SettingsDeclaredSomewhere(declared, size, name, &near) && near.Parent != parent)) {
        size_t meant = index_of_line(settings, near.Line);
        snprintf(out + written, capacity - (size_t)written, " — did you mean %s?",
                 meant < settings->Count ? settings->Settings[meant].Path : "");
        return true;
    }
    /* A value where a name was wanted: the one setting inside that takes it
     * is the one meant — "messages amber" is messages color amber. */
    const BoxSetting *takes = NULL;
    for (size_t i = 0; holder && i < settings->Count; i++) {
        Setting      inside;
        SettingValue value;
        if (settings->Settings[i].Depth != holder->Depth + 1 ||
            !SettingsDeclaredAt(declared, size, settings->Settings[i].Line, &inside) || inside.Parent != parent ||
            inside.Kind == SETTING_GROUP || SettingValueRead(&inside, name, &value) != SETTINGS_OK)
            continue;
        if (takes) return false;
        takes = &settings->Settings[i];
    }
    if (!takes) return false;
    snprintf(out + written, capacity - (size_t)written, " — did you mean %s %.*s?", takes->Path, (int)name.Length,
             (const char *)name.At);
    return true;
}

/* ---- the person's file -------------------------------------------------- */

/* "clock.settings, line 3: " — and "[work] " before a setting in a context. */
static void line_head(const BoxSettings *settings, const SettingsLine *line, char *out, size_t capacity)
{
    snprintf(out, capacity, "%s.settings, line %u: ", settings->Program, line->Line);
}

/* The setting a line names, as a sentence names it: its path, and the
 * context it is for. */
static void setting_named(const BoxSetting *entry, const SettingsLine *line, char *out, size_t capacity)
{
    if (line->ContextLine) snprintf(out, capacity, "[%.*s] %s", (int)line->Context.Length,
                                    (const char *)line->Context.At, entry->Path);
    else snprintf(out, capacity, "%s", entry->Path);
}

/* Where an earlier line of the file set this setting in this context: its
 * line, 0 for none. */
static uint32_t set_before(const BoxSettings *settings, const uint32_t *everywhere, size_t index,
                           const SettingsLine *line)
{
    if (!line->ContextLine) return everywhere[index];
    for (size_t i = settings->InContextCount; i-- > 0;) {
        const BoxSettingInContext *in = &settings->InContext[i];
        if (in->Setting == index && SettingTagsSame(span_of(in->Tags), line->Context)) return in->Line;
    }
    return 0;
}

/* A value for a context's block: kept, the earlier one for the same setting
 * there let go — the last line is what counts. */
static int keep_in_context(BoxSettings *out, size_t index, const SettingsLine *line, const Setting *declared)
{
    BoxSettingValue value;
    memset(&value, 0, sizeof(value));
    int rc = read_value(declared, line->Default ? declared->Default : line->Value, &value);
    if (rc != 0) return rc;
    for (size_t i = 0; i < out->InContextCount; i++) {
        BoxSettingInContext *in = &out->InContext[i];
        if (in->Setting != index || !SettingTagsSame(span_of(in->Tags), line->Context)) continue;
        free((void *)in->Tags);
        forget_value(&in->Value);
        memmove(in, in + 1, (out->InContextCount - i - 1) * sizeof(*in));
        out->InContextCount--;
        break;
    }
    BoxSettingInContext *grown = realloc(out->InContext, (out->InContextCount + 1) * sizeof(*grown));
    if (!grown) {
        forget_value(&value);
        return -ERR_NO_MEMORY;
    }
    out->InContext       = grown;
    BoxSettingInContext *in = &out->InContext[out->InContextCount];
    in->Tags             = keep(line->Context);
    in->Setting          = index;
    in->Value            = value;
    in->Line             = line->Line;
    if (!in->Tags) {
        forget_value(&in->Value);
        return -ERR_NO_MEMORY;
    }
    out->InContextCount++;
    return 0;
}

/* An ENTRY line whose names lead to a declared setting. *everywhere is the
 * line each setting was last set on everywhere, so a second line for it is
 * named — as one in a context's block is, by the values kept there. */
static int assemble_named(BoxSettings *out, const SettingsLine *line, uint32_t *everywhere)
{
    char        head[64], named[SETTINGS_SAYING_MAX / 2], saying[SETTINGS_SAYING_MAX];
    char        wants[SETTINGS_SAYING_MAX / 2];
    size_t      index = index_of_line(out, line->Named.Line);
    int         rc;
    if (index == out->Count) return 0;
    BoxSetting *entry = &out->Settings[index];
    if (!line->HasValue && line->Verdict == SETTINGS_OK) return 0;   /* it only opens what is under it */

    line_head(out, line, head, sizeof(head));
    setting_named(entry, line, named, sizeof(named));
    uint32_t before = set_before(out, everywhere, index, line);
    if (before != 0) {
        snprintf(saying, sizeof(saying), "%s%s is set before, on line %u; this line is taken", head, named, before);
        if ((rc = settings_say(out, saying)) != 0) return rc;
    }
    if (line->Verdict == SETTINGS_OK) {
        if (line->ContextLine) return keep_in_context(out, index, line, &line->Named);
        everywhere[index] = line->Line;
        rc = read_value(&line->Named, line->Default ? line->Named.Default : line->Value, &entry->Value);
        if (rc == 0) entry->Chosen = !line->Default;
        return rc;
    }
    /* A value that does not fit: everywhere the setting falls back to its
     * default, as the line asked it to be something else; in a context's
     * block the line is passed over, and what is set everywhere holds. */
    settings_wants(entry, wants, sizeof(wants));
    if (line->ContextLine) {
        snprintf(saying, sizeof(saying), "%s%s wants %s; the line is passed over", head, named, wants);
        return settings_say(out, saying);
    }
    everywhere[index] = line->Line;
    if ((rc = read_value(&line->Named, line->Named.Default, &entry->Value)) != 0) return rc;
    entry->Chosen = false;
    snprintf(saying, sizeof(saying), "%s%s wants %s; %s is kept", head, named, wants, shown(entry->Value.Text));
    return settings_say(out, saying);
}

/* A line of the person's file that is not taken, said: what is wrong with
 * where it stands, or with its names. */
static int assemble_untaken(BoxSettings *out, const SettingsLine *line)
{
    char head[64], what[SETTINGS_SAYING_MAX - 64], saying[SETTINGS_SAYING_MAX];
    line_head(out, line, head, sizeof(head));
    switch (line->Verdict) {
    case SETTING_NOT_THERE:
        if (!say_not_there(out, line->UnknownIn, line->Unknown, what, sizeof(what))) {
            size_t used = strlen(what);
            snprintf(what + used, sizeof(what) - used, "; the line is left as it is");
        }
        break;
    case SETTING_INSIDE_UNTAKEN:
        snprintf(what, sizeof(what), "it stands inside line %u, which is not taken; it is passed over",
                 line->Parent);
        break;
    default:
        snprintf(what, sizeof(what), "the line %s; it is passed over", SettingsVerdictText(line->Verdict));
        break;
    }
    snprintf(saying, sizeof(saying), "%s%s", head, what);
    return settings_say(out, saying);
}

/* The person's file: each line that sets a declared setting taken when its
 * value fits, every other line named — but comments, blank lines and lines
 * that only open what is under them. */
static int assemble_chosen(BoxSettings *out)
{
    SettingsChosen chosen;
    SettingsLine   line;
    char           saying[SETTINGS_SAYING_MAX];
    uint32_t      *everywhere = calloc(out->Count ? out->Count : 1, sizeof(uint32_t));
    int            rc         = 0;
    if (!everywhere) return -ERR_NO_MEMORY;
    SettingsChosenBegin((const uint8_t *)out->Declared, out->DeclaredSize, (const uint8_t *)out->Chosen,
                        out->ChosenSize, &chosen);
    while (rc == 0 && SettingsChosenNext(&chosen, &line)) {
        if (line.Kind == SETTINGS_LINE_NOT_TEXT) {
            snprintf(saying, sizeof(saying), "%s.settings, line %u is not text; it is passed over", out->Program,
                     line.Line);
            rc = settings_say(out, saying);
        } else if (line.Kind == SETTINGS_LINE_CONTEXT) {
            if (line.Verdict != SETTINGS_OK) rc = assemble_untaken(out, &line);
        } else if (line.Kind == SETTINGS_LINE_ENTRY) {
            rc = line.Named.Line != 0 ? assemble_named(out, &line, everywhere) : assemble_untaken(out, &line);
        }
    }
    free(everywhere);
    return rc;
}

int settings_assemble(BoxSettings *out)
{
    int rc = assemble_declared(out);
    return rc != 0 ? rc : assemble_chosen(out);
}

static void release_assembled(BoxSettings *settings)
{
    for (size_t i = 0; i < settings->Count; i++) {
        BoxSetting *entry = &settings->Settings[i];
        free((void *)entry->Path);
        free((void *)entry->Name);
        free((void *)entry->What);
        free((void *)entry->Default);
        free((void *)entry->Unit);
        free((void *)entry->Choices);
        forget_value(&entry->Value);
    }
    free(settings->Settings);
    for (size_t i = 0; i < settings->InContextCount; i++) {
        free((void *)settings->InContext[i].Tags);
        forget_value(&settings->InContext[i].Value);
    }
    free(settings->InContext);
    for (size_t i = 0; i < settings->ProblemCount; i++) free(settings->Problems[i]);
    free(settings->Problems);
    settings->Settings       = NULL;
    settings->Count          = 0;
    settings->InContext      = NULL;
    settings->InContextCount = 0;
    settings->Problems       = NULL;
    settings->ProblemCount   = 0;
}

int settings_reassemble(BoxSettings *out)
{
    release_assembled(out);
    return settings_assemble(out);
}

void settings_release(BoxSettings *settings)
{
    if (!settings) return;
    release_assembled(settings);
    free(settings->Declared);
    free(settings->Chosen);
    free(settings->Why);
    memset(settings, 0, sizeof(*settings));
}

/* ---- a value held to a setting ------------------------------------------ */

/* The declared setting a path names, or the sentence on why none is. */
static const BoxSetting *setting_meant(BoxSettings *settings, const char *path, Setting *declared, int *rc)
{
    char            saying[SETTINGS_SAYING_MAX];
    SettingSpan     typed = span_of(path), name;
    uint64_t        at = 0;
    uint32_t        parent = 0;
    bool            quoted;
    SettingsVerdict ignored = SETTINGS_OK;
    *rc = 0;
    while (SettingWord(typed, &at, &name, &quoted, &ignored)) {
        if (quoted || !SettingsDeclaredChild((const uint8_t *)settings->Declared, settings->DeclaredSize, parent,
                                             name, declared)) {
            say_not_there(settings, parent, name, saying, sizeof(saying));
            *rc = settings_refuse(settings, saying);
            return NULL;
        }
        parent = declared->Line;
    }
    size_t index = index_of_line(settings, parent);
    if (parent == 0 || index == settings->Count) {
        snprintf(saying, sizeof(saying), "which setting of %s? Say its name, or its whole path", settings->Program);
        *rc = settings_refuse(settings, saying);
        return NULL;
    }
    return &settings->Settings[index];
}

int settings_check(BoxSettings *settings, const char *path, const char *value)
{
    char         saying[SETTINGS_SAYING_MAX], wants[SETTINGS_SAYING_MAX / 2];
    Setting      declared;
    SettingValue read;
    int          rc;
    const BoxSetting *entry = setting_meant(settings, path, &declared, &rc);
    if (!entry) return rc;
    if (entry->Kind != SETTING_GROUP && SettingValueRead(&declared, span_of(value), &read) == SETTINGS_OK) return 0;
    /* A value that does not fit, beginning with a word near the name of what
     * the setting holds: "sign colr red" meant sign color. */
    SettingSpan     typed = span_of(value), first, second;
    uint64_t        at    = 0;
    bool            quoted;
    SettingsVerdict ignored = SETTINGS_OK;
    Setting         near;
    if (entry->Kind != SETTING_GROUP && SettingWord(typed, &at, &first, &quoted, &ignored) && !quoted &&
        SettingWord(typed, &at, &second, &quoted, &ignored) &&
        SettingsDeclaredNearest((const uint8_t *)settings->Declared, settings->DeclaredSize, entry->Line, first,
                                &near)) {
        size_t meant = index_of_line(settings, near.Line);
        snprintf(saying, sizeof(saying), "%s has no setting %.*s inside it — did you mean %s?", entry->Path,
                 (int)first.Length, (const char *)first.At,
                 meant < settings->Count ? settings->Settings[meant].Path : "");
        return settings_refuse(settings, saying);
    }
    settings_wants(entry, wants, sizeof(wants));
    snprintf(saying, sizeof(saying), "%s wants %s", entry->Path, wants);
    return settings_refuse(settings, saying);
}
