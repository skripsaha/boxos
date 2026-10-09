#include "block_map_internal.h"
#include "../scribe/scribe.h"
#include "../metadata_pool/meta_pool.h"
#include "../ink/fallow.h"
#include "amp.h"
#include "atomics.h"
#include "baton.h"

/*
 * The sectors of the map in memory — MAP_PAGE_SLOTS of them, whatever the
 * volume's size — and the debts they carry to the medium.
 *
 * ‼ A SECTOR MOVES WITH THE STATE LOCK LET GO. Every decision about a slot is
 * made under the lock, and the bytes travel without it: a slot on its way in
 * or out is READING or WRITING, its bits are the transfer's, and an edit meant
 * for it is written down as a delta and laid on when the transfer ends. A core
 * that wants a sector another core has on its way waits for that transfer
 * with the lock let go, and a core that must evict a sector memory changed
 * writes it out first. Nothing here reads or writes the medium for a caller
 * that holds a TagFS lock: those never reach this file.
 *
 * ‼ THE MEDIUM'S COPY MAY LEAVE OUT WHAT NOTHING ON THE MEDIUM NAMES YET
 * (BlockMapOweLocked), and memory never does. A sector whose copy left a block out is never taken
 * for clean: it is let go of only after a write of it whole, so whatever is
 * read in is the medium's whole word and nothing has to be laid back on.
 */

static inline uint32_t MapCoreHere(void)
{
    return (uint32_t)amp_get_core_index() + 1u;
}

/* The transfer's core was stopped by a halt: it never comes back. */
static bool MapHolderStopped(const BlockMapPage *p)
{
    uint32_t owner = __atomic_load_n(&p->holder, __ATOMIC_ACQUIRE);
    return owner != 0 &&
           __atomic_load_n(&g_amp.cores[owner - 1].halted, __ATOMIC_ACQUIRE) != 0;
}

static bool MapPageStill(const void *ctx)
{
    const BlockMapPage *p = (const BlockMapPage *)ctx;
    uint8_t state = __atomic_load_n(&p->state, __ATOMIC_ACQUIRE);
    return (state != MAP_PAGE_READING && state != MAP_PAGE_WRITING) || MapHolderStopped(p);
}

/* With the lock let go: until the sector's transfer ends. False when the
 * transfer is this very core's, beneath the caller — waiting would never end. */
static bool MapWaitForPage(BlockMapPage *p)
{
    return ScribeWaitFor(&p->holder, MapPageStill, p) != SCRIBE_SELF;
}

static error_t MapMediumSaid(int said, error_t otherwise)
{
    return said > 0 ? (error_t)said : otherwise;
}

#ifdef CONFIG_VOLUME_TESTS
/* The window + 1 whose next write runs a settle of the map on its way, and
 * that settle's answer — the stand's, to meet a write that carries a debt
 * while it is on its way. */
static volatile uint32_t s_stand_way;
static volatile error_t  s_stand_way_said;
static volatile uint8_t  s_stand_way_ran;

static void MapStandOnItsWay(uint32_t w)
{
    uint32_t armed = w + 1u;
    if (!__atomic_compare_exchange_n(&s_stand_way, &armed, 0u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    s_stand_way_said = BlockMapFlush(0);
    __atomic_store_n(&s_stand_way_ran, 1u, __ATOMIC_RELEASE);
}

void BlockMapStandFlushOnItsWay(uint64_t block)
{
    __atomic_store_n(&s_stand_way_ran, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_stand_way, (uint32_t)(block / MAP_WINDOW_BLOCKS) + 1u, __ATOMIC_RELEASE);
}

bool BlockMapStandFlushedOnItsWay(error_t *said)
{
    __atomic_store_n(&s_stand_way, 0u, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&s_stand_way_ran, __ATOMIC_ACQUIRE))
        return false;
    *said = s_stand_way_said;
    return true;
}
#endif

/*
 * The slot a fetch takes: an empty one, else the clean one least lately used.
 * None clean: the least lately used one that owes the medium — a change, a
 * take, or a block its copy there left out — comes back in *writeback, to be
 * written whole before anything else may use it, and NULL. NULL with no
 * *writeback: every slot is on its way somewhere.
 */
static BlockMapPage *MapVictim(BlockMapPage **writeback)
{
    BlockMapPage *clean = NULL, *owing = NULL;

    *writeback = NULL;
    for (uint32_t s = 0; s < MAP_PAGE_SLOTS; s++) {
        BlockMapPage *p = &g_map.page[s];
        if (p->state == MAP_PAGE_EMPTY)
            return p;
        if (p->state != MAP_PAGE_READY)
            continue;
        if (p->dirty || p->left_out || (g_map.win[p->window].flags & MAP_W_OWED)) {
            if (!owing || p->lru < owing->lru)
                owing = p;
        } else if (!clean || p->lru < clean->lru) {
            clean = p;
        }
    }
    if (!clean)
        *writeback = owing;
    return clean;
}

/* Every block of window w that memory holds taken and nothing the medium
 * holds names yet (BlockMapOweLocked), cleared in `out`: how many. */
static uint32_t MapLeaveOut(uint32_t w, uint8_t *out)
{
    uint64_t first = (uint64_t)w * MAP_WINDOW_BLOCKS;
    return PoolUnnamedLeftOut(first, MAP_WINDOW_BLOCKS, out) +
           FallowUnnamedLeftOut(first, MAP_WINDOW_BLOCKS, out) +
           MapUnsettledLeftOut(first, MAP_WINDOW_BLOCKS, out);
}

/*
 * The sector as memory has it, to the medium — `whole`, or leaving out what
 * nothing on the medium names yet (MapLeaveOut), from a copy: memory's bits
 * are never touched. Entered and left with the state lock held, taken again in
 * the name of `site`. The write carries every take laid on the sector before
 * it, so the window's flag of a debt comes off as it begins — a take made
 * while it travels is a delta, and flags the debt again — and the debt itself
 * is counted paid only when the write lands (BlockMapPage.paying): a settle
 * that meets the write on its way waits for it. A refused write owes again
 * too, and is said, and the medium may hold either copy, so a block left out
 * stays counted as one.
 */
static error_t MapWriteOut(BlockMapPage *p, bool whole, const char *site)
{
    uint32_t        w     = p->window;
    BlockMapWindow *x     = &g_map.win[w];
    bool            owed  = (x->flags & MAP_W_OWED) != 0;
    const uint8_t  *image = p->bits;
    uint32_t        left  = 0;
    uint8_t         out[TAGFS_SECTOR_SIZE];

    if (!whole) {
        memcpy(out, p->bits, sizeof(out));
        left = MapLeaveOut(w, out);
        if (left)
            image = out;
    }
    if (owed) {
        x->flags &= (uint8_t)~MAP_W_OWED;
        p->paying = 1;
    }
    p->dirty = 0;
    p->sent  = ++g_map.sends;
    __atomic_store_n(&p->holder, MapCoreHere(), __ATOMIC_RELAXED);
    __atomic_store_n(&p->state, (uint8_t)MAP_PAGE_WRITING, __ATOMIC_RELEASE);
    TagFSLockDropAt(MapLock(), site);

#ifdef CONFIG_VOLUME_TESTS
    MapStandOnItsWay(w);
#endif
    int said = tagfs_volume_write(g_map.map_vlba + w, 1, image);
    if (said != 0)
        kprintf("[TagFS] the medium would not take sector %u of this volume's block "
                "map — the blocks it covers are taken here and may still read as free "
                "there\n", w);

    TagFSLockTakeAt(MapLock(), site);
    error_t met = said == 0 ? OK : MapMediumSaid(said, ERR_WRITE_FAILED);
    p->ended      = p->sent;
    p->ended_with = met;
    if (p->paying) {
        p->paying = 0;
        g_map.owed--;
    }
    __atomic_store_n(&p->state, (uint8_t)MAP_PAGE_READY, __ATOMIC_RELEASE);
    __atomic_store_n(&p->holder, 0u, __ATOMIC_RELEASE);
    if (said != 0) {
        p->dirty = 1;
        if (owed)
            MapOwedMark(w);
        p->left_out = p->left_out || left != 0;
    } else {
        p->left_out = left != 0;
    }
    if (x->flags & MAP_W_DELTAS) {
        MapDeltaApply(w, p);
        MapMeasure(w);
    }
    return met;
}

/* The sector read into slot `p`, which was taken for window w. Entered and
 * left with the state lock held. */
static error_t MapReadIn(BlockMapPage *p, uint32_t w, const char *site)
{
    BlockMapWindow *x = &g_map.win[w];

    if (p->state == MAP_PAGE_READY)
        g_map.win[p->window].slot = 0;
    p->window     = w;
    p->dirty      = 0;
    p->ended      = 0;
    p->ended_with = OK;
    p->lru        = ++g_map.lru_tick;
    __atomic_store_n(&p->holder, MapCoreHere(), __ATOMIC_RELAXED);
    __atomic_store_n(&p->state, (uint8_t)MAP_PAGE_READING, __ATOMIC_RELEASE);
    x->slot = (uint8_t)(p - g_map.page + 1);
    TagFSLockDropAt(MapLock(), site);

    int said = tagfs_volume_read(g_map.map_vlba + w, 1, p->bits);
    if (said != 0)
        kprintf("[TagFS] sector %u of this volume's block map would not read — the "
                "blocks it covers are decided on what memory knows of them\n", w);

    TagFSLockTakeAt(MapLock(), site);
    __atomic_store_n(&p->holder, 0u, __ATOMIC_RELEASE);
    if (said != 0) {
        x->slot   = 0;
        p->window = MAP_NO_WINDOW;
        __atomic_store_n(&p->state, (uint8_t)MAP_PAGE_EMPTY, __ATOMIC_RELEASE);
        return MapMediumSaid(said, ERR_READ_FAILED);
    }
    __atomic_store_n(&p->state, (uint8_t)MAP_PAGE_READY, __ATOMIC_RELEASE);
    MapDeltaApply(w, p);
    MapMeasure(w);
    return OK;
}

error_t MapFetch(uint32_t w, const char *site)
{
    TagFSLock *lock = MapLock();
    uint8_t    core = amp_get_core_index();

    for (;;) {
        TagFSLockTakeAt(lock, site);
        if (!g_map.up || w >= g_map.windows) {
            TagFSLockDropAt(lock, site);
            return ERR_TAGFS_NOT_INITIALIZED;
        }

        BlockMapWindow *x = &g_map.win[w];
        if (x->slot) {
            BlockMapPage *p = &g_map.page[x->slot - 1];
            if (p->state == MAP_PAGE_READY) {
                p->lru = ++g_map.lru_tick;
                TagFSLockDropAt(lock, site);
                return OK;
            }
            bool stopped = MapHolderStopped(p);
            TagFSLockDropAt(lock, site);
            if (stopped)
                return ERR_DEVICE_NOT_READY;
            if (!MapWaitForPage(p))
                return ERR_WOULD_BLOCK;
            continue;
        }

        BlockMapPage *writeback = NULL;
        BlockMapPage *p         = MapVictim(&writeback);
        if (p) {
            error_t got = MapReadIn(p, w, site);
            TagFSLockDropAt(lock, site);
            return got;
        }
        if (writeback) {
            error_t wrote = MapWriteOut(writeback, true, site);
            TagFSLockDropAt(lock, site);
            if (wrote != OK)
                return wrote;
            continue;
        }
        TagFSLockDropAt(lock, site);
        BatonPump(core);
        cpu_pause();
    }
}

void BlockMapFetchRun(uint64_t start, uint32_t count, const char *site)
{
    if (!g_map.up || count == 0 || start >= g_map.blocks || LockRankAnyHeldHere())
        return;
    uint64_t last = start + count - 1 < g_map.blocks ? start + count - 1 : g_map.blocks - 1;
    (void)MapFetch((uint32_t)(start / MAP_WINDOW_BLOCKS), site);
    if (last / MAP_WINDOW_BLOCKS != start / MAP_WINDOW_BLOCKS)
        (void)MapFetch((uint32_t)(last / MAP_WINDOW_BLOCKS), site);
}

// ----------------------------------------------------------------------------
// Paying the medium
// ----------------------------------------------------------------------------

/* What window w still needs, under the lock: a transfer to wait for, a fetch
 * that lays its deltas on, a write of the takes it owes — or nothing. */
typedef enum { MAP_DUE_NOTHING, MAP_DUE_WAIT, MAP_DUE_FETCH, MAP_DUE_WRITE } MapDue;

static MapDue MapDueAt(uint32_t w, BlockMapPage **out)
{
    BlockMapWindow *x = &g_map.win[w];
    BlockMapPage   *p = x->slot ? &g_map.page[x->slot - 1] : NULL;

    *out = p;
    if (p && (p->state == MAP_PAGE_READING || p->state == MAP_PAGE_WRITING))
        return MAP_DUE_WAIT;
    if (x->flags & MAP_W_DELTAS)
        return MAP_DUE_FETCH;
    if (x->flags & MAP_W_OWED)
        return p ? MAP_DUE_WRITE : MAP_DUE_FETCH;
    return MAP_DUE_NOTHING;
}

/*
 * Every window with work brought to what the medium must hold: deltas laid on,
 * owed takes written, a write already on its way waited for — a take laid on
 * a sector before another core began writing it is carried by that write, and
 * is on the medium only when the write lands. A write the pass waited for
 * that began after the pass did carried every debt the pass came for: the
 * window is done with what it met, landed or refused. One that began before
 * may not have, and the window is looked at again; so is one fetched in. A
 * transfer whose core a halt stopped never ends: the window is refused
 * ERR_DEVICE_NOT_READY. Windows the pass finds owing nothing lose their bit,
 * MAP_SKIP_SPAN of them a hold, the lock let go between.
 */
#define MAP_SKIP_SPAN 256u

/* The write of window w the pass waited for, with the lock held again: true
 * when it decides the window — it, or one after it, ended since the pass
 * began — its refusal said into *failed. */
static bool MapWaitedDecides(uint32_t w, const BlockMapPage *p, uint64_t waited, uint64_t begin,
                             error_t *failed)
{
    if (!waited || waited <= begin || !p || p->window != w || p->ended < waited)
        return false;
    if (p->ended_with != OK && *failed == OK)
        *failed = p->ended_with;
    return true;
}

static error_t MapSettleWindows(const char *site)
{
    TagFSLock *lock   = MapLock();
    error_t    failed = OK;
    uint32_t   w      = 0;
    uint64_t   waited = 0;      /* the write of window w this pass waited for; 0 none */

    TagFSLockTakeAt(lock, site);
    uint64_t begin = g_map.sends;
    for (;;) {
        if (!g_map.up) {
            TagFSLockDropAt(lock, site);
            return ERR_TAGFS_NOT_INITIALIZED;
        }
        BlockMapPage *p       = NULL;
        MapDue        due     = MAP_DUE_NOTHING;
        uint32_t      skipped = 0;
        for (;;) {
            w = MapWorkNext(w);
            if (w >= g_map.windows || (due = MapDueAt(w, &p)) != MAP_DUE_NOTHING)
                break;
            MapWorkDone(w++);
            waited = 0;
            if (++skipped == MAP_SKIP_SPAN)
                break;
        }
        if (w >= g_map.windows) {
            TagFSLockDropAt(lock, site);
            return failed;
        }
        if (due == MAP_DUE_NOTHING || MapWaitedDecides(w, p, waited, begin, &failed)) {
            if (due != MAP_DUE_NOTHING) {
                w++;
                waited = 0;
            }
            TagFSLockDropAt(lock, site);
            TagFSLockTakeAt(lock, site);
            continue;
        }
        waited = 0;

        if (due == MAP_DUE_WRITE) {
            error_t said = MapWriteOut(p, false, site);
            if (said != OK && failed == OK)
                failed = said;
            w++;
            continue;
        }
        if (due == MAP_DUE_WAIT && MapHolderStopped(p)) {
            if (failed == OK)
                failed = ERR_DEVICE_NOT_READY;
            w++;
            continue;
        }

        error_t said = OK;
        if (due == MAP_DUE_WAIT) {
            uint64_t sent = p->state == MAP_PAGE_WRITING ? p->sent : 0;
            TagFSLockDropAt(lock, site);
            if (MapWaitForPage(p))
                waited = sent;
            else
                said = ERR_WOULD_BLOCK;     /* on its way beneath this very caller */
        } else {
            TagFSLockDropAt(lock, site);
            said = MapFetch(w, site);
        }
        TagFSLockTakeAt(lock, site);
        if (said != OK) {
            if (failed == OK)
                failed = said;
            w++;
        }
    }
}

static error_t MapSettleAll(const char *site)
{
    TagFSLockTakeAt(MapLock(), site);
    bool work = g_map.owed || g_map.deltas;
    TagFSLockDropAt(MapLock(), site);
    return work ? MapSettleWindows(site) : OK;
}

void BlockMapSettle(void)
{
    if (g_map.up && !LockRankAnyHeldHere())
        (void)MapSettleAll(__func__);
}

error_t BlockMapFlush(uint64_t target)
{
    if (!g_map.up)
        return OK;
    if (LockRankAnyHeldHere())
        return ERR_WOULD_BLOCK;

    TagFSLockTake(MapLock());
    MapUnsettledSettled(target);
    TagFSLockDrop(MapLock());

    error_t first = MapSettleAll(__func__);
    TagFSLockTake(MapLock());
    for (uint32_t s = 0; s < MAP_PAGE_SLOTS; s++) {
        BlockMapPage *p = &g_map.page[s];
        if (p->state != MAP_PAGE_READY || !p->dirty)
            continue;
        error_t wrote = MapWriteOut(p, false, __func__);
        if (wrote != OK && first == OK)
            first = wrote;
    }
    TagFSLockDrop(MapLock());
    return first;
}

void BlockMapOweLocked(uint64_t block)
{
    if (g_map.up && block < g_map.blocks)
        MapOwedMark((uint32_t)(block / MAP_WINDOW_BLOCKS));
}

// ----------------------------------------------------------------------------
// For the startup tests
// ----------------------------------------------------------------------------

#ifdef CONFIG_VOLUME_TESTS
bool BlockMapMediumSays(uint64_t block, bool *taken)
{
    if (!g_map.up || block >= g_map.blocks)
        return false;
    uint8_t *sector = kmalloc(TAGFS_SECTOR_SIZE);
    if (!sector)
        return false;
    uint32_t off  = (uint32_t)(block % MAP_WINDOW_BLOCKS);
    bool     read = tagfs_volume_read(g_map.map_vlba + block / MAP_WINDOW_BLOCKS, 1, sector) == 0;
    if (read)
        *taken = MapBit(sector, off);
    kfree(sector);
    return read;
}

void BlockMapSectorAway(uint64_t block)
{
    if (!g_map.up || block >= g_map.blocks || LockRankAnyHeldHere())
        return;
    uint32_t w = (uint32_t)(block / MAP_WINDOW_BLOCKS);

    for (;;) {
        TagFSLockTake(MapLock());
        BlockMapWindow *x = &g_map.win[w];
        BlockMapPage   *p = x->slot ? &g_map.page[x->slot - 1] : NULL;
        if (!p) {
            TagFSLockDrop(MapLock());
            return;
        }
        if (p->state != MAP_PAGE_READY) {
            TagFSLockDrop(MapLock());
            if (!MapWaitForPage(p))
                return;
            continue;
        }
        if (p->dirty || p->left_out || (x->flags & (MAP_W_OWED | MAP_W_DELTAS))) {
            error_t wrote = MapWriteOut(p, true, __func__);
            TagFSLockDrop(MapLock());
            if (wrote != OK)
                return;
            continue;
        }
        x->slot   = 0;
        p->window = MAP_NO_WINDOW;
        __atomic_store_n(&p->state, (uint8_t)MAP_PAGE_EMPTY, __ATOMIC_RELEASE);
        TagFSLockDrop(MapLock());
        return;
    }
}
#endif
