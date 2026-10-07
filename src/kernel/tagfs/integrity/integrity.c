#include "integrity.h"
#include "../tagfs.h"
#include "../box_hash/box_hash.h"
#include "../scribe/scribe.h"
#include "../mount/ground_census.h"
#include "../../lib/kernel/klib.h"
#include "amp.h"
#include "scheduler.h"    /* a page's transfer is a held span */
#include "system_halt.h"  /* and the halt stops the core only after it */
#include "integrity_internal.h"

volatile bool         g_map_initialized  = false;
TagFSLock             g_map_lock         = TAGFS_LOCK_INIT("integrity", LOCK_RANK_LEAF);
uint32_t              g_map_entries      = 0;       // == total_blocks (bounds check)
uint32_t              g_map_first_block  = 0;
uint32_t              g_map_count        = 0;       // map blocks on disk
MapSlot               g_map_cache[MAP_CACHE_SLOTS]; // ~65 KB static, fixed
uint64_t              g_map_page[MAP_CACHE_SLOTS][ENTRIES_PER_BLOCK]
                          __attribute__((aligned(TAGFS_BLOCK_SIZE)));
uint32_t              g_map_epoch        = 0;       // +1 each IntegrityShutdown
uint64_t             *g_map_forgot       = NULL;    // a bit per map page: its digests are unknown
uint32_t              g_map_forgot_count = 0;       // bits set in g_map_forgot
static uint32_t       g_lru_tick      = 0;
static bool           g_no_room_said  = false;
static BoxHashContext g_ctx;
static uint32_t       g_errors        = 0;

/* The map's own blocks — the run it stands on and the run being laid — take no
 * digest and are checked against none (RT-22). Asked once with no lock, as a
 * first look, and again under g_map_lock, which decides. */
static inline bool in_map_region(uint32_t block) {
    return (g_map_first_block != 0 &&
            block >= g_map_first_block && block < g_map_first_block + g_map_count) ||
           MapLayRunHolds(block);
}

static inline void MapForget(uint32_t idx)
{
    if (g_map_forgot && !MapForgotten(idx)) {
        g_map_forgot[idx / 64] |= 1ull << (idx % 64);
        g_map_forgot_count++;
    }
}

/* A forgotten page taken into a slot is laid again as zeros, nothing read,
 * under what was recorded since. Out of line: its caller's frame is on the
 * deepest road to the medium. */
static __attribute__((noinline)) void MapLayForgottenLocked(MapSlot *m)
{
    for (uint32_t off = 0; off < ENTRIES_PER_BLOCK; off++) {
        if (!(m->heard[off / 64] & (1ull << (off % 64))))
            m->entries[off] = 0;
    }
    m->dirty = true;
    g_map_forgot[m->map_idx / 64] &= ~(1ull << (m->map_idx % 64));
    g_map_forgot_count--;
}

static MapSlot *MapFindLocked(uint32_t idx)
{
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        if (g_map_cache[s].map_idx == idx)
            return &g_map_cache[s];
    }
    return NULL;
}

/* A slot for page idx, taken for this core to read the page into: an empty
 * one, else the clean one least lately used, among the slots no transfer
 * holds. None clean: the least lately used dirty one comes back in *writeback
 * — it has to reach the medium before anything else may use it — and NULL.
 * NULL with no *writeback: every slot is out with a transfer. */
static MapSlot *MapTakeLocked(uint32_t idx, MapSlot **writeback)
{
    MapSlot *clean = NULL;
    MapSlot *dirty = NULL;

    *writeback = NULL;
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        MapSlot *m = &g_map_cache[s];
        if (m->io != MAP_IO_NONE)
            continue;
        if (m->map_idx == SLOT_EMPTY) {
            clean = m;
            break;
        }
        if (m->dirty) {
            if (!dirty || m->lru < dirty->lru) dirty = m;
        } else if (!clean || m->lru < clean->lru) {
            clean = m;
        }
    }
    if (!clean) {
        *writeback = dirty;
        return NULL;
    }

    clean->map_idx = idx;
    clean->turn++;
    clean->dirty   = false;
    clean->lru     = ++g_lru_tick;
    memset(clean->heard, 0, sizeof(clean->heard));
    __atomic_store_n(&clean->owner, MapCoreHere(), __ATOMIC_RELAXED);
    __atomic_store_n(&clean->io, (uint8_t)MAP_IO_READ, __ATOMIC_RELEASE);
    g_no_room_said = false;
    return clean;
}

void MapTransferDone(MapSlot *m)
{
    __atomic_store_n(&m->io, (uint8_t)MAP_IO_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&m->owner, 0u, __ATOMIC_RELEASE);
}

/* The slot's entries as they stand go to the medium from its page buffer.
 * Digests recorded while the page travels land in the entries and mark them
 * dirty again, for the next page-out; a refused write leaves the page owed.
 * Both of its writes go through the meta door, which records no digest, so
 * neither comes back in here — the one into a map being laid used to, and each
 * time it pushed another dirty page out to make room, as deep as the map has
 * slots. Entered and left with g_map_lock held, taken again in the name of
 * `site` — the entry this runs for, which is what a LockRank line about the
 * map's lock names. */
static error_t MapPageOut(MapSlot *m, const char *site)
{
    uint32_t s     = (uint32_t)(m - g_map_cache);
    uint32_t idx   = m->map_idx;
    uint32_t turn  = m->turn;
    uint32_t at    = g_map_first_block + idx;
    uint32_t carry = MapLayCarried(idx);   /* a map being laid holds this page too */

    memcpy(g_map_page[s], m->entries, sizeof(m->entries));
    m->dirty = false;
    __atomic_store_n(&m->owner, MapCoreHere(), __ATOMIC_RELAXED);
    __atomic_store_n(&m->io, (uint8_t)MAP_IO_WRITE, __ATOMIC_RELEASE);
    SchedulerHoldHere();
    TagFSLockDropAt(&g_map_lock, site);

    error_t wrote = TagFSMetaBlockWrite(at, g_map_page[s]);
    error_t also  = (wrote == OK && carry) ? TagFSMetaBlockWrite(carry, g_map_page[s]) : OK;

    TagFSLockTakeAt(&g_map_lock, site);
    SchedulerLetGoHere();
    MapTransferDone(m);
    if (wrote != OK && m->turn == turn)
        m->dirty = true;
    if (also != OK)
        MapLayCarryAgain(idx);
    return wrote;
}

/* The page a slot was taken for (MapTakeLocked) is read into its page buffer
 * and laid under whatever was recorded while it travelled. Answers whether the
 * slot now holds the page. A refused read empties the slot, and the digests
 * recorded meanwhile go with it; the caller that recorded one forgets the
 * page. Entered and left with g_map_lock held, as MapPageOut is. */
bool MapPageIn(MapSlot *m, const char *site)
{
    uint32_t s    = (uint32_t)(m - g_map_cache);
    uint32_t turn = m->turn;
    uint32_t at   = g_map_first_block + m->map_idx;

    SchedulerHoldHere();
    TagFSLockDropAt(&g_map_lock, site);

    error_t got = TagFSMetaBlockRead(at, g_map_page[s]);

    TagFSLockTakeAt(&g_map_lock, site);
    SchedulerLetGoHere();
    if (m->turn != turn) {
        MapTransferDone(m);         /* emptied under the read: nothing here is ours */
        return false;
    }
    if (got != OK) {
        m->map_idx = SLOT_EMPTY;
        m->turn++;
        m->dirty = false;
        MapTransferDone(m);
        return false;
    }
    for (uint32_t off = 0; off < ENTRIES_PER_BLOCK; off++) {
        if (!(m->heard[off / 64] & (1ull << (off % 64))))
            m->entries[off] = g_map_page[s][off];
    }
    MapTransferDone(m);
    return true;
}

/* The slot that holds map page idx: found as it is — perhaps still on its way
 * in for another core — or taken for this core to read it into (*mine), or,
 * the page forgotten, taken and laid again as zeros with nothing to read. A
 * dirty page pushed aside goes to the medium first, and the search starts
 * again, a bounded number of times: pages other cores keep dirtying cannot
 * hold a block's read or write here. NULL when the map was shut down meanwhile,
 * when a page would not go out, or when no slot could be had (*no_room).
 * Entered and left with g_map_lock held, as MapPageOut is. */
MapSlot *MapSlotFor(uint32_t idx, uint32_t epoch, const char *site, bool *mine,
                           bool *no_room)
{
    *mine    = false;
    *no_room = false;
    for (uint32_t tries = 0; tries <= MAP_CACHE_SLOTS; tries++) {
        if (g_map_epoch != epoch)
            return NULL;
        MapSlot *m = MapFindLocked(idx);
        if (m)
            return m;
        MapSlot *writeback = NULL;
        m = MapTakeLocked(idx, &writeback);
        if (m && MapForgotten(idx)) {
            MapLayForgottenLocked(m);
            MapTransferDone(m);
            return m;
        }
        if (m) {
            *mine = true;
            return m;
        }
        if (!writeback)
            break;
        if (MapPageOut(writeback, site) != OK)
            return NULL;
    }
    *no_room = true;
    return NULL;
}

/* Said once for each run of misses, not once per block. Called with the lock
 * let go. */
static void MapSayNoRoom(bool said_before)
{
    if (!said_before)
        kprintf("[Integrity] every page the map keeps in memory is out with a "
                "transfer or owed to the medium, so a block went by without its "
                "digest recorded or checked — the map's page for it reads as "
                "unknown until the next flush lays it again\n");
}

/* A transfer's core was stopped by a halt: the transfer never comes back. */
bool MapOwnerStopped(const MapSlot *m)
{
    uint32_t owner = __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE);
    return owner != 0 &&
           __atomic_load_n(&g_amp.cores[owner - 1].halted, __ATOMIC_ACQUIRE) != 0;
}

/* The wait for a slot another core has out ends when its transfer does — or
 * when that core has been stopped, which the waiter then finds under the lock. */
bool MapSlotSettled(const void *ctx)
{
    const MapSlot *m = (const MapSlot *)ctx;
    return __atomic_load_n(&m->io, __ATOMIC_ACQUIRE) == MAP_IO_NONE || MapOwnerStopped(m);
}

error_t IntegrityInit(void) {
    if (IntegrityIsInitialized())
        return ERR_ALREADY_INITIALIZED;

    TagFSState *fs = tagfs_get_state();
    if (!fs || fs->layout.data_blocks == 0)
        return ERR_NOT_INITIALIZED;

    /* Every slot empty, and its turn moved on. `io` and `owner` are left as
     * they are: only a slot IntegrityShutdown left fenced can still be out, and
     * its page buffer is that transfer's until the transfer ends. */
    TagFSLockTake(&g_map_lock);
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        g_map_cache[s].map_idx = SLOT_EMPTY;
        g_map_cache[s].dirty   = false;
        g_map_cache[s].turn++;
        memset(g_map_cache[s].heard, 0, sizeof(g_map_cache[s].heard));
    }
    g_lru_tick = 0;
    TagFSLockDrop(&g_map_lock);

    uint32_t entries = fs->layout.data_blocks;
    uint32_t count   = (entries + ENTRIES_PER_BLOCK - 1) / ENTRIES_PER_BLOCK;
    BoxHashInit(&g_ctx, fs->uuid, 16);

    /* Rot is named from here on, each block once for this mount, on the tag
     * its watchers listen to (integrity_rot.c). */
    uint16_t tag = TAGFS_INVALID_TAG_ID;
    if (fs->registry) {
        TagSpelling integrity = TagSpellingOfKey("integrity");
        if (TagRegistryIntern(fs->registry, &integrity, &tag) != OK)
            tag = TAGFS_INVALID_TAG_ID;
    }
    IntegrityRotBegin(tag);

    /* A volume with no map has one laid by the cycles, and is checked once it
     * stands; one that grew past its map is checked by the map it has, over
     * what that covers, while a bigger one is laid. */
    uint32_t first  = SB_MAP_BLOCK(fs);
    uint32_t stored = SB_MAP_COUNT(fs);
    if (first == 0) {
        error_t began = MapLayBegin(count, entries, 0, 0);
        return began == OK ? ERR_WOULD_BLOCK : began;
    }
    uint32_t want         = count;
    uint32_t want_entries = entries;
    bool     grows        = stored != 0 && stored < count;
    if (grows) {
        count   = stored;
        entries = stored * ENTRIES_PER_BLOCK;
    }

    /* A bit for every page of the map, off the heap: the map is as big as
     * the volume, and so is the record of which of its pages went unrecorded. */
    uint64_t *forgot = kmalloc(((size_t)count + 63u) / 64u * sizeof(uint64_t));
    if (!forgot) {
        kprintf("[Integrity] no memory to keep a bit for each of the map's %u pages, "
                "so nothing on this volume is checked for bit rot this boot\n", count);
        return ERR_NO_MEMORY;
    }
    memset(forgot, 0, ((size_t)count + 63u) / 64u * sizeof(uint64_t));

    TagFSLockTake(&g_map_lock);
    g_map_first_block = first;
    g_map_count       = count;
    g_map_entries     = entries;
    g_map_forgot          = forgot;
    TagFSLockDrop(&g_map_lock);

    /* The hooks were no-ops until now, so Init's own I/O recorded nothing. */
    __atomic_store_n(&g_map_initialized, true, __ATOMIC_RELEASE);

    if (grows && MapLayBegin(want, want_entries, first, stored) != OK)
        kprintf("[Integrity] the map covers %u block(s) and the volume now has %u — a bigger "
                "one could not be begun, so verify-on-read covers the first %u and says "
                "nothing about the rest\n", entries, fs->layout.data_blocks, entries);
    return OK;
}

static void IntegrityRecord(uint32_t block, const void *data) {
    if (!IntegrityIsInitialized() || !data || block >= g_map_entries || in_map_region(block))
        return;
    uint64_t h       = BoxHashIntegrity(data, TAGFS_BLOCK_SIZE, &g_ctx);
    uint32_t idx     = block / ENTRIES_PER_BLOCK;
    uint32_t off     = block % ENTRIES_PER_BLOCK;
    bool     mine    = false;
    bool     no_room = false;
    bool     said    = false;

    TagFSLockTake(&g_map_lock);
    if (IntegrityIsInitialized() && block < g_map_entries && !in_map_region(block)) {
        uint32_t epoch = g_map_epoch;
        MapSlot *m     = MapSlotFor(idx, epoch, __func__, &mine, &no_room);
        if (m && m->io == MAP_IO_READ) {
            m->entries[off] = h;
            m->heard[off / 64] |= 1ull << (off % 64);
            m->dirty = true;
        } else if (m && m->entries[off] != h) {
            m->entries[off] = h;
            m->dirty = true;
        }
        if (m)
            m->lru = ++g_lru_tick;
        bool kept = m && (!mine || MapPageIn(m, __func__));
        if (!kept && g_map_epoch == epoch)
            MapForget(idx);         /* the digest went nowhere: the page is unknown */
        said = g_no_room_said;
        if (no_room)
            g_no_room_said = true;
    }
    TagFSLockDrop(&g_map_lock);
    if (no_room)
        MapSayNoRoom(said);
    IntegrityRotForget(block, 1);
    HaltStopIfOwedHere("a page of the integrity map it carried");
}

#ifdef CONFIG_VOLUME_TESTS
/* How deep a core has stood inside IntegrityUpdate, and the deepest any core
 * stood since the stand last asked: one, unless the map's own paging records
 * a digest on its way to the medium. Each core's count is its own. */
static uint32_t          s_update_depth[MAX_CORES];
static volatile uint32_t s_update_deepest;

uint32_t IntegrityStandUpdateDeepest(void)
{
    return __atomic_exchange_n(&s_update_deepest, 0u, __ATOMIC_ACQ_REL);
}
#endif

void IntegrityUpdate(uint32_t block, const void *data) {
#ifdef CONFIG_VOLUME_TESTS
    uint8_t  core  = amp_get_core_index();
    uint32_t depth = ++s_update_depth[core];
    uint32_t seen  = __atomic_load_n(&s_update_deepest, __ATOMIC_RELAXED);
    while (depth > seen &&
           !__atomic_compare_exchange_n(&s_update_deepest, &seen, depth, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        ;
    IntegrityRecord(block, data);
    s_update_depth[core]--;
#else
    IntegrityRecord(block, data);
#endif
}

/* ‼ A BLOCK THAT COMES TO REST TAKES NO DIGEST WITH IT (RT-22). Its digest is
 * zeroed before Fallow gives it back, by the cycle that writes the map — so by
 * then the medium calls it unknown, and a next owner whose first write lands
 * and whose digest does not, when the power goes, is read as unknown rather
 * than as rot. A page at a time, paged in as a write's digest would be: the
 * Settle's holder calls it, with no TagFS lock held. */
void IntegrityForget(uint32_t start, uint32_t count) {
    uint64_t end = (uint64_t)start + count;

    IntegrityRotForget(start, count);

    for (uint64_t b = start; b < end && IntegrityIsInitialized(); ) {
        uint32_t idx     = (uint32_t)(b / ENTRIES_PER_BLOCK);
        uint64_t stop    = ((uint64_t)idx + 1u) * ENTRIES_PER_BLOCK;
        bool     mine    = false;
        bool     no_room = false;
        bool     said    = false;
        if (stop > end)
            stop = end;

        TagFSLockTake(&g_map_lock);
        if (IntegrityIsInitialized() && b < g_map_entries) {
            uint32_t epoch = g_map_epoch;
            MapSlot *m     = MapSlotFor(idx, epoch, __func__, &mine, &no_room);
            for (uint64_t k = b; m && k < stop && k < g_map_entries; k++) {
                if (in_map_region((uint32_t)k))
                    continue;
                uint32_t off = (uint32_t)(k % ENTRIES_PER_BLOCK);
                if (m->io == MAP_IO_READ)
                    m->heard[off / 64] |= 1ull << (off % 64);
                m->entries[off] = 0;
                m->dirty = true;
            }
            bool kept = m && (!mine || MapPageIn(m, __func__));
            if (!kept && g_map_epoch == epoch)
                MapForget(idx);     /* the zeros went nowhere: the page is unknown, zeros too */
            said = g_no_room_said;
            if (no_room)
                g_no_room_said = true;
        }
        TagFSLockDrop(&g_map_lock);
        if (no_room)
            MapSayNoRoom(said);
        b = stop;
    }
    HaltStopIfOwedHere("a page of the integrity map it carried");
}

bool IntegrityVerify(uint32_t block, const void *data) {
    if (!IntegrityIsInitialized() || !data || block >= g_map_entries || in_map_region(block))
        return true;
    uint32_t idx     = block / ENTRIES_PER_BLOCK;
    uint32_t off     = block % ENTRIES_PER_BLOCK;
    uint64_t stored  = 0;
    bool     mine    = false;
    bool     no_room = false;
    bool     said    = false;

    TagFSLockTake(&g_map_lock);
    if (IntegrityIsInitialized() && block < g_map_entries && !in_map_region(block) &&
        !MapForgotten(idx)) {
        MapSlot *m = MapSlotFor(idx, g_map_epoch, __func__, &mine, &no_room);
        if (m && mine && !MapPageIn(m, __func__))
            m = NULL;
        /* A page still on its way in for another core knows only what was
         * recorded meanwhile; the rest is unknown, and unknown is never rot. */
        if (m && (m->io != MAP_IO_READ || (m->heard[off / 64] & (1ull << (off % 64)))))
            stored = m->entries[off];
        if (m)
            m->lru = ++g_lru_tick;
        said = g_no_room_said;
        if (no_room)
            g_no_room_said = true;
    }
    TagFSLockDrop(&g_map_lock);
    if (no_room)
        MapSayNoRoom(said);
    HaltStopIfOwedHere("a page of the integrity map it carried");

    if (stored == 0)
        return true;                          // I/O failure or unknown — never false-positive
    uint64_t h = BoxHashIntegrity(data, TAGFS_BLOCK_SIZE, &g_ctx);
    if (h == stored)
        return true;

    __atomic_fetch_add(&g_errors, 1, __ATOMIC_RELAXED);
    IntegrityRotName(block, stored, h);
    return false;
}

/* Every page still forgotten taken into a slot — laid as zeros there — so
 * the flush that follows puts the zeros on the medium over the stale digests.
 * Answers how many could not be had a slot this time: they stay forgotten,
 * reading as unknown, for the next flush. Entered and left with g_map_lock held,
 * as MapPageOut is. */
static uint32_t MapLayForgotten(uint32_t epoch)
{
    uint32_t left = 0;

    for (uint32_t w = 0; g_map_forgot_count > left && g_map_epoch == epoch &&
                         w < (g_map_count + 63u) / 64u; w++) {
        for (uint32_t b = 0; b < 64 && g_map_epoch == epoch; b++) {
            uint32_t idx = w * 64u + b;
            if (idx >= g_map_count || !MapForgotten(idx))
                continue;
            bool     mine    = false;
            bool     no_room = false;
            MapSlot *m       = MapSlotFor(idx, epoch, __func__, &mine, &no_room);
            if (!m || (mine && !MapPageIn(m, __func__)))
                left++;
        }
    }
    return left;
}

/*
 * Every page owed to the medium goes to it. A page out with another core's
 * transfer is waited for — with the lock let go, interrupts as the caller left
 * them, the drain core's continuations running meanwhile (ScribeWaitFor) —
 * and then looked at again. Two it cannot wait for: one out with a transfer on
 * this very core, beneath this call, which only ends after this call does;
 * and one left by a core stopped in the middle of its transfer, which never
 * ends. An orderly halt stops no core there — a transfer is a held span — so
 * that is a core stopped by a panic, or one the roll call gave up on as never
 * having answered. It is fenced where it stands and never taken over: the
 * stopped core's command may still be moving that page buffer.
 *
 * Pages whose digests went unrecorded are laid again first (MapLayForgotten),
 * so their zeros go out with the rest.
 */
error_t IntegrityFlush(void) {
    if (!IntegrityIsInitialized())
        return OK;

    uint32_t me        = MapCoreHere();
    error_t  answer    = OK;
    uint32_t refused   = 0, refused_page = 0;
    error_t  refusal   = OK;
    uint32_t beneath   = 0;
    uint32_t stopped   = 0, stopped_page = 0;

    TagFSLockTake(&g_map_lock);
    uint32_t epoch  = g_map_epoch;
    uint32_t unlaid = g_map_forgot_count ? MapLayForgotten(epoch) : 0;
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS && g_map_epoch == epoch; ) {
        MapSlot *m = &g_map_cache[s];
        if (m->map_idx == SLOT_EMPTY) {
            s++;
            continue;
        }
        if (m->io != MAP_IO_NONE) {
            if (m->owner == me) {
                beneath++;
                s++;
                continue;
            }
            if (MapOwnerStopped(m)) {
                if (stopped++ == 0)
                    stopped_page = m->map_idx;
                s++;
                continue;
            }
            TagFSLockDrop(&g_map_lock);
            ScribeTurn waited = ScribeWaitFor(&m->owner, MapSlotSettled, m);
            TagFSLockTake(&g_map_lock);
            if (waited == SCRIBE_SELF) {
                beneath++;
                s++;
            }
            continue;
        }
        if (m->dirty) {
            error_t wrote = MapPageOut(m, __func__);
            if (wrote != OK && refused++ == 0) {
                refused_page = m->map_idx;
                refusal      = wrote;
            }
        }
        s++;
    }
    TagFSLockDrop(&g_map_lock);

    if (beneath || stopped)
        answer = ERR_WOULD_BLOCK;
    if (refused) {
        answer = ERR_WRITE_FAILED;
        kprintf("[Integrity] %u page(s) of the map would not go to the medium, "
                "the first page %u (error %u) — the digests they hold are not "
                "on it\n", refused, refused_page, (unsigned)refusal);
    }
    if (stopped) {
        kprintf("[Integrity] %u page(s) of the map were out with a transfer on a "
                "core the halt stopped, the first page %u — never taken over, so "
                "the digests they hold are not written, and the blocks they cover "
                "may be reported as rot until they are written again\n",
                stopped, stopped_page);
    }
    if (unlaid) {
        kprintf("[Integrity] %u page(s) of the map whose digests went unrecorded "
                "could not be laid again this time — they read as unknown until a "
                "later flush lays them\n", unlaid);
    }
    HaltStopIfOwedHere("a page of the integrity map it carried");
    return answer;
}

/*
 * The map let go of in memory, its medium copy left as the last flush made
 * it. Returns only when no page of it is out with a transfer anyone will come
 * back from, so the next IntegrityInit finds every page buffer its own — with
 * two exceptions said aloud and fenced, for the reasons IntegrityFlush gives:
 * a transfer beneath this call on this core, and one a stopped core left.
 * Each stays out until its own transfer ends, and its slot is taken by nobody
 * until then.
 */
void IntegrityShutdown(void) {
    IntegrityRotEnd();
    MapLayLetGo();
    if (!IntegrityIsInitialized())
        return;
    IntegrityFlush();

    uint32_t me      = MapCoreHere();
    uint32_t beneath = 0;
    uint32_t stopped = 0;

    TagFSLockTake(&g_map_lock);
    if (!IntegrityIsInitialized()) {
        TagFSLockDrop(&g_map_lock);
        return;
    }
    __atomic_store_n(&g_map_initialized, false, __ATOMIC_RELEASE);
    g_map_epoch++;
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        g_map_cache[s].map_idx = SLOT_EMPTY;
        g_map_cache[s].dirty   = false;
        g_map_cache[s].turn++;
        memset(g_map_cache[s].heard, 0, sizeof(g_map_cache[s].heard));
    }
    g_map_first_block = 0;
    g_map_count = 0;
    g_map_entries = 0;
    uint64_t *forgot   = g_map_forgot;
    uint32_t  unlaid   = g_map_forgot_count;
    g_map_forgot       = NULL;
    g_map_forgot_count = 0;

    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; ) {
        MapSlot *m = &g_map_cache[s];
        if (m->io == MAP_IO_NONE) {
            s++;
            continue;
        }
        if (m->owner == me || MapOwnerStopped(m)) {
            if (m->owner == me) beneath++; else stopped++;
            s++;
            continue;
        }
        TagFSLockDrop(&g_map_lock);
        ScribeTurn waited = ScribeWaitFor(&m->owner, MapSlotSettled, m);
        TagFSLockTake(&g_map_lock);
        if (waited == SCRIBE_SELF) {
            beneath++;
            s++;
        }
    }
    TagFSLockDrop(&g_map_lock);
    if (forgot)
        kfree(forgot);

    if (beneath || stopped)
        kprintf("[Integrity] the map is let go of with %u page transfer(s) still "
                "out beneath this call and %u left by a stopped core — their slots "
                "stay fenced until those transfers end\n", beneath, stopped);
    if (unlaid)
        kprintf("[Integrity] the map is let go of with %u page(s) whose digests went "
                "unrecorded and were never laid again — the blocks they cover may be "
                "reported as rot on a later mount until they are written again\n",
                unlaid);
}

bool IntegrityIsInitialized(void) { return __atomic_load_n(&g_map_initialized, __ATOMIC_ACQUIRE); }
uint32_t IntegrityErrorCount(void) { return g_errors; }

void IntegrityClaimGround(void) {
    TagFSState *fs = tagfs_get_state();
    if (fs && SB_MAP_BLOCK(fs) != 0)
        GroundClaim(GROUND_INTEGRITY, SB_MAP_BLOCK(fs), SB_MAP_COUNT(fs), 0);
}
