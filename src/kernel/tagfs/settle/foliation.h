#ifndef TAGFS_SETTLE_FOLIATION_H
#define TAGFS_SETTLE_FOLIATION_H

#include "../mount/tally_read.h"

/*
 * Foliation — the Tally the next one is laid against, as memory knows it
 * (spec_v2 §16.5 п.9): every node of its FILES, TAGS and USE and every inner
 * node of its DIGESTS, each by its place — its BookPtr, its level, the keys
 * [lo, hi) its parent gives it, the count it carries — and whether it is to be
 * laid again whatever its keys say: a copy of it did not hold (§2.5). The
 * leaves of its DIGESTS are not kept twice: they are the reads' own table
 * (mount/tally_read.h), which each Tally this mount writes turns to itself.
 *
 *   the mount   every node of the Tally of the slot it stands on, as its walk
 *               reaches it (book_ground.c) — and an inner node a copy of which
 *               did not hold marked to be laid again (a leaf's keys go into
 *               DogEar, dog_ear.h)
 *   a Tally     every node it lays and every rotten node it names as it lies,
 *               as the level above is built of them (binder_items.c), the root
 *               at its tree's end: its draft, which becomes the Foliation at
 *               its DONE, and is let go when it is refused
 *
 * ‼ A HEAP THAT REFUSES FOLIATION ROOM LOSES IT (§4.2) — so does a Tally whose
 * DIGESTS the reads were not turned to: the next Tally has nothing known to be
 * laid against. That is a demand, said once; the Tally that answers it is
 * written whole, and known again.
 *
 * The Settle's holder's and the mount's (with the door shut) alone: no lock.
 */

typedef struct {
    TallyPlace place;
    bool       relay;       /* laid again by the next Tally, whatever its keys */
    uint64_t   items;       /* a leaf: the runs its Parcels carry (FILES), the tags it holds (TAGS) */
    uint64_t   text;        /* ... and the bytes of their keys and values (TAGS) */
} FoliationNode;

/* The mount: a node of `tree` of the Tally of `seq` it stands on, at `at` —
 * or one of them a copy of which did not hold, laid again by the next Tally. */
void        FoliationNoted(uint64_t tree, const TallyPlace* at, uint64_t seq);
void        FoliationRelay(uint64_t tree, const TallyPlace* at);

/* The mount: what the leaf of `tree` it noted last holds more — `items` runs
 * of a Parcel of FILES or tags of TAGS, `text` bytes of their keys and values
 * — as its walk reads the leaf (the reserve's measure by the Tally before,
 * endpaper_fact.h, counts the leaves a Tally laid in names as they lie). */
void        FoliationLeafHolds(uint64_t tree, uint64_t items, uint64_t text);

/* The leaves of `tree` of that Tally holding anything FoliationLeafHolds
 * counted — of FILES, the leaves a Parcel's runs lie in: UINT64_MAX while
 * Foliation is lost. */
uint64_t    FoliationLeavesHolding(uint64_t tree);

/* Why the Tally the next one is laid against is not known, or NULL. */
const char* FoliationLost(void);

/* The `index`-th leaf of `tree` of that Tally, in key order — FILES, TAGS or
 * USE: false past its last. */
bool        FoliationLeafAt(uint64_t tree, uint32_t index, TallyPlace* out);

/* The nodes of `tree` of that Tally — its leaves in key order, its inner
 * nodes in the order they came — *n of them; NULL for none. */
const FoliationNode* FoliationLeaves(uint64_t tree, uint32_t* n);
const FoliationNode* FoliationInner(uint64_t tree, uint32_t* n);

/* The inner nodes of `tree` at `level` (>= 1) of that Tally, in key order — *n
 * of them; NULL for none. The inner nodes FoliationInner gives are then in
 * order of level and key. */
const FoliationNode* FoliationLevel(uint64_t tree, uint64_t level, uint32_t* n);

/* The Sheets of one copy of every node of FILES, TAGS and USE and every inner
 * node of DIGESTS that Tally holds not sealed at `seq`: what of its book the
 * Tally of `seq` named as it lay — counted as a mount counts its book
 * (book_ground.c, BookGroundNodeOf), Sheets and no atom. */
uint64_t    FoliationBlocksOlder(uint64_t seq);

/* Everything Foliation holds let go — a mount begun, the volume let go of. */
void        FoliationLetGo(void);

/* The Tally under way, by the Settle's holder: its draft begun; a node of
 * `tree` at `level` it names — its first key `key` and the count it carries,
 * its pointer `ptr`, and a leaf what it holds (FoliationNode) — in key order
 * a level at a time, `root` for a tree's root; every node of the Tally before under `top`, an inner node it names as
 * it lies, taken into the draft as it was (a tree whose root it names as it
 * lies has `root` named so too); at its DONE the draft the Foliation —
 * `digests` when the reads were turned to its DIGESTS; refused, the draft let
 * go. */
void        FoliationDraftBegin(void);
void        FoliationDraftNode(uint64_t tree, uint64_t level, TallyKey key, bool root, uint64_t count,
                               const TallyPtr* ptr, uint64_t items, uint64_t text);
void        FoliationDraftUnder(uint64_t tree, const TallyPlace* top);
void        FoliationDraftDone(uint64_t seq, bool digests);
void        FoliationDraftLetGo(void);

#ifdef CONFIG_VOLUME_TESTS
/* The `index`-th node of `tree` Foliation holds — its leaves first, in key
 * order, then the inner nodes in the order they came: false past its last. */
bool        FoliationStandNode(uint64_t tree, uint32_t index, FoliationNode* out);
#endif

#endif /* TAGFS_SETTLE_FOLIATION_H */
