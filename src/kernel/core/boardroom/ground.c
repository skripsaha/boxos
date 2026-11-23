#include "ground.h"
#include "disk_table.h"
#include "boardroom.h"
#include "klib.h"

/*
 * The ground of a seat: the BoxOS partitions of its table, as DiskTable reads
 * it (disk_table.h) — every number there in the medium's own blocks, every
 * number here in the room's 512-byte sectors; the two meet here and nowhere
 * else.
 */

const char *GroundOriginName(GroundOrigin origin)
{
    switch (origin) {
    case GROUND_FROM_MBR: return "MBR";
    case GROUND_FROM_GPT: return "GPT";
    default:              return "?";
    }
}

uint8_t GroundSurvey(uint8_t seat, MediumGround *out, uint8_t max, bool *refused)
{
    if (refused) *refused = false;
    if (!out || max == 0) return 0;

    DiskTable t;
    if (DiskTableRead(seat, &t) != OK)
        return 0;
    if (t.state == DISK_TABLE_REFUSED) {
        if (refused) *refused = true;
        DiskTableLetGo(&t);
        return 0;
    }

    const uint64_t ratio   = t.block_bytes / BOARDROOM_SECTOR_BYTES;
    uint8_t        claimed = 0;
    uint32_t       boxos   = 0;
    for (uint32_t i = 0; i < t.part_count; i++) {
        const DiskPart *p = &t.parts[i];
        if (p->kind != DISK_PART_BOXOS) continue;
        if (boxos++ >= max) continue;
        out[claimed].start_sector = p->first * ratio;
        out[claimed].sectors      = (p->last - p->first + 1) * ratio;
        out[claimed].origin       = t.state == DISK_TABLE_MBR ? GROUND_FROM_MBR : GROUND_FROM_GPT;
        out[claimed].entry        = (uint8_t)p->entry;
        claimed++;
    }
    if (boxos > max) {
        kprintf("[Ground] seat %u carries %u runs of BoxOS ground and the survey it was asked for holds %u; the "
                "rest was not handed over\n", seat, boxos, max);
    }
    DiskTableLetGo(&t);
    return claimed;
}
