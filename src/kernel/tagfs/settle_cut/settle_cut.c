#include "settle_cut.h"

#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT

#include "klib.h"
#include "amp.h"
#include "serial.h"         /* WireForceRelease / WireDrain — the line is heard */
#include "system_halt.h"    /* halt_all_ap_cores — the roll call */

/* The Makefile hands the point over as SETTLE_CUT_POINT_<VALUE>; a value that
 * names no point is an undeclared identifier and never gets this far. */
_Static_assert(CONFIG_TAGFS_SETTLE_CUT_POINT >= SETTLE_CUT_POINT_REGISTRY &&
               CONFIG_TAGFS_SETTLE_CUT_POINT <= SETTLE_CUT_POINT_DONE,
               "SETTLECUT names no point of the Settle cycle");

static const char *const SettleCutWhere[] = {
    [SETTLE_CUT_POINT_REGISTRY]  = "before the tag registry is written",
    [SETTLE_CUT_POINT_POOL]      = "before the metadata pool is written",
    [SETTLE_CUT_POINT_TABLE]     = "before the file table is written",
    [SETTLE_CUT_POINT_INTEGRITY] = "before the integrity map is written",
    [SETTLE_CUT_POINT_BITMAP]    = "before the block bitmap is written",
    [SETTLE_CUT_POINT_LEDGER]    = "before the ledger is written",
    [SETTLE_CUT_POINT_FLUSH]     = "before the medium's cache is flushed",
    [SETTLE_CUT_POINT_DONE]      = "after the cache flush, with the whole cycle on the medium",
};

static volatile uint32_t g_settle_syncs;        /* cycles that reached point 1 */
static volatile uint32_t g_settle_cut_made;

void SettleCutSayArmed(void)
{
    kprintf("[SETTLECUT] armed: the first cycle to reach point %u of %u, %s, "
            "stops this machine there\n",
            (unsigned)CONFIG_TAGFS_SETTLE_CUT_POINT,
            (unsigned)SETTLE_CUT_POINT_DONE,
            SettleCutWhere[CONFIG_TAGFS_SETTLE_CUT_POINT]);
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
 * stage that holds no lock (a writer with no open-file entry, say) stops right
 * there. A scenario therefore runs a single writer, and the cycle is that
 * writer.
 *
 * And what cannot be stopped is not claimed: a command another operation had
 * already issued to the device still completes after every processor has
 * stopped. What this guarantees is that nothing past the point is ISSUED.
 */
void SettleCutAt(SettleCutPoint here)
{
    uint32_t sync_no = (here == SETTLE_CUT_POINT_REGISTRY)
        ? __atomic_add_fetch(&g_settle_syncs, 1u, __ATOMIC_ACQ_REL)
        : __atomic_load_n(&g_settle_syncs, __ATOMIC_ACQUIRE);

    if (here != CONFIG_TAGFS_SETTLE_CUT_POINT)
        return;
    if (__atomic_exchange_n(&g_settle_cut_made, 1u, __ATOMIC_ACQ_REL) != 0)
        return;                 /* the first passage, on any core, is the cut */

    __asm__ volatile("cli" ::: "memory");
    uint8_t me = amp_get_core_index();

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
            "this boot, on core %u — %u of the %u other core(s) answered the "
            "roll call and run no more, and nothing after this point was "
            "issued to the medium\n",
            (unsigned)here, (unsigned)SETTLE_CUT_POINT_DONE,
            SettleCutWhere[here], sync_no, (unsigned)me, answered,
            (unsigned)(g_amp.total_cores ? g_amp.total_cores - 1u : 0u));
    WireForceRelease();
    WireDrain();

    for (;;)
        __asm__ volatile("cli; hlt");
}

#endif /* CONFIG_TAGFS_SETTLE_CUT_POINT */
