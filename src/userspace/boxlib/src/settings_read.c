/*
 * settings_read.c — a program's settings put together from their bytes: what
 * its declaration says can be set, what the person's file chose, and in
 * words every line of either that is not taken (box/settings.h).
 *
 * Both texts are untrusted, read by the one reader (settings_reader.h); no
 * line of the person's file stops anything — the setting keeps its default
 * and the line is named. No volume here: the host stand builds this file.
 */

#include "box/core/settings_internal.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/string.h"

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
    return (SettingSpan){ (const uint8_t *)text, text ? strlen(text) : 0 };
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

void settings_wants(const BoxSetting *setting, char *out, size_t capacity)
{
    switch (setting->Kind) {
    case SETTING_YES_OR_NO: snprintf(out, capacity, "yes or no"); return;
    case SETTING_NUMBER:
        snprintf(out, capacity, "a whole number from %lld to %lld%s%s", (long long)setting->From,
                 (long long)setting->To, setting->Unit[0] ? " " : "", setting->Unit);
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
    case SETTING_COLOR: snprintf(out, capacity, "a colour written #RRGGBB"); return;
    default: snprintf(out, capacity, "what this reader does not know"); return;
    }
}

int settings_refuse(BoxSettings *out, const char *why)
{
    free(out->Why);
    out->Why = keep(span_of(why));
    return out->Why ? -ERR_INVALID_ARGUMENT : -ERR_NO_MEMORY;
}

/* A value as a sentence shows it: as it is, or "" for none. */
static const char *shown(const char *value)
{
    return value[0] ? value : "\"\"";
}

/* The entry's value from text read for it — the default or the person's. */
static int take_value(BoxSetting *entry, const Setting *declared, SettingSpan value, bool chosen)
{
    SettingValue read;
    if (SettingValueRead(declared, value, &read) != SETTINGS_OK) return -ERR_INVALID_ARGUMENT;
    char *text = keep(read.Text);
    if (!text) return -ERR_NO_MEMORY;
    free((void *)entry->Value);
    entry->Value  = text;
    entry->Yes    = read.Yes;
    entry->Number = read.Number;
    entry->Color  = read.Color;
    entry->Chosen = chosen;
    return 0;
}

static int keep_setting(BoxSetting *entry, const Setting *declared)
{
    memset(entry, 0, sizeof(*entry));
    entry->Kind      = declared->Kind;
    entry->From      = declared->From;
    entry->To        = declared->To;
    entry->NextStart = declared->NextStart;
    entry->Name      = keep(declared->Name);
    entry->What      = keep(declared->What);
    entry->Default   = keep(declared->Default);
    entry->Unit      = keep(declared->Unit);
    entry->Choices   = keep(declared->Choices);
    if (!entry->Name || !entry->What || !entry->Default || !entry->Unit || !entry->Choices) return -ERR_NO_MEMORY;
    return take_value(entry, declared, declared->Default, false);
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
        snprintf(line, sizeof(line), "%s's settings do not begin with the line \"settings 1\"; it has none",
                 out->Program);
        return settings_say(out, line);
    }
    while (SettingsDeclaredNext(&declared, &setting, &verdict)) count += verdict == SETTINGS_OK;
    if (count > 0 && !(out->Settings = calloc(count, sizeof(BoxSetting)))) return -ERR_NO_MEMORY;

    SettingsDeclaredBegin((const uint8_t *)out->Declared, out->DeclaredSize, &declared);
    while (SettingsDeclaredNext(&declared, &setting, &verdict)) {
        if (verdict == SETTINGS_OK) {
            if ((rc = keep_setting(&out->Settings[out->Count++], &setting)) != 0) return rc;
            continue;
        }
        snprintf(line, sizeof(line), "%s declares a setting it cannot have, line %u: the setting %.*s %s; it is "
                 "passed over", out->Program, setting.Line, (int)setting.Name.Length,
                 (const char *)setting.Name.At, SettingsVerdictText(verdict));
        if ((rc = settings_say(out, line)) != 0) return rc;
    }
    return 0;
}

static BoxSetting *entry_named(BoxSettings *settings, SettingSpan name, size_t *index)
{
    for (size_t i = 0; i < settings->Count; i++) {
        if (SettingSpanIs(name, settings->Settings[i].Name)) {
            *index = i;
            return &settings->Settings[i];
        }
    }
    return NULL;
}

/* "clock has no setting hourz — did you mean hours?"; true when a declared
 * name is near enough to be the one meant. */
static bool say_unknown(const BoxSettings *settings, SettingSpan name, char *out, size_t capacity)
{
    Setting near;
    if (SettingsDeclaredNearest((const uint8_t *)settings->Declared, settings->DeclaredSize, name, &near)) {
        snprintf(out, capacity, "%s has no setting %.*s — did you mean %.*s?", settings->Program,
                 (int)name.Length, (const char *)name.At, (int)near.Name.Length, (const char *)near.Name.At);
        return true;
    }
    snprintf(out, capacity, "%s has no setting %.*s", settings->Program, (int)name.Length, (const char *)name.At);
    return false;
}

/* One line of the person's file that sets a name. *set_on is the line each
 * setting was last set on, so a second line for it is named. */
static int assemble_set_line(BoxSettings *out, const SettingsLine *line, uint32_t *set_on)
{
    char        saying[SETTINGS_SAYING_MAX], wants[SETTINGS_SAYING_MAX / 2];
    size_t      index;
    BoxSetting *entry = entry_named(out, line->Name, &index);
    Setting     declared;
    int         rc;

    if (!entry) {
        /* A typo is said with the name meant; a name nothing is near may be
         * another version's, and its line stays. */
        char unknown[SETTINGS_SAYING_MAX];
        bool near = say_unknown(out, line->Name, unknown, sizeof(unknown));
        snprintf(saying, sizeof(saying), "%s.settings, line %u: %s%s", out->Program, line->Line, unknown,
                 near ? "" : "; the line is left as it is");
        return settings_say(out, saying);
    }
    if (set_on[index] != 0) {
        snprintf(saying, sizeof(saying), "%s.settings, line %u: %s is set before, on line %u; this line is taken",
                 out->Program, line->Line, entry->Name, set_on[index]);
        if ((rc = settings_say(out, saying)) != 0) return rc;
    }
    set_on[index] = line->Line;

    SettingsDeclaredFind((const uint8_t *)out->Declared, out->DeclaredSize, line->Name, &declared);
    SettingSpan value = line->Value;
    if (line->Default || (value.Length == 0 && !line->Quoted)) {
        if ((rc = take_value(entry, &declared, declared.Default, false)) != 0) return rc;
        if (line->Default) return 0;
    } else if ((rc = take_value(entry, &declared, value, true)) != -ERR_INVALID_ARGUMENT) {
        return rc;
    } else if ((rc = take_value(entry, &declared, declared.Default, false)) != 0) {
        return rc;
    }
    settings_wants(entry, wants, sizeof(wants));
    snprintf(saying, sizeof(saying), "%s.settings, line %u: %s wants %s; %s is kept", out->Program, line->Line,
             entry->Name, wants, shown(entry->Value));
    return settings_say(out, saying);
}

/* The person's file: each line that sets a declared setting taken when its
 * value fits, every other line named — but comments, blank lines and `when`
 * lines, which say nothing to this version. */
static int assemble_chosen(BoxSettings *out)
{
    SettingsChosen chosen;
    SettingsLine   line;
    char           saying[SETTINGS_SAYING_MAX];
    uint32_t      *set_on = calloc(out->Count ? out->Count : 1, sizeof(uint32_t));
    int            rc = 0;

    if (!set_on) return -ERR_NO_MEMORY;
    SettingsChosenBegin((const uint8_t *)out->Chosen, out->ChosenSize, &chosen);
    while (rc == 0 && SettingsChosenNext(&chosen, &line)) {
        switch (line.Kind) {
        case SETTINGS_LINE_HEAD:
            if (!SettingSpanIs(line.Value, SETTINGS_VERSION_WORD)) {
                snprintf(saying, sizeof(saying), "%s.settings says settings %.*s; it is read as settings 1",
                         out->Program, (int)line.Value.Length, (const char *)line.Value.At);
                rc = settings_say(out, saying);
            }
            break;
        case SETTINGS_LINE_NOT_TEXT:
            snprintf(saying, sizeof(saying), "%s.settings, line %u is not text; it is passed over", out->Program,
                     line.Line);
            rc = settings_say(out, saying);
            break;
        case SETTINGS_LINE_SET:
            rc = assemble_set_line(out, &line, set_on);
            break;
        default:
            break;
        }
    }
    free(set_on);
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
        free((void *)entry->Name);
        free((void *)entry->What);
        free((void *)entry->Default);
        free((void *)entry->Unit);
        free((void *)entry->Choices);
        free((void *)entry->Value);
    }
    free(settings->Settings);
    for (size_t i = 0; i < settings->ProblemCount; i++) free(settings->Problems[i]);
    free(settings->Problems);
    settings->Settings     = NULL;
    settings->Count        = 0;
    settings->Problems     = NULL;
    settings->ProblemCount = 0;
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

const BoxSetting *settings_find(const BoxSettings *settings, const char *name)
{
    if (!settings || !name) return NULL;
    for (size_t i = 0; i < settings->Count; i++)
        if (strcmp(settings->Settings[i].Name, name) == 0) return &settings->Settings[i];
    return NULL;
}

int settings_check(BoxSettings *settings, const char *name, const char *value)
{
    char        saying[SETTINGS_SAYING_MAX], wants[SETTINGS_SAYING_MAX / 2];
    size_t      index;
    SettingSpan named = span_of(name);
    BoxSetting *entry = entry_named(settings, named, &index);
    Setting     declared;
    SettingValue read;

    if (!entry) {
        say_unknown(settings, named, saying, sizeof(saying));
        return settings_refuse(settings, saying);
    }
    SettingsDeclaredFind((const uint8_t *)settings->Declared, settings->DeclaredSize, named, &declared);
    if (SettingValueRead(&declared, span_of(value), &read) == SETTINGS_OK) return 0;
    settings_wants(entry, wants, sizeof(wants));
    snprintf(saying, sizeof(saying), "%s wants %s", entry->Name, wants);
    return settings_refuse(settings, saying);
}
