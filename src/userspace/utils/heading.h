#ifndef UTILS_HEADING_H
#define UTILS_HEADING_H

#include "box/print.h"
#include "box/color.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"

/* The line over a file a utility looks into: its name, its size and its tags,
 * the tags written as `files` lists them. */
static inline void print_heading(const FileMetadata *metadata)
{
    const char *name = metadata->Filename ? metadata->Filename : "a file with no name";
    const char *tags = metadata->Tags ? metadata->Tags : "";
    size_t      length = strlen(tags);
    char       *spaced = malloc(2 * length + 1);

    if (spaced) {
        size_t used = 0;
        for (size_t i = 0; i < length; i++) {
            spaced[used++] = tags[i];
            if (tags[i] == ',' && tags[i + 1] != ' ') spaced[used++] = ' ';
        }
        spaced[used] = '\0';
    }

    printf("%color%s%color  %llu byte%s  [%s]\n", COLOR_CYAN, name, COLOR_DEFAULT,
           (unsigned long long)metadata->Size, metadata->Size == 1 ? "" : "s",
           spaced ? spaced : tags);
    free(spaced);
}

/* How many hex digits the offsets of a file of `size` bytes are written with:
 * eight, or as many as its last offset takes. */
static inline unsigned offset_digits(uint64_t size)
{
    unsigned digits = 8;
    while (digits < 16 && size > 0 && ((size - 1) >> (4 * digits)) != 0) digits++;
    return digits;
}

#endif
