/*
 * inside — what a program on the volume is made of: a box's parts, what each
 * is and says, its flags, its seal and every hash held (inside/inside_show.h,
 * the one source tools/inside is built from too).
 *
 * Its exit says what it found: 0 a whole box, 1 a box something of which does
 * not hold, 2 no box, 3 a file that could not be read.
 */

#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/sha256.h"
#include "box/system.h"

#include "inside/inside_show.h"

BOX_FORM("takes thing \"the program to look inside\"\n");

static bool ReadFile(void *context, uint64_t offset, void *into, uint64_t count)
{
    file_id_t file = *(const file_id_t *)context;
    uint8_t  *to   = (uint8_t *)into;
    while (count > 0) {
        int64_t got = fread(file, offset, to, (size_t)count);
        if (got <= 0) return false;
        offset += (uint64_t)got;
        to     += got;
        count  -= (uint64_t)got;
    }
    return true;
}

static void SayText(void *context, const char *text, uint64_t length)
{
    (void)context;
    printf("%.*s", (int)length, text);
}

static void HashBegin(void *state)
{
    sha256_begin((Sha256 *)state);
}

static void HashAdd(void *state, const uint8_t *bytes, uint64_t count)
{
    sha256_add((Sha256 *)state, bytes, (size_t)count);
}

static void HashEnd(void *state, uint8_t digest[BOX_HASH_BYTES])
{
    sha256_end((Sha256 *)state, digest);
}

int main(void)
{
    const file_id_t *files;
    size_t           count;
    if (form_things(0, &files, &count) != 0 || count != 1) {
        exit(INSIDE_UNREAD);
        return INSIDE_UNREAD;
    }

    FileMetadata metadata;
    if (FileMetadataRead(MY_TAGS, files[0], &metadata) != 0) {
        println("inside: that file could not be looked at; it may have been erased");
        exit(INSIDE_UNREAD);
        return INSIDE_UNREAD;
    }

    file_id_t  file  = files[0];
    Sha256     state;
    BoxHasher  hasher = { &state, HashBegin, HashAdd, HashEnd };
    InsideFile inside = { metadata.Filename, metadata.Size, ReadFile, SayText, &file, &hasher };
    InsideVerdict verdict = InsideShow(&inside);
    FileMetadataRelease(&metadata);
    exit((int)verdict);
    return (int)verdict;
}
