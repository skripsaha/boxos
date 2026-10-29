#ifndef UTILS_FILENAME_H
#define UTILS_FILENAME_H

#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"

/* A file's whole name for a message, or "a file that is gone" when the volume
 * no longer knows it. The caller frees it. NULL only when memory is out. */
static inline char *name_of_file(file_id_t id)
{
    FileMetadata metadata;
    const char  *name = "a file that is gone";
    bool         read = FileMetadataRead(id, &metadata) == 0;
    if (read && metadata.Filename) name = metadata.Filename;

    size_t length = strlen(name);
    char  *copy   = malloc(length + 1);
    if (copy) memcpy(copy, name, length + 1);
    if (read) FileMetadataRelease(&metadata);
    return copy;
}

/* The one file in the Use Context called exactly Name, or 0 when there is
 * none or more than one. A name is held to every byte: the stem tag finds the
 * candidates and the whole name picks among them. */
static inline file_id_t file_named(const char *name)
{
    char  *stem   = NULL;
    size_t length = strlen(name);
    size_t cut    = length;
    for (size_t i = length; i > 0; i--) {
        if (name[i - 1] == '.') { cut = i - 1; break; }
    }
    bool by_stem = cut > 0;
    for (size_t i = 0; i < cut && by_stem; i++) {
        if (name[i] == ':' || name[i] == ',') by_stem = false;
    }
    if (by_stem) {
        stem = malloc(cut + 1);
        if (!stem) return 0;
        memcpy(stem, name, cut);
        stem[cut] = '\0';
    }

    file_id_t *ids = NULL;
    int        count = query_all(stem, &ids);
    free(stem);
    if (count <= 0) return 0;

    file_id_t found   = 0;
    int       matches = 0;
    for (int i = 0; i < count; i++) {
        FileMetadata metadata;
        if (FileMetadataRead(ids[i], &metadata) != 0) continue;
        if (metadata.Filename && strcmp(metadata.Filename, name) == 0) {
            found = ids[i];
            matches++;
        }
        FileMetadataRelease(&metadata);
    }
    free(ids);
    return matches == 1 ? found : 0;
}

#endif
