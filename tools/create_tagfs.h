/*
 * create_tagfs.h — what the parts of the volume maker share.
 *
 * The tool is one program in parts: the modes (create_tagfs.c), the ground
 * and the I/O on it (create_tagfs_ground.c), the files and the tags they are
 * packed with (create_tagfs_cargo.c), and the writer (create_tagfs_book.c) —
 * --frame-only, a Deed with no book behind it, and the volume with its book
 * bound by create_tagfs_bind.c (create_tagfs_bind.h). What the parts share is
 * named here, once.
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

/* ====================================================================
 * The ground (create_tagfs_ground.c)
 * ==================================================================== */

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
 * A tag the kernel can hold — the widths of tag_spelling.h, which a host
 * tool cannot include: a key of at most TAG_KEY_WIDTH bytes, and a key and
 * value together of at most TAG_TEXT_ROOM (the ':' between them is not
 * stored). A tag wider than these is refused by the cargo, before the first
 * write: a volume carrying it could only be read (tally.py TagGlass).
 * ==================================================================== */

#define TAG_KEY_WIDTH   255u
#define TAG_TEXT_ROOM   4074u

void         seed_reserved_tags(void);
int          parse_tags(const char* tag_string, const char* path,
                        uint32_t** out_ids, uint32_t* out_count, uint32_t* out_role);
void         CargoNameKernel(const char* path);
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
