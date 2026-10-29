/*
 * program.c — find the program a name or a thing means.
 *
 * The rule is the kernel's (SysExecCandidate): a file counts as a program
 * when it is active and wears app or utility. The shell holds the files that
 * wear the name to the same rule before it asks the kernel to start one, so
 * what it lists is exactly what would start.
 */

#include "program.h"
#include "ask.h"
#include "them.h"
#include "box/file.h"
#include "box/form.h"
#include "box/print.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/error.h"

#define PROGRAM_FILE_ACTIVE  (1u << 0)

static bool WearsKey(const char *tags, const char *key)
{
    size_t key_length = strlen(key);
    while (*tags) {
        const char *comma = strchr(tags, ',');
        size_t      length = comma ? (size_t)(comma - tags) : strlen(tags);
        const char *colon = memchr(tags, ':', length);
        size_t      spelled = colon ? (size_t)(colon - tags) : length;
        if (spelled == key_length && memcmp(tags, key, key_length) == 0) return true;
        if (!comma) break;
        tags = comma + 1;
    }
    return false;
}

static bool IsProgram(const FileMetadata *metadata)
{
    return (metadata->Flags & PROGRAM_FILE_ACTIVE) && metadata->Tags &&
           (WearsKey(metadata->Tags, "app") || WearsKey(metadata->Tags, "utility"));
}

bool ProgramIsRunnable(file_id_t id)
{
    FileMetadata metadata;
    if (FileMetadataRead(id, &metadata) != 0) return false;
    bool runnable = IsProgram(&metadata);
    FileMetadataRelease(&metadata);
    return runnable;
}

static char *CopyName(const FileMetadata *metadata)
{
    if (!metadata->Filename) return NULL;
    size_t length = strlen(metadata->Filename);
    char  *name = malloc(length + 1);
    if (name) memcpy(name, metadata->Filename, length + 1);
    return name;
}

char *ProgramNameOf(file_id_t id)
{
    FileMetadata metadata;
    if (FileMetadataRead(id, &metadata) != 0) return NULL;
    char *name = CopyName(&metadata);
    FileMetadataRelease(&metadata);
    return name;
}

char *ProgramRunnableName(file_id_t id)
{
    FileMetadata metadata;
    if (FileMetadataRead(id, &metadata) != 0) return NULL;
    char *name = IsProgram(&metadata) ? CopyName(&metadata) : NULL;
    FileMetadataRelease(&metadata);
    return name;
}

/* Of the files in `ids`, those that are programs, kept in place; *unread the
 * first refusal met reading one, 0 when each was read. A name whose files the
 * volume will not show — its medium gone — is there and would not start, not
 * a name nobody has. */
static size_t KeepPrograms(file_id_t *ids, size_t count, int *unread)
{
    size_t kept = 0;
    *unread = 0;
    for (size_t i = 0; i < count; i++) {
        FileMetadata metadata;
        int rc = FileMetadataRead(ids[i], &metadata);
        if (rc != 0) {
            if (*unread == 0) *unread = rc;
            continue;
        }
        if (IsProgram(&metadata)) ids[kept++] = ids[i];
        FileMetadataRelease(&metadata);
    }
    return kept;
}

/* One of `ids`, asked for when there are several. */
static int ChooseOne(const char *word, const file_id_t *ids, size_t count, file_id_t *program)
{
    if (count == 1) {
        *program = ids[0];
        return PROGRAM_PICKED;
    }
    AskResult asked = AskWhich(word, ids, count, program);
    if (asked == ASK_ANSWERED) return PROGRAM_PICKED;
    AskSayCancelled(word, asked);
    return PROGRAM_SAID;
}

int ProgramPick(const char *name, file_id_t *program)
{
    file_id_t *found = NULL;
    int        total = query_all_everywhere(name, &found);
    if (total < 0) return total;

    int    unread;
    size_t count = KeepPrograms(found, (size_t)total, &unread);
    int    rc = count != 0 ? ChooseOne(name, found, count, program)
                           : unread != 0 ? unread : -ERR_FILE_NOT_FOUND;
    free(found);
    return rc;
}

/* The files called `thing`: here, or else anywhere on the volume. */
static int FindThing(const char *thing, file_id_t **ids)
{
    BoxFormLookup lookup;
    form_lookup_volume(&lookup);

    int count = lookup.Find(&lookup, thing, false, ids);
    if (count != 0) return count;
    free(*ids);
    return lookup.Find(&lookup, thing, true, ids);
}

static int PickFromThem(const char *word, file_id_t *program)
{
    bool             known;
    size_t           count;
    const file_id_t *given = ThemFiles(&count, &known);
    if (count == 0) {
        printf("run: %s is nothing yet: the last command gave no files\n", word);
        return PROGRAM_SAID;
    }
    return ChooseOne(word, given, count, program);
}

static int PickFromName(const char *thing, file_id_t *program)
{
    file_id_t *found = NULL;
    int        count = FindThing(thing, &found);
    if (count < 0) {
        printf("run: the volume would not answer for %s (error %d)\n", thing, -count);
        return PROGRAM_SAID;
    }
    if (count == 0) {
        printf("run: there is no file called %s\n", thing);
        free(found);
        return PROGRAM_SAID;
    }
    int rc = ChooseOne(thing, found, (size_t)count, program);
    free(found);
    return rc;
}

int ProgramPickThing(const char *thing, file_id_t *program)
{
    bool themselves = strcmp(thing, "it") == 0 || strcmp(thing, "them") == 0;
    int  rc = themselves ? PickFromThem(thing, program) : PickFromName(thing, program);
    if (rc != PROGRAM_PICKED) return rc;

    if (!ProgramIsRunnable(*program)) {
        printf("run: %s is not a program: it wears neither app nor utility\n", thing);
        return PROGRAM_SAID;
    }
    return PROGRAM_PICKED;
}
