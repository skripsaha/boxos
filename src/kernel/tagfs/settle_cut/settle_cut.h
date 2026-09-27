#ifndef SETTLE_CUT_H
#define SETTLE_CUT_H

/*
 * SETTLECUT — the power goes at a named point of the metadata sync.
 *
 * `make SETTLECUT=<point>` builds a kernel whose first sync to reach that
 * point stops the whole machine there: interrupts off on the syncing core,
 * every other core halted by the same roll call a shutdown takes, one line
 * said, and nothing after it. The medium then holds every write issued before
 * the point and none after it — the best case of a power cut there, the one in
 * which the drive's cache survived — and the host reads it back
 * (tools/volume-read.py check) to see what the next mount would find. A real
 * cut can also lose any write since the last cache flush (tagfs.c's sync says
 * which); those media are PowerCut's (tools/powercut.sh, volume-read.py cut),
 * so a SETTLECUT image that reads consistent proves the ORDER at the point,
 * not that a power cut there is survived.
 *
 * The points are named by the record ABOUT to be written, not by a line of
 * tagfs.c, so a scenario keeps its meaning when the sync becomes Settle at
 * step 1.4 (the stage each becomes is in brackets):
 *
 *   1 registry   before the tag registry is written              (S1)
 *   2 pool       before the metadata pool is written             (S3)
 *   3 table      before the file table is written                (S1)
 *   4 retires    before the retired records are released         (gone at 1.4)
 *   5 integrity  before the integrity map is written             (S1)
 *   6 bitmap     before the block bitmap is written              (S1)
 *   7 ledger     before the ledger is written                    (S4)
 *   8 flush      before the medium's cache is flushed            (S5)
 *   9 done       after the flush, with the whole sync on the medium (S6)
 *
 * This list is the only list of the names: the Makefile turns the value it is
 * given into SETTLE_CUT_POINT_<VALUE>, and a value that is none of these does
 * not compile.
 *
 * Not built together with VOLUME_TESTS=on, which the Makefile refuses: the
 * startup tests sync the volume themselves, and the cut would land inside a
 * test instead of in the scenario it names.
 */

typedef enum {
    SETTLE_CUT_POINT_REGISTRY = 1,
    SETTLE_CUT_POINT_POOL,
    SETTLE_CUT_POINT_TABLE,
    SETTLE_CUT_POINT_RETIRES,
    SETTLE_CUT_POINT_INTEGRITY,
    SETTLE_CUT_POINT_BITMAP,
    SETTLE_CUT_POINT_LEDGER,
    SETTLE_CUT_POINT_FLUSH,
    SETTLE_CUT_POINT_DONE
} SettleCutPoint;

#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT

/* The sync passes `here`. Returns at every point but the armed one, and at
 * that one too once the cut has been made; the first passage of the armed
 * point, on any core, does not return at all. */
void SettleCutAt(SettleCutPoint here);

/* Said once a volume is mounted: which point this kernel will cut at. */
void SettleCutSayArmed(void);

#define SETTLE_CUT_AT(point) SettleCutAt(point)

#else

#define SETTLE_CUT_AT(point) ((void)0)
static inline void SettleCutSayArmed(void) {}

#endif /* CONFIG_TAGFS_SETTLE_CUT_POINT */

#endif /* SETTLE_CUT_H */
