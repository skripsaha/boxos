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

/*
 * ‼ THE POOL IS WALKED ONCE, AND THE SEAL SAYS HOW FAR.
 *
 * The mount's walk reads each block of the chain once and puts every record
 * into the map of live files as it goes — the newest of a file's records wins,
 * because the walk is in append order (principle 26: through PoolMirrorPut,
 * the door every Ink uses) — while it holds each block against the seals the
 * Ledger's copies carry (volume_ledger.h, SettleMark):
 *
 *   the newest copy's seal borne out — its closed blocks fold to closed_crc32
 *     and its open block's records to tail_crc32 — the pool is read up to its
 *     `to` and not a byte further: what lies past it is a cycle that did not
 *     settle. One walk, the common mount;
 *   not borne out, and the other copy's seal is — the newest cycle was cut
 *     before its flush, or the medium said a flush was kept that was not: the
 *     volume stands on the cycle before, the walk is made again up to its
 *     `to`, and the medium is marked as one that once held less than a seal
 *     (SETTLE_MEDIUM_LIED);
 *   neither, and a seal's closed part is borne out while its open block is not
 *     — the open block was torn as it was rewritten (decision 5): the pool
 *     rolls back to its last closed block, loudly, and mounts writable;
 *   no seal's closed part borne out — a kernel before the seal retired records
 *     in closed blocks in place, or the blocks rotted: the chain is read whole
 *     as that kernel left it (no seal: the same), loudly, with nothing rolled
 *     back.
 *
 * Only a walk that is made again, after a cut or damage, reads a block twice.
 */
typedef enum {
    POOL_STOOD_SEAL = 1,       /* on the seal it names, borne out */
    POOL_STOOD_TORN_TAIL,      /* on the seal it names, its open block rolled back */
    POOL_STOOD_NO_SEAL,        /* the whole chain, as a kernel before the seal reads it */
} PoolStood;

typedef struct {
    const SettleMark* seal[VOLUME_LEDGER_COPIES];  /* [0] the newest copy's, [1] the other's; NULL: none */
    uint32_t          head;                        /* the Deed's head: where a walk with no seal begins */
} PoolMountAsk;

typedef struct {
    PoolStood stood;
    int       seal;            /* the seal stood on: an index into ask->seal, or -1 */
    bool      fell_back;       /* the newest copy's seal was not borne out, the other's was */
    uint32_t  torn_bytes;      /* POOL_STOOD_TORN_TAIL: the records the torn open block held */
    uint32_t  top_fid;         /* the highest file id any record walked names — past the seal too */
    uint32_t  top_tag;         /* the highest tag id a record of the span wears */
    uint32_t  from_block;      /* where the records of the next seal's cycle begin */
    uint32_t  from_bytes;
    uint32_t  unreadable;      /* records of the span that do not check out */
    uint32_t* last_span;       /* the files the stood seal's own cycle wrote records of */
    uint32_t  last_span_count;
} PoolMountOutcome;

/* The walk above, into the map of live files. OK; or ERR_TAGFS_CORRUPTED —
 * said — for a chain that cannot be read as far as any seal or the Deed's head
 * needs, and ERR_NO_MEMORY for records memory cannot hold: a map held in part
 * is a mount that lost files. `out->last_span` is the caller's to kfree. */
error_t  PoolMount(const PoolMountAsk* ask, PoolMountOutcome* out);

#ifdef CONFIG_VOLUME_TESTS
/* The seals held against the medium the way PoolMount holds them, reading
 * only: nothing the mounted volume holds in memory is touched. Which the mount
 * would stand on (POOL_STOOD_*), the seal's index in *out_seal (-1: none), and
 * whether it would say it fell back. For the stand. */
PoolStood PoolSealsJudge(const PoolMountAsk* ask, int* out_seal, bool* out_fell_back);
#endif

/* The mount's census: the chain's blocks — and the Deed's head, which the pool
 * owns whether or not the chain still begins there — claimed. */
void     PoolClaimGround(uint32_t deed_head);

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

/* S0, under the state lock, after PoolSnapTake: the pool's part of the
 * cycle's seal (volume_ledger.h) — where the chain begins, the open block and
 * the bytes of its records, the fold of the closed blocks and the CRC32 of the
 * open one's records: what the medium holds once the cycle's S3 is done. */
void     PoolSealTake(SettleMark* mark);

/* Under the state lock: the blocks the pool holds that are not files' — its
 * chain in memory, and its Spares. */
uint64_t PoolBlocksHeld(void);

/* Under the state lock: the gen of the newest record the tail holds; 0 when
 * it holds none. For the stand. */
uint64_t PoolTailGen(void);

#endif // META_POOL_H
