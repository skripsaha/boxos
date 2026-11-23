#include "foxing.h"
#include "book_unit.h"
#include "tagfs_volume.h"

/*
 * Foxing (foxing.h): the spoiled copies of one reading of the book, in the
 * order of their block, Sheets and seq, so a read asks memory in a few steps
 * however many a spoiled medium gives — a copy found spoiled cost its medium
 * a refusal of every sector already.
 */


/* Whether spot `s` lies before the copy (block, sheets, seq) in the order. */
static bool FoxingBefore(const FoxingSpot* s, uint64_t block, uint64_t sheets, uint64_t seq)
{
    if (s->block != block)
        return s->block < block;
    if (s->sheets != sheets)
        return s->sheets < sheets;
    return s->seq < seq;
}

/* The first spot not before the copy: where it is kept, or would be. */
static uint32_t FoxingAt(TagFSVolume* vol, uint64_t block, uint64_t sheets, uint64_t seq)
{
    uint32_t lo = 0, hi = vol->foxing.nspots;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (FoxingBefore(&vol->foxing.spots[mid], block, sheets, seq))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

/* Whether spot `i` is the copy (block, sheets, seq). */
static bool FoxingIs(TagFSVolume* vol, uint32_t i, uint64_t block, uint64_t sheets, uint64_t seq)
{
    return i < vol->foxing.nspots && vol->foxing.spots[i].block == block && vol->foxing.spots[i].sheets == sheets && vol->foxing.spots[i].seq == seq;
}

void FoxingBegin(TagFSVolume* vol)
{
    FoxingEnd(vol);
    vol->foxing.reading = true;
}

bool FoxingSays(TagFSVolume* vol, uint64_t block, uint64_t sheets, uint64_t seq, char* why, size_t room)
{
    if (!vol->foxing.reading)
        return false;
    uint32_t i = FoxingAt(vol, block, sheets, seq);
    if (!FoxingIs(vol, i, block, sheets, seq))
        return false;
    ksnprintf(why, room, "%s", vol->foxing.spots[i].why);
    return true;
}

void FoxingNoted(TagFSVolume* vol, uint64_t block, uint64_t sheets, uint64_t seq, const char* why)
{
    if (!vol->foxing.reading)
        return;
    uint32_t i = FoxingAt(vol, block, sheets, seq);
    if (FoxingIs(vol, i, block, sheets, seq))
        return;
    if (vol->foxing.nspots == vol->foxing.capspots) {
        uint32_t    cap  = vol->foxing.capspots ? vol->foxing.capspots * 2u : 8u;
        FoxingSpot* more = kmalloc(sizeof(FoxingSpot) * cap);
        if (!more)
            return;
        if (vol->foxing.nspots)
            memcpy(more, vol->foxing.spots, sizeof(FoxingSpot) * vol->foxing.nspots);
        kfree(vol->foxing.spots);
        vol->foxing.spots    = more;
        vol->foxing.capspots = cap;
    }
    memmove(&vol->foxing.spots[i + 1u], &vol->foxing.spots[i], sizeof(FoxingSpot) * (vol->foxing.nspots - i));
    vol->foxing.spots[i] = (FoxingSpot){ .block = block, .sheets = sheets, .seq = seq };
    ksnprintf(vol->foxing.spots[i].why, sizeof(vol->foxing.spots[i].why), "%s", why);
    vol->foxing.nspots++;
}

void FoxingEnd(TagFSVolume* vol)
{
    vol->foxing.reading = false;
    kfree(vol->foxing.spots);
    vol->foxing.spots   = NULL;
    vol->foxing.nspots  = vol->foxing.capspots = 0;
}
