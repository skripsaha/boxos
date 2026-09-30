#ifndef META_POOL_H
#define META_POOL_H

#include "../tagfs.h"
#include "../file_map/file_map.h"

/*
 * The metadata pool — every file's record, appended, and nothing written over.
 *
 * The pool is a chain of blocks on the medium, walked from the head the Deed
 * names. A record is appended to the TAIL, the chain's last block, which lives
 * in memory; a file's newest record is the one the map of live files names
 * (file_map/). A record is never retired in place: a file that changes gets a
 * newer record, and a file that goes gets a Quitclaim — a record without
 * TAGFS_FILE_ACTIVE — so a torn write can take a block back to what it held,
 * never destroy the only copy of a file's metadata.
 *
 * When the tail is full, or when a cycle has somebody waiting for what it
 * settles (an anchor, a shutdown), it CLOSES onto a Spare: a block taken and
 * stamped beforehand, the tail's link to it written only once a flush has kept
 * the stamp (THE SPARE RULE, scribe.h). A closed block is never written again
 * after the cycle that settles it. Only a Settle cycle writes the pool
 * (settle/): the closed blocks and the tail, after the data its records name.
 *
 * Everything below is changed under the volume's state lock, and nothing under
 * it touches the medium or the heap: an Ink (ink/) decides and edits, the
 * Settle's holder writes, and a Spare is made with no TagFS lock held.
 */

// ----------------------------------------------------------------------------
// The mount
// ----------------------------------------------------------------------------

/* The chain walked from `first_block` to the block whose link is zero, which
 * becomes the tail. 0, or -1 — said — for a chain that leaves the volume, goes
 * round, will not read, or is not a pool. */
int      meta_pool_init(uint32_t first_block);

/* The newest record of every file the file table names, into the map of live
 * files through PoolMirrorPut — the same door every Ink puts through. 0, or -1
 * — said, with the numbers — when memory cannot hold a record: the map is the
 * authority every read and every Ink stands on, and a file it cannot hold is
 * a file this mount does not have. */
int      meta_pool_mirror_init(void);

/* Everything the pool holds let go, not one byte written: what memory knows is
 * from before whatever took the volume down. With the door shut. */
void     meta_pool_shutdown(void);

void     tagfs_metadata_free(TagFSMetadata* meta);

// ----------------------------------------------------------------------------
// The mirror, and where a record sits
// ----------------------------------------------------------------------------

/* The one door into the map of live files for a file's record, for the mount
 * and for every Ink alike (the design's principle 26): an ACTIVE record becomes
 * the file's, sitting where `where` says; NULL — a Quitclaim — takes the file
 * out of the map, its row with it. OK, or ERR_NO_MEMORY when the stake did not
 * cover the insert and the heap refused, and then `rec` is let go. */
error_t  PoolMirrorPut(uint32_t fid, FileRecord* rec, const RecordWhere* where,
                       FileMapStake* stake);

/* Whether `where` can name a record of `fid` (MP-D9): its link is on the chain
 * and its offset inside the block's records — and where the block is in memory
 * (the tail, a closed block not yet written), a whole record of that file
 * begins exactly there. */
bool     RecordWhereValid(const RecordWhere* where, uint32_t fid);

/* The block of link `link`: false when the link is not on the chain. */
bool     PoolLinkBlock(uint32_t link, uint32_t* out_block);

// ----------------------------------------------------------------------------
// The tail, for an Ink under the volume's state lock
// ----------------------------------------------------------------------------

/* Bytes the tail can still take. */
uint32_t PoolTailRoom(void);

/* Whether `where` names a record of `fid` in the tail that no cycle has taken
 * yet — neither settled nor in flight — and may be replaced; its length. */
bool     PoolTailHolds(const RecordWhere* where, uint32_t fid, uint32_t* out_len);

/* That record taken out of the tail, the records after it moved down and the
 * map told where each now sits. Only after PoolTailHolds said yes. */
void     PoolTakeBack(const RecordWhere* where, uint32_t len);

/* The tail closed onto a Spare, and the Spare the new tail: false — nothing
 * changed — when no Spare may be had. The last SPARE_RESERVE Spares are had
 * only when `dig_reserve` (a Quitclaim, a cycle closing early). */
bool     PoolTailTurn(bool dig_reserve);

/* Whether a Spare may be had. */
bool     PoolSpareAvailable(bool dig_reserve);

/* A record packed for the medium (PackV1) appended to the tail, written by
 * Ink `gen`; where it now sits. The room was checked. */
RecordWhere PoolAppend(const uint8_t* packed, uint32_t bytes, uint64_t gen);

// ----------------------------------------------------------------------------
// Spares
// ----------------------------------------------------------------------------

/* ‼ HOW MANY SPARES THE POOL KEEPS FOR A DELETE, DERIVED. One block of the tail
 * holds every Quitclaim a run of deletes needs until a cycle can close it —
 * POOL_QUITCLAIMS_PER_BLOCK of them, 97 — and one more is a Glean step's (step
 * 1.8): the live records of the oldest block, at most one block's payload. The
 * same two serve a tail closed early: the first becomes the closed block's
 * next, the second the tail after it. A volume with no free block can still
 * delete, and the 98th delete waits for a Spare the Fallow gives back. */
#define POOL_QUITCLAIMS_PER_BLOCK  (TAGFS_MPOOL_DATA_SIZE / RECORD_HEADER_SIZE)
#define SPARE_FOR_QUITCLAIMS       ((POOL_QUITCLAIMS_PER_BLOCK * RECORD_HEADER_SIZE + \
                                     TAGFS_MPOOL_DATA_SIZE - 1) / TAGFS_MPOOL_DATA_SIZE)
#define SPARE_FOR_GLEAN            ((TAGFS_MPOOL_DATA_SIZE + TAGFS_MPOOL_DATA_SIZE - 1) / \
                                    TAGFS_MPOOL_DATA_SIZE)
#define SPARE_RESERVE              (SPARE_FOR_QUITCLAIMS + SPARE_FOR_GLEAN)

/* One Spare made: a block taken, stamped empty, the stamp's need noted, and
 * handed to the pool with the room its link will take. With no TagFS lock
 * held — a file's Helm may be. OK, or the refusal: ERR_TAGFS_NO_SPACE, ERR_WOULD_BLOCK
 * when Fallow rests blocks a cycle would give back, ERR_NO_MEMORY, or the
 * medium's refusal of the stamp. */
error_t  PoolSpareMake(void);

/* Spares made until the pool holds `want` — the reserve after a mount and
 * after every cycle. With no TagFS lock held. The first refusal ends it. */
error_t  PoolSpareRefill(uint32_t want);

/*
 * A Spare made by a maker that may not stand over the medium — an
 * asynchronous write's continuation — in three steps. PoolSpareBegin takes a
 * block on what memory knows of the map and lays its empty stamp in an image
 * of its own: OK, the allocator's refusal as it is (ERR_WOULD_BLOCK while
 * Fallow rests blocks, ERR_TAGFS_NO_SPACE), or ERR_NO_MEMORY. The maker writes
 * the image to the block its own way; once the write completed it hands both
 * over with the flush need read after the completion (PoolSpareAdopt: OK, or
 * ERR_NO_MEMORY with the block given back and the image freed), or gives them
 * back (PoolSpareDrop). With no state lock held.
 */
error_t  PoolSpareBegin(uint32_t* out_block, MetaPoolBlock** out_image);
error_t  PoolSpareAdopt(uint32_t block, MetaPoolBlock* image, uint64_t need);
void     PoolSpareDrop(uint32_t block, MetaPoolBlock* image);

typedef struct MetaPoolPage MetaPoolPage;

/* Under the state lock, in a volume's last cycle: every Spare no link names
 * given back to the allocator, so no clean shutdown leaves a stamped block
 * that nothing owns. Their pages come back, for PoolPagesFree after the lock. */
MetaPoolPage* PoolSparesGiveBack(void);

// ----------------------------------------------------------------------------
// The Settle cycle
// ----------------------------------------------------------------------------

/* What one cycle writes of the pool, taken at S0 (PoolSnapTake): the closed
 * blocks from `first` to `upto`, and the tail's image, copied into the
 * Settle's own buffer, when it changed since the medium took it. */
typedef struct {
    MetaPoolPage* first;       /* the oldest closed block this cycle writes, or NULL */
    MetaPoolPage* upto;        /* ... and the newest */
    uint32_t      tail_block;
    bool          tail_write;
    uint64_t      tail_edits;  /* the tail's edits at S0: unchanged at S6, the medium has it */
    uint64_t      link_need;   /* the stamps the written links name need a kept flush before them */
    uint64_t      gen_max;     /* the newest record in the closed blocks it writes */
} PoolSnap;

/* S0, under the state lock. `close_tail` closes a tail holding records onto a
 * Spare first — the anchor's promise: what a cycle with a waiter settles lies
 * in a block never written again. */
void     PoolSnapTake(PoolSnap* snap, bool close_tail);

/* S3, with no TagFS lock held: the closed blocks in chain order, then the
 * tail. OK, or the first refusal — said. Every write is the same bytes a
 * retry writes again. */
error_t  PoolSnapWrite(const PoolSnap* snap);

/* S6, under the state lock: the closed blocks the cycle wrote are settled and
 * leave memory — their buffers come back in *spent, for PoolPagesFree after
 * the lock — and the newest record now in a settled closed block is answered. */
uint64_t PoolSnapSettled(const PoolSnap* snap, MetaPoolPage** spent);

/* Pages PoolSnapSettled spent, freed with no TagFS lock held. */
void     PoolPagesFree(MetaPoolPage* list);

/* Under the state lock: the blocks the pool holds that are not files' — its
 * chain in memory, and its Spares. */
uint64_t PoolBlocksHeld(void);

/* Under the state lock: the gen of the newest record the tail holds; 0 when
 * it holds none. For the stand. */
uint64_t PoolTailGen(void);

#endif // META_POOL_H
