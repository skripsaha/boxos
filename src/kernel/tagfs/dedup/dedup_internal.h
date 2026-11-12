#ifndef TAGFS_DEDUP_INTERNAL_H
#define TAGFS_DEDUP_INTERNAL_H

#include "dedup.h"
#include "../settle/binder.h"

/*
 * What Dedup's files share (dedup.h): Commonplace, the index of the blocks
 * written since a Tally by their digests (commonplace.c), and the walk of the
 * mirror that names who holds a block (dedup_owners.c). Both are the
 * service's alone — one turn of it runs at a time — and live as long as the
 * volume's mount (DedupLetGo).
 */

/* A block written since a Tally, by its digest: `since` where the walk of the
 * book's DIGESTS stood, in blocks walked since the mount, when it came in. */
typedef struct {
    uint64_t digest;
    uint64_t block;
    uint64_t since;
    uint32_t fresh;     /* came in with the pass under way */
    uint32_t mark;      /* COMMONPLACE_*: what the pass under way did with its digest's entries */
} CommonplaceEntry;

#define COMMONPLACE_IN    1u    /* the digest's entries are among the pass's matches */
#define COMMONPLACE_OLD   2u    /* ... with a block the walk read off DIGESTS */
#define COMMONPLACE_GONE  4u    /* laid, or its block let go: out at the next retire */

/*
 * Commonplace (commonplace.c), sorted by digest, then block:
 *
 *   CommonplaceTake    `n` blocks written since the Tally before, by block
 *                      (`in`, their entries made), come in fresh at walk
 *                      position `since`: an entry on ground the files let go
 *                      (`gone`, `ngone` runs in order) or on a block that came
 *                      in again — let go and taken anew, its bytes new — goes.
 *                      OK, or ERR_NO_MEMORY with Commonplace as it was
 *   CommonplaceGroup   the entries of `digest`, *count of them, or NULL
 *   CommonplaceAt      the `i`-th entry, i < CommonplaceCount()
 *   CommonplaceStrike  the entry of `block` under `digest` goes at the next
 *                      retire: laid
 *   CommonplaceRetire  every entry that went, and every one the walk went
 *                      round the book past (`walked` - since >= `lap`), out;
 *                      the pass's marks cleared
 *   CommonplaceLetGo   everything let go
 */
error_t           CommonplaceTake(CommonplaceEntry* in, uint64_t n, const BinderGround* gone, uint64_t ngone,
                                  uint64_t since);
CommonplaceEntry* CommonplaceGroup(uint64_t digest, uint64_t* count);
CommonplaceEntry* CommonplaceAt(uint64_t i);
uint64_t          CommonplaceCount(void);
void              CommonplaceStrike(uint64_t digest, uint64_t block);
void              CommonplaceRetire(uint64_t walked, uint64_t lap);
void              CommonplaceLetGo(void);

/* A block the pass matched by digest: a block of Commonplace, or one the walk
 * read off DIGESTS (`old`). */
typedef struct {
    uint64_t digest;
    uint64_t block;
    uint32_t old;
    uint32_t pad;
} DedupMember;

/* Who names a block, as the walk of the mirror found it: the file of the name
 * chosen — a DATA run over a SHARED one, else the first met — its block of
 * the file there and the stitch its run was born in; and what bars the block,
 * by any name of it. */
#define DEDUP_NO_OWNER  UINT32_MAX
#define DEDUP_BAR_ONTO  1u          /* nothing is laid on it */
#define DEDUP_BAR_COPY  2u          /* it is laid on nothing */

typedef struct {
    uint64_t block;
    uint64_t fid;
    uint64_t at;
    uint64_t birth;
    uint32_t kind;                  /* the name's Strip kind, or DEDUP_NO_OWNER */
    uint32_t bars;
} DedupOwner;

typedef struct {
    DedupOwner* owner;              /* by block, one each */
    uint64_t    n;
    uint64_t    from;               /* the next file id the walk asks the map for; UINT64_MAX once done */
    uint32_t    off_tag;            /* the id of dedup:off, or TAGFS_INVALID_TAG_ID */
} DedupOwners;

/*
 * The walk of the mirror (dedup_owners.c):
 *
 *   DedupOwnersBegin  the blocks of `n` members (by digest) to be named, one
 *                     each, in block order: OK, or ERR_NO_MEMORY
 *   DedupOwnersStep   up to `files` live files walked: true once every one is
 *   DedupOwnerOf      the owner of `block`, or NULL for a block not asked
 *   DedupOwnersLetGo  everything let go
 */
error_t           DedupOwnersBegin(DedupOwners* w, const DedupMember* m, uint64_t n);
bool              DedupOwnersStep(DedupOwners* w, uint32_t files);
const DedupOwner* DedupOwnerOf(const DedupOwners* w, uint64_t block);
void              DedupOwnersLetGo(DedupOwners* w);

/* What a Tally's DONE hands the service (dedup_glean.c): the blocks the files
 * took since the Tally before and hold, with their digests, in block order —
 * `n` entries made — the ground they let go meanwhile, and the Sheets of
 * DIGESTS the walk may read for it. */
typedef struct DedupBatch {
    struct DedupBatch* next;
    uint64_t           tally;
    uint64_t           budget;
    CommonplaceEntry*  entry;
    uint64_t           n;
    BinderGround*      gone;
    uint64_t           ngone;
} DedupBatch;

/*
 * The service's own (dedup_glean.c), for the pass (dedup_pass.c):
 *
 *   DedupPendingTake  every batch handed over, oldest first, taken: NULL for
 *                     none
 *   DedupBatchLetGo   a batch let go
 *   DedupHeld         whether DedupHold holds the pass down
 *   DedupRoomWait     the pass waits for the ribbon's room: a cycle demanded,
 *                     and the service listed again at its DONE (DedupWeigh)
 *   DedupHolding      what Commonplace holds, published at a pass's end: a
 *                     Tally that took nothing in begins a pass only while the
 *                     walk has blocks to go round the book for
 *   DedupPassSaid     the stand's: what a pass said, summed
 *
 * And the pass's:
 *
 *   DedupPassTurn     a turn inside the door: whether the service is listed
 *                     again
 *   DedupPassLetGo    everything the pass and Commonplace hold let go, the
 *                     walk's place with them — a mount begun, the door shut
 */
DedupBatch* DedupPendingTake(void);
void        DedupBatchLetGo(DedupBatch* b);
bool        DedupHeld(void);
void        DedupRoomWait(void);
void        DedupHolding(uint64_t entries);
#ifdef CONFIG_VOLUME_TESTS
void        DedupPassSaid(const DedupSaid* said);
#endif

bool        DedupPassTurn(void);
void        DedupPassLetGo(void);

#endif /* TAGFS_DEDUP_INTERNAL_H */
