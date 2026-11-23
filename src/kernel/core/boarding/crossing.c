/*
 * crossing.c — this start's crossing marked landed. What a crossing is, and
 * where the crossings lie: boxos_crossings.h. Gangway kept this start's in
 * the front of the medium it started from and named the slot and the number in
 * the Boarding Pass (BOARDING_STAMP_CROSSING); when the display takes the
 * screen the slot is read back off the medium the volume stands on, held to
 * what the pass says of it — its number, the volume it started — and written
 * again with the landing in it: how long after the kernel's first instruction.
 *
 * Where the medium answers by itself (AHCI, USB — BoardroomSeatCanReadAsync)
 * nobody waits: the read and the write go out with a berth of their own, each
 * answer knocks, and a K-Core takes the next step (CrossingVisit). A legacy
 * channel has no answer to be told of — the processor moves its bytes — and
 * is read and written by the display's own call, which stands over it the few
 * milliseconds it takes once a start.
 *
 * The write is not flushed here: the volume's own flushes, seconds apart, take
 * the medium's cache down with everything else. A landing lost to a cut in
 * that window is one start counted as a miss — and the panel waits for
 * CROSSINGS_MISSES_PANEL in a row.
 *
 * Lock ordering: none of its own. CrossingLand runs with no lock held; the
 * answer takes none and allocates nothing; a visit runs on a K-Core holding
 * nothing.
 */

#include "crossing.h"
#include "boarding.h"
#include "journey.h"
#include "boxos_crossings.h"
#include "boardroom.h"
#include "roadstead.h"
#include "baton.h"
#include "tagfs.h"
#include "pmm.h"
#include "vmm.h"
#include "crypto.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "klib.h"

typedef enum {
    CROSSING_STEP_READ,
    CROSSING_STEP_WRITE,
} CrossingStep;

static struct {
    uint32_t         begun;       /* CrossingLand has run: once a start */
    uint64_t         sector;
    uint64_t         number;
    uint8_t          seat;
    void*            dma;         /* the slot's page, physical */
    uint32_t         landed_ms;
    CrossingStep     step;        /* what the answer that knocks answers */
    volatile error_t status;
    RoadsteadBerth   berth;
    Knock            knock;
} g_crossing;

/* The kernel's first instruction to now, in milliseconds. */
static uint32_t LandedMs(void)
{
    uint64_t khz   = cpu_get_tsc_freq_khz();
    uint64_t began = JourneyTscOf("kernel");
    uint64_t now   = rdtsc();
    if (!khz || !began || now < began) return 0;
    return (uint32_t)((now - began) / khz);
}

/* The slot read held to the pass and the landing written into it: NULL, or
 * why it is not this start's crossing to mark. */
static const char* Mark(uint8_t* slot)
{
    Crossing c;
    uint8_t  volume[16];

    memcpy(&c, slot, sizeof(c));
    uint32_t crc = c.crc;
    c.crc        = 0;
    if (memcmp(c.magic, CROSSING_MAGIC, sizeof(c.magic)) != 0 || c.size != sizeof(c) ||
        KCrc32((const uint8_t*)&c, sizeof(c)) != crc)
        return "its slot holds no crossing that checks out";
    if (c.number != g_crossing.number) return "its slot holds another crossing";
    if (BoardingPassVolume(volume) && memcmp(c.volume, volume, sizeof(volume)) != 0)
        return "its slot holds a crossing of another volume";
    c.landed    = CROSSING_LANDED;
    c.landed_ms = g_crossing.landed_ms;
    c.crc       = KCrc32((const uint8_t*)&c, sizeof(c));
    memcpy(slot, &c, sizeof(c));
    return NULL;
}

/* Said, and the page given back — nothing is outstanding on it by now. */
static void Done(const char* why)
{
    if (why) {
        kprintf("[Crossing] %[W]crossing %llu's landing is not kept: %s%[D]\n",
                (unsigned long long)g_crossing.number, why);
    } else {
        kprintf("[Crossing] crossing %llu landed %u ms after the kernel began — kept at sector %llu of seat %u\n",
                (unsigned long long)g_crossing.number, g_crossing.landed_ms,
                (unsigned long long)g_crossing.sector, g_crossing.seat);
        JourneyMark("landed");
    }
    if (g_crossing.dma) pmm_free(g_crossing.dma, 1);
    g_crossing.dma = NULL;
}

static void CrossingAnswered(uint8_t index, uint8_t slot, error_t status, void* ctx)
{
    (void)index;
    (void)slot;
    (void)ctx;
    g_crossing.status = status;
    KnockOn(&g_crossing.knock);
}

static void CrossingVisit(void* ctx)
{
    (void)ctx;
    KnockOpen(&g_crossing.knock);
    if (g_crossing.status != OK) {
        Done(g_crossing.step == CROSSING_STEP_READ ? "the medium would not read its slot"
                                                   : "the medium would not write its slot");
        return;
    }
    if (g_crossing.step == CROSSING_STEP_WRITE) {
        Done(NULL);
        return;
    }
    const char* why = Mark((uint8_t*)vmm_phys_to_virt((uintptr_t)g_crossing.dma));
    if (why) {
        Done(why);
        return;
    }
    g_crossing.step = CROSSING_STEP_WRITE;
    if (BoardroomWriteAsync(g_crossing.seat, g_crossing.sector, CROSSINGS_SLOT_SECTORS, g_crossing.dma,
                            CrossingAnswered, NULL, &g_crossing.berth) != OK)
        Done("the medium would not take its slot's write");
}

/* A legacy channel's: read, marked and written by the caller, standing. */
static void Stood(void)
{
    uint8_t*    slot   = (uint8_t*)vmm_phys_to_virt((uintptr_t)g_crossing.dma);
    uint32_t    landed = 0;
    const char* why    = NULL;

    if (BoardroomRead(g_crossing.seat, g_crossing.sector, CROSSINGS_SLOT_SECTORS, slot) != 0)
        why = "the medium would not read its slot";
    else if (!(why = Mark(slot)) &&
             BoardroomWrite(g_crossing.seat, g_crossing.sector, CROSSINGS_SLOT_SECTORS, slot, &landed) != 0)
        why = "the medium would not write its slot";
    Done(why);
}

void CrossingLand(void)
{
    if (__atomic_exchange_n(&g_crossing.begun, 1u, __ATOMIC_ACQ_REL)) return;
    if (!BoardingPassCrossing(&g_crossing.sector, &g_crossing.number)) return;
    g_crossing.landed_ms = LandedMs();

    if (g_crossing.sector < CROSSINGS_FIRST_SLOT || g_crossing.sector >= CROSSINGS_END_SECTOR ||
        (g_crossing.sector - CROSSINGS_FIRST_SLOT) % CROSSINGS_SLOT_SECTORS != 0) {
        Done("the pass names a slot outside the crossings");
        return;
    }
    g_crossing.seat = tagfs_get_seat(TagFSHome());
    if (!BoardroomSeatOccupied(g_crossing.seat)) {
        Done("the medium the volume stood on is gone");
        return;
    }
    g_crossing.dma = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    if (!g_crossing.dma) {
        Done("there is no page to read its slot into");
        return;
    }
    if (!BoardroomSeatCanReadAsync(g_crossing.seat) || !BoardroomSeatCanWriteAsync(g_crossing.seat)) {
        Stood();
        return;
    }
    KnockInit(&g_crossing.knock, CrossingVisit, NULL);
    g_crossing.step = CROSSING_STEP_READ;
    if (BoardroomReadAsync(g_crossing.seat, g_crossing.sector, CROSSINGS_SLOT_SECTORS, g_crossing.dma,
                           CrossingAnswered, NULL, &g_crossing.berth) != OK)
        Done("the medium would not take its slot's read");
}
