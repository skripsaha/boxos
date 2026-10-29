#ifndef TAGFS_SETTLE_TIP_IN_H
#define TAGFS_SETTLE_TIP_IN_H

#include "dog_ear.h"

/*
 * TipIn — a Tally laid in among the nodes of the one before it (spec_v2 §4.1,
 * §4.3 TALLY_NODES, §16.5 п.16): the leaves whose keys changed laid anew, the
 * inner nodes above them, and every other node named as it lies — by the
 * pointer, the separator and the count the Tally before named it with. The
 * book keeps one rule of packing (§16.2 п.24): the nodes laid anew are packed
 * greedily between the ones named as they lie, as a whole Tally packs them
 * around a rotten node (§16.4 п.5).
 *
 * TipInSift weighs one tree of the Tally before — its leaves in key order and
 * its inner nodes in any order, as Foliation keeps them — against the spans
 * DogEar took, and gives each node its fate:
 *
 *   TIP_IN_LAID    a leaf a span meets, or one a copy of which did not hold;
 *                  an inner node above such a leaf, a margin, or an inner node
 *                  a copy of which did not hold
 *   TIP_IN_MARGIN  the leaf after a row of leaves laid anew under the same
 *                  parent — the one before it when the row ends its parent:
 *                  named as it lies unless the row comes to less than half a
 *                  Sheet, which then takes it in (Р-d1 (а)); its keys are
 *                  sliced so the row can. Never a leaf under another parent,
 *                  whose path would be laid anew for nothing
 *   TIP_IN_KEPT    named as it lies by the level above it, which is laid anew;
 *                  every node no copy of which reads too, whatever changed in
 *                  its range — it cannot be laid (§16.4 п.5) — and its parent
 *                  laid anew when a span meets it, so that a Tally names it
 *                  beside what changed there
 *   TIP_IN_UNDER   under a node named as it lies: nothing of it is said
 *
 * A tree nothing changed in is its root, KEPT, and everything under it UNDER.
 * Pure: memory and the keys alone — the host holds it to the same leaves.
 * OK, or ERR_NO_MEMORY with nothing given.
 */

typedef struct {
    TallyPlace place;
    bool       relay;       /* a copy of it did not hold: laid again whatever its keys */
    bool       rotten;      /* no copy of it reads */
} TipInNode;

typedef enum {
    TIP_IN_UNDER = 0,
    TIP_IN_KEPT,
    TIP_IN_LAID,
    TIP_IN_MARGIN,
} TipInFate;

error_t TipInSift(uint64_t tree, const TipInNode* leaf, uint32_t nleaf, const TipInNode* inner, uint32_t ninner,
                  const DogEarSpan* span, uint32_t nspan, TipInFate* leaf_fate, TipInFate* inner_fate);

/* Whether key `a` comes before `b` in `tree`; whether the range of `p` holds
 * key `k`; whether the range of `in` lies inside the range of `out`. */
bool    TipInKeyLess(uint64_t tree, TallyKey a, TallyKey b);
bool    TipInHolds(uint64_t tree, const TallyPlace* p, TallyKey k);
bool    TipInWithin(uint64_t tree, const TallyPlace* in, const TallyPlace* out);

/* The places of a tree's `n` leaves in key order, each `lo` the key its
 * parent names it by, closed as a mount's walk gives them (tally_walk.c,
 * TallyKid): every leaf's lower key its own — the first's too, unless it is
 * the root, which alone has none — and its upper key the next one's lower. */
void    TipInLeavesClosed(TallyPlace* p, uint32_t n);

#endif /* TAGFS_SETTLE_TIP_IN_H */
