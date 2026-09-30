#ifndef TAGFS_FILE_MAP_H
#define TAGFS_FILE_MAP_H

#include "../tagfs.h"

/*
 * The files a mounted volume holds, by file id — and nothing at all for an id
 * no live file has.
 *
 * What memory knows about the files used to be three arrays indexed by file
 * id: the mirror of every record, where each record sat, and the file table's
 * rows — each as long as the highest id the volume ever issued. A volume that
 * had made and deleted a million files paid for a million slots on every mount
 * with a dozen files on it, and the mirror silently dropped any id past the
 * length it was given.
 *
 * So they are one ordered map, a B+-tree of file id -> entry, and an entry
 * exists only while a file does. A B+-tree rather than a radix of 256-pointer
 * leaves: the radix costs a whole leaf (2 KiB) for an id with no live
 * neighbour, and the files a long-lived volume keeps — a few from every era of
 * its history — are exactly such ids; the tree costs at most two slots of a
 * node for every live file, however its ids are spread. It walks in id order,
 * which is what a cursor over the live files wants.
 *
 * Every access is under the map's own lock, a leaf: nothing under it reads the
 * medium, allocates or takes another TagFS lock. The nodes an insert may need
 * are taken before the lock (a FileMapStake), and nodes and records let go are
 * given back after it.
 */

/* The pool's newest record of a file, as memory keeps it: one allocation,
 * never changed once built, shared by reference and freed with its last one.
 * A reader holds a reference while it reads, so a writer that replaces the
 * record never frees it under the reader. */
typedef struct FileRecord {
    volatile uint32_t  refs;
    uint32_t           bytes;        /* the whole allocation, for the map's count */
    uint32_t           file_id;
    uint32_t           flags;
    uint64_t           size;
    uint64_t           created_time;
    uint64_t           modified_time;
    /* When the Ink that made this record was a small append — its data cost
     * the medium less than a Settle cycle does — in the cadence's
     * microseconds; 0 for any other change. Memory only: the cadence asks it
     * of the next append (settle.h, a frequent small writer). */
    uint64_t           small_append_us;
    uint32_t           tag_count;
    uint32_t           extent_count;
    uint32_t           name_len;
    const uint16_t*    tags;         /* these three point into the allocation */
    const FileExtent*  extents;
    const char*        name;         /* NUL-terminated */
} FileRecord;

/* A record built from `meta`, with one reference: NULL when there is no memory. */
FileRecord* FileRecordOf(const TagFSMetadata* meta);
void        FileRecordRelease(FileRecord* rec);

/* A TagFSMetadata of the caller's own made from a record: 0, or -1 with `out`
 * left empty when there is no memory. */
int         FileRecordCopyOut(const FileRecord* rec, TagFSMetadata* out);

/* Where a file's newest record sits in the metadata pool, and which Ink put
 * it there: `link` is the pool chain's link, counted from the chain's head,
 * which a link keeps whatever is unlinked around it, and `offset` the record's
 * byte in that link's block. `gen` is the Ink that wrote the record and
 * `shape` the Ink that last gave the file its size and extents — both 0 for a
 * record the mount read, which every Ink since is newer than. */
typedef struct {
    uint32_t link;
    uint32_t offset;
    uint64_t gen;
    uint64_t shape;
} RecordWhere;

/* The deepest the tree can be for 32-bit file ids, and so the most nodes one
 * insert can split (every level, and a new root). */
#define FILE_MAP_HEIGHT_MOST 9u
#define FILE_MAP_STAKE_MOST  (FILE_MAP_HEIGHT_MOST + 1u)

/* The nodes one insert may need, taken with no lock held. */
typedef struct {
    void*    node[FILE_MAP_STAKE_MOST];
    uint32_t count;
} FileMapStake;

FileMap* FileMapCreate(void);
/* Everything the map holds let go. Nobody may be inside it. */
void     FileMapDestroy(FileMap* map);

/* Room staked for an insert of `fid`: none when the file is in the map, else
 * as many nodes as the insert would split at this moment. ERR_NO_MEMORY, said
 * once with the map's numbers, when the heap refused one. */
error_t  FileMapStakeFor(FileMap* map, uint32_t fid, FileMapStake* stake);
/* What an insert did not spend, given back. */
void     FileMapStakeLetGo(FileMapStake* stake);

/*
 * `rec` becomes the file's record, sitting where `where` says. The reference is
 * the map's from here on, and the record it replaces is let go. The file is
 * added when it is not in the map, from the stake; OK, or ERR_NO_MEMORY —
 * said — when the stake does not cover the insert and the heap refused the
 * difference, and then `rec` is let go and the map is as it was. A caller that
 * holds the volume's state lock checks FileMapNodesToAdd against its stake
 * first, so the put never reaches the heap under that lock.
 */
error_t  FileMapRecordPut(FileMap* map, uint32_t fid, FileRecord* rec,
                          const RecordWhere* where, FileMapStake* stake);

/* How many nodes adding `fid` would take now; 0 when the file is in the map. */
uint32_t FileMapNodesToAdd(FileMap* map, uint32_t fid);

/* The file's record with a reference the caller lets go (FileRecordRelease),
 * and where it sits (out may be NULL); NULL when the map holds none. */
FileRecord* FileMapRecordHold(FileMap* map, uint32_t fid, RecordWhere* out);

/* The file's record moved inside its block, from byte `from` to byte `to` of
 * link `link`, block `block` — the pool closed a gap in its tail — and the
 * file's row with it when the row named the record there. Nothing changes
 * when the map has the file's record anywhere else. */
void     FileMapRecordMoved(FileMap* map, uint32_t fid, uint32_t link, uint32_t block,
                            uint32_t from, uint32_t to);

/* The file table's row of a file. Set adds the file when it is not in the
 * map (ERR_NO_MEMORY, said, when there is no room); Get answers false for a
 * file with no row; Clear takes the row away, and the file with it when it has
 * no record either. */
error_t  FileMapRowSet(FileMap* map, uint32_t fid, uint32_t block, uint32_t offset);
bool     FileMapRowGet(FileMap* map, uint32_t fid, uint32_t* out_block, uint32_t* out_offset);
void     FileMapRowClear(FileMap* map, uint32_t fid);

/* The file out of the map, row and record both. */
void     FileMapDrop(FileMap* map, uint32_t fid);

/* Up to `room` file ids from `from` on that have a row — the live files — in
 * id order: how many were written. A walk of them all asks again from one
 * past the last id it was given, so no lock is held across the walk and what
 * it does with each file may change the map. */
uint32_t FileMapRows(FileMap* map, uint32_t from, uint32_t* out, uint32_t room);

/* Up to `room` file ids from `from` on that the map holds at all — a row, a
 * record or both — in id order: the mount's walk, which decides what each of
 * them is, asks this way. */
uint32_t FileMapFiles(FileMap* map, uint32_t from, uint32_t* out, uint32_t room);

/* The rows of file ids first .. first+count-1 into `out`, a row of zeros for
 * an id with none — one block of the file table, as the medium holds it. */
void     FileMapRowsInto(FileMap* map, uint32_t first, uint32_t count, FileTableEntry* out);

/* The files in the map, and the bytes of kernel heap it holds for them: its
 * nodes and its records. */
uint32_t FileMapCount(FileMap* map);
size_t   FileMapBytes(FileMap* map);

#endif /* TAGFS_FILE_MAP_H */
