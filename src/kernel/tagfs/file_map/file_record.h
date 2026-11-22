#ifndef TAGFS_FILE_RECORD_H
#define TAGFS_FILE_RECORD_H

#include "../tagfs.h"
#include "volume_tally.h"   /* PARCEL_STRIPS_MOST: the cut every writer of a Tally keeps alike */

/* FileRecord.flags is one word spelled twice: the kernel's TAGFS_FILE_*
 * (tagfs_constants.h) and the book's TALLY_FILE_* (volume_tally.h), which the
 * Tally and the ribbon carry as they stand (binder_items.c, endpaper_fact.c,
 * ribbon_records.c). Each bit is held to its other spelling here. */
STATIC_ASSERT(TAGFS_FILE_ACTIVE      == TALLY_FILE_ACTIVE,      "TAGFS_FILE_ACTIVE is the book's TALLY_FILE_ACTIVE");
STATIC_ASSERT(TAGFS_FILE_UNDONE      == TALLY_FILE_UNDONE,      "TAGFS_FILE_UNDONE is the book's TALLY_FILE_UNDONE");
STATIC_ASSERT(TAGFS_FILE_ROLE_MASK   == TALLY_FILE_ROLE_MASK,   "TAGFS_FILE_ROLE_MASK is the book's TALLY_FILE_ROLE_MASK");
STATIC_ASSERT(TAGFS_FILE_ROLE_SYSTEM == TALLY_FILE_ROLE_SYSTEM, "TAGFS_FILE_ROLE_SYSTEM is the book's TALLY_FILE_ROLE_SYSTEM");
STATIC_ASSERT(TAGFS_FILE_ROLE_BOOT   == TALLY_FILE_ROLE_BOOT,   "TAGFS_FILE_ROLE_BOOT is the book's TALLY_FILE_ROLE_BOOT");
STATIC_ASSERT(TAGFS_FILE_ROLE_KERNEL == TALLY_FILE_ROLE_KERNEL, "TAGFS_FILE_ROLE_KERNEL is the book's TALLY_FILE_ROLE_KERNEL");
STATIC_ASSERT(TAGFS_FILE_DELETED     == TALLY_FILE_DELETED,     "TAGFS_FILE_DELETED is the book's TALLY_FILE_DELETED");
STATIC_ASSERT(TAGFS_FILE_MADE_BY     == TALLY_FILE_MADE_BY,     "TAGFS_FILE_MADE_BY is the book's TALLY_FILE_MADE_BY");
STATIC_ASSERT(TAGFS_FILE_SHOWN       == TALLY_FILE_SHOWN,       "TAGFS_FILE_SHOWN is the book's TALLY_FILE_SHOWN");
STATIC_ASSERT(TAGFS_FILE_HEAD        == TALLY_FILE_HEAD,        "TAGFS_FILE_HEAD is the book's TALLY_FILE_HEAD");
STATIC_ASSERT(TAGFS_HEAD_MOST        == TALLY_HEAD_MOST,        "a record keeps as many first bytes as the book does");
STATIC_ASSERT(TAGFS_HEAD_MOST        <= sizeof(((TagFSMetadata*)0)->head), "and TagFSMetadata room for them");

/*
 * FileRecord — what memory knows of one live file: a header (name, tags,
 * flags, size, times) and the file's runs, as the map of live files holds them
 * for every reader and every Ink (file_map.h).
 *
 * ‼ A CHANGE COPIES WHAT IT CHANGES, NOT THE FILE. The runs
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
 * Why 128 is the format's to say (volume_tally.h, PARCEL_STRIPS_MOST): a
 * Parcel of memory is one Parcel of the medium, cut the same way by every
 * writer of a Tally, and the book never has to cut one.
 *
 * A record keeps the Strips exactly as its writer gave them — two Strips that
 * meet on the medium stay two.
 */

/* A Parcel holds at most PARCEL_STRIPS_MOST Strips; a Parcel of a file of
 * more than one holds at least PARCEL_STRIPS_LEAST — fewer is joined to a
 * neighbour by the splice that leaves it so (the boundary is the writer's to
 * choose, §2.9 :651-652). */
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

typedef struct Parcel   Parcel;
typedef struct Tract    Tract;
typedef struct TailSlip TailSlip;

/* Where a record stands in the map's name index (name_index.h): one link for
 * the whole name, one for its stem. Written by the map alone, under its lock,
 * as the record is put in the map and let go of it — never by a reader, and
 * zero in a record no map holds. */
typedef struct NameLink {
    struct NameLink* next;
    struct NameLink* prev;
    uint64_t         file_id;
    uint32_t         hash;
    bool             linked;
    uint8_t          which;          /* 0 the whole name, 1 its stem */
} NameLink;

/* One allocation for the header, its tags, the programs it shows its tags to
 * and its name; the runs are the
 * Tract's. Never changed once it is in the map, its name links apart: shared
 * by reference and freed with its last one, the Tract's references let go
 * with it. */
typedef struct FileRecord {
    volatile uint32_t  refs;
    uint32_t           flags;
    uint64_t           bytes;        /* the header and every node and Parcel it names, for the map's count */
    uint64_t           file_id;
    uint64_t           size;
    uint64_t           created_time;
    uint64_t           modified_time;
    /* With TAGFS_FILE_DELETED, the time — a book's nanoseconds, as created_time
     * is — until which the file is kept deleted; 0 for a file that is not. */
    uint64_t           keep_until;
    /* With TAGFS_FILE_MADE_BY, the volume's id of the file of the program that
     * made this file; 0 for a file no program of the volume made. Set once,
     * when the file is made, and the same in every record of the file after:
     * the builders here keep it and the flag one (FileRecordBuild). */
    uint64_t           made_by;
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
    /* `gen` is the seq of the stitch that last changed the file — a record of
     * the ribbon applies only over the gen its base names — not the Ink number
     * of RecordWhere.gen (§16.2 п.14). The one field of a header written after
     * it is built: a record Ink makes on a book holds what it was made from —
     * 0 for a file born — until the stitch that carries it settles, and then
     * that stitch's seq — or its base's, when the stitch carried nothing for
     * the file — written once by the Settle's holder (ink/docket.h,
     * DocketSettled), whose next stitch's BUILD is the one reader that trusts
     * it. An Ink on another core copies it into the record it makes of this
     * one (FileRecordRelabel) while that write may land, so both are atomic;
     * the copy is written over the same way before any BUILD reads it. `slip`
     * is the bytes the Slips of the ribbon lay over the file's last block
     * (tail_slip.h), shared by every record that keeps them; NULL for none. */
    uint64_t           gen;
    const TailSlip*    slip;
    NameLink           name_link[2];
    /* With TAGFS_FILE_SHOWN, the programs this file — a program's — shows its
     * own tags to: their files' ids, rising, 0 first for every program;
     * `shown_count` of them, never 0 with the bit and 0 without it. */
    const uint64_t*    shown_to;
    uint32_t           shown_count;
    /* With TAGFS_FILE_HEAD, the file's first bytes: min(size, TAGFS_HEAD_MOST)
     * of them, exact through every change of what the file holds — the Ink
     * that makes the change sets them (FileRecordHeadSet) before the record is
     * shared; head_len 0 for a file of no bytes. */
    uint8_t            head[TAGFS_HEAD_MOST];
    uint8_t            head_len;
    const uint32_t*    tags;         /* these three point into the header's allocation */
    const char*        name;         /* NUL-terminated */
#ifdef CONFIG_VOLUME_TESTS
    /* Every step FileRecordFind has taken on this record, down the Tract and
     * inside a Parcel: the stand's measure that a read of it is O(n log n). */
    volatile uint64_t  find_steps;
#endif
} FileRecord;

/* Where block k of a file lies: what the Strip holding it is (STRIP_DATA,
 * STRIP_HOLE or STRIP_SHARED), the block on the medium (0 for a hole, which
 * owns none), and how many of the Strip's blocks follow it. */
typedef struct {
    uint32_t kind;
    uint32_t left;
    uint64_t block;
} FileSpot;

/*
 * One splice of a file's runs, as a Shaped record of the book says it:
 * `remove` blocks taken out from block `at` and `ninsert` Strips put in their
 * place. `at` is counted in the file as the splices before this one have left
 * it — a list of splices is in ascending order and none overlaps another — so
 * one function applies a change an Ink made and a change the book says was
 * made.
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
 * block (count 0). Its flags hold TAGFS_FILE_MADE_BY exactly when meta->made_by
 * is not 0, TAGFS_FILE_SHOWN exactly when meta->shown_count is, and
 * TAGFS_FILE_HEAD exactly when meta->head_len is. Every allocation is made
 * here, with no lock held. */
FileRecord* FileRecordBuild(const TagFSMetadata* meta);

/* The same, its Strips cut as `cut` gives — the Strips of each Parcel, `ncut`
 * of them, as a Tally laid them (tract.h TractOfParcels): a book's mount. */
FileRecord* FileRecordBuildCut(const TagFSMetadata* meta, const uint32_t* cut, uint64_t ncut, bool* recut);

/* A record with a new header — `name`, `tags`, the programs it shows its
 * tags to (`shown`, rising), `flags`, `keep_until` — and every one of the old
 * record's runs, shared rather than copied; its times, size, file id, made_by
 * and first bytes the old one's (`flags` has TAGFS_FILE_MADE_BY and
 * TAGFS_FILE_HEAD as the old record's fields say and TAGFS_FILE_SHOWN as
 * `shown_count` does, whatever the caller gave). NULL when there is no
 * memory. */
FileRecord* FileRecordRelabel(const FileRecord* old, const char* name, const uint32_t* tags,
                              uint32_t tag_count, const uint64_t* shown, uint32_t shown_count,
                              uint32_t flags, uint64_t keep_until);

/*
 * The old record with `count` splices applied, its size `size` and its
 * modified time `mtime` — the Parcels no splice touches shared with the old
 * record, the ones a splice touches built anew (a Parcel past the most is
 * cut, one below the least joined to a neighbour) — and old's Slips as their
 * one rule keeps them (tail_slip.h, TailSlipKept). OK and the new record with
 * one reference in *out; ERR_INVALID_ARGUMENT, said, for a splice that does
 * not fit the runs (out of order, past their end, an inserted Strip of no
 * block), ERR_NO_MEMORY — and nothing built either way.
 *
 * *freed, when it is asked for (freed not NULL), is the ground the old record
 * named and the new one does not — ground taken out by a splice and not put
 * back by another, each Strip DATA or SHARED as it was named — in a new array
 * the caller frees, *nfreed of them. A hole owns no ground and is never in it.
 */
/* The first bytes of `rec` — a record just built, that no map holds yet and
 * nobody else sees — made `head`, `len` of them (TAGFS_HEAD_MOST at the most),
 * TAGFS_FILE_HEAD on exactly when there are any. */
void        FileRecordHeadSet(FileRecord* rec, const uint8_t* head, uint32_t len);

error_t     FileRecordSplice(const FileRecord* old, const BookSplice* sp, uint64_t count,
                             uint64_t size, uint64_t mtime, FileRecord** out,
                             Strip** freed, uint64_t* nfreed);

/* The splices that make `was` into `now` (file_record_splices.c): applied to
 * `was` by FileRecordSplice they build `now`, Strip for Strip. OK, *count of
 * them in a new array *out and the Strips they put in in *strips — both the
 * caller's to free, NULL for none — or ERR_NO_MEMORY. A Parcel the two share
 * at one place in the file is passed over whole. */
error_t     FileRecordSplices(const FileRecord* was, const FileRecord* now, BookSplice** out, uint64_t* count,
                              Strip** strips);

/* Block k of the file, found down the Tract and inside its Parcel by halving:
 * O(log Parcels + log 128). False when the runs do not reach block k. */
bool        FileRecordFind(const FileRecord* rec, uint64_t k, FileSpot* out);

/* The walk set at the record's first Strip; each Next the next Strip, holes
 * included — a walk that wants ground walks StripWalkNextGround below — and
 * false once there is none. The record is held by the caller for the walk. */
void        StripWalkBegin(const FileRecord* rec, StripWalk* w);
bool        StripWalkNext(StripWalk* w, Strip* out);

/* The walk set at the Strip that holds block k — the Strip its next Next
 * gives — found down the Tract by halving, with *begins the block that Strip
 * begins at: false, the walk over, past the runs' end. */
bool        StripWalkAt(const FileRecord* rec, uint64_t k, StripWalk* w, uint64_t* begins);

/* The rest of the Parcel the walk stands in, whole — its Strips from where
 * the walk stands in it, *count of them, the walk past them: false once there
 * is none. A walk begun at a record's first Strip and moved by this alone
 * hands each Parcel of the record whole, in order: memory's Parcels are the
 * Tally's, one to one (§16.1 п.7). */
bool        StripWalkParcel(StripWalk* w, const Strip** strips, uint32_t* count);

/* ‼ THE ONE WALK OF A RECORD'S GROUND. The next Strip of the walk that lies on
 * ground — STRIP_DATA or STRIP_SHARED (StripOnGround); every hole on the way
 * passed over, for a hole owns no block whatever its start reads as — and
 * false once there is none. Every walk that claims a file's blocks, gives them
 * back, forgets their digests, guards them or counts their seeks walks this
 * one, so no such walk can take a hole for ground; one that claims or gives
 * back asks the Strip's kind, for a SHARED block is let go by its last owner
 * alone (ink/commonhold.h). */
bool        StripWalkNextGround(StripWalk* w, Strip* out);

#ifdef CONFIG_VOLUME_TESTS
/* Every Strip of the record, in order, in a new array the caller frees — for
 * the stand, which holds a record to the Strips it was built of. OK;
 * ERR_NO_MEMORY for no memory, or for a record of more Strips than a u32
 * counts — said. A record of none gives NULL and 0. */
error_t     FileRecordFlatten(const FileRecord* rec, Strip** out, uint32_t* count);
#endif

/* `count` Strips of ground laid as the ground they name, in place, each DATA
 * whatever its kind: in order of their first block, those that meet joined
 * into one — how many are left.
 * Two lists of the same ground, in whatever order and cut, come out the same
 * (a join stops short of a u32 count of blocks). */
uint64_t    StripsAsGround(Strip* strips, uint64_t count);

void        FileRecordRelease(FileRecord* rec);

/* The header as a TagFSMetadata of the caller's own — name and tags copied,
 * no runs (extents NULL): 0, or -1 with `out` left empty when there is no
 * memory. */
int         FileRecordCopyOut(const FileRecord* rec, TagFSMetadata* out);

#endif /* TAGFS_FILE_RECORD_H */
