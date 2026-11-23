#ifndef TAGFS_SETTLE_BINDER_INTERNAL_H
#define TAGFS_SETTLE_BINDER_INTERNAL_H

#include "binder.h"
#include "dog_ear.h"
#include "foliation.h"
#include "tip_in.h"

/*
 * The Binder, between its own files (binder.h): the Tally under way, which
 * binder.c runs a step at a time, and what one of its files lends another —
 * included by binder*.c alone, so no file outside the Binder sees a name of
 * it:
 *
 *   binder_items.c  the elements of each tree, for binder_nodes.c to pack
 *   binder_kept.c   the nodes a Tally names as they lie: TipIn's sifting of
 *                   the Tally before into them, and the walk's cursors in them
 *   binder_sift.c   TipIn's sifting of one tree, where it changed
 *   binder_digests.c  DIGESTS laid in: the leaves it lays read, the ground it
 *                   speaks for, the reads' table its DONE turns them to
 *   binder_slice.c  the slice and the ground DIGESTS speaks for, for TALLY_TAKE
 *   binder_done.c   what TAKE makes for a Tally's end, its DONE, and a slot
 *                   whose write was refused
 *   binder_check.c  a whole Tally held to the one before it, element by
 *                   element outside DogEar — a STRICT kernel's and the stand's
 */

/* binder_items.c: whether key `a` comes before `b` in `tree`; whether element
 * key `k` lies inside the range of node `r` named as it lies; the next element
 * of the tree's leaves, and of the level above the leaves being laid — the
 * level below's next node or node named as it lies — *got false at its end.
 * OK, ERR_NO_MEMORY, a refusal of a DIGESTS leaf the Tally stood on, or
 * ERR_TAGFS_CORRUPTED. */
bool        BinderKeyLess(uint64_t tree, TallyKey a, TallyKey b);
bool        BinderKeptCovers(const BinderKept* r, TallyKey k);
error_t     BinderLeafItemNext(TagFSVolume* vol, BinderWork* w, BinderItem* it, bool* got);
error_t     BinderInnerItemNext(TagFSVolume* vol, BinderWork* w, BinderItem* it, bool* got);

/* binder_items.c: the next DIGESTS element of ground `g` (`n` runs, in order)
 * from where `at` stands, its body in `buf` (BINDER_DIGEST_ITEM_BYTES) —
 * *got false at its end; OK, or a refusal of a DIGESTS leaf the Tally stood
 * on. */
#define BINDER_DIGEST_ITEM_BYTES  (10u + 8u * (uint32_t)DIGEST_ITEM_MOST)
error_t     BinderDigestsElement(TagFSVolume* vol, BinderWork* w, const BinderGround* g, uint64_t n, BinderDigestAt* at, uint8_t* buf,
                                 BinderItem* it, bool* got);

/* A run of file ids [from, to) the slice takes: UINT64_MAX `to` for none. */
typedef struct {
    uint64_t from;
    uint64_t to;
} BinderFids;

/* One tree of the Tally before as TipIn sifts it (binder_sift.c): the leaves
 * it is handed — the tree's first and every child of a node of level 1 that
 * changed — each with its place among the tree's leaves, every inner node, and
 * the fate of each (tip_in.h). */
typedef struct {
    uint64_t   tree;
    TipInNode* leaf;
    uint32_t*  at;
    TipInFate* lf;
    uint32_t   n;
    TipInNode* inner;
    TipInFate* inf;
    uint32_t   ninner;
} BinderSift;

/* The dry count of a Tally laid in (§16.5 п.16): what laying it in would lay —
 * the nodes laid and offered, at the size they had, a leaf laid that keeps no
 * key of the state at none (§16.6 п.6) — and what laying it whole would, every
 * node of the trees sifted but those leaves; and every node of them, each at
 * the size it had: a Tally before with none is laid in, never weighed. */
typedef struct {
    uint64_t tip;
    uint64_t whole;
    uint64_t sifted;
} TipInCount;

/* A DIGESTS leaf TipIn lays or offers, read: a run of blocks one of its
 * elements names (`leaf` the sifted leaf it is of). */
typedef struct {
    uint64_t start;
    uint64_t count;
    uint32_t leaf;
} BinderOldRun;

/* The Tally under way. */
typedef struct {
    bool        active;
    TallyStep   step;
    const char* stage;
    BinderWork  w;
    uint64_t    target;         /* every Ink the stitch settled — what a SETTLECUT asks it covers */
    uint64_t    k;              /* the stitch it is the state after, and its Clasp */
    uint32_t    clasp;
    SpineRibbon ribbon;
    SpineCounts counts;
    uint64_t    made_ns;
    uint64_t    useful_at;      /* the useful bytes up to k */
    uint64_t    used;           /* the blocks the nodes took in each run */
    uint32_t    slot;           /* the slot written over */
    uint64_t    slot_seq;
    uint64_t    chain_at;       /* the chain's index of the Quire `at` lies in */
    bool        sliced;         /* TALLY_TAKE: the slice taken, ... */
    bool        rots_read;      /* ... every DIGESTS leaf weighed, from rot_leaf on */
    uint32_t    rot_leaf;
    bool        turned;         /* TALLY_DONE: the reads of a digest turned to it — or left, said — ... */
    bool        captured;       /* ... and the Gloss may let go of what its DIGESTS names */
    bool        taken;          /* the runs at A and B are taken from the map */
    bool        slot_begun;     /* a write of the slot was issued: it may hold either Tally */
    SlotSealed* sealed;         /* the slot's unit as sealed: the slot's to keep when it is held unsure */
    SlotGround  fresh;          /* the ground the slot will hold, made at TAKE */
    BinderKept* kept;           /* the nodes it names as they lie: those TipIn keeps, the FILES ones the
                                 * mount found rotten, ... */
    uint32_t    nkept;          /* ... and every DIGESTS node no copy of reads, found at TAKE */
    uint32_t    capkept;
    BinderFids* fids;           /* the file ids the slice takes */
    uint32_t    nfids;
    uint32_t    capfids;
    BinderSift  digests;        /* TipIn: DIGESTS sifted, its leaves laid and offered read a turn each — */
    uint8_t*    dread;          /* ... each sifted leaf's: 0 unread, 1 read, 2 reads in no copy — */
    uint32_t    dread_at;       /* ... the next looked at, */
    BinderOldRun* old;          /* ... and the runs their elements name */
    uint64_t    nold;
    uint64_t    capold;
} BinderRun;

/* binder_slice.c: the slice of the state after the last settled stitch, the
 * record of every live file whose id lies in one of `fids` (`n`) — OK or
 * ERR_NO_MEMORY; everything it holds let go; the ground DIGESTS speaks for,
 * every live file's and what the mount holds, in order of first block, the
 * runs that meet joined — OK or ERR_NO_MEMORY. */
error_t     BinderSliceTake(TagFSVolume* vol, BinderWork* w, const BinderFids* fids, uint32_t n);
void        BinderSliceLetGo(BinderWork* w);
error_t     BinderGroundTake(TagFSVolume* vol, BinderWork* w);

/* binder_slice.c: runs of ground in order of their first block — a heap sort,
 * in place — those that meet or overlap joined: how many are left. */
uint64_t    BinderGroundJoined(BinderGround* g, uint64_t n);

/* binder_sift.c: one tree of the Tally before sifted against the spans DogEar
 * took — the leaves under a node of level 1 a span meets or a copy of which
 * did not hold, the tree's first, every inner node — or sifted again once a
 * leaf of it is found to read in no copy; the nodes of it laying it in lays
 * and offers and every node of it counted dry into *c, a node on the atom of
 * `grid` blocks; its leaves: how many, the `i`-th's place and whether it
 * reads in no copy as far as is known, the first of them from `from` (below
 * `n`) whose lower key is not below `key` — `n` for none — and the one whose
 * range holds `key`, the last whose lower key is not past it; everything
 * let go. OK or ERR_NO_MEMORY, nothing held. */
error_t     BinderSiftTree(TagFSVolume* vol, BinderSift* s, uint64_t tree);
error_t     BinderSiftAgain(TagFSVolume* vol, BinderSift* s);
void        BinderSiftCount(TagFSVolume* vol, const BinderSift* s, uint32_t grid, TipInCount* c);
uint32_t    BinderSiftLeaves(TagFSVolume* vol, uint64_t tree);
bool        BinderSiftLeafAt(TagFSVolume* vol, uint64_t tree, uint32_t i, TallyPlace* p, bool* rotten);
uint32_t    BinderSiftLeafFrom(TagFSVolume* vol, uint64_t tree, uint32_t from, uint32_t n, TallyKey key);
uint32_t    BinderSiftLeafOf(TagFSVolume* vol, uint64_t tree, uint32_t n, TallyKey key);
void        BinderSiftLetGo(BinderSift* s);

/* binder_digests.c: DIGESTS laid in — a leaf it lays or offers read, a turn
 * each, *done once every one is (a leaf that reads in no copy sifts the tree
 * again, kept); then the nodes it names as they lie put among the Tally's and
 * the ground it speaks for made — in the leaves it lays and offers, the runs
 * their elements name less what the files let go since and with what they
 * took; a STRICT kernel's walk, laying every element of the state, holds those
 * of that ground to the ones it lays (BinderDigestsHeld: ERR_TAGFS_CORRUPTED,
 * a DEFECT said); the leaves a TipIn DONE turns the reads to, its own and the
 * Tally before's under the nodes it names as they lie; the leaves under them
 * counted into the reserve's measure. OK, ERR_NO_MEMORY, or the medium's
 * refusal. */
error_t     BinderDigestsReadNext(TagFSVolume* vol, BinderRun* r, bool* done);
error_t     BinderDigestsSifted(TagFSVolume* vol, BinderRun* r);
error_t     BinderDigestsHeld(TagFSVolume* vol, BinderWork* w, const BinderItem* it, bool end);
error_t     BinderDigestsTipTurn(TagFSVolume* vol, const BinderWork* w);
void        BinderDigestsKeptLaid(TagFSVolume* vol, BinderWork* w);
void        BinderDigestsLetGo(BinderRun* r);

/* binder_cost.c: the blocks of one copy of the leaves the keys DogEar holds
 * meet — each at what is left live in it — and of their path: what a Tally
 * laid in now would lay again; UINT64_MAX when what it would be laid against,
 * or what changed, is lost. */
uint64_t    BinderTipInBlocks(TagFSVolume* vol);

/* binder_kept.c: the file ids a leaf of FILES holds keys of, [*from, *to):
 * from its lower key's — the tree's first leaf from none — to its upper
 * key's, that file too when the leaf ends inside it. What a Tally laid in
 * slices of the leaf (BinderSiftTaken) and what is left live in it
 * (binder_live.c) are asked by these ids alike. */
void        BinderLeafFids(const TallyPlace* p, bool first, uint64_t* from, uint64_t* to);

/* binder_live.c: whether the leaf of `tree` at `p` — the tree's `first`, its
 * range open below — keeps a key of the state now (§16.6 п.6), weighed by the
 * ground the Tally under way took (`taken`) or by what moved since the last
 * slice. By the Settle's holder or the mount, no TagFS lock held. */
bool        BinderLeafLives(TagFSVolume* vol, uint64_t tree, const TallyPlace* p, bool first, bool taken);

/* binder_kept.c: TipIn at the slice — the Tally laid in when Foliation knows
 * the Tally before and DogEar what changed since, each tree of FILES, TAGS and
 * USE sifted into r->kept and the file ids of the leaves laid and offered into
 * r->fids, DIGESTS sifted and held in r->digests; else whole, every file. OK
 * or ERR_NO_MEMORY, said. What one tree sifted gives the Tally: its nodes kept
 * or offered named as they lie, and — FILES — the file ids of the leaves laid
 * or offered; OK or ERR_NO_MEMORY. The walk's look
 * at the nodes named as they lie, tree by tree, a level at a time, with
 * cursors that only go forward: the one of the tree being laid whose range
 * holds `key` at the leaves, or NULL; the last of this level or above
 * between the last key the level passed and `key` — the one right before
 * `key`, which a row after it begins at — or NULL; whether one at
 * `level` or above stands; the tree's root, when it is named whole as it
 * lies. BinderKeptLaid: what the walk names as it lies counted into the
 * reserve's measure (w->laid), as the leaves it lays are. */
error_t           BinderTipInTake(TagFSVolume* vol, BinderRun* r);
error_t           BinderSiftTaken(TagFSVolume* vol, BinderRun* r, const BinderSift* s);

/* binder_quires.c: the Tally-Quires the new slot holds for the nodes of the
 * Tally before its Tally names as they lie — made at TAKE. */
error_t           BinderKeptQuires(TagFSVolume* vol, BinderRun* r);
void              BinderKeptTreeBegin(BinderWork* w);
void              BinderKeptLevelBegin(BinderWork* w);
BinderKept*       BinderKeptAt(BinderWork* w, TallyKey key);
void              BinderKeptLaid(TagFSVolume* vol, BinderWork* w);
const BinderKept* BinderKeptBars(BinderWork* w, TallyKey key);
bool              BinderKeptAbove(const BinderWork* w, uint64_t level);
const BinderKept* BinderKeptRoot(const BinderWork* w);

/* binder_done.c: a node more the Tally names as it lies, in the order of tree
 * and key; a DIGESTS node no copy of reads, for the Tally `ctx` names; the
 * nodes the mount found so; the ground the new slot will hold, made at TAKE;
 * the Tally's DONE; a slot whose write or flush was refused held as naming
 * either Tally. */
error_t     BinderKeptPut(BinderRun* r, const BinderKept* k);
error_t     BinderRotOfDigests(void* ctx, const TallyPlace* at);
error_t     BinderRotsTaken(TagFSVolume* vol, BinderRun* r);
error_t     BinderFreshGround(TagFSVolume* vol, BinderRun* r);
error_t     BinderTallyDone(TagFSVolume* vol, BinderRun* r);

/* ‼ GLEAN_SLACK IS A POLICY, NOT A DERIVATION. How many times the blocks of
 * the live nodes the Tally-Quires may come to before Glean has the emptiest
 * laid again: a point on the curve between a mount's time — which reads every
 * Quire a slot names — and the write amplification every node laid again
 * adds. Measured for, not argued (the stand, stand_book_glean_tests.c). */
#define GLEAN_SLACK  2u

/* binder_glean.c: Glean of the Tally-Quires at a Tally's DONE (§4.8) — the
 * emptiest Quire the new slot names a node in outside the pair, its nodes to
 * be laid again by the next Tally, once they come to GLEAN_SLACK times the
 * live; and Glean's Dedup step (§8.2, §16.7 п.9), the ground the Tally took in
 * handed to Dedup's pass — asked by the DONE before the Gloss lets go of the
 * digests that ground holds. binder_proof.c: both copies of as many leaves checked longest ago as a
 * Tally laid in laid, read at its DONE, a copy that does not hold putting its
 * leaf's keys into DogEar (§16.5 п.18); where the sweep stands and the Quire
 * Glean chose last, let go with the mount. The Settle's holder's, with no TagFS lock held. */
void        BinderGleanAt(TagFSVolume* vol, const BinderRun* r);
void        BinderGleanDedup(TagFSVolume* vol, const BinderRun* r);
void        BinderGleanLetGo(TagFSVolume* vol);
void        BinderProofAt(TagFSVolume* vol, const BinderRun* r);
void        BinderProofLetGo(TagFSVolume* vol);

/* binder.c: the nodes of the Tally `r` laid in the pair of Tally-Quires the
 * Tallies go on in — its DONE, or its slot held as naming either Tally: the
 * next lays past them. */
void        BinderPairUsed(TagFSVolume* vol, const BinderRun* r);
void        BinderSlotUnsure(TagFSVolume* vol, BinderRun* r);

/* The Binder's self-check is built where a DEFECT is looked for: a STRICT
 * kernel, and the stand's. */
#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)
#define BINDER_CHECKS 1
#else
#define BINDER_CHECKS 0
#endif

#if BINDER_CHECKS
/* binder_check.c: a whole Tally held to the one it is laid against (§16.5
 * п.9) — begun with the walk that lays it; a turn of the Settle's holder
 * begun, in which it reads one leaf of the Tally before at the most; a leaf
 * element of the tree being laid, as it comes — and whether it is owed to the
 * next turn's leaf, unchecked, to be handed again; the tree's leaves spent,
 * what is left of the Tally before's read a leaf a turn, *more while some is
 * — and whether the walk stands so; the elements held to the Tally before by
 * the last check; its own let go. OK, or ERR_TAGFS_CORRUPTED for an element
 * changed outside DogEar: a DEFECT, said — a STRICT kernel stops there. */
void     BinderCheckBegin(TagFSVolume* vol);
void     BinderCheckTurn(TagFSVolume* vol);
error_t  BinderCheckItem(TagFSVolume* vol, const BinderWork* w, const BinderItem* it);
bool     BinderCheckOwed(TagFSVolume* vol);
error_t  BinderCheckTreeEnd(TagFSVolume* vol, const BinderWork* w, bool* more);
bool     BinderCheckDraining(TagFSVolume* vol);
uint64_t BinderCheckHeld(TagFSVolume* vol);
void     BinderCheckLetGo(TagFSVolume* vol);
#endif

/* Where the sweep goes on: the tree, and the least lower key of the next leaf
 * it reads there — the tree's first leaf when it has none. */
typedef struct {
    uint32_t tree;
    bool     has_from;
    TallyKey from;
    bool     begun;
} ProofSweep;

/* What one volume holds of binder_proof.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    ProofSweep proof;
} BinderProofState;

typedef struct BirthChunk BirthChunk;

/* What one volume holds of births.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    BirthChunk** chunk;  /* in the order of their first runs */
    uint32_t chunks;
    uint32_t room;
    BirthChunk* spent;  /* emptied under the state lock, not yet given the heap */
} BirthsState;

#if BINDER_CHECKS
typedef struct {
    TallyKey key;
    uint32_t at;          /* its body in the leaf's bytes */
    uint32_t len;
} CheckOld;

typedef struct {
    bool       on;
    bool       draining;    /* the tree's leaves are spent, the Tally before's still being read */
    uint64_t   tree;
    uint32_t   leaf;        /* the next leaf of the Tally before */
    bool       loaded;      /* a leaf of it in hand */
    bool       ended;       /* none left of the tree */
    bool       rotten;      /* the leaf in hand reads in no copy */
    TallyPlace at;
    uint8_t*   bytes;       /* its elements' bodies, one after another */
    uint32_t   len, cap;
    CheckOld*  old;
    uint32_t   n, capn, next;
    uint8_t*   spell;       /* a Parcel of it spelled again */
    uint32_t   spell_room;
    uint64_t   held;        /* elements held to the Tally before */
    bool       turn_read;   /* a leaf of it read this turn of the Settle's holder */
    bool       owed;        /* the element in hand waits for the next turn's leaf */
} BinderCheck;

/* What one volume holds of binder_check.c (TagFSVolume, volume/tagfs_volume.h): its self-check. */
#endif

/* What one volume holds of binder_glean.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    /* The Quire the last Glean chose: chosen again, its nodes did not all leave it
     * with the Tally it demanded — they are marked again, and no Tally more is
     * demanded for it: the next one TALLY_SLACK owes takes them, so a Quire that
     * will not empty never turns the Tallies over and over. The Settle's
     * holder's. */
    uint64_t glean_last;
} BinderGleanState;

typedef struct {
    uint64_t fid;
    uint64_t start;
    uint64_t count;
    uint64_t seq;
} SharedBirth;

/* What one volume holds of births_shared.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    SharedBirth* birth;
    uint64_t n;
    uint64_t cap;
} BirthsSharedState;

/* What one volume holds of binder.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    BinderRun run;
    volatile uint8_t under_way;  /* binding.run.active, for the cores that are not the holder */
} BinderState;

void BinderGleanVolumeBegin(TagFSVolume* vol);

#endif /* TAGFS_SETTLE_BINDER_INTERNAL_H */
