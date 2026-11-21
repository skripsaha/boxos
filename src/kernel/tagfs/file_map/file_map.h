#ifndef TAGFS_FILE_MAP_H
#define TAGFS_FILE_MAP_H

#include "../tagfs.h"
#include "file_record.h"

/*
 * The files a mounted volume holds, by file id — and nothing at all for an id
 * no live file has.
 *
 * What memory knows about the files used to be arrays indexed by file id —
 * the mirror of every record, and where each record sat — each as long as the
 * highest id the volume ever issued. A volume that
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

/* Which Ink put a file's newest record in memory: `gen` is the Ink that wrote
 * the record and `shape` the Ink that last gave the file its size and extents
 * — both 0 for a record the mount read, which every Ink since is newer than. */
typedef struct {
    uint64_t gen;
    uint64_t shape;
} RecordWhere;

/* The deepest the tree can be for 64-bit file ids — every node but the root
 * holds at least 16, so h levels hold at least 2 * 16^(h-1) ids, and 2^64 of
 * them fit in 17 — and so the most nodes one insert can split (every level,
 * and a new root). */
#define FILE_MAP_HEIGHT_MOST 17u
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
error_t  FileMapStakeFor(FileMap* map, uint64_t fid, FileMapStake* stake);
/* What an insert did not spend, given back. */
void     FileMapStakeLetGo(FileMapStake* stake);

/*
 * `rec` becomes the file's record, put by the Ink `where` says. The reference
 * is the map's from here on, and the record it replaces is let go — which Ink
 * put that one comes back in *was, zeros when the file had none. The file is
 * added when it is not in the map, from the stake; OK, or ERR_NO_MEMORY —
 * said — when the stake does not cover the insert and the heap refused the
 * difference, and then `rec` is let go and the map is as it was. A caller that
 * holds the volume's state lock checks FileMapNodesToAdd against its stake
 * first, so the put never reaches the heap under that lock.
 */
error_t  FileMapRecordPut(FileMap* map, uint64_t fid, FileRecord* rec,
                          const RecordWhere* where, FileMapStake* stake, RecordWhere* was);

/* How many nodes adding `fid` would take now; 0 when the file is in the map. */
uint32_t FileMapNodesToAdd(FileMap* map, uint64_t fid);

/* The file's record with a reference the caller lets go (FileRecordRelease),
 * and which Ink put it (out may be NULL); NULL when the map holds none. */
FileRecord* FileMapRecordHold(FileMap* map, uint64_t fid, RecordWhere* out);

/* The file out of the map; which Ink put its record in *was (may be NULL),
 * zeros when it had none. */
void     FileMapDrop(FileMap* map, uint64_t fid, RecordWhere* was);

/* Up to `room` file ids from `from` on — the live files, each of which the
 * map holds a record of — in id order: how many were written. A walk of them
 * all asks again from one past the last id it was given, so no lock is held
 * across the walk and what it does with each file may change the map. */
uint32_t FileMapFiles(FileMap* map, uint64_t from, uint64_t* out, uint32_t room);

/* The files called `word` — its `length` bytes are the whole of the file's
 * name or of its stem, name_index.h — from file id `from` on, the live ones
 * alone, in file id order: up to `room` ids into `out`, how many were
 * written. *next is the first match past `room`, 0 for none. Exact whatever
 * puts or lets go a record: the index is the map's, kept in the same hold of
 * its lock. */
uint32_t FileMapNamed(FileMap* map, const char* word, size_t length, uint64_t from, uint64_t* out,
                      uint32_t room, uint64_t* next);

/* As FileMapFiles, of the files whose record holds Slips alone, kept as a
 * list where a record is put or let go — UINT32_MAX when the list is not
 * every one of them (it had no room), and a walk asks every file. */
uint32_t FileMapSlipped(FileMap* map, uint64_t from, uint64_t* out, uint32_t room);

/* The files in the map, and the bytes of kernel heap it holds for them: its
 * nodes, its records and the table of its name index. */
uint32_t FileMapCount(FileMap* map);
size_t   FileMapBytes(FileMap* map);

#ifdef CONFIG_VOLUME_TESTS
/* How many links the name index holds — one for the whole of the name of each
 * file in it and one for its stem, where it has one: the stand's measure that
 * no record put or let go left a link behind (stand_name_tests.c). */
uint32_t FileMapNameLinks(FileMap* map);
#endif

/* The files whose record holds Slips (tail_slip.h) — each keeps a block
 * promised to the Engross that lays its tail (block_map.h, AllotmentPromised):
 * counted wherever a file's record is put or let go, and nowhere else, so a
 * promise lasts exactly as long as a record of the map holds Slips. With no
 * lock: a word stored whole, which a take of ground reads beside the state
 * lock's counts (AllotmentPromised — never fewer than are promised); 0 for
 * NULL. */
uint64_t FileMapSlips(const FileMap* map);

#endif /* TAGFS_FILE_MAP_H */
