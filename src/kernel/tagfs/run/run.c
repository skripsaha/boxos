#include "run.h"
#include "boardroom.h"
#include "../braid/braid.h"
#include "../read/read.h"
#include "../scribe/scribe.h"   /* VolumeWriteSeen — what a flush must cover now */
#include "tagfs_volume.h"
#ifdef CONFIG_VOLUME_TESTS
#include "../mount/away.h"      /* TagFSStandMediumLeaves — the stand's departure after a run lands */
#endif

/*
 * The data run's door for runs (run.h). Every block goes to its seat by its
 * sector (tagfs_block_to_sector), which is where the volume's base is applied;
 * the door LockRank watches is the Boardroom's, behind it.
 */

#ifdef CONFIG_VOLUME_TESTS
static volatile bool s_stand_leaves_after;

void TagFSStandLeavesAfterRun(void)
{
    __atomic_store_n(&s_stand_leaves_after, true, __ATOMIC_RELEASE);
}
#endif

uint32_t TagFSRunBlocks(TagFSVolume* vol)
{
    uint32_t run = BoardroomSeatRun(tagfs_get_seat(vol)) / TAGFS_BLOCK_SECTORS;
    return run ? run : 1u;
}

/* A Braid (TagFSState.braid) addresses a block
 * at a time: a run through it is its blocks one by one, each off the volume's
 * own medium when the Braid would not give it. A refusal the medium's
 * departure explains is ERR_DEVICE_NOT_READY, as at a block's doors. */
error_t TagFSReadRun(TagFSVolume* vol, uint64_t first, uint32_t count, void *buffer)
{
    if (TagFSMediumGone(vol))
        return ERR_DEVICE_NOT_READY;

    BraidState *braid = vol->state.braid;
    uint8_t    *out   = (uint8_t *)buffer;
    bool        got   = true;
    if (braid) {
        for (uint32_t i = 0; i < count && got; i++) {
            uint8_t *data   = out + (size_t)i * TAGFS_BLOCK_SIZE;
            uint64_t sector = tagfs_block_to_sector(vol, first + i);
            got = BraidReadBlock(braid, sector, data, NULL) == OK ||
                  BoardroomRead(tagfs_get_seat(vol), sector, TAGFS_BLOCK_SECTORS, data) == 0;
        }
    } else {
        got = BoardroomRead(tagfs_get_seat(vol), tagfs_block_to_sector(vol, first),
                            count * TAGFS_BLOCK_SECTORS, buffer) == 0;
    }
    if (!got) {
        TagFSNoteReadRefused(vol);
        return TagFSMediumGone(vol) ? ERR_DEVICE_NOT_READY : ERR_READ_FAILED;
    }
    return OK;
}

error_t TagFSWriteRun(TagFSVolume* vol, uint64_t first, uint32_t count, const void *buffer, uint32_t *out_landed)
{
    const uint8_t *in     = (const uint8_t *)buffer;
    uint32_t       landed = 0;

    *out_landed = 0;
    if (!buffer)
        return ERR_NULL_POINTER;
    if (TagFSMediumGone(vol))
        return ERR_DEVICE_NOT_READY;
    if (TagFSIsUnderGlass(vol))
        return TagFSGlassRefused(vol, "the data run door");

    if (vol->state.braid) {
        /* A Braid addresses a block at a time, each recorded as it lands. */
        while (landed < count &&
               tagfs_write_block(vol, first + landed, in + (size_t)landed * TAGFS_BLOCK_SIZE) == OK)
            landed++;
    } else {
        uint32_t sectors = 0;
        (void)BoardroomWrite(tagfs_get_seat(vol), tagfs_block_to_sector(vol, first),
                             count * TAGFS_BLOCK_SECTORS, in, &sectors);
        if (sectors)
            VolumeWriteSeen(vol);
        landed = sectors / TAGFS_BLOCK_SECTORS;   /* a block cut part way is not one that landed */
        for (uint32_t i = 0; i < landed; i++)
            ReadAheadForgetBlock(vol, first + i);
    }
    *out_landed = landed;
#ifdef CONFIG_VOLUME_TESTS
    if (landed && __atomic_exchange_n(&s_stand_leaves_after, false, __ATOMIC_ACQ_REL))
        TagFSStandMediumLeaves(vol);
#endif
    if (landed == count)
        return OK;
    return TagFSMediumGone(vol) ? ERR_DEVICE_NOT_READY : ERR_WRITE_FAILED;
}

// ----------------------------------------------------------------------------
// What the files' bytes cost the medium
// ----------------------------------------------------------------------------

void TagFSDataCostLaid(TagFSVolume* vol, uint32_t blocks)
{
    uint64_t sectors = (uint64_t)blocks * TAGFS_BLOCK_SECTORS;
    uint64_t most    = BoardroomSeatRun(tagfs_get_seat(vol));
    __atomic_add_fetch(&vol->run.cost.commands, (sectors + most - 1) / most, __ATOMIC_RELAXED);
    __atomic_add_fetch(&vol->run.cost.bytes, (uint64_t)blocks * TAGFS_BLOCK_SIZE, __ATOMIC_RELAXED);
}

void TagFSDataCostWrite(TagFSVolume* vol)
{
    __atomic_add_fetch(&vol->run.cost.writes, 1u, __ATOMIC_RELAXED);
}

void TagFSDataCostTake(TagFSVolume* vol, TagFSDataCost *out)
{
    out->writes   = __atomic_load_n(&vol->run.cost.writes, __ATOMIC_RELAXED);
    out->commands = __atomic_load_n(&vol->run.cost.commands, __ATOMIC_RELAXED);
    out->bytes    = __atomic_load_n(&vol->run.cost.bytes, __ATOMIC_RELAXED);
}

/* n / d with two decimals, as hundredths. */
static uint64_t Hundredths(uint64_t n, uint64_t d)
{
    return d ? (n * 100u + d / 2u) / d : 0;
}

void TagFSDataCostSay(void)
{
    TagFSVolume* vol = TagFSHome();
    TagFSDataCost c;
    TagFSDataCostTake(vol, &c);
    uint64_t a_write = Hundredths(c.commands, c.writes);
    uint64_t a_mib   = Hundredths(c.commands << 20, c.bytes);
    kprintf("[TagFS] files' bytes since boot: %llu write(s) laid %llu byte(s) in %llu "
            "command(s) — %llu.%02llu command(s) a write, %llu.%02llu a MiB; the seat "
            "takes %u block(s) in one\n",
            (unsigned long long)c.writes, (unsigned long long)c.bytes,
            (unsigned long long)c.commands, (unsigned long long)(a_write / 100u),
            (unsigned long long)(a_write % 100u), (unsigned long long)(a_mib / 100u),
            (unsigned long long)(a_mib % 100u), TagFSRunBlocks(vol));
}
