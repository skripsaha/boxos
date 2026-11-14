#include "program_keys.h"
#include "tagfs.h"
#include "tag_spelling.h"
#include "klib.h"

static const char KeyApp[]    = "app";
static const char KeySystem[] = "system";

/* How much of the name is the stem: up to its last dot, as the volume stamps
 * a file's name (edit.box is called edit). */
static size_t StemLength(const char *filename)
{
    size_t length = strlen(filename);
    for (size_t at = length; at > 0; at--)
        if (filename[at - 1] == '.')
            return at - 1;
    return length;
}

/* Whether the stem can be a key of the process: spelled as a key, and none of
 * the words the system keeps (system, utility, god, ...) — a file called
 * `system.box` is not the system. */
static bool StemIsKey(const char *stem, size_t length)
{
    TagSpelling spelled;
    return length > 0 && TagSpellKey(stem, length, &spelled) == OK && !spelled.value &&
           !tagfs_key_is_reserved(stem, length);
}

char *ProgramKeys(const char *filename, uint32_t flags)
{
    size_t stem_length = StemLength(filename);
    bool   named       = StemIsKey(filename, stem_length);
    bool   system_role = (flags & TAGFS_FILE_ROLE_MASK) == TAGFS_FILE_ROLE_SYSTEM;

    char *list = kmalloc(sizeof(KeyApp) + (named ? stem_length + 1 : 0) +
                         (system_role ? sizeof(KeySystem) : 0));
    if (!list)
        return NULL;

    size_t at = 0;
    memcpy(list, KeyApp, sizeof(KeyApp) - 1);
    at += sizeof(KeyApp) - 1;
    if (named) {
        list[at++] = ',';
        memcpy(list + at, filename, stem_length);
        at += stem_length;
    }
    if (system_role) {
        list[at++] = ',';
        memcpy(list + at, KeySystem, sizeof(KeySystem) - 1);
        at += sizeof(KeySystem) - 1;
    }
    list[at] = '\0';
    return list;
}
