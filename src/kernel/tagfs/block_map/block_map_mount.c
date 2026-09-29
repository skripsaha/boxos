#include "block_map_internal.h"

/*
 * The block map brought up with its volume, and let go with it.
 *
 * ‼ THE MAP IS READ ONCE, A BLOCK AT A TIME, AND NEVER HELD WHOLE. A mount
 * used to allocate a copy of the whole map and a second one for its roll call,
 * and when the heap could not give the second the roll call was skipped with
 * nothing said. Now the medium's map passes through one block of memory, eight
 * windows at a time: each window is measured into its summary and judged
 * beside the roll call's own map, and nothing of it is kept but the summary.
 */

#define MAP_NAMED_RUNS 32u

/* The runs of one kind the roll call names: the first MAP_NAMED_RUNS, lowest
 * block first, and the rest as one count — the owner's bound (09-27). A run
 * may cross windows; it is closed where it ends, not where a window does. */
typedef struct {
    uint32_t start[MAP_NAMED_RUNS];
    uint32_t len[MAP_NAMED_RUNS];
    uint32_t named;
    uint32_t more_runs;
    uint32_t more_blocks;
    uint32_t blocks;
    uint32_t open_at;
    uint32_t open_len;
} MapRunTally;

/* The mount runs on one core with the door shut, so the two tallies are
 * nobody else's. */
static MapRunTally s_orphans;
static MapRunTally s_missing;

static void MapTallyClose(MapRunTally *t)
{
    if (!t->open_len)
        return;
    t->blocks += t->open_len;
    if (t->named < MAP_NAMED_RUNS) {
        t->start[t->named] = t->open_at;
        t->len[t->named]   = t->open_len;
        t->named++;
    } else {
        t->more_runs++;
        t->more_blocks += t->open_len;
    }
    t->open_len = 0;
}

static void MapTallyBlock(MapRunTally *t, uint32_t block, bool in)
{
    if (!in) {
        MapTallyClose(t);
        return;
    }
    if (!t->open_len)
        t->open_at = block;
    t->open_len++;
}

/* NAMED, NOT ONLY COUNTED: which blocks, as runs of the data run. The whole
 * list is tools/volume-read.py check's, off the same image, and logcheck's
 * census scenario holds the first 32 and the remainder against it. */
static void MapTallySay(const MapRunTally *t, const char *kind)
{
    for (uint32_t i = 0; i < t->named; i++)
        kprintf("[TagFS FSCK] %s run at data block %u, %u block(s)\n",
                kind, t->start[i], t->len[i]);
    if (t->more_runs)
        kprintf("[TagFS FSCK] and %u more %s run(s), %u block(s) — "
                "tools/volume-read.py names every one\n",
                t->more_runs, kind, t->more_blocks);
}

error_t BlockMapMount(uint32_t data_blocks, uint64_t map_vlba)
{
    BlockMapUnmount();

    uint32_t windows    = (data_blocks + MAP_WINDOW_BLOCKS - 1) / MAP_WINDOW_BLOCKS;
    size_t   win_bytes  = (size_t)windows * sizeof(BlockMapWindow);
    size_t   page_bytes = (size_t)MAP_PAGE_SLOTS * sizeof(BlockMapPage);

    g_map.win  = kmalloc(win_bytes);
    g_map.page = kmalloc(page_bytes);
    if (!g_map.win || !g_map.page) {
        kprintf("[TagFS] the block map of %u blocks keeps a summary of %u window(s) in "
                "%llu bytes and a cache of %u of its sectors in %llu more, and the kernel "
                "heap would not give them\n", data_blocks, windows,
                (unsigned long long)win_bytes, MAP_PAGE_SLOTS,
                (unsigned long long)page_bytes);
        BlockMapUnmount();
        return ERR_NO_MEMORY;
    }
    memset(g_map.win, 0, win_bytes);
    memset(g_map.page, 0, page_bytes);
    for (uint32_t s = 0; s < MAP_PAGE_SLOTS; s++)
        g_map.page[s].window = MAP_NO_WINDOW;

    g_map.blocks   = data_blocks;
    g_map.windows  = windows;
    g_map.map_vlba = map_vlba;
    return OK;
}

/*
 * One window of the medium's map against the roll call's: its orphans and
 * missing blocks counted and named, and the missing ones laid into `sector`,
 * so that its summary counts them taken. They are written down as takes of
 * the window, owed to the medium, and reach it with the first settle — the
 * mount itself writes nothing. False when there is no memory to write the
 * repair down.
 */
static bool MapCensusWindow(uint32_t w, uint8_t *sector, const uint8_t *computed)
{
    uint32_t       base = w * MAP_WINDOW_BLOCKS;
    uint32_t       in   = g_map.blocks - base < MAP_WINDOW_BLOCKS ? g_map.blocks - base
                                                                  : MAP_WINDOW_BLOCKS;
    const uint8_t *owns = computed + base / 8;

    uint32_t runs = 0;
    bool     was  = false;
    for (uint32_t b = 0; b < in; b++) {
        bool missing = !MapBit(sector, b) && MapBit(owns, b);
        runs += missing && !was;
        was   = missing;
    }
    if (runs && !MapDeltaRoom(runs))
        return false;

    uint32_t open = 0, open_at = 0;
    for (uint32_t b = 0; b < in; ) {
        uint8_t orphan  = (uint8_t)(sector[b / 8] & (uint8_t)~owns[b / 8]);
        uint8_t missing = (uint8_t)((uint8_t)~sector[b / 8] & owns[b / 8]);
        if ((b % 8) == 0 && b + 8 <= in && !orphan && !missing) {
            MapTallyClose(&s_orphans);
            MapTallyClose(&s_missing);
            if (open) {
                MapDeltaAdd(base + open_at, open, true);
                open = 0;
            }
            b += 8;
            continue;
        }
        uint8_t bit = (uint8_t)(1u << (b % 8));
        MapTallyBlock(&s_orphans, base + b, (orphan & bit) != 0);
        MapTallyBlock(&s_missing, base + b, (missing & bit) != 0);
        if (missing & bit) {
            if (!open)
                open_at = b;
            open++;
        } else if (open) {
            MapDeltaAdd(base + open_at, open, true);
            open = 0;
        }
        b++;
    }
    if (open)
        MapDeltaAdd(base + open_at, open, true);

    if (runs) {
        for (uint32_t i = 0; i < (in + 7) / 8; i++)
            sector[i] |= owns[i];
        MapOwedMark(w);
    }
    return true;
}

error_t BlockMapCensus(const uint8_t *computed, BlockMapCensusCount *out)
{
    uint8_t *chunk = kmalloc(TAGFS_BLOCK_SIZE);
    if (!chunk) {
        kprintf("[TagFS] no memory for the one block this volume's block map is read "
                "through\n");
        return ERR_NO_MEMORY;
    }

    memset(&s_orphans, 0, sizeof(s_orphans));
    memset(&s_missing, 0, sizeof(s_missing));
    uint64_t free = 0;
    error_t  rc   = OK;

    for (uint32_t w0 = 0; w0 < g_map.windows && rc == OK; w0 += TAGFS_BLOCK_SECTORS) {
        uint32_t n = g_map.windows - w0 < TAGFS_BLOCK_SECTORS ? g_map.windows - w0
                                                             : TAGFS_BLOCK_SECTORS;
        if (tagfs_volume_read(g_map.map_vlba + w0, n, chunk) != 0) {
            rc = ERR_TAGFS_CORRUPTED;
            break;
        }
        for (uint32_t k = 0; k < n; k++) {
            uint32_t w      = w0 + k;
            uint8_t *sector = chunk + k * TAGFS_SECTOR_SIZE;
            if (computed && !MapCensusWindow(w, sector, computed)) {
                kprintf("[TagFS FSCK] no memory to write down the repair of window %u of "
                        "the block map\n", w);
                rc = ERR_NO_MEMORY;
                break;
            }
            MapMeasureBits(w, sector);
            free += g_map.win[w].free;
        }
    }
    kfree(chunk);
    if (rc != OK)
        return rc;

    MapTallyClose(&s_orphans);
    MapTallyClose(&s_missing);
    if (s_orphans.blocks || s_missing.blocks) {
        /* ‼ SAID ALOUD. An orphan is the footprint a severed chain leaves —
         * shorter than it was, its lost blocks still taken and nothing
         * pointing at them — and the count is the evidence; a volume that has
         * lost something says so on the boot that finds it. The repair keeps
         * every block the medium calls taken: a roll call that missed an owner
         * costs a block, never a file. */
        kprintf("[TagFS FSCK] Bitmap mismatch: %u orphan, %u missing - additive repair\n",
                s_orphans.blocks, s_missing.blocks);
        MapTallySay(&s_orphans, "orphan");
        MapTallySay(&s_missing, "missing");
    }

    g_map.first_room = 0;
    g_map.up         = true;
    out->orphan  = s_orphans.blocks;
    out->missing = s_missing.blocks;
    out->free    = free;
    return OK;
}

void BlockMapUnmount(void)
{
    AllotmentDropAll();
    kfree(g_map.win);
    kfree(g_map.page);
    kfree(g_map.delta);
    memset(&g_map, 0, sizeof(g_map));
    g_map.work_lo = 1;
    g_map.work_hi = 0;
}
