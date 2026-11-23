#include "disk_fat.h"
#include "boardroom.h"

static bool DiskFatThere(const DiskFatPlace* p)
{
    return BoardroomSeatOccupied(p->seat) && BoardroomSeatSeating(p->seat) == p->seating;
}

static int DiskFatRead(void* ctx, uint64_t sector, uint32_t count, void* into)
{
    const DiskFatPlace* p = ctx;
    if (!DiskFatThere(p))
        return -1;
    return BoardroomRead(p->seat, p->first + sector * p->per, count * p->per, into) == 0 ? 0 : -1;
}

static int DiskFatWrite(void* ctx, uint64_t sector, uint32_t count, const void* from)
{
    const DiskFatPlace* p = ctx;
    if (!DiskFatThere(p))
        return -1;
    return BoardroomWrite(p->seat, p->first + sector * p->per, count * p->per, from, NULL) == 0 ? 0 : -1;
}

static int DiskFatFlush(void* ctx)
{
    const DiskFatPlace* p = ctx;
    return DiskFatThere(p) && BoardroomFlush(p->seat) == 0 ? 0 : -1;
}

void DiskFatPlaced(DiskFatPlace* p, uint8_t seat, uint32_t seating, uint64_t first, uint64_t blocks,
                   uint32_t block_bytes)
{
    p->seat    = seat;
    p->seating = seating;
    p->per     = block_bytes / BOARDROOM_SECTOR_BYTES;
    p->first   = first * p->per;
    p->medium  = (FatMedium){ .read = DiskFatRead, .write = DiskFatWrite, .flush = DiskFatFlush, .ctx = p,
                              .sector_bytes = block_bytes, .sectors = blocks };
}
