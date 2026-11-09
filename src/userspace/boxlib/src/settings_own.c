/*
 * settings_own.c — a program's own settings (box/settings.h). Its declaration
 * is the part settings the kernel put into its memory at start (a box), or
 * the bytes between the symbols round .boxos.settings (an ELF image); the
 * person's file is found by the program's own name. Both are read at the
 * first ask, and the file again when the program asks — or when a watch sees
 * the file saved.
 */

#include "box/core/settings_internal.h"
#include "box/core/cabin.h"
#include "box/error.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/program_part.h"
#include "box/string.h"
#include "box/sync.h"
#include "box/touch.h"

/* PROVIDEd by user.ld (and defined by BoxASM's linker) round an ELF image's
 * declaration — hidden, reached relative to the code. A box has its
 * declaration elsewhere: these name nothing loaded there. */
extern const char __boxos_settings_start[] __attribute__((visibility("hidden")));
extern const char __boxos_settings_end[] __attribute__((visibility("hidden")));

#define TOUCH_FILE_ANCHORED 2u   /* the op of a file's anchor: saved whole */

static umutex_t    g_lock = UMUTEX_INIT;
static BoxSettings g_own;
static bool        g_read;
static bool        g_mistake_said;

static int own_declaration(BoxSettings *own)
{
    const uint8_t *bytes = NULL;
    size_t         size  = 0;
    if (cabin_info()->parts_addr != 0) {
        if (program_part_loaded("settings", &bytes, &size) != 1) size = 0;
    } else if ((uintptr_t)__boxos_settings_end > (uintptr_t)__boxos_settings_start) {
        bytes = (const uint8_t *)__boxos_settings_start;
        size  = (size_t)(__boxos_settings_end - __boxos_settings_start);
    }
    while (size > 0 && bytes[size - 1] == 0) size--;
    if (size == 0) return 0;
    if (!(own->Declared = malloc(size))) return -ERR_NO_MEMORY;
    memcpy(own->Declared, bytes, size);
    own->DeclaredSize = size;
    return 0;
}

/* The person's file as it is now, into g_own.Chosen, and what was found to
 * say of it. */
static void own_chosen(char **saying)
{
    char  *bytes = NULL;
    size_t size  = 0;
    *saying      = NULL;
    free(g_own.Chosen);
    g_own.Chosen     = NULL;
    g_own.ChosenSize = 0;
    if (g_own.Program[0] == '\0' || settings_file_find(g_own.Program, &g_own.File, saying) != 0 ||
        g_own.File == 0)
        return;
    int rc = settings_file_read(g_own.File, &bytes, &size);
    if (rc == 0) {
        g_own.Chosen     = bytes;
        g_own.ChosenSize = size;
    } else if (!*saying && (*saying = malloc(SETTINGS_SAYING_MAX)) != NULL) {
        if (rc == -ERR_BINARY_TOO_LARGE)
            snprintf(*saying, SETTINGS_SAYING_MAX, "%s.settings is larger than %u KiB and is not read; everything "
                     "is as %s came", g_own.Program, SETTINGS_FILE_MAX / 1024u, g_own.Program);
        else
            snprintf(*saying, SETTINGS_SAYING_MAX, "%s.settings could not be read; everything is as %s came",
                     g_own.Program, g_own.Program);
    }
}

/* The settings put together from the declaration and the file as it is now,
 * with what was found to say of the file first. */
static void own_assemble(void)
{
    char *saying;
    own_chosen(&saying);
    settings_reassemble(&g_own);
    if (saying) settings_say(&g_own, saying);
    free(saying);
}

static void own_read_locked(void)
{
    if (g_read) return;
    g_read = true;
    file_id_t program = cabin_info()->program_file;
    if (program == 0 || settings_program_name(program, g_own.Program) != 0) g_own.Program[0] = '\0';
    own_declaration(&g_own);
    own_assemble();
}

/* The setting called `name`, when it is of `kind` — else the program's
 * mistake, said once. Under the lock. */
static const BoxSetting *own_setting(const char *name, SettingKind kind)
{
    own_read_locked();
    const BoxSetting *setting = settings_find(&g_own, name);
    if (setting && setting->Kind == kind) return setting;
    if (!g_mistake_said) {
        g_mistake_said = true;
        if (!setting)
            printf("settings: %s asks for its setting %s, which it does not declare\n", g_own.Program,
                   name ? name : "(none)");
        else
            printf("settings: %s asks for its setting %s as %s, and it is %s\n", g_own.Program, name,
                   SettingKindWord(kind), SettingKindWord(setting->Kind));
    }
    return NULL;
}

bool settings_yes(const char *name)
{
    umutex_lock(&g_lock);
    const BoxSetting *setting = own_setting(name, SETTING_YES_OR_NO);
    bool              yes     = setting && setting->Yes;
    umutex_unlock(&g_lock);
    return yes;
}

int64_t settings_number(const char *name)
{
    umutex_lock(&g_lock);
    const BoxSetting *setting = own_setting(name, SETTING_NUMBER);
    int64_t           number  = setting ? setting->Number : 0;
    umutex_unlock(&g_lock);
    return number;
}

static const char *own_text(const char *name, SettingKind kind)
{
    umutex_lock(&g_lock);
    const BoxSetting *setting = own_setting(name, kind);
    const char       *text    = setting ? setting->Value : "";
    umutex_unlock(&g_lock);
    return text;
}

const char *settings_word(const char *name)
{
    return own_text(name, SETTING_WORD);
}

const char *settings_text(const char *name)
{
    return own_text(name, SETTING_TEXT);
}

const char *settings_choice(const char *name)
{
    return own_text(name, SETTING_ONE_OF);
}

uint32_t settings_color(const char *name)
{
    umutex_lock(&g_lock);
    const BoxSetting *setting = own_setting(name, SETTING_COLOR);
    uint32_t          color   = setting ? setting->Color : 0;
    umutex_unlock(&g_lock);
    return color;
}

bool settings_reload(void)
{
    umutex_lock(&g_lock);
    if (!g_read) {
        own_read_locked();
        umutex_unlock(&g_lock);
        return false;
    }
    /* What each setting was, to tell whether anything changed. */
    size_t count   = g_own.Count;
    char **before  = calloc(count ? count : 1, sizeof(char *));
    bool   changed = before == NULL;
    for (size_t i = 0; before && i < count; i++) {
        const char *value = g_own.Settings[i].Value;
        if ((before[i] = malloc(strlen(value) + 1)) != NULL) memcpy(before[i], value, strlen(value) + 1);
        else changed = true;
    }
    own_assemble();
    for (size_t i = 0; before && i < count; i++) {
        if (before[i] && (i >= g_own.Count || strcmp(before[i], g_own.Settings[i].Value) != 0)) changed = true;
        free(before[i]);
    }
    free(before);
    changed = changed || count != g_own.Count;
    umutex_unlock(&g_lock);
    return changed;
}

/* ---- the watch ---------------------------------------------------------- */

struct SettingsWatch {
    TouchTag Tag;
};

int settings_watch_begin(SettingsWatch **out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;
    *out = NULL;
    TouchTag tag = touch_pair_choose(touch_intern("settings"));
    if (tag == TOUCH_TAG_INVALID) return -ERR_TAG_NOT_FOUND;
    SettingsWatch *watch = malloc(sizeof(*watch));
    if (!watch) return -ERR_NO_MEMORY;
    int rc = touch_claim(tag, TOUCH_REST, 0, 0);
    if (rc != 0) {
        free(watch);
        return rc;
    }
    watch->Tag = tag;
    *out       = watch;
    return 0;
}

/* Whether the anchored file is this program's settings file: the one it read,
 * or one of that name made since. */
static bool own_file(file_id_t file)
{
    char         name[40];
    FileMetadata metadata;
    umutex_lock(&g_lock);
    own_read_locked();
    bool mine = g_own.File != 0 && file == g_own.File;
    snprintf(name, sizeof(name), "%s.settings", g_own.Program);
    bool named = g_own.Program[0] != '\0';
    umutex_unlock(&g_lock);
    if (mine || !named || FileMetadataRead(file, &metadata) != 0) return mine;
    mine = metadata.Filename && strcmp(metadata.Filename, name) == 0;
    FileMetadataRelease(&metadata);
    return mine;
}

bool settings_watch_poll(SettingsWatch *watch)
{
    Touch touch;
    bool  saved = false;
    if (!watch) return false;
    while (touch_try_pop_tag(watch->Tag, &touch)) {
        uint64_t file;
        if (touch.payload_len < 9 || touch.payload[8] != TOUCH_FILE_ANCHORED) continue;
        memcpy(&file, touch.payload, sizeof(file));
        if (own_file((file_id_t)file)) saved = true;
    }
    return saved && settings_reload();
}

void settings_watch_end(SettingsWatch *watch)
{
    if (!watch) return;
    touch_release(watch->Tag);
    free(watch);
}
