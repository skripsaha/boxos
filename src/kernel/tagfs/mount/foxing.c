#include "foxing.h"
#include "book_unit.h"

/*
 * Foxing (foxing.h): the spoiled copies of one reading of the book, in the
 * order of their block, Sheets and seq, so a read asks memory in a few steps
 * however many a spoiled medium gives — a copy found spoiled cost its medium
 * a refusal of every sector already.
 */

typedef struct {
    uint64_t block;
    uint64_t sheets;
    uint64_t seq;
    char     why[sizeof(((BookUnit*)0)->why)];
} FoxingSpot;

static FoxingSpot* s_spots;
static uint32_t    s_nspots;
static uint32_t    s_capspots;
static bool        s_reading;

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
static uint32_t FoxingAt(uint64_t block, uint64_t sheets, uint64_t seq)
{
    uint32_t lo = 0, hi = s_nspots;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (FoxingBefore(&s_spots[mid], block, sheets, seq))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

/* Whether spot `i` is the copy (block, sheets, seq). */
static bool FoxingIs(uint32_t i, uint64_t block, uint64_t sheets, uint64_t seq)
{
    return i < s_nspots && s_spots[i].block == block && s_spots[i].sheets == sheets && s_spots[i].seq == seq;
}

void FoxingBegin(void)
{
    FoxingEnd();
    s_reading = true;
}

bool FoxingSays(uint64_t block, uint64_t sheets, uint64_t seq, char* why, size_t room)
{
    if (!s_reading)
        return false;
    uint32_t i = FoxingAt(block, sheets, seq);
    if (!FoxingIs(i, block, sheets, seq))
        return false;
    ksnprintf(why, room, "%s", s_spots[i].why);
    return true;
}

void FoxingNoted(uint64_t block, uint64_t sheets, uint64_t seq, const char* why)
{
    if (!s_reading)
        return;
    uint32_t i = FoxingAt(block, sheets, seq);
    if (FoxingIs(i, block, sheets, seq))
        return;
    if (s_nspots == s_capspots) {
        uint32_t    cap  = s_capspots ? s_capspots * 2u : 8u;
        FoxingSpot* more = kmalloc(sizeof(FoxingSpot) * cap);
        if (!more)
            return;
        if (s_nspots)
            memcpy(more, s_spots, sizeof(FoxingSpot) * s_nspots);
        kfree(s_spots);
        s_spots    = more;
        s_capspots = cap;
    }
    memmove(&s_spots[i + 1u], &s_spots[i], sizeof(FoxingSpot) * (s_nspots - i));
    s_spots[i] = (FoxingSpot){ .block = block, .sheets = sheets, .seq = seq };
    ksnprintf(s_spots[i].why, sizeof(s_spots[i].why), "%s", why);
    s_nspots++;
}

void FoxingEnd(void)
{
    s_reading = false;
    kfree(s_spots);
    s_spots   = NULL;
    s_nspots  = s_capspots = 0;
}
