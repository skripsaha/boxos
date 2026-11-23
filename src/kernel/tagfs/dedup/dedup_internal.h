#ifndef TAGFS_DEDUP_INTERNAL_H
#define TAGFS_DEDUP_INTERNAL_H

#include "dedup.h"
#include "../settle/binder.h"
#include "conjugate.h"

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
 *   CommonplaceKeep    the entry of `block` under `digest` comes in anew at
 *                      walk position `since`: a block whose act was refused
 *                      for a moment — its file held by a writer — has the
 *                      book to go round again before it goes
 *   CommonplaceRetire  every entry that went, and every one the walk went
 *                      round the book past (`walked` - since >= `lap`), out;
 *                      the pass's marks cleared
 *   CommonplaceLetGo   everything let go
 */
error_t           CommonplaceTake(TagFSVolume* vol, CommonplaceEntry* in, uint64_t n, const BinderGround* gone, uint64_t ngone,
                                  uint64_t since);
CommonplaceEntry* CommonplaceGroup(TagFSVolume* vol, uint64_t digest, uint64_t* count);
CommonplaceEntry* CommonplaceAt(TagFSVolume* vol, uint64_t i);
uint64_t          CommonplaceCount(TagFSVolume* vol);
void              CommonplaceStrike(TagFSVolume* vol, uint64_t digest, uint64_t block);
void              CommonplaceKeep(TagFSVolume* vol, uint64_t digest, uint64_t block, uint64_t since);
void              CommonplaceRetire(TagFSVolume* vol, uint64_t walked, uint64_t lap);
void              CommonplaceLetGo(TagFSVolume* vol);

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
bool              DedupOwnersStep(TagFSVolume* vol, DedupOwners* w, uint32_t files);
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
DedupBatch* DedupPendingTake(TagFSVolume* vol);
void        DedupBatchLetGo(DedupBatch* b);
bool        DedupHeld(TagFSVolume* vol);
void        DedupRoomWait(TagFSVolume* vol);
void        DedupHolding(TagFSVolume* vol, uint64_t entries);
#ifdef CONFIG_VOLUME_TESTS
void        DedupPassSaid(TagFSVolume* vol, const DedupSaid* said);
#endif

bool        DedupPassTurn(TagFSVolume* vol);
void        DedupPassLetGo(TagFSVolume* vol);

/* What one volume holds of commonplace.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    CommonplaceEntry* entry;
    uint64_t n;
} CommonplaceState;

typedef enum {
    DEDUP_IDLE = 0,
    DEDUP_WALK,          /* the book's DIGESTS, a leaf a turn */
    DEDUP_OWNERS,        /* the mirror, a few files a turn */
    DEDUP_LAY,           /* an act a turn */
} DedupStep;

/* A copy and its original: the copy's file and its block of it, the
 * original's file and block, and the copy's block itself by its digest —
 * struck from Commonplace once laid. `book`: the original was read off
 * DIGESTS. */
typedef struct {
    uint64_t fid;
    uint64_t at;
    uint64_t onto;
    uint64_t onto_at;
    uint64_t digest;
    uint64_t block;
    uint32_t book;
    uint32_t pad;
} DedupPair;

typedef struct {
    DedupStep    step;
    uint64_t     tally;              /* the newest Tally whose blocks the pass weighs */
    uint64_t     budget;             /* Sheets of DIGESTS the walk may read yet */
    uint64_t     began;              /* where the walk stood when the pass's blocks came in */
    error_t      walk_rc;            /* the refusal that stopped the walk, OK for none */
    DedupMember* member;
    uint64_t     nmember;
    uint64_t     capmember;
    DedupOwners  owners;
    DedupPair*   pair;
    uint64_t     npair;
    uint64_t     ask;                /* the pair the next act begins at */
    DedupSaid    said;
    uint64_t     why[CONJUGATE_REFUSED + 1u];
} DedupPass;

/* What one volume holds of dedup_pass.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    DedupPass pass;
    uint64_t cursor;  /* the block the walk of DIGESTS reads on from */
    uint64_t walked;  /* blocks the walk went past since the mount: a lap is the volume's */
    uint64_t epoch;
} DedupPassState;

/* What one volume holds of dedup_glean.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    TagFSLock lock;
    DedupBatch* pending;  /* under dedup_glean.lock, oldest first */
    HelmWaiter service;
    volatile uint32_t listed;
    volatile uint32_t room;  /* a pass waits for the ribbon's room */
    volatile uint32_t again;  /* the room came while a turn was listed: its end lists again */
    volatile bool held;
    volatile uint64_t holding;  /* what Commonplace held at the last pass's end */
} DedupGleanState;

void DedupGleanVolumeBegin(TagFSVolume* vol);

#endif /* TAGFS_DEDUP_INTERNAL_H */
