#ifndef TAGFS_LOOSE_LEAVES_H
#define TAGFS_LOOSE_LEAVES_H

#include "../ink/docket.h"

/*
 * LooseLeaves — the Slips the settled state carries (spec_v2 §16.5 п.13-14):
 * the files whose record, as the last stitch to settle left it, keeps Slips
 * over its last block.
 *
 * A Tally carries no Slip (§2.8, §4.3): its slice is the settled state, and a
 * tail that lives only in the ribbon would be lost to the slot that moves the
 * ribbon's `at` past it. So no Tally takes its slice while one is left
 * (binder.c, BinderMayNot) — each tail goes into a block first, by the one
 * door, the Ink (Engross, helm/engross.h) — and this count is how the Binder
 * knows. The mount counts it from the files its replay of the ribbon leaves
 * standing — a Tally holds none, so only those can — and each stitch's DONE
 * moves it by the Jots it carried, before the Binder weighs that stitch.
 * Written by the mount and by the Settle's holder, one at a time; read from
 * any core.
 */

uint64_t LooseLeaves(void);

/* The mount's count: the files its replay leaves with Slips over their last
 * block. */
void     LooseLeavesMounted(uint64_t n);

/* A stitch settled, carrying the Jots of `t`: each file it names now stands as
 * its Jot's `now`, no more as its `was`. With no lock held. */
void     LooseLeavesSettled(const DocketTaken* t);

#endif /* TAGFS_LOOSE_LEAVES_H */
