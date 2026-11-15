#include "settle_cut.h"

#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT

#include "klib.h"
#include "amp.h"
#include "serial.h"         /* WireForceRelease / WireDrain — the line is heard */
#include "system_halt.h"    /* halt_all_ap_cores — the roll call */

/* The Makefile hands the point over as SETTLE_CUT_POINT_<VALUE>; a value that
 * names no point is an undeclared identifier and never gets this far. */
_Static_assert(CONFIG_TAGFS_SETTLE_CUT_POINT >= SETTLE_CUT_POINT_FLUSH &&
               CONFIG_TAGFS_SETTLE_CUT_POINT <= SETTLE_CUT_POINT_LAST,
               "SETTLECUT names no point of the Settle cycle");

static const char *const SettleCutWhere[] = {
    [SETTLE_CUT_POINT_FLUSH]     = "before the medium's cache is flushed",
    [SETTLE_CUT_POINT_DONE]      = "after the cache flush, with the whole cycle on the medium",
    [SETTLE_CUT_POINT_STITCH]    = "before the stitch's last atom is written",
    [SETTLE_CUT_POINT_TALLY]     = "before a Tally's flush, its nodes written and no slot naming them",
    [SETTLE_CUT_POINT_SLOT]      = "before the second half of a Tally's slot is written",
    [SETTLE_CUT_POINT_SPARED]    = "before the last flush of a stitch whose toll spared the flush of its data",
    [SETTLE_CUT_POINT_UNDONE]    = "before the first atom of the stitch that lays the mount's verdict down",
    [SETTLE_CUT_POINT_ENGROSS]   = "before the first atom of the stitch that names a block an Engross laid",
};

/* The points the cycle reaches with no anchor's change in it: the verdict's
 * stitch, asked at the door's opening, and an Engross's, laid for a Tally. */
static bool SettleCutUnanchored(SettleCutPoint here)
{
    return here == SETTLE_CUT_POINT_UNDONE || here == SETTLE_CUT_POINT_ENGROSS;
}

static volatile uint32_t g_settle_cycles;       /* cycles that took their snapshot */
static volatile uint64_t g_settle_target;       /* the cycle in flight's: its holder's alone */
static volatile uint64_t g_settle_anchor;       /* the Ink the anchor waits for; 0 until one is taken */
static volatile uint32_t g_settle_cut_made;

void SettleCutSayArmed(void)
{
    __atomic_store_n(&g_settle_anchor, 0, __ATOMIC_RELEASE);
    /* The verdict's stitch is the session's first, asked at the door's opening
     * before any program can anchor (tagfs.c, ErrataAsk) — so that point waits
     * for no anchor, and says so in a sentence of its own: the anchored one is
     * the sentence PowerCut's settlecut waits for (tools/powercut.sh,
     * SC_ARMED_ERE), held word for word to this file by its selftest. */
    if (CONFIG_TAGFS_SETTLE_CUT_POINT == SETTLE_CUT_POINT_UNDONE) {
        kprintf("[SETTLECUT] armed for the verdict: the first stitch to reach point %u of %u, "
                "%s, stops this machine there, anchored or not\n",
                (unsigned)CONFIG_TAGFS_SETTLE_CUT_POINT, (unsigned)SETTLE_CUT_POINT_LAST,
                SettleCutWhere[CONFIG_TAGFS_SETTLE_CUT_POINT]);
        return;
    }
    if (CONFIG_TAGFS_SETTLE_CUT_POINT == SETTLE_CUT_POINT_ENGROSS) {
        kprintf("[SETTLECUT] armed for the Engross: the first stitch to reach point %u of %u, "
                "%s, stops this machine there, anchored or not\n",
                (unsigned)CONFIG_TAGFS_SETTLE_CUT_POINT, (unsigned)SETTLE_CUT_POINT_LAST,
                SettleCutWhere[CONFIG_TAGFS_SETTLE_CUT_POINT]);
        return;
    }
    kprintf("[SETTLECUT] armed: the first cycle to reach point %u of %u, %s, with the "
            "change of the first anchor after this line in its snapshot stops this "
            "machine there\n",
            (unsigned)CONFIG_TAGFS_SETTLE_CUT_POINT,
            (unsigned)SETTLE_CUT_POINT_LAST,
            SettleCutWhere[CONFIG_TAGFS_SETTLE_CUT_POINT]);
}

void SettleCutCycle(uint64_t target)
{
    __atomic_add_fetch(&g_settle_cycles, 1u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_settle_target, target, __ATOMIC_RELEASE);
}

/* Only the first: a later anchor's cycle covers it too, and the scenario asks
 * about the cycle its own anchor waits for. */
void SettleCutAnchored(uint64_t gen)
{
    uint64_t none = 0;
    (void)__atomic_compare_exchange_n(&g_settle_anchor, &none, gen, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/*
 * The cut.
 *
 * Interrupts go off on this core first, so nothing of its own runs after the
 * point; then every other core is stopped by the roll call a shutdown takes
 * (system_halt.c). What that roll call proves is narrower than it sounds: a
 * core that answered was not inside a stretch that kept interrupts off — so it
 * held no spin_lock whose section keeps them off to its end, a legacy-IDE DMA
 * wait's caller included, since that wait no longer turns them on (it takes
 * the drive's answer itself, drivers/disk/ata_async.c). It does NOT prove a
 * core was between writes: one frozen between two synchronous writes of a
 * stage that holds no lock (a file's writer, whose Helm is no lock) stops
 * right there. A scenario therefore runs a single writer, and the cycle is that
 * writer.
 *
 * And what cannot be stopped is not claimed: a command another operation had
 * already issued to the device still completes after every processor has
 * stopped. What this guarantees is that nothing past the point is ISSUED.
 */
void SettleCutAt(SettleCutPoint here)
{
    if (here != CONFIG_TAGFS_SETTLE_CUT_POINT)
        return;
    uint64_t anchor = __atomic_load_n(&g_settle_anchor, __ATOMIC_ACQUIRE);
    uint64_t target = __atomic_load_n(&g_settle_target, __ATOMIC_ACQUIRE);
    if (!SettleCutUnanchored(here) && (anchor == 0 || target < anchor))
        return;                 /* a cycle of the cadence's own, not the anchor's */
    if (__atomic_exchange_n(&g_settle_cut_made, 1u, __ATOMIC_ACQ_REL) != 0)
        return;                 /* the first passage, on any core, is the cut */

    __asm__ volatile("cli" ::: "memory");
    uint8_t  me    = amp_get_core_index();
    uint32_t cycle = __atomic_load_n(&g_settle_cycles, __ATOMIC_ACQUIRE);

    halt_all_ap_cores(false);   /* a power cut: every core stops where it stands */

    uint32_t answered = 0;
    for (uint8_t c = 0; c < g_amp.total_cores; c++) {
        if (c != me && __atomic_load_n(&g_amp.cores[c].halted, __ATOMIC_ACQUIRE))
            answered++;
    }

    /* The line cannot drive itself with interrupts off, and a core stopped
     * while it held the line's lock never gives it back — the same pair, in
     * the same order, as every rung of system_halt. */
    WireForceRelease();
    WireDrain();
    kprintf("[SETTLECUT] the power went at point %u of %u, %s, in cycle %u of "
            "this boot, whose snapshot reaches Ink %llu of the anchor's %llu, on "
            "core %u — %u of the %u other core(s) answered the roll call and run "
            "no more, and nothing after this point was issued to the medium\n",
            (unsigned)here, (unsigned)SETTLE_CUT_POINT_LAST,
            SettleCutWhere[here], cycle, (unsigned long long)target,
            (unsigned long long)anchor, (unsigned)me, answered,
            (unsigned)(g_amp.total_cores ? g_amp.total_cores - 1u : 0u));
    WireForceRelease();
    WireDrain();

    for (;;)
        __asm__ volatile("cli; hlt");
}

#endif /* CONFIG_TAGFS_SETTLE_CUT_POINT */
