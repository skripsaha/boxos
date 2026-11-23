/*
 * fat — a FAT32 laid and written on the host by the one code the kernel and
 * Gangway use (src/lib/fat/fat.c): the ESP of every image the build makes,
 * and the gates' hand on one.
 *
 *   fat IMG lay SIZE|least [--label L] [--id N]   a FAT32 on all of SIZE (512M,
 *                                                 2G) — or the smallest there is
 *                                                 in its sector — the file made
 *                                                 or cut to it
 *   fat IMG put PATH FILE                         FILE's bytes as PATH
 *   fat IMG zeros PATH BYTES                      BYTES of zeros as PATH
 *   fat IMG mkdir PATH                            every directory of PATH
 *   fat IMG get PATH OUT                          PATH's bytes into OUT
 *   fat IMG run PATH                              "SECTOR COUNT": the one run of
 *                                                 PATH, counted from the image
 *   fat IMG rm PATH                               PATH taken away
 *   fat IMG open                                  the FAT32 read, and its layout
 *                                                 said: nothing written
 *
 * Options before the command: --sector N, the FAT's sector (512, or 4096 for a
 * 4Kn medium); --at N, the sector of IMG the FAT begins at (a partition of a
 * disk image), in the FAT's sectors; --hidden N, the BPB's HiddenSectors for
 * `lay`; --time N, the seconds since 1970 what is written bears
 * (SOURCE_DATE_EPOCH when set, else now).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "fat.h"

typedef struct {
    FILE*    file;
    uint64_t at;          /* the FAT's first sector in the image */
    uint32_t sector;
} Image;

static int ImageRead(void* ctx, uint64_t sector, uint32_t count, void* into)
{
    Image* im = ctx;
    if (fseeko(im->file, (off_t)((im->at + sector) * im->sector), SEEK_SET) != 0)
        return -1;
    size_t want = (size_t)count * im->sector, got = fread(into, 1, want, im->file);
    if (got < want)
        memset((uint8_t*)into + got, 0, want - got);   /* past the file's end reads as zeros */
    return ferror(im->file) ? -1 : 0;
}

static int ImageWrite(void* ctx, uint64_t sector, uint32_t count, const void* from)
{
    Image* im = ctx;
    if (fseeko(im->file, (off_t)((im->at + sector) * im->sector), SEEK_SET) != 0)
        return -1;
    return fwrite(from, im->sector, count, im->file) == count ? 0 : -1;
}

static int ImageFlush(void* ctx)
{
    return fflush(((Image*)ctx)->file) == 0 ? 0 : -1;
}

static const char* Said(int rc)
{
    switch (rc) {
    case FAT_IO:            return "the image did not read or take a sector";
    case FAT_NOT_FAT32:     return "there is no FAT32 there";
    case FAT_NOT_FOUND:     return "there is no such entry";
    case FAT_NO_ROOM:       return "there are not that many free clusters in one run";
    case FAT_BAD_NAME:      return "that is not a name a FAT holds";
    case FAT_TOO_SMALL:     return "too few sectors for a FAT32 of this sector";
    case FAT_NOT_DIRECTORY: return "a part of the path is a file, or a directory is not empty";
    case FAT_DAMAGED:       return "a cluster chain is broken";
    default:                return "it failed";
    }
}

/* A size: digits, and K, M, G or T. 0 for none. */
static uint64_t SizeOf(const char* s)
{
    char*    end;
    uint64_t n = strtoull(s, &end, 10);
    switch (*end) {
    case 'K': return n << 10;
    case 'M': return n << 20;
    case 'G': return n << 30;
    case 'T': return n << 40;
    case 0:   return n;
    default:  return 0;
    }
}

/* The smallest FAT32 of `sector`: 70000 data clusters of one sector, a margin
 * past the 65525 that make one, its two FATs and its reserved sectors. */
static uint64_t LeastSectors(uint32_t sector)
{
    uint64_t clusters = 70000u, fat = ((clusters + 2u) * 4u + sector - 1u) / sector;
    return 32u + 2u * fat + clusters;
}

static int Usage(void)
{
    fprintf(stderr, "usage: fat [--sector N] [--at N] [--hidden N] [--time N] IMG "
                    "lay SIZE|least [--label L] [--id N] | put PATH FILE | zeros PATH BYTES | mkdir PATH | "
                    "get PATH OUT | run PATH | rm PATH | open\n");
    return 2;
}

static int Lay(Image* im, FatMedium* m, int argc, char** argv, uint64_t hidden)
{
    if (argc < 1)
        return Usage();
    uint64_t sectors = strcmp(argv[0], "least") == 0 ? LeastSectors(im->sector) : SizeOf(argv[0]) / im->sector;
    const char* label = NULL;
    uint32_t    id    = (uint32_t)time(NULL);
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--label") == 0)
            label = argv[i + 1];
        else if (strcmp(argv[i], "--id") == 0)
            id = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        else
            return Usage();
    }
    if (!sectors)
        return Usage();
    if (ftruncate(fileno(im->file), (off_t)((im->at + sectors) * im->sector)) != 0) {
        fprintf(stderr, "fat: the image could not be made %llu sectors long\n", (unsigned long long)sectors);
        return 1;
    }
    m->sectors = sectors;
    FatVolume* work = calloc(1, sizeof(FatVolume));
    int        rc   = work ? FatLay(m, work, hidden, label, id) : FAT_IO;
    free(work);
    if (rc != FAT_OK) {
        fprintf(stderr, "fat: no FAT32 was laid: %s\n", Said(rc));
        return 1;
    }
    printf("fat: a FAT32 of %llu sector(s) of %u bytes laid\n", (unsigned long long)sectors, im->sector);
    return 0;
}

static int Command(FatVolume* v, const char* cmd, int argc, char** argv, uint64_t at)
{
    int rc = FAT_OK;
    if (strcmp(cmd, "open") == 0 && argc == 0) {
        printf("fat: a FAT32 of %u-byte sectors, %u cluster(s) of %u bytes from sector %llu, %lld free\n",
               v->sector_bytes, v->clusters, v->cluster_bytes, (unsigned long long)(at + v->data_start),
               (long long)v->free_count);
        return 0;
    }
    if (strcmp(cmd, "put") == 0 && argc == 2) {
        FILE* f = fopen(argv[1], "rb");
        if (!f) {
            fprintf(stderr, "fat: %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
        fseeko(f, 0, SEEK_END);
        long long size = ftello(f);
        fseeko(f, 0, SEEK_SET);
        uint8_t* data = malloc(size > 0 ? (size_t)size : 1u);
        if (!data || size < 0 || size > 0xFFFFFFFFll || fread(data, 1, (size_t)size, f) != (size_t)size) {
            fprintf(stderr, "fat: %s could not be read whole\n", argv[1]);
            fclose(f);
            free(data);
            return 1;
        }
        fclose(f);
        rc = FatPut(v, argv[0], data, (uint32_t)size, NULL);
        free(data);
    } else if (strcmp(cmd, "zeros") == 0 && argc == 2) {
        rc = FatPut(v, argv[0], NULL, (uint32_t)SizeOf(argv[1]), NULL);
    } else if (strcmp(cmd, "mkdir") == 0 && argc == 1) {
        rc = FatMakeDirs(v, argv[0], NULL);
    } else if (strcmp(cmd, "rm") == 0 && argc == 1) {
        rc = FatRemove(v, argv[0]);
    } else if ((strcmp(cmd, "get") == 0 && argc == 2) || (strcmp(cmd, "run") == 0 && argc == 1)) {
        FatEntry e;
        rc = FatFind(v, argv[0], &e);
        if (rc == FAT_OK && cmd[0] == 'r') {
            uint64_t first, count;
            rc = FatRun(v, &e, &first, &count);
            if (rc == FAT_OK)
                printf("%llu %llu\n", (unsigned long long)(at + first), (unsigned long long)count);
        } else if (rc == FAT_OK) {
            uint8_t* data = malloc(e.size ? e.size : 1u);
            int64_t  got  = data ? FatRead(v, &e, 0, data, e.size) : FAT_IO;
            FILE*    out  = got >= 0 ? fopen(argv[1], "wb") : NULL;
            rc = got < 0 ? (int)got : !out || fwrite(data, 1, (size_t)got, out) != (size_t)got ? FAT_IO : FAT_OK;
            if (out)
                fclose(out);
            free(data);
        }
    } else {
        return Usage();
    }
    if (rc != FAT_OK) {
        fprintf(stderr, "fat: %s %s: %s\n", cmd, argc ? argv[0] : "", Said(rc));
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    Image    im     = { NULL, 0, 512 };
    uint64_t hidden = 0, stamp = getenv("SOURCE_DATE_EPOCH") ? strtoull(getenv("SOURCE_DATE_EPOCH"), NULL, 10)
                                                             : (uint64_t)time(NULL);
    int      i      = 1;
    for (; i + 1 < argc && strncmp(argv[i], "--", 2) == 0; i += 2) {
        uint64_t n = strtoull(argv[i + 1], NULL, 0);
        if (strcmp(argv[i], "--sector") == 0)
            im.sector = (uint32_t)n;
        else if (strcmp(argv[i], "--at") == 0)
            im.at = n;
        else if (strcmp(argv[i], "--hidden") == 0)
            hidden = n;
        else if (strcmp(argv[i], "--time") == 0)
            stamp = n;
        else
            return Usage();
    }
    if (argc - i < 2 || im.sector < 512u || im.sector > FAT_SECTOR_MOST || im.sector % 512u)
        return Usage();
    const char* path = argv[i], *cmd = argv[i + 1];
    int         lay  = strcmp(cmd, "lay") == 0;
    im.file = fopen(path, lay ? "a+b" : "r+b");
    if (im.file && lay) {
        fclose(im.file);
        im.file = fopen(path, "r+b");
    }
    if (!im.file) {
        fprintf(stderr, "fat: %s: %s\n", path, strerror(errno));
        return 1;
    }
    FatMedium m = { ImageRead, ImageWrite, ImageFlush, &im, im.sector, 0 };
    int       rc;
    if (lay) {
        rc = Lay(&im, &m, argc - i - 2, argv + i + 2, hidden);
    } else {
        fseeko(im.file, 0, SEEK_END);
        m.sectors = (uint64_t)ftello(im.file) / im.sector - im.at;
        FatVolume* v = calloc(1, sizeof(FatVolume));
        rc = v ? FatOpen(v, &m) : FAT_IO;
        if (rc != FAT_OK) {
            fprintf(stderr, "fat: %s: %s\n", path, Said(rc));
            rc = 1;
        } else {
            FatStamp(v, stamp);
            rc = Command(v, cmd, argc - i - 2, argv + i + 2, im.at);
        }
        free(v);
    }
    if (fclose(im.file) != 0 && rc == 0) {
        fprintf(stderr, "fat: %s: the image did not take the last writes\n", path);
        rc = 1;
    }
    return rc;
}
