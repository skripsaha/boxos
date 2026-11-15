#include "settle_internal.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../mount/away.h"

/*
 * The machine stopping (settle.h): the shut that lets no change begin, and
 * the halt's last cycles — until every Ink settled, the last stitch saying
 * CLEAN (§16.2 п.9).
 */

/* The halt's last cycle, for tagfs_shutdown. */
static bool     s_halt_ran;
static error_t  s_halt_answer;

/* The machine is stopping: from the halt's shut on, every cycle begun is one
 * of its last (SettleRunBegin), and no change begins (TagFSShutForHalt).
 * Raised once, never lowered. */
static volatile bool s_halting;

void SettleHaltMountReset(void)
{
    s_halt_ran = false;
}

error_t SettleHaltAnswer(uint64_t* unsettled)
{
    uint64_t inked   = InkedGen();
    uint64_t settled = SettledGen();
    *unsettled       = inked > settled ? inked - settled : 0;
    return s_halt_ran ? s_halt_answer : ERR_TAGFS_METADATA_ERROR;
}

/*
 * ‼ THE LAST STITCH IS CLEAN, AND NOTHING RESTS THAT A HALT MUST WRITE FREE.
 * The book keeps no map on the medium, and a mount derives its ground from
 * the names alone (spec_v2 §4.6). The cycles go on while an Ink is unsettled
 * — one begun before the shut may be inked while the first runs — each stitch
 * CLEAN when no change is in flight under it (HelmOwed), and the next mount
 * reads the last one CLEAN.
 */
static error_t SettleHaltCycles(void)
{
    error_t rc = SettleAskedFor(SETTLE_FOR_HALT);
    for (uint64_t more = 0; rc == OK && more <= FallowLag() + 1u && InkedGen() > SettledGen(); more++)
        rc = SettleAskedFor(SETTLE_FOR_HALT);
    return rc;
}

#ifdef CONFIG_VOLUME_TESTS
error_t TagFSStandSettleForHalt(void)
{
    return SettleHaltCycles();
}

void TagFSStandShutForHalt(bool shut)
{
    __atomic_store_n(&s_halting, shut, __ATOMIC_SEQ_CST);
}
#endif

/* Each side looks only after it has said: a write counts itself among the
 * Helms' before its guard asks here (PlotLoad), and the halt's cycle counts the
 * Helms after the shut — so either the write sees the shut, or the cycle sees
 * the write in flight and seals nothing clean. */
void TagFSShutForHalt(void)
{
    if (__atomic_exchange_n(&s_halting, true, __ATOMIC_SEQ_CST))
        return;
    if (tagfs_get_state()->initialized)
        kprintf("[TagFS] the machine is stopping: the volume takes no change from here — the "
                "ones already begun run to their end, and the halt's cycles settle them\n");
}

bool SettleHalting(void)
{
    return __atomic_load_n(&s_halting, __ATOMIC_SEQ_CST);
}

error_t TagFSSettleForHalt(void)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    TagFSShutForHalt();

    /* A medium that is away takes no cycle, and the stopping machine is the
     * event its open book waited for. */
    if (TagFSMediumLeft()) {
        AwayGiveUp(AWAY_HALT, tagfs_get_seat());
        s_halt_answer = ERR_DEVICE_NOT_READY;
        s_halt_ran    = true;
        tagfs_leave();
        return ERR_DEVICE_NOT_READY;
    }

    /* A volume under glass was given nothing this boot: there is nothing for
     * a cycle to settle, and none is asked for (halt_sync_storage says so). */
    if (TagFSIsUnderGlass()) {
        kprintf("[TagFS] the machine is stopping and the volume is under glass: there is nothing to "
                "settle\n");
        s_halt_answer = OK;
        s_halt_ran    = true;
        tagfs_leave();
        return OK;
    }

    error_t rc    = SettleHaltCycles();
    s_halt_answer = rc;
    s_halt_ran    = true;
    tagfs_leave();
    return rc;
}
