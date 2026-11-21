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
 * none or more than one. A name is held to every byte: the name index finds
 * the candidates and the whole name picks among them. */
static inline file_id_t file_named(const char *name)
{
    file_id_t found[2];
    return find_file_by_name(name, NULL, found, NULL, 2) == 1 ? found[0] : 0;
}

#endif
