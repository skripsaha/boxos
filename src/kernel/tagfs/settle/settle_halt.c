#include "settle_internal.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../mount/away.h"
#include "tagfs_volume.h"

/*
 * The machine stopping (settle.h): the shut that lets no change begin, and
 * the halt's last cycles — until every Ink settled, the last stitch saying
 * CLEAN (§16.2 п.9).
 */


void SettleHaltMountReset(TagFSVolume* vol)
{
    vol->settle_halt.halt_ran = false;
}

error_t SettleHaltAnswer(TagFSVolume* vol, uint64_t* unsettled)
{
    uint64_t inked   = InkedGen(vol);
    uint64_t settled = SettledGen(vol);
    *unsettled       = inked > settled ? inked - settled : 0;
    return vol->settle_halt.halt_ran ? vol->settle_halt.halt_answer : ERR_TAGFS_METADATA_ERROR;
}

/*
 * ‼ THE LAST STITCH IS CLEAN, AND NOTHING RESTS THAT A HALT MUST WRITE FREE.
 * The book keeps no map on the medium, and a mount derives its ground from
 * the names alone. The cycles go on while an Ink is unsettled
 * — one begun before the shut may be inked while the first runs — each stitch
 * CLEAN when no change is in flight under it (HelmOwed), and the next mount
 * reads the last one CLEAN.
 */
static error_t SettleHaltCycles(TagFSVolume* vol)
{
    error_t rc = SettleAskedFor(vol, SETTLE_FOR_HALT);
    for (uint64_t more = 0; rc == OK && more <= FallowLag(vol) + 1u && InkedGen(vol) > SettledGen(vol); more++)
        rc = SettleAskedFor(vol, SETTLE_FOR_HALT);
    return rc;
}

#ifdef CONFIG_VOLUME_TESTS
error_t TagFSStandSettleForHalt(TagFSVolume* vol)
{
    return SettleHaltCycles(vol);
}

void TagFSStandShutForHalt(TagFSVolume* vol, bool shut)
{
    __atomic_store_n(&vol->settle_halt.halting, shut, __ATOMIC_SEQ_CST);
}
#endif

/* Each side looks only after it has said: a write counts itself among the
 * Helms' before its guard asks here (PlotLoad), and the halt's cycle counts the
 * Helms after the shut — so either the write sees the shut, or the cycle sees
 * the write in flight and seals nothing clean. */
void TagFSShutForHalt(TagFSVolume* vol)
{
    if (__atomic_exchange_n(&vol->settle_halt.halting, true, __ATOMIC_SEQ_CST))
        return;
    if (vol->state.initialized)
        kprintf("[TagFS] the machine is stopping: the volume takes no change from here — the "
                "ones already begun run to their end, and the halt's cycles settle them\n");
}

bool SettleHalting(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->settle_halt.halting, __ATOMIC_SEQ_CST);
}

error_t TagFSSettleForHalt(void)
{
    TagFSVolume* vol = TagFSHome();
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    TagFSShutForHalt(vol);

    /* A medium that is away takes no cycle, and the stopping machine is the
     * event its open book waited for. */
    if (TagFSMediumLeft(vol)) {
        AwayGiveUp(vol, AWAY_HALT, tagfs_get_seat(vol));
        vol->settle_halt.halt_answer = ERR_DEVICE_NOT_READY;
        vol->settle_halt.halt_ran    = true;
        tagfs_leave(vol);
        return ERR_DEVICE_NOT_READY;
    }

    /* A volume under glass was given nothing this boot: there is nothing for
     * a cycle to settle, and none is asked for (halt_sync_storage says so). */
    if (TagFSIsUnderGlass(vol)) {
        kprintf("[TagFS] the machine is stopping and the volume is under glass: there is nothing to "
                "settle\n");
        vol->settle_halt.halt_answer = OK;
        vol->settle_halt.halt_ran    = true;
        tagfs_leave(vol);
        return OK;
    }

    error_t rc    = SettleHaltCycles(vol);
    vol->settle_halt.halt_answer = rc;
    vol->settle_halt.halt_ran    = true;
    tagfs_leave(vol);
    return rc;
}
