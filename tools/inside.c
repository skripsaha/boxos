/*
 * tools/inside — what a program file is made of, on the build machine: the
 * same words the utility inside says in BoxOS, from the same source
 * (src/userspace/utils/inside/inside_show.c, built here with INSIDE_HOST).
 *
 *     inside FILE...
 *
 * Its exit is the worst it found: 0 every file a whole box, 1 a box something
 * of which does not hold, 2 a file that is no box, 3 one that could not be read.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box_host.h"
#include "inside_show.h"

typedef struct {
    FILE *File;
} HostFile;

static bool ReadFile(void *context, uint64_t offset, void *into, uint64_t count)
{
    FILE *file = ((HostFile *)context)->File;
    return fseeko(file, (off_t)offset, SEEK_SET) == 0 && fread(into, 1, (size_t)count, file) == count;
}

static void SayText(void *context, const char *text, uint64_t length)
{
    (void)context;
    fwrite(text, 1, (size_t)length, stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: inside FILE...\n");
        return INSIDE_UNREAD;
    }

    InsideVerdict worst = INSIDE_WHOLE;
    for (int i = 1; i < argc; i++) {
        HostFile    host = { fopen(argv[i], "rb") };
        BoxHostHash state = { 0 };
        BoxHasher   hasher = BoxHostHasher(&state);
        InsideVerdict verdict = INSIDE_UNREAD;
        if (!host.File) {
            printf("%s: could not be opened\n", argv[i]);
        } else if (fseeko(host.File, 0, SEEK_END) != 0) {
            printf("%s: could not be measured\n", argv[i]);
        } else {
            InsideFile inside = { argv[i], (uint64_t)ftello(host.File), ReadFile, SayText, &host, &hasher };
            verdict = InsideShow(&inside);
        }
        if (host.File) fclose(host.File);
        BoxHostHashFree(&state);
        if (verdict > worst) worst = verdict;
    }
    return worst;
}
