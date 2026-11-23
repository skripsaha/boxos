/*
 * create_tagfs.h — what the parts of the volume maker share.
 *
 * The tool is one program in parts: the modes (create_tagfs.c), the ground
 * and the I/O on it (create_tagfs_ground.c), the files and the tags they are
 * packed with (create_tagfs_cargo.c), and the writer (create_tagfs_book.c) —
 * --frame-only, a Deed with no book behind it, and the volume with its book
 * bound by the binder every maker of a volume binds with (book_bind.h). What
 * the parts share is named here, once.
 */

#ifndef CREATE_TAGFS_H
#define CREATE_TAGFS_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "volume_deed.h"     /* the title; types come from <stdint.h> above    */
#include "deed_pen.h"        /* and the one pen it is written with              */
#include "volume_tally.h"    /* TALLY_FILE_ROLE_*, the role a file is kept in    */
#include "book_bind.h"       /* the binder, the kernel's own as well             */

#define TAGFS_BLOCK_SIZE       VOLUME_BLOCK_BYTES
#define TAGFS_SECTOR_BYTES     VOLUME_DEED_SECTOR_BYTES
#define TAGFS_BLOCK_SECTORS    VOLUME_BLOCK_SECTORS

/* Every volume alignment is on the conventional mebibyte — the grain the Deed
 * records and every flash translation layer is built around. */
#define GROUND_GRAIN_SECTORS         2048u

/* The room a running machine works in, past what a volume is made to hold —
 * logs, drafts, the files a test suite makes and deletes: sixty-four
 * mebibytes, in the volume's own blocks. A policy, stated once, here, with its
 * reason: a machine that cannot write is a machine that cannot be used, and
 * the cost of being generous is sixty-four mebibytes on a medium that has
 * gigabytes. The ground is what the tool answers (--ground), not a size a
 * Makefile names: a ceiling discovered by what breaks under it is a trap. */
#define GROUND_WORKING_ROOM_BLOCKS   ((64u * 1024u * 1024u) / TAGFS_BLOCK_SIZE)

/* ====================================================================
 * In-memory tracking
 * ==================================================================== */

typedef struct {
    char*    key;
    char*    value;   /* NULL for label tags */
    uint32_t tag_id;
} TagEntry;

extern TagEntry* g_tags;
extern uint32_t  g_tag_count;

typedef struct {
    char*       filepath;
    char*       filename;
    uint32_t    file_id;
    uint64_t    file_size;
    uint32_t    start_block;
    uint32_t    block_count;
    uint32_t*   tag_ids;
    uint32_t    tag_count;
    uint32_t    role;        /* TALLY_FILE_ROLE_*: what the system keeps it as; 0 for an ordinary file */
} FileInfo;

/* A file as the book lays it: the cargo's account of it and its one run —
 * blocks counted in u64, for the ground has no width. An empty file has no
 * run: a header alone (§2.9). */
typedef struct {
    const char* path;
    const char* name;          /* its base name, the name it is listed by */
    uint64_t    fid;
    uint64_t    size;
    uint64_t    start;         /* in the volume's blocks */
    uint64_t    blocks;
    uint32_t*   tags;          /* rising */
    uint32_t    ntags;
    uint32_t    role;          /* TALLY_FILE_ROLE_*: what the system keeps it as, 0 for none */
    uint8_t     head[TALLY_HEAD_MOST];   /* its first bytes, head_len of them: min(size, TALLY_HEAD_MOST) */
    uint32_t    head_len;
    uint64_t    passport;      /* the signed program it is, 0 for none (PassportOf) */
} BindFile;

/* ====================================================================
 * The ground (create_tagfs_ground.c)
 * ==================================================================== */

extern uint32_t g_medium_block;    /* the block the image's medium is addressed in: 512, or 4096 on 4Kn */
extern const char* g_volume_name;  /* the name --name gives the volume (TallyNameSpelled); NULL for none */
extern uint64_t g_ground_start;    /* first sector of the volume's ground */
extern uint64_t g_ground_sectors;  /* how far it runs */

int            write_at_sector(FILE* disk, uint64_t vsector, const void* data, size_t size);
int            write_file_data(FILE* disk, uint32_t data_start_block, uint32_t block,
                               FILE* src, uint64_t file_size, uint32_t block_count);
uint32_t       tagfs_crc32(const uint8_t* data, uint32_t len);
FILE*          GroundOpen(const char* disk_path);
int            generate_uuid(uint8_t uuid[16]);
VolumeGeometry DeedGeometry(void);

/* ====================================================================
 * The cargo (create_tagfs_cargo.c)
 *
 * A tag the kernel can hold — the widths tagfs_reserved.h states for both: a
 * key of at most TAG_KEY_WIDTH bytes, and a key and value together of at most
 * TAG_KEY_VALUE_WIDTH (the ':' between them is not stored). A tag wider than
 * these is refused by the cargo, before the first write: a volume carrying it
 * could only be read (tally.py TagGlass).
 * ==================================================================== */

void         seed_reserved_tags(void);
int          parse_tags(const char* tag_string, const char* path,
                        uint32_t** out_ids, uint32_t* out_count, uint32_t* out_role);

/* The passport of the signed program the box `filename` is — the SHA-256 of its
 * author's key and its name, TALLY_PASSPORT_BIT set, as the kernel makes it
 * (box_trust.c) — into *passport; 0 for a file that is no signed box. The
 * signature is not checked: the kernel checks it when it starts the program.
 * 0, or -1 having said why. */
int PassportOf(const char* filename, uint64_t* passport);
void         CargoNameKernel(const char* path);
int          CargoKeepSystem(const char* path);
VolumePacked PackedStamp(int file_count, char* argv[], int first,
                         const char* kernel_path);
void         CargoRelease(void);

/* ====================================================================
 * The writer (create_tagfs_book.c) — --frame-only, the volume, and --ground
 * ==================================================================== */

int FrameOnlyMake(const char* disk_path, int file_count, char* argv[], int first);
int BookMake(const char* disk_path, int file_count, char* argv[], int first);
int GroundSay(int file_count, char* argv[], int first);

#endif /* CREATE_TAGFS_H */
