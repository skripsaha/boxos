#ifndef BOXOS_FAT_H
#define BOXOS_FAT_H

/*
 * FAT32 — the file system of an EFI System Partition, read and written by one
 * code for every side that touches one (src/lib/fat/fat.c): the host's image
 * builder (tools/fat.c) laying the ESP of a stick, the kernel laying and
 * writing the ESP of a disk a system is installed on — its own, or one another
 * system made and shares (UEFI 2.10 §13.3) — and Gangway finding its own files
 * on the ESP it was started from.
 *
 * ‼ ONE CODE, NO LIBRARY. Nothing here calls memcpy, malloc or anything of a
 * C library: Gangway has none, and the kernel's is its own. Integer types come
 * from the includer, as volume_tally.h's do. The medium is the caller's,
 * through the three calls of a FatMedium; a FatVolume is the caller's memory
 * too, two sectors of buffer inside it.
 *
 * ‼ WHAT A WRITE LEAVES WHEN IT IS CUT. FAT32 keeps no journal. A file is
 * written into clusters no entry names yet, the FATs are flushed with them,
 * only then is its entry pointed at them and flushed, and the clusters it held
 * before are given back last: cut anywhere, a file reads as it was or as it is
 * written, never half of each — what a cut can leave is clusters no file
 * names, which every checker gives back. Both FATs are always written alike.
 *
 * Names are ASCII. A name that is a short one exactly — 8.3, every letter of
 * its base one case and of its extension one case — is written as one, its
 * lower case kept as Windows keeps it (the entry's case bits); any other is a
 * long name with a short alias made for it. Names are found without regard to
 * case, by a long name or a short one.
 */

#define FAT_SECTOR_MOST      4096u     /* the largest sector a FAT32 is laid in */
#define FAT_CLUSTERS_LEAST   65525u    /* fewer data clusters is a FAT16 by definition */
#define FAT_NAME_MOST        255u      /* a long name's characters */

#define FAT_ATTR_READ_ONLY   0x01u
#define FAT_ATTR_HIDDEN      0x02u
#define FAT_ATTR_SYSTEM      0x04u
#define FAT_ATTR_VOLUME_ID   0x08u
#define FAT_ATTR_DIRECTORY   0x10u
#define FAT_ATTR_ARCHIVE     0x20u
#define FAT_ATTR_LONG_NAME   0x0Fu

/* How a FAT32 of `sectors` of `sector_bytes` is laid (FatShape): its
 * cluster, its reserved sectors — at least 32, grown so the data region
 * begins on a cluster and on 4 KiB, the physical block of every disk of
 * 512-byte sectors made since 2011 (UEFI 2.10 §13.3.1.1 asks it on the
 * physical block) — one FAT's sectors, and its data clusters. */
typedef struct {
    uint32_t cluster_sectors;
    uint32_t reserved;
    uint32_t fat_sectors;
    uint32_t clusters;
    uint64_t sectors;           /* what the FAT32 spans of them */
} FatLayout;

#define FAT_CLUSTERS_MOST    0x0FFFFFF4u   /* data clusters 2 to 0x0FFFFFF5: past them the FAT's own values */

/* The layout of a FAT32 on `sectors` of `sector_bytes`, its clusters as
 * large as Microsoft's table sizes them — 512 bytes to 260 MB, 4K to 8 GB,
 * 8K to 16 GB, 16K to 32 GB, 32K past — never smaller than a sector: 1, or
 * 0 for too few sectors to hold one. One rule for FatLay and every side that
 * asks how large an ESP must be. */
static inline int FatShape(uint64_t sectors, uint32_t sector_bytes, FatLayout* out)
{
    uint32_t s = sector_bytes;
    if (s < 512u || s > FAT_SECTOR_MOST || s % 512u)
        return 0;
    uint64_t bytes   = sectors * s;
    uint32_t cluster = bytes <= (260ull << 20) ? 512u : bytes <= (8ull << 30) ? 4096u
                     : bytes <= (16ull << 30) ? 8192u : bytes <= (32ull << 30) ? 16384u : 32768u;
    uint32_t spc     = cluster < s ? 1u : cluster / s;
    uint32_t align   = spc > 4096u / s ? spc : (4096u / s ? 4096u / s : 1u);
    uint64_t fat = 1, clusters = 0, reserved = 32u;
    for (uint32_t round = 0; round < 64u; round++) {
        if (sectors <= reserved + 2u * fat)
            return 0;
        clusters      = (sectors - reserved - 2u * fat) / spc;
        uint64_t need = ((clusters + 2u) * 4u + s - 1u) / s;
        uint64_t pad  = (align - (32u + 2u * need) % align) % align;
        if (need == fat && reserved == 32u + pad)
            break;
        fat      = need;
        reserved = 32u + pad;
    }
    if (clusters < FAT_CLUSTERS_LEAST)
        return 0;
    if (clusters > FAT_CLUSTERS_MOST)
        clusters = FAT_CLUSTERS_MOST;
    out->cluster_sectors = spc;
    out->reserved        = (uint32_t)reserved;
    out->fat_sectors     = (uint32_t)fat;
    out->clusters        = (uint32_t)clusters;
    out->sectors         = reserved + 2u * fat + clusters * spc;
    return 1;
}

/* The fewest sectors of `sector_bytes` a FAT32 is laid on: some 33 MB of
 * 512-byte sectors, 257 MB of 4096-byte ones. */
static inline uint64_t FatLeastSectors(uint32_t sector_bytes)
{
    FatLayout l;
    uint64_t  n = FAT_CLUSTERS_LEAST + 32u + 2u * (((uint64_t)FAT_CLUSTERS_LEAST + 2u) * 4u / sector_bytes);
    while (!FatShape(n, sector_bytes, &l))
        n++;
    return n;
}

/* What a call answers. */
typedef enum {
    FAT_OK            = 0,
    FAT_IO            = -1,   /* the medium did not read or take a sector */
    FAT_NOT_FAT32     = -2,   /* no FAT32 this code reads: another FAT, a damaged boot sector, none */
    FAT_NOT_FOUND     = -3,   /* no entry of that name */
    FAT_NO_ROOM       = -4,   /* not as many free clusters in one run as a file takes */
    FAT_BAD_NAME      = -5,   /* not a name a FAT holds, or not ASCII */
    FAT_TOO_SMALL     = -6,   /* too few sectors for a FAT32 of this sector */
    FAT_NOT_DIRECTORY = -7,   /* a part of a path is a file, or the last a directory where a file was meant */
    FAT_DAMAGED       = -8,   /* a cluster chain that runs off the volume, loops, or ends early */
} FatVerdict;

/* Where a FAT lives: `read` and `write` move `count` of its sectors from its
 * sector `sector` — counted from its first, the boot sector — and `flush`
 * puts what was written on the medium; each answers 0, or not 0 for a medium
 * that refused. `sector_bytes` is its sector, 512 to FAT_SECTOR_MOST, and
 * `sectors` how many it has. */
typedef struct {
    int    (*read)(void* ctx, uint64_t sector, uint32_t count, void* into);
    int    (*write)(void* ctx, uint64_t sector, uint32_t count, const void* from);
    int    (*flush)(void* ctx);
    void*    ctx;
    uint32_t sector_bytes;
    uint64_t sectors;
} FatMedium;

/* A FAT32 opened (FatOpen): its layout, what is known of its free clusters,
 * the stamp what is written bears, and the working sectors. */
typedef struct {
    const FatMedium* medium;
    uint32_t sector_bytes;
    uint32_t cluster_sectors;
    uint32_t cluster_bytes;
    uint32_t reserved;          /* sectors before the first FAT */
    uint32_t fats;
    uint32_t fat_sectors;       /* one FAT's */
    uint64_t data_start;        /* the sector cluster 2 begins at */
    uint32_t clusters;          /* data clusters: 2 to clusters + 1 */
    uint32_t root;              /* the root directory's first cluster */
    uint32_t fsinfo;            /* its FSInfo sector, 0 for none this code trusts */
    uint32_t free_hint;         /* where a search for free clusters begins */
    int64_t  free_count;        /* -1 when unknown */
    uint16_t stamp_date, stamp_time;   /* DOS date and time of what is written (FatStamp) */
    uint64_t fat_at;            /* the FAT sector fat_buf holds; ~0 for none */
    int      fat_dirty;         /* fat_buf changed since it was read */
    uint8_t  fat_buf[FAT_SECTOR_MOST];
    uint8_t  buf[FAT_SECTOR_MOST];
} FatVolume;

/* An entry found or made: its first cluster (0 for an empty file), size and
 * attributes, the directory it lies in, and the place of its first slot — a
 * long name's first — and of its short entry, counted in slots from the
 * directory's beginning. */
typedef struct {
    uint32_t cluster;
    uint32_t size;
    uint8_t  attr;
    uint32_t dir;               /* the cluster of the directory it lies in */
    uint32_t first_slot;
    uint32_t slot;
} FatEntry;

/* A FAT32 laid on all of `m` — its boot sector, FSInfo and their copies, two
 * FATs and an empty root — its clusters as large as Microsoft's table sizes
 * them, never smaller than a sector. `hidden` is the sectors before it on its
 * disk (BPB HiddenSectors), `label` its 11-character label (NULL for none),
 * `volume_id` its serial. `work` is any FatVolume, a buffer only. FAT_OK,
 * FAT_TOO_SMALL, FAT_IO. */
int FatLay(const FatMedium* m, FatVolume* work, uint64_t hidden, const char* label, uint32_t volume_id);

/* The FAT32 on `m` opened into `v`. FAT_OK, FAT_NOT_FAT32, FAT_IO. */
int FatOpen(FatVolume* v, const FatMedium* m);

/* What is written from now on bears this time (seconds since 1970, UTC). */
void FatStamp(FatVolume* v, uint64_t unix_seconds);

/* The entry at `path` — its parts parted by '/' or '\', from the root.
 * FAT_OK, FAT_NOT_FOUND, FAT_NOT_DIRECTORY, FAT_BAD_NAME, FAT_DAMAGED, FAT_IO. */
int FatFind(FatVolume* v, const char* path, FatEntry* out);

/* Every directory of `path` made where it is not yet: the last's entry in
 * *out (NULL for none). FAT_OK, FAT_NOT_DIRECTORY, FAT_NO_ROOM, FAT_BAD_NAME,
 * FAT_DAMAGED, FAT_IO. */
int FatMakeDirs(FatVolume* v, const char* path, FatEntry* out);

/* The file at `path` holding `bytes` from `data` — zeros for NULL — in one run
 * of clusters, its directory made where it is not; one there is replaced as
 * the header says, whole or not at all. *out (NULL for none) its entry.
 * FAT_OK, FAT_NO_ROOM, FAT_NOT_DIRECTORY, FAT_BAD_NAME, FAT_DAMAGED, FAT_IO. */
int FatPut(FatVolume* v, const char* path, const void* data, uint32_t bytes, FatEntry* out);

/* `bytes` of the file `e` from `offset` into `into`; past its end is not read.
 * The bytes read, or a FatVerdict below 0. */
int64_t FatRead(FatVolume* v, const FatEntry* e, uint32_t offset, void* into, uint32_t bytes);

/* Whether file `e` lies in one run of clusters: its first sector and how many
 * it spans into *first and *count. FAT_OK; FAT_NOT_FOUND for an empty file;
 * FAT_DAMAGED for one in more than one run, or a broken chain; FAT_IO. */
int FatRun(FatVolume* v, const FatEntry* e, uint64_t* first, uint64_t* count);

/* The entry at `path` taken away — a file, or a directory with nothing in it
 * — and its clusters given back. FAT_OK, FAT_NOT_FOUND, FAT_NOT_DIRECTORY for
 * a directory that holds something, FAT_BAD_NAME, FAT_DAMAGED, FAT_IO. */
int FatRemove(FatVolume* v, const char* path);

#endif /* BOXOS_FAT_H */
