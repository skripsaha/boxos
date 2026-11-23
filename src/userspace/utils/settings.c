/*
 * settings — what a person can set in a program, and setting it.
 *
 *     settings clock                         every setting: what it is now, what it takes, as it came
 *     settings clock digits                  one setting, or a thing and all it holds
 *     settings clock digits color amber      set it — held to what the program declares
 *     settings clock [work] hours 12         set it for the context work
 *     settings clock digits color default    as it came
 *     settings clock digits default          all the thing holds, as it came
 *     settings clock [work] default          everything set for work, as it came
 *     settings clock default                 every setting as it came, in every context
 *
 * What is set for a context holds while the Use Context holds its tags (box/
 * settings.h says which block of several); the line of a block that holds
 * now is said so.
 *
 * What can be set the program declares (box/settings.h), a tree of things and
 * what each has; a setting is named by its path, its names from the top down.
 * What the person sets goes into the file <program>.settings, the lines of
 * that one setting changed and no other — comments, order and the way the
 * person wrote the file kept — saved whole, so a program watching it reads it
 * again at once. A value between double quotes is taken as it stands:
 * "  two edges  ", "default".
 *
 * A part of the system the volume carries no program file of is named as a
 * program all the same — gangway, the loader: its declaration is the one it
 * reads by itself (boxos_gangway_settings.h), its file gangway.settings.
 *
 * Its exit: 0 when it did what it was asked, 1 when it refused, or the
 * program has lines in its file it could not take.
 */

#include "box/error.h"
#include "box/file.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/settings.h"
#include "box/string.h"
#include "box/system.h"
#include "box/program_part.h"
#include "box/use.h"
#include "boxos_gangway_settings.h"
#include "settings_pen.h"

BOX_FORM("takes name program \"the program\"\n"
         "takes optional text \"a setting by its path — [tags] before it for a context — and what it becomes; "
         "default for as it came\"\n");

/* Whether the file is a program the shell can start: it holds a box or an ELF
 * image, whatever it wears. */
static bool runnable(file_id_t file)
{
    ProgramKind kind;
    return program_kind(file, &kind) == 0 && kind != PROGRAM_NONE;
}

/* The program a word names: by its whole file name, or as the shell names a
 * command, by its name before the dot — on the whole volume. */
static int find_program(const char *word, file_id_t *out)
{
    const char *dot = strrchr(word, '.');
    size_t      length = dot ? (size_t)(dot - word) : strlen(word);
    file_id_t  *ids = NULL;
    file_id_t   found[8];
    int         count = 0;

    if (length == 0) {
        printf("settings: %s names no program\n", word);
        return -ERR_INVALID_ARGUMENT;
    }
    int n = query_name_all_everywhere(MY_TAGS, word, NULL, NULL, &ids);
    for (int i = 0; i < n; i++) {
        FileMetadata metadata;
        if (FileMetadataRead(MY_TAGS, ids[i], &metadata) != 0) continue;
        const char *name = metadata.Filename ? metadata.Filename : "";
        const char *its_dot = strrchr(name, '.');
        size_t      its_length = its_dot ? (size_t)(its_dot - name) : strlen(name);
        bool        named = dot ? strcmp(name, word) == 0
                                : its_length == length && memcmp(name, word, length) == 0;
        if (named && runnable(ids[i]) &&
            count < (int)(sizeof(found) / sizeof(found[0])))
            found[count++] = ids[i];
        FileMetadataRelease(&metadata);
    }
    free(ids);
    if (n < 0) {
        printf("settings: the volume did not answer for %s\n", word);
        return n;
    }
    count = (int)files_home_first(found, (size_t)count);   /* the home volume's first */
    if (count == 0) {
        printf("settings: there is no program called %s\n", word);
        return -ERR_FILE_NOT_FOUND;
    }
    if (count > 1) {
        printf("settings: %s could be any of these — say its whole name:\n", word);
        for (int i = 0; i < count; i++) {
            FileMetadata metadata;
            if (FileMetadataRead(MY_TAGS, found[i], &metadata) != 0) continue;
            printf("  %s\n", metadata.Filename);
            FileMetadataRelease(&metadata);
        }
        return -ERR_INVALID_ARGUMENT;
    }
    *out = found[0];
    return 0;
}

/* A value as its file writes it: between double quotes when it must be. */
static void print_value(const char *value)
{
    SettingSpan span = { (const uint8_t *)value, strlen(value) };
    printf(SettingsPenQuotes(span) ? "\"%s\"" : "%s", value);
}

static void print_indent(uint32_t depth)
{
    for (uint32_t i = 0; i < depth; i++) printf("    ");
}

/* NAME VALUE — what it does, and under it what it takes and whether the
 * person set it; a thing that only holds others by its name and what it
 * holds. Indented as deep as it stands. */
static void show_setting(const BoxSettings *settings, const BoxSetting *setting)
{
    char wants[160];
    print_indent(setting->Depth);
    printf("%color%s%color", COLOR_CYAN, setting->Name, COLOR_DEFAULT);
    if (setting->Kind == SETTING_GROUP) {
        printf(" — %s\n", setting->What);
        return;
    }
    printf(" ");
    print_value(setting->Value.Text);
    if (setting->Kind == SETTING_NUMBER && setting->Unit[0] && strcmp(setting->Value.Text, "off") != 0)
        printf(" %s", setting->Unit);
    settings_wants(setting, wants, sizeof(wants));
    printf(" — %s\n", setting->What);
    print_indent(setting->Depth + 1);
    printf("%s; ", wants);
    if (setting->Chosen) {
        printf("%coloryou set it%color in %s.settings, ", COLOR_GREEN, COLOR_DEFAULT, settings->Program);
        print_value(setting->Default);
        printf(" as it came");
    } else {
        printf("as it came");
    }
    printf("%s\n", setting->NextStart ? "; takes effect when it starts next" : "");
}

/* The Use Context as it is now, comma-joined; NULL for none — or when the
 * kernel would not say, and then no block holds. The caller frees it. */
static char *context_now(void)
{
    for (;;) {
        size_t needed = 0, again = 0;
        char  *context;
        if (use_get(NULL, 0, &needed) < 0 || needed <= 1 || !(context = malloc(needed))) return NULL;
        if (use_get(context, needed, &again) > 0 && again <= needed) return context;
        free(context);
        if (again <= needed) return NULL;
    }
}

/* What the person's file sets in a context: [TAGS] PATH VALUE — as the
 * command would set it, and whether it holds now. Only those of the settings
 * `under` holds (all for NULL), and of the context `tags` (any for NULL). */
static void show_in_contexts(const BoxSettings *settings, const BoxSetting *under, const char *tags)
{
    size_t      length  = under ? strlen(under->Path) : 0;
    SettingSpan wanted  = { (const uint8_t *)(tags ? tags : ""), tags ? strlen(tags) : 0 };
    char       *context = context_now();
    for (size_t i = 0; i < settings->InContextCount; i++) {
        const BoxSettingInContext *in      = &settings->InContext[i];
        const BoxSetting          *setting = &settings->Settings[in->Setting];
        SettingSpan                its     = { (const uint8_t *)in->Tags, strlen(in->Tags) };
        if (under && setting != under &&
            !(strncmp(setting->Path, under->Path, length) == 0 && setting->Path[length] == ' '))
            continue;
        if (tags && !SettingTagsSame(its, wanted)) continue;
        printf("%color[%s]%color %s ", COLOR_GREEN, in->Tags, COLOR_DEFAULT, setting->Path);
        print_value(in->Value.Text);
        printf("%s\n", settings_in_context(settings, in->Setting, context) == in ? " — holds now" : "");
    }
    free(context);
}

/* Whether the value just set for the context `tags` holds now: said after
 * it — and which block holds over it, when the context holds its tags and
 * one of more tags holds too. */
static void say_holds(const BoxSettings *settings, const BoxSetting *setting, const char *tags)
{
    char                      *context = context_now();
    SettingSpan                its     = { (const uint8_t *)tags, strlen(tags) };
    SettingSpan                now     = { (const uint8_t *)(context ? context : ""), context ? strlen(context) : 0 };
    const BoxSettingInContext *holds   = NULL;
    if (setting && context && SettingTagsWithin(its, now))
        holds = settings_in_context(settings, (size_t)(setting - settings->Settings), context);
    if (holds && SettingTagsSame(SettingSpanOf((const uint8_t *)holds->Tags, strlen(holds->Tags)), its))
        printf(" — it does now");
    else if (holds)
        printf(" — now [%s] holds over it", holds->Tags);
    printf("\n");
    free(context);
}

static void show_problems(const BoxSettings *settings)
{
    for (size_t i = 0; i < settings->ProblemCount; i++)
        printf("%color%s%color\n", COLOR_YELLOW, settings->Problems[i], COLOR_DEFAULT);
}

static int show_all(const BoxSettings *settings)
{
    if (settings->Count == 0) {
        printf("%s declares no settings\n", settings->Program);
    } else {
        for (size_t i = 0; i < settings->Count; i++) show_setting(settings, &settings->Settings[i]);
        show_in_contexts(settings, NULL, NULL);
        if (settings->File == 0) printf("nothing is set: there is no %s.settings\n", settings->Program);
    }
    show_problems(settings);
    return settings->ProblemCount == 0 ? 0 : 1;
}

/* A setting, or a thing and every setting it holds. */
static void show_under(const BoxSettings *settings, const BoxSetting *setting)
{
    size_t length = strlen(setting->Path);
    for (size_t i = 0; i < settings->Count; i++) {
        const BoxSetting *one = &settings->Settings[i];
        if (one == setting || (strncmp(one->Path, setting->Path, length) == 0 && one->Path[length] == ' '))
            show_setting(settings, one);
    }
    show_in_contexts(settings, setting, NULL);
}

/* When what was set takes effect. */
static const char *when(const BoxSetting *setting)
{
    return setting && setting->NextStart ? "when it starts next" : "now";
}

/* What was typed after the program, cut: [TAGS] — the context, NULL for
 * everywhere — then the names of a path down the declared tree, then the
 * rest, the value. A name that only holds others takes no value, so a word
 * after it names what it holds; after a name with a value of its own a word
 * names what it holds while another word follows. */
typedef struct {
    char       *Tags;
    char        Path[256];
    const char *Value;          /* the rest of the line, its blanks at both ends taken off */
    bool        Named;          /* a word named a setting */
} Asked;

static bool cut(BoxSettings *settings, char *typed, Asked *out)
{
    out->Tags  = NULL;
    out->Path[0] = '\0';
    out->Named = false;
    while (*typed == ' ' || *typed == '\t') typed++;
    if (*typed == '[') {
        char *close = strchr(typed, ']');
        if (!close) {
            printf("settings: %s: a context is its tags between [ and ]\n", typed);
            return false;
        }
        *close    = '\0';
        out->Tags = typed + 1;
        typed     = close + 1;
    }

    const uint8_t *declared = (const uint8_t *)settings->Declared;
    SettingSpan    line     = { (const uint8_t *)typed, strlen(typed) }, word, next;
    uint64_t       at       = 0;
    uint32_t       parent   = 0;
    bool           quoted, group = true;
    Setting        setting;
    SettingsVerdict ignored = SETTINGS_OK;
    for (;;) {
        uint64_t look = at, after;
        if (!SettingWord(line, &look, &word, &quoted, &ignored) || quoted) break;
        if (!group) {
            after = look;
            if (!SettingWord(line, &after, &next, &quoted, &ignored)) break;
        }
        if (!SettingsDeclaredChild(declared, settings->DeclaredSize, parent, word, &setting)) {
            /* A word where only a name may stand: named as a name, so the
             * refusal says what was meant. */
            if (group && out->Named) {
                size_t used = strlen(out->Path);
                snprintf(out->Path + used, sizeof(out->Path) - used, " %.*s", (int)word.Length,
                         (const char *)word.At);
                at = look;
            }
            break;
        }
        size_t used = strlen(out->Path);
        snprintf(out->Path + used, sizeof(out->Path) - used, "%s%.*s", used ? " " : "", (int)word.Length,
                 (const char *)word.At);
        parent     = setting.Line;
        group      = setting.Kind == SETTING_GROUP;
        out->Named = true;
        at         = look;
    }
    char *rest = typed + at;
    while (*rest == ' ' || *rest == '\t') rest++;
    size_t length = strlen(rest);
    while (length > 0 && (rest[length - 1] == ' ' || rest[length - 1] == '\t')) rest[--length] = '\0';
    out->Value = rest;
    return true;
}

/* The value as typed: between double quotes, as it stands. *bare_default:
 * the bare word default. */
static const char *value_of(const char *typed, char *copy, bool *bare_default)
{
    size_t length = strlen(typed);
    *bare_default = strcmp(typed, "default") == 0;
    if (length >= 2 && typed[0] == '"' && typed[length - 1] == '"') {
        memcpy(copy, typed + 1, length - 2);
        copy[length - 2] = '\0';
        return copy;
    }
    return typed;
}

static int written_or_said(const BoxSettings *settings, int rc, const char *why)
{
    if (rc == -ERR_INVALID_ARGUMENT && why) {
        printf("settings: %s\n", why);
        return 1;
    }
    if (rc != 0) {
        printf("settings: %s.settings could not be written\n", settings->Program);
        return 1;
    }
    return 0;
}

/* `[TAGS] default`: everything set for that context, as it came. */
static int clear_context(BoxSettings *settings, const char *tags)
{
    SettingSpan wanted = { (const uint8_t *)tags, strlen(tags) };
    for (;;) {
        size_t i = 0;
        while (i < settings->InContextCount &&
               !SettingTagsSame(SettingSpanOf((const uint8_t *)settings->InContext[i].Tags,
                                              strlen(settings->InContext[i].Tags)),
                                wanted))
            i++;
        if (i == settings->InContextCount) break;
        const char *why = NULL;
        int rc = settings_erase(settings, tags, settings->Settings[settings->InContext[i].Setting].Path, &why);
        if (written_or_said(settings, rc, why) != 0) return 1;
    }
    printf("every setting of %s for [%s] is as it came\n", settings->Program, tags);
    return 0;
}

static int change(BoxSettings *settings, char *typed)
{
    Asked       asked;
    const char *why = NULL;
    if (!cut(settings, typed, &asked)) return 1;

    bool        bare_default;
    char       *copy  = malloc(strlen(asked.Value) + 1);
    if (!copy) return 1;
    const char *value = value_of(asked.Value, copy, &bare_default);
    int         status;
    if (!asked.Named && bare_default) {
        if (asked.Tags) {
            status = clear_context(settings, asked.Tags);
        } else if (settings_clear(settings) != 0) {
            printf("settings: %s.settings could not be written\n", settings->Program);
            status = 1;
        } else {
            printf("every setting of %s is as it came\n", settings->Program);
            status = 0;
        }
        free(copy);
        return status;
    }
    if (!asked.Named && asked.Tags && asked.Value[0] == '\0') {
        free(copy);
        show_in_contexts(settings, NULL, asked.Tags);
        return 0;
    }
    const char *path = asked.Named ? asked.Path : asked.Value;
    if (!asked.Named || asked.Value[0] == '\0') {
        const BoxSetting *setting = asked.Named ? settings_find(settings, asked.Path) : NULL;
        free(copy);
        if (!setting) {
            settings_check(settings, path, "");
            printf("settings: %s\n", settings->Why);
            return 1;
        }
        show_under(settings, setting);
        return 0;
    }

    int rc = bare_default ? settings_erase(settings, asked.Tags, asked.Path, &why)
                          : settings_set(settings, asked.Tags, asked.Path, value, &why);
    free(copy);
    if ((status = written_or_said(settings, rc, why)) != 0) return status;
    const BoxSetting *setting = settings_find(settings, asked.Path);
    if (asked.Tags) printf("[%s] ", asked.Tags);
    printf("%s: ", asked.Path);
    if (setting && setting->Kind == SETTING_GROUP) {
        printf("all it holds as it came\n");
        return 0;
    }
    if (asked.Tags) {
        if (bare_default) printf("as it came there\n");
        else {
            print_value(value);
            printf(", while the Use Context holds %s", asked.Tags);
            say_holds(settings, setting, asked.Tags);
        }
        return 0;
    }
    print_value(setting ? setting->Value.Text : "");
    printf("%s, %s\n", bare_default ? " as it came" : "", when(setting));
    return 0;
}

/* The parts of the system the volume carries no program file of, and the
 * declarations they read their settings by. */
static const struct {
    const char *name;
    const char *declared;
    size_t      size;
} g_boxless[] = {
    { GANGWAY_SETTINGS_PROGRAM, GANGWAY_SETTINGS, sizeof(GANGWAY_SETTINGS) - 1 },
};

/* The settings the word names: a part of the system without a program file,
 * or a program — 0 or a negative -error_t; a word that names neither ends the
 * utility, said (find_program). */
static int read_named(const char *word, BoxSettings *settings)
{
    for (size_t i = 0; i < sizeof(g_boxless) / sizeof(g_boxless[0]); i++)
        if (strcmp(word, g_boxless[i].name) == 0)
            return settings_read_declared(g_boxless[i].name, g_boxless[i].declared, g_boxless[i].size, settings);
    file_id_t program = 0;
    if (find_program(word, &program) != 0) exit(1);
    return settings_read(program, settings);
}

int main(void)
{
    const char *const *words;
    size_t             count;
    BoxSettings        settings;

    if (form_words(0, &words, &count) != 0 || count != 1) exit(1);
    int rc = read_named(words[0], &settings);
    if (rc != 0) {
        printf("settings: %s could not be read\n", words[0]);
        exit(1);
    }
    const char *typed = form_text(1);
    int         status;
    if (!typed || typed[0] == '\0') {
        status = show_all(&settings);
    } else {
        char *copy = malloc(strlen(typed) + 1);
        if (!copy) exit(1);
        memcpy(copy, typed, strlen(typed) + 1);
        status = change(&settings, copy);
        free(copy);
    }
    settings_release(&settings);
    exit(status);
    return status;
}
