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
 *   1 registry   before the tag registry is written              (S1)
 *   2 pool       before the metadata pool is written             (S3)
 *   3 table      before the file table is written                (S1)
 *   4 integrity  before the integrity map is written             (S1)
 *   5 bitmap     before the block bitmap is written              (S1)
 *   6 ledger     before the ledger is written                    (S4)
 *   7 flush      before the medium's cache is flushed            (S5)
 *   8 done       after the flush, with the whole cycle on the medium (S6)
 *   9 usechain   before the Use Context's chain is written         (S1)
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
 * has the point ahead of it, else a later one — the cycle that closes the tail
 * for the anchor's waiter takes its snapshot after the waiter was linked.
 * Every cycle before it, a shutdown's included, runs as in any kernel. What
 * this cannot stop: a cycle already past the point that covers G and closes
 * its tail for another asker answers the anchor itself, and the machine runs
 * on — which the scenario says, as a machine that never stopped.
 *
 * Not built together with VOLUME_TESTS=on, which the Makefile refuses: the
 * startup tests settle the volume themselves, and the cut would land inside a
 * test instead of in the scenario it names.
 */

#include "../../../lib/kernel/ktypes.h"

typedef enum {
    SETTLE_CUT_POINT_REGISTRY = 1,
    SETTLE_CUT_POINT_POOL,
    SETTLE_CUT_POINT_TABLE,
    SETTLE_CUT_POINT_INTEGRITY,
    SETTLE_CUT_POINT_BITMAP,
    SETTLE_CUT_POINT_LEDGER,
    SETTLE_CUT_POINT_FLUSH,
    SETTLE_CUT_POINT_DONE,
    SETTLE_CUT_POINT_USECHAIN
} SettleCutPoint;

#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT

/* The cycle took its snapshot (S0): it settles every Ink up to `target`. By
 * the cycle's holder, before any point of that cycle is passed. */
void SettleCutCycle(uint64_t target);

/* A program waits for Ink `gen` to settle (SettleWaitLink): the first such wait
 * after the "armed" line is the anchor the cut waits for. From any core. */
void SettleCutAnchored(uint64_t gen);

/* The cycle passes `here`. Returns at every point but the armed one, at that
 * one in a cycle that does not cover the anchor, and at it once the cut has
 * been made; the first passage of the armed point by a cycle that covers the
 * anchor, on any core, does not return at all. */
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
