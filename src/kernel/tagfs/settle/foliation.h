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
} FoliationNode;

/* The mount: a node of `tree` of the Tally of `seq` it stands on, at `at` —
 * or one of them a copy of which did not hold, laid again by the next Tally. */
void        FoliationNoted(uint64_t tree, const TallyPlace* at, uint64_t seq);
void        FoliationRelay(uint64_t tree, const TallyPlace* at);

/* Why the Tally the next one is laid against is not known, or NULL. */
const char* FoliationLost(void);

/* The `index`-th leaf of `tree` of that Tally, in key order — FILES, TAGS or
 * USE: false past its last. */
bool        FoliationLeafAt(uint64_t tree, uint32_t index, TallyPlace* out);

/* Everything Foliation holds let go — a mount begun, the volume let go of. */
void        FoliationLetGo(void);

/* The Tally under way, by the Settle's holder: its draft begun; a node of
 * `tree` at `level` it names — its first key `key` and the count it carries,
 * its pointer `ptr` — in key order a level at a time, `root` for a tree's
 * root; at its DONE the draft the Foliation — `digests` when the reads were
 * turned to its DIGESTS; refused, the draft let go. */
void        FoliationDraftBegin(void);
void        FoliationDraftNode(uint64_t tree, uint64_t level, TallyKey key, bool root, uint64_t count,
                               const TallyPtr* ptr);
void        FoliationDraftDone(uint64_t seq, bool digests);
void        FoliationDraftLetGo(void);

#ifdef CONFIG_VOLUME_TESTS
/* The `index`-th node of `tree` Foliation holds — its leaves first, in key
 * order, then the inner nodes in the order they came: false past its last. */
bool        FoliationStandNode(uint64_t tree, uint32_t index, FoliationNode* out);
#endif

#endif /* TAGFS_SETTLE_FOLIATION_H */
