#ifndef TAGFS_FILE_RECORD_H
#define TAGFS_FILE_RECORD_H

#include "../tagfs.h"

/*
 * FileRecord — what memory knows of one live file: a header (name, tags,
 * flags, size, times) and the file's runs, as the map of live files holds them
 * for every reader and every Ink (file_map.h), whatever format the volume's
 * book is written in.
 *
 * ‼ A CHANGE COPIES WHAT IT CHANGES, NOT THE FILE (spec_v2 §3.7, §2.9). The runs
 * used to be one flat array inside the record's one allocation, so renaming a
 * file, or writing one block of it, built a copy of every run it had — and so
 * did every open, every load and every look at its metadata. A file a disk
 * image grows on fresh ground has its runs counted in millions (§2.9: ~1.4
 * million after a week), and every 4 KiB write would have copied ~34 MB.
 *
 * So the runs are cut into Parcels, each at most PARCEL_STRIPS_MOST Strips and
 * never changed once built, and a Tract — an immutable tree of references,
 * ordered by the file's blocks — names them. Parcels and Tract nodes are
 * shared by every record that names them and freed with their last reference.
 * A change builds a new header and new copies of only the Parcels it touches
 * and of the Tract's nodes on the way down to them (FileRecordSplice); a
 * rename builds a new header and shares the whole Tract (FileRecordRelabel).
 * A reader holds a reference on the record for as long as it reads, so a
 * writer that replaces the record never frees what the reader walks.
 *
 * Why 128: a DATA run in the book's own form takes at most 5 + 10 + 10 bytes
 * (§2.8, head, zigzag start, birth_delta), so at most ~155 fit the useful part
 * of a Sheet whatever the numbers grow to (§2.9 :625): a Parcel of memory is
 * one Parcel of the medium, and the book never has to cut one.
 *
 * A record keeps the Strips exactly as its writer gave them — two Strips that
 * meet on the medium stay two: format v1's packer counts extents off them
 * (PackV1), and the record a v1 Ink packs must be the one it always packed.
 */

/* The most Strips a Parcel holds; a Parcel of a file of more than one holds
 * at least PARCEL_STRIPS_LEAST — fewer is joined to a neighbour by the splice
 * that leaves it so (the boundary is the writer's to choose, §2.9 :651-652). */
#define PARCEL_STRIPS_MOST   128u
#define PARCEL_STRIPS_LEAST  32u

/* A Tract node names at most TRACT_KIDS_MOST, and every node but the root at
 * least TRACT_KIDS_LEAST. Why TRACT_HEIGHT_MOST is enough for any record: a
 * Tract of height h >= 2 has a root of at least two kids and every other node
 * at least 32, so it names at least 2 * 32^(h-1) Parcels; a record has no more
 * Parcels than Strips, nor Strips than u64 blocks, and 2 * 32^13 = 2^66 is
 * past that — 13 levels hold every record there can be. */
#define TRACT_KIDS_MOST      64u
#define TRACT_KIDS_LEAST     32u
#define TRACT_HEIGHT_MOST    13u

typedef struct Parcel Parcel;
typedef struct Tract  Tract;

/* One allocation for the header, its tags and its name; the runs are the
 * Tract's. Never changed once it is in the map: shared by reference and freed
 * with its last one, the Tract's references let go with it. */
typedef struct FileRecord {
    volatile uint32_t  refs;
    uint32_t           flags;
    uint64_t           bytes;        /* the header and every node and Parcel it names, for the map's count */
    uint64_t           file_id;
    uint64_t           size;
    uint64_t           created_time;
    uint64_t           modified_time;
    /* When the Ink that made this record was a small append — its data cost
     * the medium less than a Settle cycle does — in the cadence's
     * microseconds; 0 for any other change. Memory only: the cadence asks it
     * of the next append (settle.h, a frequent small writer). */
    uint64_t           small_append_us;
    uint32_t           tag_count;
    uint32_t           name_len;
    uint64_t           strip_count;  /* the Strips of its runs, in the file's order */
    uint64_t           blocks;       /* the file's blocks they cover */
    const Tract*       tract;        /* the runs; NULL for a record that names none */
    const uint32_t*    tags;         /* these two point into the header's allocation */
    const char*        name;         /* NUL-terminated */
#ifdef CONFIG_VOLUME_TESTS
    /* Every step FileRecordFind has taken on this record, down the Tract and
     * inside a Parcel: the stand's measure that a read of it is O(n log n). */
    volatile uint64_t  find_steps;
#endif
} FileRecord;

/* Where block k of a file lies: what the Strip holding it is (STRIP_DATA or
 * STRIP_HOLE), the block on the medium (0 for a hole, which owns none), and
 * how many of the Strip's blocks follow it. */
typedef struct {
    uint32_t kind;
    uint32_t left;
    uint64_t block;
} FileSpot;

/*
 * One splice of a file's runs, as a Shaped record of the book says it (spec_v2
 * §2.8 :538-552): `remove` blocks taken out from block `at` and `ninsert`
 * Strips put in their place. `at` is counted in the file as the splices before
 * this one have left it — a list of splices is in ascending order and none
 * overlaps another — so one function applies a change an Ink made and a
 * change the book says was made.
 */
typedef struct {
    uint64_t     at;
    uint64_t     remove;
    uint64_t     ninsert;
    const Strip* insert;
} BookSplice;

/* A walk of a record's Strips in the file's order, with no recursion: the
 * path down the Tract is held here, never on the stack. */
typedef struct {
    const Tract*  node[TRACT_HEIGHT_MOST];
    uint32_t      slot[TRACT_HEIGHT_MOST];
    uint32_t      depth;         /* the nodes on the path; 0 when the walk is over */
    const Parcel* parcel;
    uint32_t      strip;         /* the next Strip of `parcel` */
} StripWalk;

/* The record `meta` describes — its Strips cut into Parcels — with one
 * reference: NULL when there is no memory, or when a Strip of it names no
 * block (count 0). Every allocation is made here, with no lock held. */
FileRecord* FileRecordBuild(const TagFSMetadata* meta);

/* A record with a new header — `name`, `tags`, `flags` — and every one of the
 * old record's runs, shared rather than copied; its times, size and file id
 * the old one's. NULL when there is no memory. */
FileRecord* FileRecordRelabel(const FileRecord* old, const char* name, const uint32_t* tags,
                              uint32_t tag_count, uint32_t flags);

/*
 * The old record with `count` splices applied, its size `size` and its
 * modified time `mtime` — the Parcels no splice touches shared with the old
 * record, the ones a splice touches built anew (a Parcel past the most is
 * cut, one below the least joined to a neighbour). OK and the new record with
 * one reference in *out; ERR_INVALID_ARGUMENT, said, for a splice that does
 * not fit the runs (out of order, past their end, an inserted Strip of no
 * block), ERR_NO_MEMORY — and nothing built either way.
 *
 * *freed, when it is asked for (freed not NULL), is the ground the old record
 * named and the new one does not — DATA ground taken out by a splice and not
 * put back by another — as Strips in a new array the caller frees, *nfreed of
 * them. A hole owns no ground and is never in it.
 */
error_t     FileRecordSplice(const FileRecord* old, const BookSplice* sp, uint64_t count,
                             uint64_t size, uint64_t mtime, FileRecord** out,
                             Strip** freed, uint64_t* nfreed);

/* Block k of the file, found down the Tract and inside its Parcel by halving:
 * O(log Parcels + log 128). False when the runs do not reach block k. */
bool        FileRecordFind(const FileRecord* rec, uint64_t k, FileSpot* out);

/* The walk set at the record's first Strip; each Next the next Strip, holes
 * included — a walk that wants ground walks StripWalkNextGround below — and
 * false once there is none. The record is held by the caller for the walk. */
void        StripWalkBegin(const FileRecord* rec, StripWalk* w);
bool        StripWalkNext(StripWalk* w, Strip* out);

/* ‼ THE ONE WALK OF A RECORD'S GROUND. The next Strip of the walk that lies on
 * ground — STRIP_DATA; every hole on the way passed over, for a hole owns no
 * block whatever its start reads as — and false once there is none. Every walk
 * that claims a file's blocks, gives them back, forgets their digests, guards
 * them or counts their seeks walks this one, so no such walk can take a hole
 * for ground. */
bool        StripWalkNextGround(StripWalk* w, Strip* out);

/* Every Strip of the record, in order, in a new array the caller frees: the
 * flat runs format v1's packer and a v1 Plot work on. OK; ERR_NO_MEMORY for no
 * memory, or for a record of more Strips than a u32 counts — said. A record of
 * none gives NULL and 0. */
error_t     FileRecordFlatten(const FileRecord* rec, Strip** out, uint32_t* count);

/* `count` DATA Strips laid as the ground they name, in place: in order of
 * their first block, those that meet joined into one — how many are left.
 * Two lists of the same ground, in whatever order and cut, come out the same
 * (a join stops short of a u32 count of blocks, which no volume of format v1
 * reaches). */
uint64_t    StripsAsGround(Strip* strips, uint64_t count);

void        FileRecordRelease(FileRecord* rec);

/* The header as a TagFSMetadata of the caller's own — name and tags copied,
 * no runs (extents NULL): 0, or -1 with `out` left empty when there is no
 * memory. */
int         FileRecordCopyOut(const FileRecord* rec, TagFSMetadata* out);

#endif /* TAGFS_FILE_RECORD_H */
