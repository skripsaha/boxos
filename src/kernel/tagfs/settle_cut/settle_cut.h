#ifndef SETTLE_CUT_H
#define SETTLE_CUT_H

/*
 * SETTLECUT — the power goes at a named point of the Settle cycle.
 *
 * `make SETTLECUT=<point>` builds a kernel that stops the whole machine at
 * that point of the cycle that settles a program's anchor: interrupts off on
 * the committing core, every other core halted by the same roll call a
 * shutdown takes, one line said, and nothing after it. The medium then holds
 * every write issued before the point and none after it — the best case of a
 * power cut there, the one in which the drive's cache survived — and the host
 * reads it back (tools/volume-read.py check) to see what the next mount would
 * find. A real cut can also lose any write since the last cache flush; those
 * media are PowerCut's (tools/powercut.sh, volume-read.py cut), so a SETTLECUT
 * image that reads consistent proves the ORDER at the point, not that a power
 * cut there is survived.
 *
 * The points are named by the record ABOUT to be written, and each stands at
 * one stage of the cycle (settle/settle.h):
 *
 *   1 flush      before the medium's cache is flushed, the stitch's
 *                last flush                                       (LAST_FLUSH)
 *   2 done       after the flush, with the whole cycle on the medium (DONE)
 *   3 stitch     before a stitch's last atom is written — a stitch of one
 *                atom not begun, one of more torn                 (STITCH)
 *   4 tally      before a Tally's flush, its nodes written and no slot naming
 *                them                                             (TALLY_FLUSH)
 *   5 slot       before the second half of a Tally's slot is written —
 *                the slot torn                                    (TALLY_SLOT)
 *   6 spared     before the last flush of a stitch whose toll spared the
 *                flush of its data: its data and its atoms written, neither
 *                flushed                                          (LAST_FLUSH)
 *   7 undone     before the first atom of the stitch that lays a mount's
 *                verdict down — the first such stitch, anchored or not:
 *                the verdict is the scenario                      (STITCH)
 *   8 engross    before the first atom of the stitch that names a block an
 *                Engross laid — a tail out of its Slips, laid for the Tally
 *                that waits for it, anchored or not: the block on the medium,
 *                no stitch naming it (C22)                        (STITCH)
 *
 * `tally` and `slot` are passed by the Tally that covers the anchor — the
 * state after a stitch that carries it (settle/binder.c), a work of the same
 * Scribe between the cycles.
 *
 * This list is the only list of the names: the Makefile turns the value it is
 * given into SETTLE_CUT_POINT_<VALUE>, and a value that is none of these does
 * not compile.
 *
 * ‼ ARMED BY THE ANCHOR, NOT BY THE FIRST CYCLE. The cadence begins cycles of
 * its own — after a mount's Inks, between a program's create and its write —
 * so the first cycle of a boot is not the one a scenario asks about. The cut
 * waits for the first anchor taken after the "armed" line (a program parked
 * on SettleWaitLink for Ink G) and stops the first cycle that reaches the
 * point, after that, with G in its snapshot: the cycle in flight when it still
 * has the point ahead of it, else a later one — the cycle that answers the
 * anchor's waiter takes its snapshot after the waiter was linked.
 * Every cycle before it, a shutdown's included, runs as in any kernel. What
 * this cannot stop: a cycle already past the point that covers G for another
 * asker answers the anchor itself, and the machine runs on — which the
 * scenario says, as a machine that never stopped.
 *
 * Not built together with VOLUME_TESTS=on, which the Makefile refuses: the
 * startup tests settle the volume themselves, and the cut would land inside a
 * test instead of in the scenario it names.
 */

#include "../../../lib/kernel/ktypes.h"

typedef enum {
    SETTLE_CUT_POINT_FLUSH = 1,
    SETTLE_CUT_POINT_DONE,
    SETTLE_CUT_POINT_STITCH,
    SETTLE_CUT_POINT_TALLY,
    SETTLE_CUT_POINT_SLOT,
    SETTLE_CUT_POINT_SPARED,
    SETTLE_CUT_POINT_UNDONE,
    SETTLE_CUT_POINT_ENGROSS
} SettleCutPoint;

/* The last point: the count every line of the cut says "of". */
#define SETTLE_CUT_POINT_LAST SETTLE_CUT_POINT_ENGROSS

#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT

/* The cycle took its snapshot (its TAKE): it settles every Ink up to `target`. By
 * the cycle's holder, before any point of that cycle is passed. */
void SettleCutCycle(uint64_t target);

/* A program waits for Ink `gen` to settle (SettleWaitLink): the first such wait
 * after the "armed" line is the anchor the cut waits for. From any core. */
void SettleCutAnchored(uint64_t gen);

/* The cycle passes `here`. Returns at every point but the armed one, at that
 * one in a cycle that does not cover the anchor, and at it once the cut has
 * been made; the first passage of the armed point by a cycle that covers the
 * anchor, on any core, does not return at all. `undone` and `engross` wait
 * for no anchor: the first passage of either is the cut. */
void SettleCutAt(SettleCutPoint here);

/* Said once a volume is mounted: which point this kernel will cut at — and
 * from here the anchor the cut waits for is the first one taken. */
void SettleCutSayArmed(void);

#define SETTLE_CUT_CYCLE(target) SettleCutCycle(target)
#define SETTLE_CUT_ANCHORED(gen) SettleCutAnchored(gen)
#define SETTLE_CUT_AT(point)     SettleCutAt(point)

#else

#define SETTLE_CUT_CYCLE(target) ((void)0)
#define SETTLE_CUT_ANCHORED(gen) ((void)0)
#define SETTLE_CUT_AT(point)     ((void)0)
static inline void SettleCutSayArmed(void) {}

#endif /* CONFIG_TAGFS_SETTLE_CUT_POINT */

#endif /* SETTLE_CUT_H */
