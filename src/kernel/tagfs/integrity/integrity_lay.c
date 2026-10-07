#include "integrity_internal.h"
#include "../scribe/scribe.h"
#include "../settle/settle.h"
#include "../ink/fallow.h"
#include "../block_map/block_map.h"
#include "../../lib/kernel/klib.h"
#include "kheap.h"         /* MapZeros: the reserve serves the map's pages */
#include "scheduler.h"     /* a page's transfer is a held span */
#include "system_halt.h"   /* and the halt stops the core only after it */
#include "atomics.h"
#include "cpu_calibrate.h" /* a lay step's time against the last cycle's */

/*
 * ‼ A MAP IS LAID A PAGE AT A TIME BY THE SETTLE CYCLES, AND NAMED ONLY WHEN IT
 * IS WHOLE (RT-15).
 *
 * A volume that carries no map — made before mkfs laid one — or one that grew
 * past the map it has gets a new run of blocks. Every cycle lays some of its
 * pages in a step of its own, for no longer than the last cycle took, so what
 * the medium spends on settling stays within SETTLE_SHARE: zeros for a page no
 * map had; for a page the old map has, its entries as memory holds them,
 * taken through the map's own cache and written as one of its slot's
 * transfers — and from then on every page-out of it goes to both runs
 * (MapPageOut), so neither is ever older than the other. The run is named
 * nowhere until every page is carried and a kept flush covers them: then a
 * cycle's S0 names it in the Ledger, and once that cycle settled the map stands
 * on it and the old run rests until no copy of the Ledger can name it. Until
 * then the old map serves what it covers — or, on a volume that had none,
 * nothing is checked for bit rot, said once. Nothing about the lay is written
 * down: a mount that finds it unfinished begins it again.
 */
typedef struct {
    uint32_t    first;        /* the new run; 0 when no map is being laid */
    uint32_t    count;        /* its pages */
    uint32_t    entries;      /* the blocks it covers */
    uint32_t    carried;      /* pages [0, carried) laid and kept current — under g_map_lock */
    uint64_t    need;         /* the flush need the last page laid holds */
    bool        named;        /* a cycle's S0 named it; it stands once that cycle settles */
    uint32_t    old_first;    /* the run it replaces; 0 for none */
    uint32_t    old_count;
    uint64_t   *forgot;       /* its bit per page, made when the lay began */
    FallowNode *rest;         /* the old run's rest, made when the lay began */
    uint64_t   *spent;        /* the old map's bits, freed once the lay is said */
} MapLay;

static MapLay g_lay;

uint32_t MapLayCarried(uint32_t idx)
{
    return (g_lay.first && idx < g_lay.carried) ? g_lay.first + idx : 0;
}

bool MapLayRunHolds(uint32_t block)
{
    uint32_t first = __atomic_load_n(&g_lay.first, __ATOMIC_ACQUIRE);
    return first != 0 && block >= first && block - first < g_lay.count;
}

void MapLayCarryAgain(uint32_t idx)
{
    if (g_lay.first && g_lay.carried > idx)
        g_lay.carried = idx;
}

/* A block of zeros, or the old run's rest: one of the kernel's own paths, so
 * the heap serves them from its reserve first (kheap.h). */
static uint8_t *MapZeros(void)
{
    KHeapReserveOpen();
    uint8_t *page = (uint8_t *)kmalloc(TAGFS_BLOCK_SIZE);
    KHeapReserveClose();
    if (page)
        memset(page, 0, TAGFS_BLOCK_SIZE);
    return page;
}

/*
 * ‼ A RUN A LEDGER FIELD NAMED GOES BACK ONLY ONCE NO COPY NAMES IT (RT-05).
 * The Ledger has two copies and a cycle writes the older one, so the run the
 * field named before a change stays named by one copy until the cycle after —
 * and a mount that finds the newest copy torn stands on that one. The run
 * rests in Fallow, freed by the next cycle's Ledger, and goes back once both
 * copies moved past it.
 */
static FallowNode *MapRunRest(uint32_t start, uint32_t count)
{
    uint32_t    pieces = (count + 0xFFFFu - 1u) / 0xFFFFu;
    FileExtent *runs   = kmalloc(sizeof(FileExtent) * pieces);
    FallowNode *node   = NULL;
    for (uint32_t i = 0; runs && i < pieces; i++) {
        uint32_t left = count - i * 0xFFFFu;
        runs[i].start_block = start + i * 0xFFFFu;
        runs[i].block_count = (uint16_t)(left < 0xFFFFu ? left : 0xFFFFu);
    }
    if (runs)
        node = FallowNodeMake(runs, pieces, FALLOW_BY_LEDGER, 0);
    kfree(runs);
    return node;
}

/* Page idx of the old map carried into the new run: its entries as memory holds
 * them, written as a transfer of its slot, so no page-out of it can pass the
 * write. OK, or why not — the page is carried again by a later step. */
static error_t MapCarryPage(uint32_t idx)
{
    uint32_t me = MapCoreHere();

    TagFSLockTake(&g_map_lock);
    uint32_t epoch = g_map_epoch;
    for (;;) {
        bool     mine    = false;
        bool     no_room = false;
        MapSlot *m       = MapSlotFor(idx, epoch, __func__, &mine, &no_room);
        if (!m || (mine && !MapPageIn(m, __func__))) {
            TagFSLockDrop(&g_map_lock);
            return ERR_WOULD_BLOCK;
        }
        if (m->io != MAP_IO_NONE) {
            if (m->owner == me || MapOwnerStopped(m)) {
                TagFSLockDrop(&g_map_lock);
                return ERR_WOULD_BLOCK;
            }
            TagFSLockDrop(&g_map_lock);
            (void)ScribeWaitFor(&m->owner, MapSlotSettled, m);
            TagFSLockTake(&g_map_lock);
            continue;
        }

        uint32_t s = (uint32_t)(m - g_map_cache);
        memcpy(g_map_page[s], m->entries, sizeof(m->entries));
        __atomic_store_n(&m->owner, me, __ATOMIC_RELAXED);
        __atomic_store_n(&m->io, (uint8_t)MAP_IO_WRITE, __ATOMIC_RELEASE);
        g_lay.carried = idx + 1u;
        SchedulerHoldHere();
        TagFSLockDropAt(&g_map_lock, __func__);

        error_t wrote = TagFSMetaBlockWrite(g_lay.first + idx, g_map_page[s]);

        TagFSLockTakeAt(&g_map_lock, __func__);
        SchedulerLetGoHere();
        MapTransferDone(m);
        if (wrote != OK && g_lay.carried > idx)
            g_lay.carried = idx;
        TagFSLockDrop(&g_map_lock);
        HaltStopIfOwedHere("a page of the integrity map it carried");
        return wrote;
    }
}

/* The lay begun: its run taken, its bits and the old run's rest made. With no
 * TagFS lock held, from the mount. */
error_t MapLayBegin(uint32_t count, uint32_t entries, uint32_t old_first,
                           uint32_t old_count)
{
    if (IntegrityLayOwed())
        return OK;                  /* the lay this mount began goes on */

    size_t      words  = ((size_t)count + 63u) / 64u;
    uint64_t   *forgot = kmalloc(words * sizeof(uint64_t));
    FallowNode *rest   = old_count ? MapRunRest(old_first, old_count) : NULL;
    uint32_t    start  = 0;

    if (!forgot || (old_count && !rest)) {
        kprintf("[Integrity] no memory to begin laying a %u-block map, so %s this boot\n",
                count, old_count ? "the map covers only what it covered"
                                 : "nothing on this volume is checked for bit rot");
        kfree(forgot);
        FallowNodeLetGo(rest);
        return ERR_NO_MEMORY;
    }
    if (tagfs_alloc_blocks(count, &start) != 0 || start == 0) {
        kprintf("[Integrity] this volume has no room for a %u-block map, so %s this boot\n",
                count, old_count ? "the map covers only what it covered"
                                 : "nothing on it is checked for bit rot");
        kfree(forgot);
        FallowNodeLetGo(rest);
        return ERR_NO_MEMORY;
    }
    memset(forgot, 0, words * sizeof(uint64_t));

    TagFSLockTake(&g_map_lock);
    memset(&g_lay, 0, sizeof(g_lay));
    g_lay.first     = start;
    g_lay.count     = count;
    g_lay.entries   = entries;
    g_lay.old_first = old_first;
    g_lay.old_count = old_count;
    g_lay.forgot    = forgot;
    g_lay.rest      = rest;
    TagFSLockDrop(&g_map_lock);

    kprintf("[Integrity] a %u-block map is laid at %u a page at a time by the Settle cycles — "
            "%s until it stands\n", count, start,
            old_count ? "the map it replaces checks what it covers"
                      : "nothing on this volume is checked for bit rot");
    SettleKnock();
    return OK;
}

bool IntegrityLayOwed(void)
{
    return __atomic_load_n(&g_lay.first, __ATOMIC_ACQUIRE) != 0 && !g_lay.named;
}

uint64_t IntegrityLayNeed(void)
{
    return IntegrityLayOwed() ? g_lay.need : 0;
}

uint32_t IntegrityLayBlocks(void)
{
    return g_lay.first ? g_lay.count : 0;
}

void IntegrityLayStep(uint64_t budget_us)
{
    if (!IntegrityLayOwed())
        return;

    uint8_t *zeros = NULL;
    uint64_t began = cpu_tsc_to_us(rdtsc());
    bool     laid  = false;
    for (;;) {
        TagFSLockTake(&g_map_lock);
        uint32_t idx     = g_lay.carried;
        bool     through = IntegrityIsInitialized() && idx < g_map_count;
        TagFSLockDrop(&g_map_lock);
        if (idx >= g_lay.count)
            break;

        error_t rc = OK;
        if (through) {
            rc = MapCarryPage(idx);
        } else if (!zeros && !(zeros = MapZeros())) {
            break;
        } else if ((rc = TagFSMetaBlockWrite(g_lay.first + idx, zeros)) == OK) {
            TagFSLockTake(&g_map_lock);
            if (g_lay.carried == idx)
                g_lay.carried = idx + 1u;
            TagFSLockDrop(&g_map_lock);
        }
        if (rc != OK)
            break;
        laid = true;
        if (cpu_tsc_to_us(rdtsc()) - began >= budget_us)
            break;
    }
    kfree(zeros);
    if (laid)
        g_lay.need = VolumeFlushNeed();
}

bool IntegrityLayName(void)
{
    TagFSState *fs = tagfs_get_state();
    if (!IntegrityLayOwed())
        return false;
    TagFSLockTake(&g_map_lock);
    bool whole = g_lay.carried >= g_lay.count;
    TagFSLockDrop(&g_map_lock);
    if (!whole || !VolumeFlushKept(g_lay.need))
        return false;
    SB_MAP_BLOCK(fs) = g_lay.first;
    SB_MAP_COUNT(fs) = g_lay.count;
    g_lay.named      = true;
    return true;
}

bool IntegrityLayStands(void)
{
    if (!g_lay.first || !g_lay.named)
        return false;

    TagFSLockTake(&g_map_lock);
    uint32_t forgotten = 0;
    for (uint32_t idx = 0; idx < g_map_count && idx < g_lay.count; idx++) {
        if (MapForgotten(idx)) {
            g_lay.forgot[idx / 64] |= 1ull << (idx % 64);
            forgotten++;
        }
    }
    g_lay.spent       = g_map_forgot;
    g_map_forgot          = g_lay.forgot;
    g_map_forgot_count    = forgotten;
    g_map_first_block = g_lay.first;
    g_map_count       = g_lay.count;
    g_map_entries     = g_lay.entries;
    g_lay.first       = 0;
    TagFSLockDrop(&g_map_lock);

    __atomic_store_n(&g_map_initialized, true, __ATOMIC_RELEASE);
    if (g_lay.rest)
        FallowAdd(g_lay.rest, 0);
    g_lay.rest = NULL;
    return true;
}

void IntegrityLaySay(void)
{
    kprintf("[Integrity] the map stands: %u block(s) at %u cover every block of the volume%s\n",
            g_map_count, g_map_first_block,
            g_lay.old_count ? ", and the run it replaces rests until no copy of the Ledger "
                              "names it" : "");
    kfree(g_lay.spent);
    g_lay.spent = NULL;
}

void IntegrityLayDrop(void)
{
    if (!IntegrityLayOwed())
        return;
    (void)BlockMapGiveLocked(g_lay.first, g_lay.count);
    TagFSLockTake(&g_map_lock);
    g_lay.first = 0;
    TagFSLockDrop(&g_map_lock);
}

/* A lay the volume leaves unfinished is let go of with it: the next mount
 * begins it again. */
void MapLayLetGo(void)
{
    TagFSLockTake(&g_map_lock);
    MapLay gone = g_lay;
    memset(&g_lay, 0, sizeof(g_lay));
    TagFSLockDrop(&g_map_lock);
    if (gone.first)
        kfree(gone.forgot);
    FallowNodeLetGo(gone.rest);
    kfree(gone.spent);
}

#ifdef CONFIG_VOLUME_TESTS
error_t IntegrityStandLayBegin(void)
{
    TagFSLockTake(&g_map_lock);
    uint32_t first   = g_map_first_block;
    uint32_t count   = g_map_count;
    uint32_t entries = g_map_entries;
    TagFSLockDrop(&g_map_lock);
    if (!IntegrityIsInitialized() || first == 0 || IntegrityLayOwed())
        return ERR_BUSY;

    error_t began = MapLayBegin(count, entries, first, count);
    if (began != OK)
        return began;
    IntegrityLayStep(UINT64_MAX);
    TagFSLockTake(&g_map_lock);
    bool whole = g_lay.carried >= g_lay.count;
    TagFSLockDrop(&g_map_lock);
    return whole ? OK : ERR_WRITE_FAILED;
}

uint32_t IntegrityStandLayFirst(void)
{
    return IntegrityLayOwed() ? g_lay.first : 0;
}

void IntegrityStandLayEnd(void)
{
    TagFSLockTake(&g_map_lock);
    uint32_t first = g_lay.named ? 0 : g_lay.first;
    uint32_t count = g_lay.count;
    TagFSLockDrop(&g_map_lock);
    if (!first)
        return;
    MapLayLetGo();
    tagfs_free_blocks(first, count);
}
#endif
