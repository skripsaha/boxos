#include "plot_internal.h"
#include "scribe.h"

/*
 * PlotGather — a record out of room for extents gets it back by moving the
 * fewest blocks that give it (plot.h).
 *
 * ‼ CONSECUTIVE EXTENTS, BECAUSE AN EXTENT IS A PLACE IN THE FILE. Gathering
 * extents that are not neighbours in the file into one run on the medium saves
 * nothing: each still begins where the file's order puts it, so the record
 * still needs one extent for each. Only a window of neighbours laid in one run
 * becomes one extent — or as many as its length needs at PLOT_RUN_MOST blocks
 * an extent — and among the windows that shed enough, the one with the fewest
 * blocks is the cheapest to copy.
 *
 * The new run is a Stake like any ground the call takes, so a write that goes
 * no further gives it back; the ground it leaves is the record's until a
 * record that no longer names it is inked, and then rests in Fallow.
 */

/* How many extents `blocks` in one run take. */
static uint32_t PlotPieces(uint64_t blocks)
{
    return (uint32_t)((blocks + PLOT_RUN_MOST - 1) / PLOT_RUN_MOST);
}

/* The window [*first, *last] of extents the loaded record named that sheds at
 * least `shed` extents with the fewest blocks; `shed` 0 wants every extent. */
static bool PlotChooseWindow(const Plot *p, uint32_t shed, uint32_t *first, uint32_t *last,
                             uint64_t *blocks)
{
    uint64_t best = UINT64_MAX;
    uint64_t base = 0;

    if (shed == 0) {
        uint64_t all = 0;
        for (uint32_t i = 0; i < p->run_count; i++)
            all += p->runs[i].block_count;
        if (all > p->blocks_before || p->run_count == 0)
            return false;
        *first  = 0;
        *last   = p->run_count - 1;
        *blocks = all;
        return true;
    }

    for (uint32_t i = 0; i < p->run_count; base += p->runs[i].block_count, i++) {
        uint64_t sum = 0, end = base;
        for (uint32_t j = i; j < p->run_count; j++) {
            end += p->runs[j].block_count;
            if (end > p->blocks_before)
                break;                          /* only ground the record named moves */
            sum += p->runs[j].block_count;
            if (sum >= best)
                break;
            if ((j - i + 1) >= PlotPieces(sum) + shed) {
                best   = sum;
                *first = i;
                *last  = j;
                break;
            }
        }
    }
    *blocks = best;
    return best != UINT64_MAX;
}

/* The window's blocks copied into [to, to + blocks), and made durable before
 * any record can name them there — so the copy owes a cycle no flush. */
static error_t PlotCopyWindow(const Plot *p, uint32_t first, uint32_t last, uint32_t to)
{
    uint8_t *buf = kmalloc(TAGFS_BLOCK_SIZE);
    if (!buf)
        return ERR_NO_MEMORY;

    error_t rc = OK;
    for (uint32_t i = first; i <= last && rc == OK; i++) {
        for (uint32_t b = 0; b < p->runs[i].block_count; b++) {
            uint32_t from = p->runs[i].start_block + b;
            rc = tagfs_read_block(from, buf);
            if (rc == OK)
                rc = tagfs_write_block(to, buf);
            if (rc != OK) {
                kprintf("[TagFS] file %u: block %u would not move to %u (error %u) — its "
                        "extents are left where they are\n", p->fid, from, to, (unsigned)rc);
                break;
            }
            to++;
        }
    }
    kfree(buf);
    if (rc == OK)
        rc = VolumeFlushFor(VolumeFlushNeed());
    return rc;
}

/* The runs with [first, last] replaced by the run [start, start + blocks). */
static error_t PlotSplice(Plot *p, uint32_t first, uint32_t last, uint32_t start, uint32_t blocks)
{
    uint64_t    room = (uint64_t)p->run_count + PlotPieces(blocks) + 2;
    FileExtent *runs = room <= UINT32_MAX ? kmalloc(sizeof(FileExtent) * room) : NULL;
    if (!runs)
        return ERR_NO_MEMORY;

    uint32_t n = 0;
    for (uint32_t i = 0; i < first; i++)
        PlotRunsPut(runs, &n, p->runs[i].start_block, p->runs[i].block_count);
    PlotRunsPut(runs, &n, start, blocks);
    for (uint32_t i = last + 1; i < p->run_count; i++)
        PlotRunsPut(runs, &n, p->runs[i].start_block, p->runs[i].block_count);

    kfree(p->runs);
    p->runs      = runs;
    p->run_count = n;
    p->run_room  = (uint32_t)room;
    return OK;
}

error_t PlotGather(Plot *p, uint32_t shed, uint32_t hint)
{
    uint32_t first = 0, last = 0;
    uint64_t blocks = 0;
    if (!PlotChooseWindow(p, shed, &first, &last, &blocks)) {
        kprintf("[TagFS] file %u: no run of its %u extent(s) can be gathered to shed %u of them\n",
                p->fid, p->run_count, shed);
        return ERR_TAGFS_RECORD_FULL;
    }

    uint32_t start = 0;
    error_t  rc    = BlockMapTake((uint32_t)blocks, hint, &start, __func__);
    if (rc != OK) {
        kprintf("[TagFS] file %u: gathering extents %u to %u of it needs one free run of %llu "
                "block(s), and the volume has none that long\n", p->fid, first, last,
                (unsigned long long)blocks);
        return rc;
    }
    rc = PlotStake(p, start, (uint32_t)blocks);
    if (rc != OK) {
        (void)tagfs_free_blocks(start, (uint32_t)blocks);
        return rc;
    }

    rc = PlotCopyWindow(p, first, last, start);
    if (rc != OK)
        return rc;

    /* The old ground of the window stays the record's until the new record
     * is inked; it is noted now, before the runs forget it. A move that goes
     * no further notes nothing: the new run is a Stake, and goes back. */
    uint32_t noted = p->freed_count;
    uint32_t count = last - first + 1;
    for (uint32_t i = first; i <= last && rc == OK; i++)
        rc = PlotLetGoLater(p, p->runs[i].start_block, p->runs[i].block_count);
    if (rc == OK)
        rc = PlotSplice(p, first, last, start, (uint32_t)blocks);
    if (rc != OK) {
        p->freed_count = noted;
        return rc;
    }
    kprintf("[TagFS] file %u: %u extent(s) of it, %llu block(s), gathered into one run at %u — "
            "it now has %u\n", p->fid, count, (unsigned long long)blocks, start, p->run_count);
    return OK;
}
