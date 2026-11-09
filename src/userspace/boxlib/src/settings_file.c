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

/* Whether the file is called `name`, and wears the tag settings. */
static bool file_called(file_id_t file, const char *name, bool *tagged)
{
    FileMetadata metadata;
    if (FileMetadataRead(file, &metadata) != 0) return false;
    bool called = metadata.Filename && strcmp(metadata.Filename, name) == 0;
    *tagged = called && FileMetadataWears(&metadata, "settings");
    FileMetadataRelease(&metadata);
    return called;
}

int settings_file_find(const char *program, file_id_t *file, char **saying)
{
    char       name[32];
    file_id_t *ids = NULL;
    file_id_t  first = 0, tagged_one = 0;
    int        called = 0, tagged_count = 0;

    *file = 0;
    if (saying) *saying = NULL;
    /* A name too long for a file name, or one that cannot be asked as a tag,
     * has no settings file. */
    if (strlen(program) > SETTINGS_NAME_MAX || strchr(program, ':') || strchr(program, ',')) return 0;
    snprintf(name, sizeof(name), "%s.settings", program);
    /* Every file wears the stem of its name as a tag: clock.settings wears
     * clock. A settings file in the trash says nothing: the person set it
     * aside. */
    int count = query_all_everywhere(program, "trashed", &ids);
    if (count < 0) return count;
    for (int i = 0; i < count; i++) {
        bool tagged;
        if (!file_called(ids[i], name, &tagged)) continue;
        if (called++ == 0 || ids[i] < first) first = ids[i];
        if (tagged && (tagged_count++ == 0 || ids[i] < tagged_one)) tagged_one = ids[i];
    }
    free(ids);
    if (called == 0) return 0;
    *file = tagged_count == 1 ? tagged_one : first;
    if (called > 1 && saying) {
        char text[SETTINGS_SAYING_MAX];
        snprintf(text, sizeof(text), "%d files are called %s; #%llu%s is read", called, name,
                 (unsigned long long)*file, tagged_count == 1 ? ", tagged settings," : "");
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

static void pen(const BoxSettings *settings, SettingsChange change, const char *name, const char *value,
                uint8_t *out, uint64_t *bytes)
{
    const uint8_t *file  = (const uint8_t *)settings->Chosen;
    uint64_t       size  = settings->ChosenSize;
    SettingSpan    named = { (const uint8_t *)name, name ? strlen(name) : 0 };
    if (change == CHANGE_SET)
        SettingsPenSet(file, size, named, (SettingSpan){ (const uint8_t *)value, strlen(value) }, out, bytes);
    else if (change == CHANGE_ERASE)
        SettingsPenErase(file, size, named, out, bytes);
    else
        SettingsPenClear(file, size, (const uint8_t *)settings->Declared, settings->DeclaredSize, out, bytes);
}

/* The person's file as it is on the volume now, changed by the pen, written
 * whole — made first when there is none and the change sets something — and
 * the settings put together again from it. */
static int change_file(BoxSettings *settings, SettingsChange change, const char *name, const char *value)
{
    char     refusal[SETTINGS_SAYING_MAX];
    uint64_t size;
    int      rc;

    if (strlen(settings->Program) > SETTINGS_NAME_MAX) {
        snprintf(refusal, sizeof(refusal), "%s has a name too long for a settings file: %u letters at most",
                 settings->Program, SETTINGS_NAME_MAX);
        return settings_refuse(settings, refusal);
    }
    if ((rc = chosen_refresh(settings)) != 0) return rc;
    if (settings->File == 0 && change != CHANGE_SET) return settings_reassemble(settings);

    pen(settings, change, name, value, NULL, &size);
    uint8_t *bytes = malloc(size ? (size_t)size : 1);
    if (!bytes) return -ERR_NO_MEMORY;
    pen(settings, change, name, value, bytes, &size);

    if (settings->File == 0) {
        char    file_name[32];
        snprintf(file_name, sizeof(file_name), "%s.settings", settings->Program);
        int64_t made = create_everywhere(file_name, "settings");
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

int settings_read(file_id_t program, BoxSettings *out)
{
    char  *text = NULL;
    size_t size = 0;
    char  *saying = NULL;
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

    if ((rc = settings_file_find(out->Program, &out->File, &saying)) == 0 && out->File != 0)
        rc = settings_file_read(out->File, &out->Chosen, &out->ChosenSize);
    if (rc == 0 && saying) rc = settings_say(out, saying);
    free(saying);
    if (rc == 0) rc = settings_assemble(out);
    if (rc != 0) settings_release(out);
    return rc;
}

int settings_set(BoxSettings *settings, const char *name, const char *value, const char **why)
{
    if (!settings || !name || !value) return -ERR_INVALID_ARGUMENT;
    int rc = settings_check(settings, name, value);
    if (rc == 0) rc = change_file(settings, CHANGE_SET, name, value);
    if (why) *why = rc == -ERR_INVALID_ARGUMENT ? settings->Why : NULL;
    return rc;
}

int settings_erase(BoxSettings *settings, const char *name, const char **why)
{
    if (!settings || !name) return -ERR_INVALID_ARGUMENT;
    const BoxSetting *setting = settings_find(settings, name);
    int rc = setting ? change_file(settings, CHANGE_ERASE, name, NULL)
                     : settings_check(settings, name, "");
    if (why) *why = rc == -ERR_INVALID_ARGUMENT ? settings->Why : NULL;
    return rc;
}

int settings_clear(BoxSettings *settings)
{
    if (!settings) return -ERR_INVALID_ARGUMENT;
    return change_file(settings, CHANGE_CLEAR, NULL, NULL);
}
