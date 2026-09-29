#include "integrity.h"
#include "../tagfs.h"
#include "../box_hash/box_hash.h"
#include "../scribe/scribe.h"
#include "../../lib/kernel/klib.h"
#include "touch.h"
#include "amp.h"
#include "kheap.h"        /* MapPage: the reserve serves the map's pages */
#include "scheduler.h"    /* a page's transfer is a held span */
#include "system_halt.h"  /* and the halt stops the core only after it */

/* Where the map is, and how much of it there is, are two fields of the volume's
 * Ledger (volume_ledger.h). They used to be two four-byte windows into the
 * superblock's reserved[] area, at offsets this file chose and the CoW layer
 * had to be told to stay out of. They are the volume's state, changed under
 * its lock like every other field of the Ledger. */
#define SB_MAP_BLOCK(fs)  ((fs)->ledger.integrity_map_block)
#define SB_MAP_COUNT(fs)  ((fs)->ledger.integrity_map_blocks)

#define ENTRIES_PER_BLOCK (TAGFS_BLOCK_SIZE / (uint32_t)sizeof(uint64_t))  // 512
#define MAP_CACHE_SLOTS   16          // bounded RAM: 16 * 4 KB = 64 KB, ANY disk size
#define ROT_RING_MAX      64
#define SLOT_EMPTY        0xFFFFFFFFu

#define MAP_IO_NONE   0u   /* the slot's page is nobody's transfer */
#define MAP_IO_READ   1u   /* the page is on its way from the medium into g_page[slot] */
#define MAP_IO_WRITE  2u   /* g_page[slot] is on its way to the medium */

/*
 * One page of the map held in memory: 512 per-block digests. The map is paged
 * on demand: only MAP_CACHE_SLOTS pages live in RAM, so memory is constant
 * regardless of disk size (the old design held total_blocks*8 bytes resident,
 * ~2 GB on a 1 TB disk).
 *
 * ‼ A PAGE GOES TO AND FROM THE MEDIUM WITH THE MAP'S LOCK LET GO.
 *
 * The lock is an interrupts-off spinlock, taken from inside every block read
 * and write. Held across a page's transfer, it kept every core that read or
 * wrote a block spinning with interrupts off for as long as the medium took.
 * So a slot records whose transfer it is out with (`io`, `owner`), and every
 * decision about it is made under the lock while the bytes move without it:
 *
 *   `entries` are the truth in memory, and never a transfer's buffer — a
 *   transfer moves g_page[slot], which only the slot's transfer touches;
 *
 *   a digest recorded while the slot's page is still coming in is written
 *   into `entries` at once and marked in `heard`, so the page that lands is
 *   laid UNDER it rather than over it;
 *
 *   `turn` moves whenever what the slot holds changes, so a transfer that
 *   comes back to a slot emptied under it applies nothing.
 *
 * No block read or write ever waits for a slot: they are called under the
 * locks of whoever reads or writes the block, and a wait there would be a
 * wait with interrupts off. A slot out with a transfer is simply not taken.
 *
 * ‼ A DIGEST THAT COULD NOT BE RECORDED MAKES ITS PAGE UNKNOWN, NOT WRONG.
 *
 * When no slot can be had for a block's page — every one out with a transfer,
 * or pushed aside dirty again and again, or a page that would not go out or
 * come in — the block's new digest is lost, and the medium's page still holds
 * the digest of what the block held before. Checked against that, a healthy
 * block would read as rot. So the page is marked forgotten (g_forgot): until
 * it is laid again every digest in it reads as unknown, and it is laid again
 * as zeros under whatever was recorded since — the next time it is taken into
 * a slot, which the next flush does for every page still forgotten — so the
 * medium loses the stale digests too.
 *
 * AND A TRANSFER IS A HELD SPAN. A page on its way to or from the medium
 * belongs to its slot until the transfer comes back, and a core stopped by an
 * orderly halt in between would fence that slot for good — its digests never
 * written. So the core holds its scheduler across the transfer
 * (SchedulerHoldHere): the halt lets it finish, and it stops only once the
 * map's lock is let go (HaltStopIfOwedHere).
 */
typedef struct {
    uint32_t          map_idx;                        /* the page it holds; SLOT_EMPTY if none */
    uint32_t          lru;
    uint32_t          turn;                           /* +1 whenever what it holds changes */
    volatile uint8_t  io;                             /* MAP_IO_*: out with a transfer */
    volatile uint32_t owner;                          /* 0, or the transfer's core index + 1 */
    bool              dirty;                          /* entries the medium does not have */
    uint64_t          heard[ENTRIES_PER_BLOCK / 64];  /* recorded while MAP_IO_READ travels */
    uint64_t          entries[ENTRIES_PER_BLOCK];
} MapSlot;

static volatile bool  g_initialized   = false;
static TagFSLock      g_lock          = TAGFS_LOCK_INIT("integrity", LOCK_RANK_LEAF);
static uint32_t       g_map_entries   = 0;       // == total_blocks (bounds check)
static uint32_t       g_map_first_block = 0;
static uint32_t       g_map_count     = 0;       // map blocks on disk
static MapSlot        g_cache[MAP_CACHE_SLOTS];  // ~65 KB static, fixed
static uint64_t       g_page[MAP_CACHE_SLOTS][ENTRIES_PER_BLOCK]
                          __attribute__((aligned(TAGFS_BLOCK_SIZE)));
static uint32_t       g_map_epoch     = 0;       // +1 each IntegrityShutdown
static uint32_t       g_lru_tick      = 0;
static bool           g_no_room_said  = false;
static uint64_t      *g_forgot        = NULL;    // a bit per map page: its digests are unknown
static uint32_t       g_forgot_count  = 0;       // bits set in g_forgot
static BoxHashContext g_ctx;
static uint32_t       g_errors        = 0;
static uint32_t       g_rot_ring[ROT_RING_MAX];
static uint32_t       g_rot_count     = 0;
static uint16_t       g_integrity_tag = 0xFFFF;

static inline bool in_map_region(uint32_t block) {
    return g_map_first_block != 0 &&
           block >= g_map_first_block && block < g_map_first_block + g_map_count;
}

static inline uint32_t MapCoreHere(void)
{
    return (uint32_t)amp_get_core_index() + 1u;
}

/* Under g_lock. */
static inline bool MapForgotten(uint32_t idx)
{
    return g_forgot && (g_forgot[idx / 64] & (1ull << (idx % 64)));
}

static inline void MapForget(uint32_t idx)
{
    if (g_forgot && !MapForgotten(idx)) {
        g_forgot[idx / 64] |= 1ull << (idx % 64);
        g_forgot_count++;
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
    g_forgot[m->map_idx / 64] &= ~(1ull << (m->map_idx % 64));
    g_forgot_count--;
}

static MapSlot *MapFindLocked(uint32_t idx)
{
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        if (g_cache[s].map_idx == idx)
            return &g_cache[s];
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
        MapSlot *m = &g_cache[s];
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

static void MapTransferDone(MapSlot *m)
{
    __atomic_store_n(&m->io, (uint8_t)MAP_IO_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&m->owner, 0u, __ATOMIC_RELEASE);
}

/* The slot's entries as they stand go to the medium from its page buffer.
 * Digests recorded while the page travels land in the entries and mark them
 * dirty again, for the next page-out; a refused write leaves the page owed.
 * The map's own blocks record no digest (in_map_region), so the write never
 * comes back in here. Entered and left with g_lock held, taken again in the
 * name of `site` — the entry this runs for, which is what a LockRank line
 * about the map's lock names. */
static error_t MapPageOut(MapSlot *m, const char *site)
{
    uint32_t s    = (uint32_t)(m - g_cache);
    uint32_t turn = m->turn;
    uint32_t at   = g_map_first_block + m->map_idx;

    memcpy(g_page[s], m->entries, sizeof(m->entries));
    m->dirty = false;
    __atomic_store_n(&m->owner, MapCoreHere(), __ATOMIC_RELAXED);
    __atomic_store_n(&m->io, (uint8_t)MAP_IO_WRITE, __ATOMIC_RELEASE);
    SchedulerHoldHere();
    TagFSLockDropAt(&g_lock, site);

    error_t wrote = tagfs_write_block(at, g_page[s]);

    TagFSLockTakeAt(&g_lock, site);
    SchedulerLetGoHere();
    MapTransferDone(m);
    if (wrote != OK && m->turn == turn)
        m->dirty = true;
    return wrote;
}

/* The page a slot was taken for (MapTakeLocked) is read into its page buffer
 * and laid under whatever was recorded while it travelled. Answers whether the
 * slot now holds the page. A refused read empties the slot, and the digests
 * recorded meanwhile go with it; the caller that recorded one forgets the
 * page. Entered and left with g_lock held, as MapPageOut is. */
static bool MapPageIn(MapSlot *m, const char *site)
{
    uint32_t s    = (uint32_t)(m - g_cache);
    uint32_t turn = m->turn;
    uint32_t at   = g_map_first_block + m->map_idx;

    SchedulerHoldHere();
    TagFSLockDropAt(&g_lock, site);

    error_t got = tagfs_read_block(at, g_page[s]);

    TagFSLockTakeAt(&g_lock, site);
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
            m->entries[off] = g_page[s][off];
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
 * Entered and left with g_lock held, as MapPageOut is. */
static MapSlot *MapSlotFor(uint32_t idx, uint32_t epoch, const char *site, bool *mine,
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
static bool MapOwnerStopped(const MapSlot *m)
{
    uint32_t owner = __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE);
    return owner != 0 &&
           __atomic_load_n(&g_amp.cores[owner - 1].halted, __ATOMIC_ACQUIRE) != 0;
}

/* The wait for a slot another core has out ends when its transfer does — or
 * when that core has been stopped, which the waiter then finds under the lock. */
static bool MapSlotSettled(const void *ctx)
{
    const MapSlot *m = (const MapSlot *)ctx;
    return __atomic_load_n(&m->io, __ATOMIC_ACQUIRE) == MAP_IO_NONE || MapOwnerStopped(m);
}

/*
 * ‼ THE LEDGER NAMES A RUN, AND A REFUSAL IS TAKEN BACK THE SAME WAY.
 *
 * The fields are set under the volume's state lock, so the Ledger's snapshot
 * has both or neither, and the Ledger is written. If that write is refused
 * the fields go back and the Ledger is written again — because the refusal
 * does not say the medium holds no copy naming the run: another core's
 * Ledger write may have carried the fields while this one waited, and landed,
 * and the refused write itself may have reached the medium all the same.
 *
 * A Ledger write that landed is one the medium took into a cache it can lose,
 * and a power cut keeps any subset of what was written since the last kept
 * flush — so the copy naming the run may survive and the copy that takes it
 * back may not. *put_back is true only once a copy naming the old fields
 * landed AND a flush issued after it was kept: that, and nothing less, is what
 * makes the refused run free to give back.
 */
static error_t MapNameInLedger(TagFSState *fs, uint32_t block, uint32_t count,
                               uint32_t was_block, uint32_t was_count, bool *put_back)
{
    TagFSLockTake(&fs->lock);
    SB_MAP_BLOCK(fs) = block;
    SB_MAP_COUNT(fs) = count;
    TagFSLockDrop(&fs->lock);

    *put_back = false;
    error_t written = tagfs_write_ledger();
    if (written == OK)
        return OK;

    TagFSLockTake(&fs->lock);
    SB_MAP_BLOCK(fs) = was_block;
    SB_MAP_COUNT(fs) = was_count;
    TagFSLockDrop(&fs->lock);
    *put_back = tagfs_write_ledger() == OK && VolumeFlushFor(VolumeFlushNeed()) == OK;
    return written;
}

/* One page the map is laid or carried through: one of the kernel's own paths,
 * so the heap serves it from its reserve first (kheap.h). */
static uint8_t *MapPage(void)
{
    KHeapReserveOpen();
    uint8_t *page = (uint8_t *)kmalloc(TAGFS_BLOCK_SIZE);
    KHeapReserveClose();
    return page;
}

/*
 * A fresh map: a run of blocks, zeroed on the medium, made durable, and only
 * then named by the Ledger. Named first, a cut that kept the Ledger and lost
 * the zeros would leave a map made of whatever those blocks held before —
 * every digest in it a false report of rot. Answers the run in *out_first.
 */
static error_t MapLayFresh(TagFSState *fs, uint32_t count, uint32_t *out_first)
{
    uint32_t start = 0;
    uint8_t *page  = MapPage();   /* the zeros, off the stack */

    if (!page) {
        kprintf("[Integrity] no memory for the one page a fresh map is zeroed "
                "with, so nothing on this volume is checked for bit rot this "
                "boot\n");
        return ERR_NO_MEMORY;
    }
    if (tagfs_alloc_blocks(count, &start) != 0 || start == 0) {
        /* Said aloud: a volume mounted without its integrity map is a
         * volume whose bit rot nobody will ever be told about, and that
         * is a property of this machine's disk rather than of this
         * build. It was a debug_printf, which compiles to nothing in a
         * shipped build — so the one boot where it mattered was silent. */
        kprintf("[Integrity] this volume has no room for a %u-block map, "
                "so nothing on it is checked for bit rot this boot\n", count);
        kfree(page);
        return ERR_NO_MEMORY;
    }

    memset(page, 0, TAGFS_BLOCK_SIZE);
    error_t  wrote = OK;
    uint32_t at    = 0;
    while (at < count && (wrote = tagfs_write_block(start + at, page)) == OK)
        at++;
    kfree(page);
    if (wrote != OK) {
        tagfs_free_blocks(start, count);
        kprintf("[Integrity] block %u of a fresh %u-block map would not take its "
                "zeros (error %u), so the room is given back and nothing on this "
                "volume is checked for bit rot this boot\n",
                at, count, (unsigned)wrote);
        return wrote;
    }
    error_t flushed = VolumeFlushFor(VolumeFlushNeed());
    if (flushed != OK) {
        tagfs_free_blocks(start, count);
        kprintf("[Integrity] the zeros of a fresh %u-block map are not proven on "
                "the medium (error %u), so the room is given back and nothing on "
                "this volume is checked for bit rot this boot\n",
                count, (unsigned)flushed);
        return flushed;
    }

    /* ‼ AND THE LEDGER'S ANSWER IS HEARD. The run is allocated, the Ledger is
     * the only thing that will ever name it again, and if the write refuses
     * then the volume comes up with a region that is marked taken in RAM,
     * named nowhere on the medium, and therefore lost to the next mount. */
    bool put_back = false;
    error_t named = MapNameInLedger(fs, start, count, 0, 0, &put_back);
    if (named != OK) {
        if (put_back) {
            tagfs_free_blocks(start, count);
            kprintf("[Integrity] the ledger would not take a fresh %u-block "
                    "map, so the room is given back and nothing on this "
                    "volume is checked for bit rot this boot\n", count);
        } else {
            kprintf("[Integrity] the ledger would not take a fresh %u-block map "
                    "at %u, nor prove it taken back — the run stays taken, since "
                    "the medium may still name it, and nothing on this volume is "
                    "checked for bit rot this boot\n", count, start);
        }
        return named;
    }
    debug_printf("[Integrity] fresh map: %u blocks at %u (%u-slot cache)\n",
                 count, start, MAP_CACHE_SLOTS);
    *out_first = start;
    return OK;
}

/*
 * ‼ THE MAP ON THE MEDIUM IS AS BIG AS IT WAS MADE, NOT AS BIG AS THE
 * VOLUME IS NOW.
 *
 * The count wanted is derived from data_blocks — the size of the volume
 * TODAY. The region was allocated once and the Ledger records how many blocks
 * that was. The day a volume grows into the ground behind it (tagfs.c:
 * volume_take_more_ground) those two numbers disagree, and believing the
 * derived one is not a wrong number on a screen: a map page is written at
 * the map's first block + its index, so every page past the region would land
 * on blocks that belong to FILES.
 *
 * The answer is to lay down a region that covers the volume and CARRY THE OLD
 * ENTRIES INTO THE FRONT OF IT. Every block that already had a checksum keeps
 * it — the map is indexed by block number and the data run never moves, so
 * entry N still describes block N — and the blocks the volume has just gained
 * get room of their own.
 *
 * ‼ AND THREE THINGS IN ORDER, EACH MADE DURABLE BEFORE THE NEXT.
 *
 * The new run is written and flushed before the Ledger names it, so a Ledger
 * that names it never points at a run that is not there. The Ledger is
 * written and flushed before the old run is given back, because the free
 * clears the bits and splices the run into the free list — a file can be
 * handed those blocks at once, and a cut that kept the file and lost the
 * Ledger would leave a volume whose next mount reads a file as checksums, and
 * grows again, and frees that file's blocks. A refusal anywhere leaves the
 * map where the medium says it is.
 *
 * Answers whether the map moved; *first then says where to.
 */
static bool MapGrow(TagFSState *fs, uint32_t *first, uint32_t stored, uint32_t count)
{
    uint32_t start = 0;
    uint8_t *page  = MapPage();   /* one page of the copy, off the stack */
    bool     laid  = page && tagfs_alloc_blocks(count, &start) == 0 && start != 0;

    for (uint32_t i = 0; laid && i < count; i++) {
        if (i < stored)
            laid = tagfs_read_block(*first + i, page) == OK;
        else
            memset(page, 0, TAGFS_BLOCK_SIZE);
        if (laid)
            laid = tagfs_write_block(start + i, page) == OK;
    }
    kfree(page);
    if (laid)
        laid = VolumeFlushFor(VolumeFlushNeed()) == OK;
    if (!laid) {
        if (start)
            tagfs_free_blocks(start, count);
        return false;
    }

    bool put_back = false;
    if (MapNameInLedger(fs, start, count, *first, stored, &put_back) != OK) {
        if (put_back) {
            tagfs_free_blocks(start, count);
            kprintf("[Integrity] a bigger map was laid down and the ledger would "
                    "not take it — the map stays where it was and the room is "
                    "given back\n");
        } else {
            kprintf("[Integrity] a bigger map was laid down and the ledger would "
                    "not take it, nor prove it taken back — the map stays where it "
                    "was, and the %u-block run at %u stays taken, since the medium "
                    "may still name it\n", count, start);
        }
        return false;
    }

    if (VolumeFlushFor(VolumeFlushNeed()) == OK) {
        tagfs_free_blocks(*first, stored);
    } else {
        kprintf("[Integrity] the ledger that names the bigger map is not proven on "
                "the medium, so the old %u-block map at %u stays taken — a volume "
                "that comes up on the old ledger still finds its map there\n",
                stored, *first);
    }
    kprintf("[Integrity] the volume grew, so its map did too: %u block(s) at %u, "
            "carrying the %u it already had — every block of %u is covered\n",
            count, start, stored, fs->layout.data_blocks);
    *first = start;
    return true;
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
    TagFSLockTake(&g_lock);
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        g_cache[s].map_idx = SLOT_EMPTY;
        g_cache[s].dirty   = false;
        g_cache[s].turn++;
        memset(g_cache[s].heard, 0, sizeof(g_cache[s].heard));
    }
    g_lru_tick  = 0;
    g_rot_count = 0;
    TagFSLockDrop(&g_lock);

    uint32_t entries = fs->layout.data_blocks;
    uint32_t count   = (entries + ENTRIES_PER_BLOCK - 1) / ENTRIES_PER_BLOCK;
    BoxHashInit(&g_ctx, fs->uuid, 16);

    uint32_t first = SB_MAP_BLOCK(fs);
    if (first == 0) {
        // First mount: lazily allocate a contiguous map region (first-mount only;
        // later mounts just reuse it). Bounded RAM regardless of how big it is.
        error_t laid = MapLayFresh(fs, count, &first);
        if (laid != OK)
            return laid;
    } else {
        uint32_t stored = SB_MAP_COUNT(fs);
        if (stored != 0 && stored < count && !MapGrow(fs, &first, stored, count)) {
            count   = stored;
            entries = stored * ENTRIES_PER_BLOCK;
            kprintf("[Integrity] the map covers %u block(s) and the volume "
                    "now has %u — a bigger one could not be laid down, so "
                    "verify-on-read covers the first %u and says nothing "
                    "about the rest\n",
                    entries, fs->layout.data_blocks, entries);
        }
        debug_printf("[Integrity] map at %u (%u blocks, %u-slot paged cache)\n",
                     first, count, MAP_CACHE_SLOTS);
        // No full load — pages fault in on demand.
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

    TagFSLockTake(&g_lock);
    g_map_first_block = first;
    g_map_count       = count;
    g_map_entries     = entries;
    g_forgot          = forgot;
    TagFSLockDrop(&g_lock);

    if (fs->registry) {
        TagSpelling integrity = TagSpellingOfKey("integrity");
        if (TagRegistryIntern(fs->registry, &integrity, &g_integrity_tag) != OK)
            g_integrity_tag = TAGFS_INVALID_TAG_ID;
    }

    /* The hooks were no-ops until now, so Init's own I/O recorded nothing. */
    __atomic_store_n(&g_initialized, true, __ATOMIC_RELEASE);
    return OK;
}

void IntegrityUpdate(uint32_t block, const void *data) {
    if (!IntegrityIsInitialized() || !data || block >= g_map_entries || in_map_region(block))
        return;
    uint64_t h       = BoxHashIntegrity(data, TAGFS_BLOCK_SIZE, &g_ctx);
    uint32_t idx     = block / ENTRIES_PER_BLOCK;
    uint32_t off     = block % ENTRIES_PER_BLOCK;
    bool     mine    = false;
    bool     no_room = false;
    bool     said    = false;

    TagFSLockTake(&g_lock);
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
    TagFSLockDrop(&g_lock);
    if (no_room)
        MapSayNoRoom(said);
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

    TagFSLockTake(&g_lock);
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
    TagFSLockDrop(&g_lock);
    if (no_room)
        MapSayNoRoom(said);
    HaltStopIfOwedHere("a page of the integrity map it carried");

    if (stored == 0)
        return true;                          // I/O failure or unknown — never false-positive
    uint64_t h = BoxHashIntegrity(data, TAGFS_BLOCK_SIZE, &g_ctx);
    if (h == stored)
        return true;

    __atomic_fetch_add(&g_errors, 1, __ATOMIC_RELAXED);
    TagFSLockTake(&g_lock);
    if (g_rot_count < ROT_RING_MAX)
        g_rot_ring[g_rot_count++] = block;    // queued for deferred Touch report
    TagFSLockDrop(&g_lock);
    debug_printf("[Integrity] BIT-ROT on block %u: stored=%016lx read=%016lx\n",
                 block, (unsigned long)stored, (unsigned long)h);
    return false;
}

/* Every page still forgotten taken into a slot — laid as zeros there — so
 * the flush that follows puts the zeros on the medium over the stale digests.
 * Answers how many could not be had a slot this time: they stay forgotten,
 * reading as unknown, for the next flush. Entered and left with g_lock held,
 * as MapPageOut is. */
static uint32_t MapLayForgotten(uint32_t epoch)
{
    uint32_t left = 0;

    for (uint32_t w = 0; g_forgot_count > left && g_map_epoch == epoch &&
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

    TagFSLockTake(&g_lock);
    uint32_t epoch  = g_map_epoch;
    uint32_t unlaid = g_forgot_count ? MapLayForgotten(epoch) : 0;
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS && g_map_epoch == epoch; ) {
        MapSlot *m = &g_cache[s];
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
            TagFSLockDrop(&g_lock);
            ScribeTurn waited = ScribeWaitFor(&m->owner, MapSlotSettled, m);
            TagFSLockTake(&g_lock);
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
    TagFSLockDrop(&g_lock);

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

void IntegrityDrainReports(void) {
    if (!IntegrityIsInitialized() || g_integrity_tag == 0xFFFF)
        return;
    uint32_t local[ROT_RING_MAX];
    uint32_t n;
    TagFSLockTake(&g_lock);
    n = g_rot_count;
    if (n > ROT_RING_MAX) n = ROT_RING_MAX;
    memcpy(local, g_rot_ring, n * sizeof(uint32_t));
    g_rot_count = 0;
    TagFSLockDrop(&g_lock);

    for (uint32_t i = 0; i < n; i++) {
        struct __attribute__((packed)) {
            uint32_t block;
            uint8_t  op;        // 3 = integrity / bit-rot
            uint8_t  _pad[3];
        } ev = { local[i], 3, {0, 0, 0} };
        TouchPublishId(g_integrity_tag, &ev, sizeof(ev), 0, TOUCH_FLAG_TAGFS);
    }
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
    if (!IntegrityIsInitialized())
        return;
    IntegrityFlush();

    uint32_t me      = MapCoreHere();
    uint32_t beneath = 0;
    uint32_t stopped = 0;

    TagFSLockTake(&g_lock);
    if (!IntegrityIsInitialized()) {
        TagFSLockDrop(&g_lock);
        return;
    }
    __atomic_store_n(&g_initialized, false, __ATOMIC_RELEASE);
    g_map_epoch++;
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        g_cache[s].map_idx = SLOT_EMPTY;
        g_cache[s].dirty   = false;
        g_cache[s].turn++;
        memset(g_cache[s].heard, 0, sizeof(g_cache[s].heard));
    }
    g_map_first_block = 0;
    g_map_count = 0;
    g_map_entries = 0;
    uint64_t *forgot   = g_forgot;
    uint32_t  unlaid   = g_forgot_count;
    g_forgot       = NULL;
    g_forgot_count = 0;

    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; ) {
        MapSlot *m = &g_cache[s];
        if (m->io == MAP_IO_NONE) {
            s++;
            continue;
        }
        if (m->owner == me || MapOwnerStopped(m)) {
            if (m->owner == me) beneath++; else stopped++;
            s++;
            continue;
        }
        TagFSLockDrop(&g_lock);
        ScribeTurn waited = ScribeWaitFor(&m->owner, MapSlotSettled, m);
        TagFSLockTake(&g_lock);
        if (waited == SCRIBE_SELF) {
            beneath++;
            s++;
        }
    }
    TagFSLockDrop(&g_lock);
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

bool IntegrityIsInitialized(void) { return __atomic_load_n(&g_initialized, __ATOMIC_ACQUIRE); }
uint32_t IntegrityErrorCount(void) { return g_errors; }

void IntegrityMarkMapBlocks(uint8_t *computed_bm, uint32_t total_blocks) {
    if (!computed_bm)
        return;
    TagFSState *fs = tagfs_get_state();
    if (!fs)
        return;
    uint32_t first = SB_MAP_BLOCK(fs);
    uint32_t cnt   = SB_MAP_COUNT(fs);
    if (first == 0)
        return;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t b = first + i;
        if (b < total_blocks)
            computed_bm[b / 8] |= (uint8_t)(1u << (b % 8));
    }
}
