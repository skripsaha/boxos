/*
 * settings — what a person can set in a program, and setting it.
 *
 *     settings clock                   every setting: what it is now, what it takes, as it came
 *     settings clock hours             one setting
 *     settings clock hours 12          set it — held to what the program declares
 *     settings clock hours default     as it came
 *     settings clock default           every setting as it came
 *
 * What can be set the program declares (box/settings.h); what the person sets
 * goes into the file <program>.settings, one line changed and no other —
 * comments and order kept — saved whole, so a program watching it reads it
 * again at once. A value between double quotes is taken as it stands:
 * "  two edges  ", "default".
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

#include "settings_pen.h"

BOX_FORM("takes name program \"the program\"\n"
         "takes optional text \"a setting, and what it becomes — default for as it came\"\n");

/* Whether the file is a program the shell can start: it wears app or utility. */
static bool runnable(const FileMetadata *metadata)
{
    for (const char *at = metadata->Tags; at && *at;) {
        const char *end = strchr(at, ',');
        size_t      length = end ? (size_t)(end - at) : strlen(at);
        if ((length >= 3 && memcmp(at, "app", 3) == 0 && (length == 3 || at[3] == ':')) ||
            (length >= 7 && memcmp(at, "utility", 7) == 0 && (length == 7 || at[7] == ':')))
            return true;
        at = end ? end + 1 : NULL;
    }
    return false;
}

/* The program a word names: by its whole file name, or as the shell names a
 * command, by its name before the dot — on the whole volume. */
static int find_program(const char *word, file_id_t *out)
{
    char       stem[32];
    const char *dot = strrchr(word, '.');
    size_t      length = dot ? (size_t)(dot - word) : strlen(word);
    file_id_t  *ids = NULL;
    file_id_t   found[8];
    int         count = 0;

    if (length == 0 || length >= sizeof(stem)) {
        printf("settings: %s names no program\n", word);
        return -ERR_INVALID_ARGUMENT;
    }
    memcpy(stem, word, length);
    stem[length] = '\0';
    int n = query_all_everywhere(stem, &ids);
    for (int i = 0; i < n; i++) {
        FileMetadata metadata;
        if (FileMetadataRead(ids[i], &metadata) != 0) continue;
        const char *name = metadata.Filename ? metadata.Filename : "";
        const char *its_dot = strrchr(name, '.');
        size_t      its_length = its_dot ? (size_t)(its_dot - name) : strlen(name);
        bool        named = dot ? strcmp(name, word) == 0
                                : its_length == length && memcmp(name, stem, length) == 0;
        if (named && runnable(&metadata) && (metadata.Flags & FILE_FLAG_TRASHED) == 0 &&
            count < (int)(sizeof(found) / sizeof(found[0])))
            found[count++] = ids[i];
        FileMetadataRelease(&metadata);
    }
    free(ids);
    if (n < 0) {
        printf("settings: the volume did not answer for %s\n", word);
        return n;
    }
    if (count == 0) {
        printf("settings: there is no program called %s\n", word);
        return -ERR_FILE_NOT_FOUND;
    }
    if (count > 1) {
        printf("settings: %s could be any of these — say its whole name:\n", word);
        for (int i = 0; i < count; i++) {
            FileMetadata metadata;
            if (FileMetadataRead(found[i], &metadata) != 0) continue;
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

static void show_setting(const BoxSettings *settings, const BoxSetting *setting)
{
    char wants[160];
    settings_wants(setting, wants, sizeof(wants));
    printf("%color%s%color ", COLOR_CYAN, setting->Name, COLOR_DEFAULT);
    print_value(setting->Value);
    if (setting->Kind == SETTING_NUMBER && setting->Unit[0]) printf(" %s", setting->Unit);
    printf(" — %s\n    %s; ", setting->What, wants);
    if (setting->Chosen) {
        printf("%coloryou set it%color in %s.settings, ", COLOR_GREEN, COLOR_DEFAULT, settings->Program);
        print_value(setting->Default);
        printf(" as it came");
    } else {
        printf("as it came");
    }
    printf("%s\n", setting->NextStart ? "; takes effect when it starts next" : "");
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
        if (settings->File == 0) printf("nothing is set: there is no %s.settings\n", settings->Program);
    }
    show_problems(settings);
    return settings->ProblemCount == 0 ? 0 : 1;
}

/* When what was set takes effect. */
static const char *when(const BoxSetting *setting)
{
    return setting && setting->NextStart ? "when it starts next" : "now";
}

/* The value as typed: its blanks at both ends taken off; between double
 * quotes, as it stands. *bare_default: the bare word default. */
static const char *value_of(char *typed, bool *bare_default)
{
    while (*typed == ' ' || *typed == '\t') typed++;
    size_t length = strlen(typed);
    while (length > 0 && (typed[length - 1] == ' ' || typed[length - 1] == '\t')) typed[--length] = '\0';
    *bare_default = strcmp(typed, "default") == 0;
    if (length >= 2 && typed[0] == '"' && typed[length - 1] == '"') {
        typed[length - 1] = '\0';
        return typed + 1;
    }
    return typed;
}

/* `settings PROGRAM NAME [VALUE]`, or `settings PROGRAM default`. */
static int change(BoxSettings *settings, char *typed)
{
    char       *name = typed;
    const char *why  = NULL;
    while (*name == ' ' || *name == '\t') name++;
    char *rest = name;
    while (*rest && *rest != ' ' && *rest != '\t') rest++;
    if (*rest) *rest++ = '\0';

    bool        bare_default;
    const char *value = value_of(rest, &bare_default);
    if (value[0] == '\0' && strcmp(name, "default") == 0) {
        int rc = settings_clear(settings);
        if (rc != 0) {
            printf("settings: %s.settings could not be written\n", settings->Program);
            return 1;
        }
        printf("every setting of %s is as it came\n", settings->Program);
        return 0;
    }
    if (value[0] == '\0' && !bare_default && rest[0] == '\0') {
        const BoxSetting *setting = settings_find(settings, name);
        if (!setting) {
            settings_check(settings, name, "");
            printf("settings: %s\n", settings->Why);
            return 1;
        }
        show_setting(settings, setting);
        return 0;
    }

    int rc = bare_default ? settings_erase(settings, name, &why) : settings_set(settings, name, value, &why);
    if (rc == -ERR_INVALID_ARGUMENT && why) {
        printf("settings: %s\n", why);
        return 1;
    }
    if (rc != 0) {
        printf("settings: %s.settings could not be written\n", settings->Program);
        return 1;
    }
    const BoxSetting *setting = settings_find(settings, name);
    printf("%s: ", name);
    print_value(setting ? setting->Value : "");
    printf("%s, %s\n", bare_default ? " as it came" : "", when(setting));
    return 0;
}

int main(void)
{
    const char *const *words;
    size_t             count;
    file_id_t          program = 0;
    BoxSettings        settings;

    if (form_words(0, &words, &count) != 0 || count != 1) exit(1);
    if (find_program(words[0], &program) != 0) exit(1);
    int rc = settings_read(program, &settings);
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
