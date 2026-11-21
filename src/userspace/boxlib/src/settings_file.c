/*
 * settings_file.c — the person's settings file on the volume (box/settings.h):
 * found by the program's name everywhere, read whole, and changed by one
 * setting at a time with the pen (settings_pen.h) — the file written whole
 * and anchored, which tells every program watching it to read it again.
 */

#include "box/core/settings_internal.h"
#include "box/error.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/program_part.h"
#include "box/string.h"

#include "settings_pen.h"

#define SETTINGS_NAME_MAX 22u   /* "<program>.settings" in a file name of fewer than 32 bytes */

int settings_program_name(file_id_t program, char out[32])
{
    FileMetadata metadata;
    int          rc = FileMetadataRead(program, &metadata);
    if (rc != 0) return rc;
    const char *name = metadata.Filename ? metadata.Filename : "";
    const char *dot  = strrchr(name, '.');
    size_t      length = dot ? (size_t)(dot - name) : strlen(name);
    if (length == 0 || length >= 32) {
        FileMetadataRelease(&metadata);
        return -ERR_INVALID_ARGUMENT;
    }
    memcpy(out, name, length);
    out[length] = '\0';
    FileMetadataRelease(&metadata);
    return 0;
}

/* Whether the file is called `name`. */
static bool file_called(file_id_t file, const char *name)
{
    FileMetadata metadata;
    if (FileMetadataRead(file, &metadata) != 0) return false;
    bool called = metadata.Filename && strcmp(metadata.Filename, name) == 0;
    FileMetadataRelease(&metadata);
    return called;
}

int settings_file_find(const char *program, file_id_t *file, char **saying)
{
    char       name[32];
    file_id_t *ids = NULL;
    file_id_t  first = 0;
    int        called = 0;

    *file = 0;
    if (saying) *saying = NULL;
    /* A name too long for a file name has no settings file. */
    if (strlen(program) > SETTINGS_NAME_MAX) return 0;
    snprintf(name, sizeof(name), "%s.settings", program);
    /* Asked of the name index by the whole name. A settings file in the trash
     * says nothing: the person set it aside. */
    int count = query_name_all_everywhere(name, NULL, NULL, &ids);
    if (count < 0) return count;
    for (int i = 0; i < count; i++) {
        if (!file_called(ids[i], name)) continue;
        if (called++ == 0 || ids[i] < first) first = ids[i];
    }
    free(ids);
    if (called == 0) return 0;
    *file = first;
    if (called > 1 && saying) {
        char text[SETTINGS_SAYING_MAX];
        snprintf(text, sizeof(text), "%d files are called %s; #%llu, the first made, is read", called, name,
                 (unsigned long long)*file);
        *saying = malloc(strlen(text) + 1);
        if (*saying) memcpy(*saying, text, strlen(text) + 1);
    }
    return 0;
}

int settings_file_read(file_id_t file, char **bytes, size_t *size)
{
    FileMetadata metadata;
    int          rc = FileMetadataRead(file, &metadata);
    *bytes = NULL;
    *size  = 0;
    if (rc != 0) return rc;
    uint64_t length = metadata.Size;
    FileMetadataRelease(&metadata);
    if (length > SETTINGS_FILE_MAX) return -ERR_BINARY_TOO_LARGE;

    char    *read = malloc(length ? (size_t)length : 1);
    uint64_t at   = 0;
    if (!read) return -ERR_NO_MEMORY;
    while (at < length) {
        int64_t got = fread(file, at, read + at, (size_t)(length - at));
        if (got <= 0) {
            free(read);
            return got < 0 ? (int)got : -ERR_IO;
        }
        at += (uint64_t)got;
    }
    *bytes = read;
    *size  = (size_t)length;
    return 0;
}

/* The file's whole contents replaced, and anchored: saved whole. */
static int file_write(file_id_t file, const uint8_t *bytes, uint64_t size)
{
    uint64_t at = 0;
    while (at < size) {
        int64_t put = fwrite(file, at, bytes + at, (size_t)(size - at));
        if (put <= 0) return put < 0 ? (int)put : -ERR_IO;
        at += (uint64_t)put;
    }
    int rc = file_span(file, size, 0);
    return rc != 0 ? rc : anchor(file);
}

/* The person's file read as it is on the volume now, into settings->Chosen;
 * settings->File the file, 0 for none. */
static int chosen_refresh(BoxSettings *settings)
{
    char  *bytes = NULL;
    size_t size  = 0;
    int    rc    = settings_file_find(settings->Program, &settings->File, NULL);
    if (rc == 0 && settings->File != 0) rc = settings_file_read(settings->File, &bytes, &size);
    if (rc != 0) return rc;
    free(settings->Chosen);
    settings->Chosen     = bytes;
    settings->ChosenSize = size;
    return 0;
}

typedef enum { CHANGE_SET, CHANGE_ERASE, CHANGE_CLEAR } SettingsChange;

/* A change: what it does, to the setting on declared line Node, in the block
 * of Tags (empty: everywhere). */
typedef struct {
    SettingsChange What;
    SettingSpan    Tags;
    uint32_t       Node;
    SettingSpan    Value;
} SettingsChangeAsked;

/* `file` changed by the pen into a buffer of its own, *out — the caller's to
 * free. */
static int pen(const BoxSettings *settings, const SettingsChangeAsked *change, const uint8_t *file, uint64_t size,
               uint8_t **out, uint64_t *out_size)
{
    const uint8_t *declared = (const uint8_t *)settings->Declared;
    for (int pass = 0; pass < 2; pass++) {
        uint8_t *into = pass ? *out : NULL;
        if (change->What == CHANGE_SET)
            SettingsPenSet(declared, settings->DeclaredSize, file, size, change->Tags, change->Node, change->Value,
                           into, out_size);
        else if (change->What == CHANGE_ERASE)
            SettingsPenErase(declared, settings->DeclaredSize, file, size, change->Tags, change->Node, into,
                             out_size);
        else
            SettingsPenClear(declared, settings->DeclaredSize, file, size, into, out_size);
        if (!pass && !(*out = malloc(*out_size ? (size_t)*out_size : 1))) return -ERR_NO_MEMORY;
    }
    return 0;
}

/* The change made to the person's file as it is: a thing that only holds
 * others erased is every setting inside it erased, the file written once. */
static int changed(const BoxSettings *settings, const SettingsChangeAsked *change, uint8_t **out,
                   uint64_t *out_size)
{
    size_t index = 0;
    while (index < settings->Count && settings->Settings[index].Line != change->Node) index++;
    if (change->What != CHANGE_ERASE || index == settings->Count ||
        settings->Settings[index].Kind != SETTING_GROUP)
        return pen(settings, change, (const uint8_t *)settings->Chosen, settings->ChosenSize, out, out_size);

    const char *group  = settings->Settings[index].Path;
    size_t      length = strlen(group);
    uint8_t    *bytes  = malloc(settings->ChosenSize ? settings->ChosenSize : 1);
    uint64_t    size   = settings->ChosenSize;
    if (!bytes) return -ERR_NO_MEMORY;
    memcpy(bytes, settings->Chosen, settings->ChosenSize);
    for (size_t i = 0; i < settings->Count; i++) {
        const BoxSetting *inside = &settings->Settings[i];
        if (inside->Kind == SETTING_GROUP || strncmp(inside->Path, group, length) != 0 || inside->Path[length] != ' ')
            continue;
        SettingsChangeAsked one = *change;
        uint8_t            *next;
        one.Node = inside->Line;
        int rc   = pen(settings, &one, bytes, size, &next, &size);
        free(bytes);
        if (rc != 0) return rc;
        bytes = next;
    }
    *out      = bytes;
    *out_size = size;
    return 0;
}

/* The person's file as it is on the volume now, changed, written whole —
 * made first when there is none and the change sets something — and the
 * settings put together again from it. */
static int change_file(BoxSettings *settings, const SettingsChangeAsked *change)
{
    char     refusal[SETTINGS_SAYING_MAX];
    uint8_t *bytes;
    uint64_t size;
    int      rc;
    if (strlen(settings->Program) > SETTINGS_NAME_MAX) {
        snprintf(refusal, sizeof(refusal), "%s has a name too long for a settings file: %u letters at most",
                 settings->Program, SETTINGS_NAME_MAX);
        return settings_refuse(settings, refusal);
    }
    if ((rc = chosen_refresh(settings)) != 0) return rc;
    if (settings->File == 0 && change->What != CHANGE_SET) return settings_reassemble(settings);
    if ((rc = changed(settings, change, &bytes, &size)) != 0) return rc;
    if (settings->File == 0) {
        char    file_name[32];
        snprintf(file_name, sizeof(file_name), "%s.settings", settings->Program);
        int64_t made = create_everywhere(file_name, NULL);
        if (made < 0) {
            free(bytes);
            return (int)made;
        }
        settings->File = (file_id_t)made;
    }
    rc = file_write(settings->File, bytes, size);
    if (rc != 0) {
        free(bytes);
        return rc;
    }
    free(settings->Chosen);
    settings->Chosen     = (char *)bytes;
    settings->ChosenSize = (size_t)size;
    return settings_reassemble(settings);
}

/* The context a change is for, read: its tags as a [ ] line would hold
 * them, or none for NULL or blanks — everywhere. */
static int change_tags(BoxSettings *settings, const char *tags, SettingSpan *out)
{
    char        line[SETTINGS_SAYING_MAX];
    SettingSpan typed = SettingTrimmed(SettingSpanOf((const uint8_t *)(tags ? tags : ""), tags ? strlen(tags) : 0));
    SettingSpan read;
    *out = typed;
    if (typed.Length == 0) return 0;
    snprintf(line, sizeof(line), "[%.*s]", (int)typed.Length, (const char *)typed.At);
    if (typed.Length < sizeof(line) - 2 && SettingContextRead(SettingSpanOf((const uint8_t *)line, strlen(line)), 0,
                                                              &read))
        return 0;
    snprintf(line, sizeof(line), "[%.*s] is no context: tags apart by blanks or commas, none twice, no bracket or "
             "quote in one", (int)typed.Length, (const char *)typed.At);
    return settings_refuse(settings, line);
}

/* The person's file of out->Program found and read, and the settings put
 * together with out->Declared: 0, or the settings released and an error. */
static int settings_take(BoxSettings *out)
{
    char *saying = NULL;
    int   rc;
    if ((rc = settings_file_find(out->Program, &out->File, &saying)) == 0 && out->File != 0)
        rc = settings_file_read(out->File, &out->Chosen, &out->ChosenSize);
    if (rc == 0 && saying) rc = settings_say(out, saying);
    free(saying);
    if (rc == 0) rc = settings_assemble(out);
    if (rc != 0) settings_release(out);
    return rc;
}

int settings_read(file_id_t program, BoxSettings *out)
{
    char  *text = NULL;
    size_t size = 0;
    int    rc;

    if (!out) return -ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if ((rc = settings_program_name(program, out->Program)) != 0) return rc;
    rc = program_part_read(program, "settings", SETTINGS_FILE_MAX, &text, &size, NULL);
    if (rc < 0) return rc;
    /* An ELF image keeps the declaration as a C string: its zeros are not the text's. */
    while (size > 0 && text[size - 1] == '\0') size--;
    out->Declared     = text;
    out->DeclaredSize = size;
    return settings_take(out);
}

int settings_read_declared(const char *program, const char *declared, size_t size, BoxSettings *out)
{
    if (!out || !program || !declared || strlen(program) >= sizeof(out->Program)) return -ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    memcpy(out->Program, program, strlen(program) + 1);
    if (!(out->Declared = malloc(size ? size : 1))) return -ERR_NO_MEMORY;
    memcpy(out->Declared, declared, size);
    out->DeclaredSize = size;
    return settings_take(out);
}

int settings_set(BoxSettings *settings, const char *tags, const char *path, const char *value, const char **why)
{
    if (!settings || !path || !value) return -ERR_INVALID_ARGUMENT;
    SettingsChangeAsked change = { CHANGE_SET, SettingSpanOf(NULL, 0), 0,
                                   SettingSpanOf((const uint8_t *)value, strlen(value)) };
    int rc = change_tags(settings, tags, &change.Tags);
    if (rc == 0) rc = settings_check(settings, path, value);
    if (rc == 0) {
        change.Node = settings_find(settings, path)->Line;
        rc          = change_file(settings, &change);
    }
    if (why) *why = rc == -ERR_INVALID_ARGUMENT ? settings->Why : NULL;
    return rc;
}

int settings_erase(BoxSettings *settings, const char *tags, const char *path, const char **why)
{
    if (!settings || !path) return -ERR_INVALID_ARGUMENT;
    SettingsChangeAsked change = { CHANGE_ERASE, SettingSpanOf(NULL, 0), 0, SettingSpanOf(NULL, 0) };
    const BoxSetting   *setting = settings_find(settings, path);
    int                 rc      = change_tags(settings, tags, &change.Tags);
    if (rc == 0 && !setting) rc = settings_check(settings, path, "");
    if (rc == 0) {
        change.Node = setting->Line;
        rc          = change_file(settings, &change);
    }
    if (why) *why = rc == -ERR_INVALID_ARGUMENT ? settings->Why : NULL;
    return rc;
}

int settings_clear(BoxSettings *settings)
{
    if (!settings) return -ERR_INVALID_ARGUMENT;
    SettingsChangeAsked change = { CHANGE_CLEAR, SettingSpanOf(NULL, 0), 0, SettingSpanOf(NULL, 0) };
    return change_file(settings, &change);
}
