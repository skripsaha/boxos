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
 *   its closed blocks borne out and its open block not, and the other copy's
 *     open block is one of those closed blocks — the open block was torn as it
 *     was written (decision 5, the owner's B3): the pool stands on the newest
 *     seal and rolls back its open block alone, loudly, and mounts writable;
 *     standing on the other seal would lose every closed block the newest
 *     cycle settled;
 *   not borne out, and the other copy's seal is — its closed part not borne
 *     out, or both seals share one open block: the newest cycle was cut
 *     before its flush, or the medium said a flush was kept that was not, and
 *     the volume stands on the cycle before, the walk made again up to its
 *     `to`. A seal whose chain begins at another block — the newest cycle's
 *     Glean took blocks off the front — is judged by that walk from its own
 *     first block, since the first walk never met them;
 *   neither, and a seal's closed part is borne out while its open block is not
 *     — both seals' open block torn: it rolls back as above;
 *   the newest seal alone, its open block not borne out — the other copy was
 *     written by a kernel before the seal, which wrote that block in place: the
 *     chain is read whole as such a kernel reads it, since rolling the block
 *     back would lose the records that kernel kept there;
 *   no seal's closed part borne out — a kernel before the seal retired records
 *     in closed blocks in place, or the blocks rotted: the chain is read whole
 *     as that kernel left it (no seal: the same), loudly, with nothing rolled
 *     back.
 *
 * A mount that rolls the newest cycle back — falling back, rolling an open
 * block back, or the newest seal alone not borne out — marks the medium as one
 * that once held less than a seal (SETTLE_MEDIUM_LIED): format v1 cannot tell
 * a cut before the flush from a flush the medium said it kept and did not. A
 * pool whose closed blocks no seal bears out is rot or a kernel before the
 * seal, and marks nothing.
 *
 * A block the medium refuses to read is no verdict on any seal: a refusal says
 * nothing of what the block holds, so the mount is refused as a read the
 * medium refused, and asked again when the seat answers.
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
    bool      lied;            /* the newest cycle rolled back: SETTLE_MEDIUM_LIED from now on */
    uint32_t  torn_block;      /* POOL_STOOD_TORN_TAIL: the open block rolled back, and the */
    uint32_t  torn_end;        /*   byte its rolled-back records ended at, its header counted */
    uint32_t  top_fid;         /* the highest file id a record that checks out names — past the seal too */
    uint32_t  top_tag;         /* the highest tag id a record of the span wears */
    uint32_t  from_block;      /* where the records of the next seal's cycle begin */
    uint32_t  from_bytes;
    uint32_t  unreadable;      /* records of the span that do not check out */
    uint32_t* last_span;       /* the files the stood seal's own cycle wrote records of */
    uint32_t  last_span_count;
    uint32_t* gone;            /* the files whose newest record is a Quitclaim: their rows go too */
    uint32_t  gone_count;
} PoolMountOutcome;

/* The walk above, into the map of live files. OK; ERR_TAGFS_CORRUPTED — said —
 * for a chain that cannot be read as far as any seal or the Deed's head
 * needs; the read's own error when the medium refused one, which no seal is
 * judged on — a sector it answered it cannot read is no refusal, and is
 * judged by where it lies (meta_pool_mount.c WalkBadSector); and
 * ERR_NO_MEMORY for records memory cannot hold: a map held in
 * part is a mount that lost files. `out->last_span` and `out->gone` are the
 * caller's to kfree. */
error_t  PoolMount(const PoolMountAsk* ask, PoolMountOutcome* out);

#ifdef CONFIG_VOLUME_TESTS
/* The seals held against the medium the way PoolMount holds them, reading
 * only: nothing the mounted volume holds in memory is touched. Which the mount
 * would stand on (POOL_STOOD_*), the seal's index in *out_seal (-1: none),
 * whether it would say it fell back, and whether it would mark the medium
 * SETTLE_MEDIUM_LIED. For the stand. */
PoolStood PoolSealsJudge(const PoolMountAsk* ask, int* out_seal, bool* out_fell_back,
                         bool* out_lied);
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
 * cover the insert and the heap refused, and then `rec` is let go. Every link
 * of the chain counts the bytes of the records the map names in it, and the
 * door keeps the count: what a record leaves, what one comes to — and, for the
 * link a record leaves, the newest link a record that took its place lies in,
 * the Quitclaim's included (PoolFront.replaced_by). `row_block`, when it is not
 * 0, is the block of the file table's row naming the record — set with it, in
 * one hold of the map (FileMapRecordPut): a new file's. */
error_t  PoolMirrorPut(uint64_t fid, FileRecord* rec, const RecordWhere* where,
                       uint32_t row_block, FileMapStake* stake);

/* The file out of the map — its record and its row there — through the same
 * door, nothing taking its record's place; the file table is the caller's to
 * tell. */
void     PoolMirrorDrop(uint64_t fid);

/* The file's record, where `from` says, laid again where `to` says (Glean):
 * the map, the file's row and the links' counts follow. False, and nothing
 * changed, when the file's record is not the one at `from`. */
bool     PoolMirrorMove(uint64_t fid, const RecordWhere* from, const RecordWhere* to);

/* Whether `where` can name a record of `fid` (MP-D9): its link is on the chain
 * and its offset inside the block's records — and where the block is in memory
 * (the tail, a closed block not yet written), a whole record of that file
 * begins exactly there. */
bool     RecordWhereValid(const RecordWhere* where, uint64_t fid);

/* The block of link `link`: false when the link is not on the chain. */
bool     PoolLinkBlock(uint32_t link, uint32_t* out_block);

// ----------------------------------------------------------------------------
// The tail, for an Ink under the volume's state lock
// ----------------------------------------------------------------------------

/* Bytes the tail can still take. */
uint32_t PoolTailRoom(void);

/* Whether `where` names a record of `fid` in the tail that no cycle has taken
 * yet — neither settled nor in flight — and may be replaced; its length. */
bool     PoolTailHolds(const RecordWhere* where, uint64_t fid, uint32_t* out_len);

/* That record taken out of the tail, the records after it moved down and the
 * map told where each now sits. Only after PoolTailHolds said yes. */
void     PoolTakeBack(const RecordWhere* where, uint32_t len);

/* The tail closed onto a Spare, and the Spare the new tail, leaving the pool
 * at least `keep` Spares (the reserve below): false — nothing changed — when
 * no Spare may be had. */
bool     PoolTailTurn(uint32_t keep);

/* A record packed for the medium (PackV1) appended to the tail, written by
 * Ink `gen`; where it now sits. The room was checked. */
RecordWhere PoolAppend(const uint8_t* packed, uint32_t bytes, uint64_t gen);

// ----------------------------------------------------------------------------
// Spares
// ----------------------------------------------------------------------------

/*
 * ‼ HOW MANY SPARES THE POOL KEEPS, AND WHO MAY TAKE WHICH, DERIVED. The
 * reserve is two shares, Glean's beneath the deletes':
 *
 *   Glean's, SPARE_FOR_GLEAN — what one step of Glean (glean/) needs from the
 *     worst tail anything leaves it, one a run of deletes filled: the live
 *     records a step moves cannot land there, so the step turns it (1); the
 *     front may be the Deed's head, whose live records — a block's payload at
 *     most — fill a block that gives nothing back, the head staying the
 *     pool's (RT-08) (1); and the next front's, a block's payload at most,
 *     land in a block the cycle closes before its seal lets their source go
 *     (1). Three, and the step gives a block back;
 *   the deletes', SPARE_FOR_QUITCLAIMS — one block of the tail holds every
 *     Quitclaim a run of deletes needs until a cycle can close it,
 *     POOL_QUITCLAIMS_PER_BLOCK of them, 97.
 *
 * Every taker leaves the pool a floor. While the volume has a free block it is
 * the whole reserve, and a change makes a Spare of its own past it. With none,
 * a record that frees nothing — a write's, a name's, a tag's, a new file's —
 * still turns the tail only onto a Spare past the whole reserve; a Quitclaim
 * that frees nothing, and a tail a cycle closes early for a waiter, onto one
 * past Glean's share; a change that frees ground — a delete of a file that
 * holds some, a write that lets blocks go — leaves the one Spare the close at
 * S0 its ground needs before it can rest (fallow.h): that ground is what makes
 * the reserve whole again. Glean's own step in a sweep for the deletes, the
 * close a cycle makes for ground resting on the open tail, and the halt's last
 * cycle dig to the bottom; Glean at any other pass takes only Spares it made
 * past the reserve, which is not for room a write is short of.
 *
 * ‼ NOTHING LEAVES THE CLOSE OR GLEAN'S NEXT STEP SHORT. On a volume with no
 * free block, a delete that frees no ground — an empty file's, whose Quitclaim
 * only fills the tail — has its room made by Glean alone, which takes dead
 * blocks off the chain's front. So below Glean's share no change takes the
 * tail's room while the pool would be left without the Spare of the close
 * that lets what the tail holds go, and none that frees nothing while what is
 * left would not cover the step Glean takes next (GleanStepNeedLocked): it
 * waits instead. Below Glean's share, Glean or a close for ground spent part
 * of it, and what they let go rests in Fallow on its way back.
 *
 * ‼ THE RESERVE IS MADE WHOLE FIRST FROM WHAT COMES BACK. Ground a cycle
 * gives back on a full volume would go straight to a write waiting for room,
 * and a reserve made only at a cycle's steps would never be whole again. So
 * while the pool holds less than SPARE_RESERVE, a take that is not the pool's
 * own Spare leaves free the blocks the reserve still lacks (PoolSparesOwed).
 */
#define POOL_QUITCLAIMS_PER_BLOCK  (TAGFS_MPOOL_DATA_SIZE / RECORD_HEADER_SIZE)
#define SPARE_FOR_QUITCLAIMS       ((POOL_QUITCLAIMS_PER_BLOCK * RECORD_HEADER_SIZE + \
                                     TAGFS_MPOOL_DATA_SIZE - 1) / TAGFS_MPOOL_DATA_SIZE)
#define SPARE_FOR_GLEAN            (1 + 2 * ((TAGFS_MPOOL_DATA_SIZE + TAGFS_MPOOL_DATA_SIZE - 1) / \
                                             TAGFS_MPOOL_DATA_SIZE))
#define SPARE_RESERVE              (SPARE_FOR_QUITCLAIMS + SPARE_FOR_GLEAN)

/* Under the state lock: the Spares the reserve lacks — the blocks a take that
 * is not the pool's own Spare leaves free for it; none while no pool is up. */
uint32_t PoolSparesOwed(void);

/* One Spare made: a block taken — the ground every other take leaves for the
 * reserve among what it may have — stamped empty, the stamp's need noted, and
 * handed to the pool with the room its link will take. With no TagFS lock
 * held — a file's Helm may be. OK, or the refusal: ERR_TAGFS_NO_SPACE, ERR_WOULD_BLOCK
 * when Fallow rests blocks a cycle would give back, ERR_NO_MEMORY, or the
 * medium's refusal of the stamp. */
error_t  PoolSpareMake(void);

/* Spares made until the pool holds `want` — the reserve at a mount and in
 * every cycle (settle/). With no TagFS lock held. The first refusal ends it. */
error_t  PoolSpareRefill(uint32_t want);

/* Under the state lock: how many Spares the pool holds, and the blocks of up
 * to `cap` of them into `out` (NULL for the count alone). */
uint32_t PoolSparesHeld(uint32_t* out, uint32_t cap);

/*
 * ‼ A BLOCK NO LINK NAMES IS FREE ON THE MEDIUM. A Spare is taken in memory
 * the moment it is made, and named on the medium only by the link of the
 * block that closes onto it — a cycle's S3. Until then the map's sector goes
 * out leaving it free: a volume cut by the power finds no stamped block owned
 * by nothing, and gives the ground straight back to its allocator. A Spare
 * stops being left out at the S0 of the cycle whose S3 writes the link naming
 * it — PoolSnapTake owes the medium its sector again (BlockMapOweLocked) — so
 * the map that cycle's S1 writes holds it taken before the link lands.
 *
 * Under the state lock, by the block map as a sector of it goes out: the bit
 * of every such block in [first, first + count) — the Spares in the stock and
 * the tails turned onto one since the last S0 — cleared in `bits`, which
 * covers exactly that run. How many were cleared.
 */
uint32_t PoolUnnamedLeftOut(uint64_t first, uint32_t count, uint8_t* bits);

/*
 * A Spare made by a maker that may not stand over the medium — an
 * asynchronous write's continuation — in three steps. PoolSpareBegin takes a
 * block on what memory knows of the map, as PoolSpareMake's take may have it,
 * and lays its empty stamp in an image
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
 * that nothing owns. Their pages come back, for PoolPagesFree after the lock.
 * One whose give the block map has no room held to write down stays in the
 * stock, said — free on the medium all the same (PoolUnnamedLeftOut) — for
 * the next of the last cycles to give back. */
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
    MetaPoolPage* next;        /* the next closed block S3 writes; NULL once past `upto` */
    uint32_t      tail_block;
    bool          tail_write;
    bool          tail_written;
    bool          unclosed;    /* a close was owed and no Spare was there to close onto */
    uint64_t      tail_edits;  /* the tail's edits at S0: unchanged at S6, the medium has it */
    uint64_t      link_need;   /* the stamps the written links name need a kept flush before them */
    uint64_t      gen_max;     /* the newest record in the closed blocks it writes */
} PoolSnap;

/* S0, under the state lock. Every record up to Ink `close_upto` is to lie in a
 * closed block — the anchor's promise: what a cycle with a waiter settles lies
 * in a block never written again; and Glean's: a record it moved, or the
 * record that replaced a front's last live one, lies there before the seal
 * lets its source go — so a tail holding one is closed onto a Spare first; 0
 * closes nothing. The close leaves the pool `keep` Spares: Glean's share for
 * a waiter's close; none for Glean's own, which its step held the Spare for,
 * for the machine's last cycle, whose S0 gives every Spare back right after
 * (PoolSparesGiveBack), and for a cycle asked for ground a record in the open
 * tail freed — ground that comes back only once a block holding that record
 * is closed (fallow.h), and one no other close makes while the tail has room.
 * `unclosed` says a close was owed and no Spare could be had: the waiter is
 * answered once its records settled in the open tail, and that is said once
 * until a close is made again. The links this snapshot writes stop leaving
 * the blocks they name out of the map (PoolUnnamedLeftOut). */
void     PoolSnapTake(PoolSnap* snap, uint64_t close_upto, uint32_t keep);

/* S3, with no TagFS lock held, one command at a time: the next closed block in
 * chain order, then the tail. OK with *done once nothing of the snapshot is
 * left to write, or the refusal — said. Every write is the same bytes a retry
 * writes again. */
error_t  PoolSnapWriteNext(PoolSnap* snap, bool* done);

/* The newest Ink whose record lies in a closed block a cycle settled — what a
 * waiter owed a closed block is answered by. */
uint64_t PoolClosedSettledGen(void);

/* Under the state lock: the newest Ink whose record lies in a closed block,
 * settled or not — every record newer lies in the open tail. */
uint64_t PoolClosedGenMax(void);

/* Whether a closed block waits for a cycle — the tail filled and turned. A
 * read with no lock, for the cadence. */
bool     PoolClosedWaiting(void);

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
 * chain in memory, its Spares, and the Deed's head once the chain begins past
 * it. */
uint64_t PoolBlocksHeld(void);

// ----------------------------------------------------------------------------
// Glean's view of the chain (glean/), under the state lock
// ----------------------------------------------------------------------------

/* The blocks of the chain, the tail's included; and how many blocks the
 * records the map of live files names would fill, packed. */
uint32_t PoolChainBlocks(void);
uint32_t PoolLiveBlocks(void);

/* The bytes of every record that stopped being its file's newest — replaced
 * by a newer one, or its file deleted — counted from boot and never down; a
 * record Glean lays again is the same record, and is not counted. */
uint64_t PoolDiedBytes(void);

/* The block the chain begins at; and whether `block` is one of its links. */
uint32_t PoolChainFirst(void);
bool     PoolChainHolds(uint32_t block);

/* A link at the chain's front as Glean may take it: a closed block a settled
 * cycle wrote — never the tail, never one no cycle has written yet. */
typedef struct {
    uint32_t block;
    uint32_t link;
    uint32_t live;          /* the bytes of its records the map names */
    uint32_t next;          /* the block after it — what its next_block says */
    uint32_t mix;           /* its part in the seal's fold */
    uint32_t replaced_by;   /* the newest link a record that replaced one of its own lies in */
} PoolFront;

/* The i-th link from the chain's front, when Glean may take it. */
bool     PoolFrontLink(uint32_t i, PoolFront* out);

/* The i-th link from the chain's front, closed — written by a cycle or not
 * yet: what Glean will meet there once it may (GleanStepNeedLocked). */
bool     PoolChainLink(uint32_t i, PoolFront* out);

/* The open tail's link: every link before it is closed. */
uint32_t PoolTailLink(void);

/* Whether `img`, read off the medium, is the block the front link says it is:
 * its records sum to the link's mix. */
bool     PoolFrontReadsTrue(const PoolFront* front, const MetaPoolBlock* img);

/* The `count` links at the chain's front off the chain: their mixes out of
 * the seal's fold, and the chain begins at the link after them. */
void     PoolUnlinkFront(uint32_t count);

/* Under the state lock: the gen of the newest record the tail holds; 0 when
 * it holds none — what a close a sweep buys is owed up to (glean.h), and the
 * stand's. */
uint64_t PoolTailGen(void);

#endif // META_POOL_H
