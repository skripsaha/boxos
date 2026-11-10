/*
 * settings_own.c — a program's own settings (box/settings.h). Its declaration
 * is the part settings the kernel put into its memory at start (a box), or
 * the bytes between the symbols round .boxos.settings (an ELF image); the
 * person's file is found by the program's own name. Both are read at the
 * first ask, with the Use Context its values hold in; the file and the
 * context again when the program asks — or each when a watch hears it change.
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
#include "box/use.h"

/* PROVIDEd by user.ld (and defined by BoxASM's linker) round an ELF image's
 * declaration — hidden, reached relative to the code. A box has its
 * declaration elsewhere: these name nothing loaded there. */
extern const char __boxos_settings_start[] __attribute__((visibility("hidden")));
extern const char __boxos_settings_end[] __attribute__((visibility("hidden")));

#define TOUCH_FILE_ANCHORED 2u   /* the op of a file's anchor: saved whole */

static umutex_t    g_lock = UMUTEX_INIT;
static BoxSettings g_own;
static char       *g_context;   /* the Use Context the values hold in, comma-joined; NULL for none */
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

/* The Use Context as it is now, into g_context: asked for its size first,
 * and read again if it grew in between. A kernel that would not say is no
 * context — what is set everywhere holds. */
static void own_context(void)
{
    free(g_context);
    g_context = NULL;
    for (;;) {
        size_t needed = 0, again = 0;
        if (use_get(NULL, 0, &needed) < 0 || needed <= 1) return;
        if (!(g_context = malloc(needed))) return;
        if (use_get(g_context, needed, &again) > 0 && again <= needed) return;
        free(g_context);
        g_context = NULL;
        if (again <= needed) return;
    }
}

static void own_read_locked(void)
{
    if (g_read) return;
    g_read = true;
    file_id_t program = cabin_info()->program_file;
    if (program == 0 || settings_program_name(program, g_own.Program) != 0) g_own.Program[0] = '\0';
    own_declaration(&g_own);
    own_assemble();
    own_context();
}

/* What the setting is in the context the settings were read in. Under the
 * lock. */
static const BoxSettingValue *own_value(const BoxSetting *setting)
{
    const BoxSettingInContext *in = settings_in_context(&g_own, (size_t)(setting - g_own.Settings), g_context);
    return in ? &in->Value : &setting->Value;
}

/* The setting at `path`, when it is of `kind` — else the program's mistake,
 * said once. Under the lock. */
static const BoxSetting *own_setting(const char *path, SettingKind kind)
{
    own_read_locked();
    const BoxSetting *setting = settings_find(&g_own, path);
    if (setting && setting->Kind == kind) return setting;
    if (!g_mistake_said) {
        g_mistake_said = true;
        if (!setting)
            printf("settings: %s asks for its setting %s, which it does not declare\n", g_own.Program,
                   path ? path : "(none)");
        else
            printf("settings: %s asks for its setting %s as %s, and it is %s\n", g_own.Program, path,
                   SettingKindWord(kind), setting->Kind == SETTING_GROUP ? "a thing that only holds others"
                                                                       : SettingKindWord(setting->Kind));
    }
    return NULL;
}

bool settings_on(const char *path)
{
    umutex_lock(&g_lock);
    const BoxSetting *setting = own_setting(path, SETTING_ON_OR_OFF);
    bool              on      = setting && own_value(setting)->On;
    umutex_unlock(&g_lock);
    return on;
}

int64_t settings_number(const char *path)
{
    umutex_lock(&g_lock);
    const BoxSetting *setting = own_setting(path, SETTING_NUMBER);
    int64_t           number  = setting ? own_value(setting)->Number : 0;
    umutex_unlock(&g_lock);
    return number;
}

static const char *own_text(const char *path, SettingKind kind)
{
    umutex_lock(&g_lock);
    const BoxSetting *setting = own_setting(path, kind);
    const char       *text    = setting ? own_value(setting)->Text : "";
    umutex_unlock(&g_lock);
    return text;
}

const char *settings_word(const char *path)
{
    return own_text(path, SETTING_WORD);
}

const char *settings_text(const char *path)
{
    return own_text(path, SETTING_TEXT);
}

const char *settings_choice(const char *path)
{
    return own_text(path, SETTING_ONE_OF);
}

uint32_t settings_color(const char *path)
{
    umutex_lock(&g_lock);
    const BoxSetting *setting = own_setting(path, SETTING_COLOR);
    uint32_t          color   = setting ? own_value(setting)->Color : 0;
    umutex_unlock(&g_lock);
    return color;
}

/* What each setting holds now, its text copied — to tell afterwards whether
 * one changed; NULL when there was no room, and then each counts as changed.
 * Under the lock. */
static char **own_held(void)
{
    char **held = calloc(g_own.Count ? g_own.Count : 1, sizeof(char *));
    for (size_t i = 0; held && i < g_own.Count; i++) {
        const char *text = own_value(&g_own.Settings[i])->Text;
        size_t      size = strlen(text) + 1;
        if ((held[i] = malloc(size)) != NULL) memcpy(held[i], text, size);
    }
    return held;
}

/* Whether a setting holds another value now than in `held`, of `count`
 * settings; `held` let go of. Under the lock. */
static bool own_changed(char **held, size_t count)
{
    bool changed = !held || count != g_own.Count;
    for (size_t i = 0; held && i < count; i++) {
        if (!held[i] || i >= g_own.Count || strcmp(held[i], own_value(&g_own.Settings[i])->Text) != 0)
            changed = true;
        free(held[i]);
    }
    free(held);
    return changed;
}

/* The person's file read again when `file`, the Use Context when `context`:
 * true when a value that holds changed. */
static bool own_refresh(bool file, bool context)
{
    umutex_lock(&g_lock);
    if (!g_read) {
        own_read_locked();
        umutex_unlock(&g_lock);
        return false;
    }
    size_t count = g_own.Count;
    char **held  = own_held();
    if (file) own_assemble();
    if (context) own_context();
    bool changed = own_changed(held, count);
    umutex_unlock(&g_lock);
    return changed;
}

KeyChord settings_key(const char *path)
{
    const KeyChord none = { 0, 0, 0, 0 };
    umutex_lock(&g_lock);
    const BoxSetting *setting = own_setting(path, SETTING_KEY);
    KeyChord          key     = setting ? own_value(setting)->Key : none;
    umutex_unlock(&g_lock);
    return key;
}

bool settings_reload(void)
{
    return own_refresh(true, true);
}

/* ---- the watch ---------------------------------------------------------- */

struct SettingsWatch {
    TouchTag Saved;     /* settings: a settings file anchored */
    TouchTag Context;   /* USE_TOUCH_TAG: the Use Context changed */
};

static int watch_claim(const char *name, TouchTag *out)
{
    TouchTag tag = touch_pair_choose(touch_intern(name));
    if (tag == TOUCH_TAG_INVALID) return -ERR_TAG_NOT_FOUND;
    int rc = touch_claim(tag, TOUCH_REST, 0, 0);
    if (rc == 0) *out = tag;
    return rc;
}

int settings_watch_begin(SettingsWatch **out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;
    *out = NULL;
    SettingsWatch *watch = malloc(sizeof(*watch));
    if (!watch) return -ERR_NO_MEMORY;
    int rc = watch_claim("settings", &watch->Saved);
    if (rc == 0 && (rc = watch_claim(USE_TOUCH_TAG, &watch->Context)) != 0) touch_release(watch->Saved);
    if (rc != 0) {
        free(watch);
        return rc;
    }
    *out = watch;
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
    bool  saved = false, context = false;
    if (!watch) return false;
    while (touch_try_pop_tag(watch->Saved, &touch)) {
        uint64_t file;
        if (touch.payload_len < 9 || touch.payload[8] != TOUCH_FILE_ANCHORED) continue;
        memcpy(&file, touch.payload, sizeof(file));
        if (own_file((file_id_t)file)) saved = true;
    }
    while (touch_try_pop_tag(watch->Context, &touch)) context = true;
    return (saved || context) && own_refresh(saved, context);
}

void settings_watch_end(SettingsWatch *watch)
{
    if (!watch) return;
    touch_release(watch->Saved);
    touch_release(watch->Context);
    free(watch);
}
