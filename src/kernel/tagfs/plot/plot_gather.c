#include "plot_internal.h"
#include "scribe.h"
#include "../run/run.h"
#include "../integrity/integrity.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"   /* InkWearsGuarded, InkTouchesBoot — the files whose blocks stay put */
#include "../block_map/block_map.h"
#include "../settle/sewing.h"

/*
 * PlotGather — a record out of room for extents gets it back by moving the
 * fewest blocks that give it (plot.h).
 *
 * ‼ CONSECUTIVE RUNS, BECAUSE A RUN IS A PLACE IN THE FILE. Gathering runs
 * that are not neighbours in the file into one run on the medium saves
 * nothing: each still begins where the file's order puts it, so the record
 * still needs an extent for each. Only a window of neighbours laid in one run
 * becomes one Strip — as many extents as PackV1 cuts its length into — and
 * among the windows that shed enough, the one with the fewest blocks is the
 * cheapest to copy.
 *
 * The new run is a Stake like any ground the call takes, so a write that goes
 * no further gives it back; the ground it leaves is the record's until a
 * record that no longer names it is inked, and then rests in Fallow.
 *
 * ‼ THE COPY GOES A WINDOW AT A TIME (R24). A defrag gathers every extent of
 * the file, and a file may be gigabytes: its blocks are copied as many at a
 * time as the seat takes in one command (PlotGatherStep), and its holder gives
 * its core back between two windows, the Helm kept. Nothing is named until the
 * last window is copied and made durable, so the move is one Ink, as before.
 *
 * On a book (format v2) the Plot's window is made the whole file first
 * (PlotWindowWhole): the gathered run is fresh ground like any write's, taken
 * whole or not at all, a hole of the file laid in it as zeros, and every block
 * of it named with its digest; no Strip the record names is grown into it.
 */

/* How many extents of the record one run of `blocks` blocks takes — no more
 * blocks than a Strip counts. */
static uint64_t PlotRunExtents(uint64_t blocks)
{
    Strip one = { .count = (uint32_t)blocks, .kind = STRIP_DATA };
    return PackV1Extents(&one, 1);
}

/* B10, whatever the warrant: a warrant lets a call change a file the system
 * keeps, and never moves that file's blocks. Asked with the tags the file
 * wears and the runs the Plot names. */
static error_t PlotGatherKept(const Plot *p, const uint32_t *tags, uint32_t count)
{
    if (!InkWearsGuarded(tags, count) && !InkTouchesBoot(p->runs, p->run_count))
        return OK;
    kprintf("[TagFS] file %llu is one the system keeps — it wears system, boot or god, or "
            "holds the kernel the Deed points the loader at — and its blocks are not "
            "moved\n", p->fid);
    return ERR_PERMISSION_DENIED;
}

/* The window [*first, *last] of runs the loaded record named that sheds at
 * least `shed` extents with the fewest blocks — no more than one Strip counts;
 * `shed` 0 wants every run. */
static bool PlotChooseWindow(const Plot *p, uint64_t shed, uint32_t *first, uint32_t *last,
                             uint64_t *blocks)
{
    uint64_t best = UINT64_MAX;
    uint64_t base = 0;

    if (shed == 0) {
        uint64_t all = 0;
        for (uint32_t i = 0; i < p->run_count; i++)
            all += p->runs[i].count;
        if (all > p->blocks_before || all > UINT32_MAX || p->run_count == 0)
            return false;
        *first  = 0;
        *last   = p->run_count - 1;
        *blocks = all;
        return true;
    }

    for (uint32_t i = 0; i < p->run_count; base += p->runs[i].count, i++) {
        uint64_t sum = 0, end = base, extents = 0;
        for (uint32_t j = i; j < p->run_count; j++) {
            end += p->runs[j].count;
            if (end > p->blocks_before)
                break;                          /* only ground the record named moves */
            sum += p->runs[j].count;
            if (sum >= best || sum > UINT32_MAX)
                break;                          /* ... into one run, as long as a Strip counts */
            extents += PackV1Extents(&p->runs[j], 1);
            if (extents >= PlotRunExtents(sum) + shed) {
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

/* The runs with [first, last] replaced by the run [start, start + blocks) —
 * on a book, every other Strip as it was and the run one of its own. */
static error_t PlotSplice(Plot *p, uint32_t first, uint32_t last, uint64_t start, uint32_t blocks)
{
    uint64_t room = (uint64_t)p->run_count + 1;
    Strip   *runs = room <= UINT32_MAX ? kmalloc(sizeof(Strip) * room) : NULL;
    if (!runs)
        return ERR_NO_MEMORY;

    uint32_t n = 0;
    for (uint32_t i = 0; i < first; i++) {
        if (p->book)
            runs[n++] = p->runs[i];
        else
            PlotRunsPut(runs, &n, p->runs[i].start, p->runs[i].count);
    }
    if (p->book)
        runs[n++] = (Strip){ .start = start, .count = blocks, .kind = STRIP_DATA };
    else
        PlotRunsPut(runs, &n, start, blocks);
    for (uint32_t i = last + 1; i < p->run_count; i++) {
        if (p->book)
            runs[n++] = p->runs[i];
        else
            PlotRunsPut(runs, &n, p->runs[i].start, p->runs[i].count);
    }

    kfree(p->runs);
    p->runs      = runs;
    p->run_count = n;
    p->run_room  = (uint32_t)room;
    return OK;
}

/* One free run of exactly `blocks` near `hint` for the gather: on a book from
 * its derived map past the reserve it keeps, given back when shorter. */
static error_t PlotGatherTake(Plot *p, uint64_t blocks, uint64_t hint, uint64_t *start)
{
    if (!p->book)
        return BlockMapTakeFor(p->fid, (uint32_t)blocks, hint, start, __func__);
    uint32_t got = 0;
    error_t  rc  = AllotmentTakeBook(p->fid, (uint32_t)blocks, p->blocks, hint, false, EndpaperDataKeeps(true),
                                     start, &got);
    if (rc == OK && got < blocks) {
        (void)PlotGiveBack(p, *start, got);
        rc = ERR_TAGFS_NO_SPACE;
    }
    return rc;
}

error_t PlotGatherBegin(Plot *p, uint64_t shed, uint64_t hint, PlotGathering *g)
{
    memset(g, 0, sizeof(*g));
    error_t rc = PlotWindowWhole(p);
    if (rc == OK)
        rc = PlotGatherKept(p, p->meta.tag_ids, p->meta.tag_count);
    if (rc != OK)
        return rc;

    uint32_t first = 0, last = 0;
    uint64_t blocks = 0;
    if (!PlotChooseWindow(p, shed, &first, &last, &blocks)) {
        kprintf("[TagFS] file %llu: no run of its %u extent(s) can be gathered to shed %llu of "
                "them\n", p->fid, p->run_count, (unsigned long long)shed);
        return ERR_TAGFS_RECORD_FULL;
    }

    uint64_t start = 0;
    rc = PlotGatherTake(p, blocks, hint, &start);
    if (rc != OK) {
        kprintf("[TagFS] file %llu: gathering extents %u to %u of it needs one free run of %llu "
                "block(s), and the volume has none that long\n", p->fid, first, last,
                (unsigned long long)blocks);
        return rc;
    }
    rc = PlotStake(p, start, (uint32_t)blocks);
    if (rc != OK) {
        (void)PlotGiveBack(p, start, (uint32_t)blocks);
        return rc;
    }
    /* On a book the record the gather leaves is held its room on the ribbon
     * before a block is copied (P21); the run goes back with the Plot. */
    if (p->book && (rc = PlotWindowRoomAsked(p)) != OK)
        return rc;
    g->first  = first;
    g->last   = last;
    g->blocks = blocks;
    g->to     = start;
    g->extent = first;
    return OK;
}

/* `n` blocks from `from` read into `buf`, each checked against its digest on
 * the way through: OK, or why they would not move to `to` — said. */
static error_t PlotGatherRead(const Plot *p, uint64_t from, uint32_t n, uint8_t *buf, uint64_t to)
{
    error_t rc = TagFSReadRun(from, n, buf);
    for (uint32_t j = 0; j < n && rc == OK; j++)
        if (!IntegrityVerify(from + j, buf + (size_t)j * TAGFS_BLOCK_SIZE))
            rc = ERR_CHECKSUM;
    if (rc != OK)
        kprintf("[TagFS] file %llu: blocks %llu..%llu would not move to %llu (error %u) — its "
                "extents are left where they are\n", p->fid, (unsigned long long)from,
                (unsigned long long)(from + n - 1), (unsigned long long)to, (unsigned)rc);
    return rc;
}

/* The next window of the gather: the blocks after the last one copied, from
 * as many runs as it reaches, read one run's piece at a time and laid into
 * the run in one command. */
static error_t PlotGatherWindow(Plot *p, PlotGathering *g)
{
    uint32_t most = TagFSRunBlocks();
    uint8_t *buf  = PlotRunBuffer(&most);
    if (!buf)
        return ERR_NO_MEMORY;

    uint64_t to     = g->to + g->copied;
    uint32_t filled = 0;
    error_t  rc     = OK;
    while (rc == OK && filled < most && g->extent <= g->last) {
        const Strip *e    = &p->runs[g->extent];
        uint64_t     from = e->start + g->within;
        uint32_t     n    = e->count - g->within;
        if (n > most - filled)
            n = most - filled;
        if (e->kind == STRIP_HOLE)
            memset(buf + (size_t)filled * TAGFS_BLOCK_SIZE, 0, (size_t)n * TAGFS_BLOCK_SIZE);
        else
            rc = PlotGatherRead(p, from, n, buf + (size_t)filled * TAGFS_BLOCK_SIZE, to + filled);
        if (rc != OK)
            break;
        filled    += n;
        g->within += n;
        if (g->within == e->count) {
            g->extent++;
            g->within = 0;
        }
    }
    if (rc == OK && filled > 0) {
        uint32_t landed = 0;
        rc = TagFSWriteRun(to, filled, buf, &landed);
        if (rc == OK)
            rc = PlotWindowLanded(p, to, buf, filled);
        if (rc != OK)
            kprintf("[TagFS] file %llu: the gathered blocks would not be laid at %llu..%llu (error "
                    "%u) — its extents are left where they are\n", p->fid,
                    (unsigned long long)to, (unsigned long long)(to + filled - 1), (unsigned)rc);
    }
    kfree(buf);
    if (rc == OK)
        g->copied += filled;
    return rc;
}

/* Every block copied: the copy made durable before any record can name it
 * there — so it owes a cycle no flush — B10 asked again of the record as it
 * is now, since a tag of those keys put on while the copy went on keeps the
 * file's blocks where they lie as much as one it wore at the load, and the
 * Plot made to name the run. */
static error_t PlotGatherLay(Plot *p, PlotGathering *g)
{
    error_t rc = VolumeFlushFor(VolumeFlushNeed());
    if (rc != OK)
        return rc;

    FileRecord *now = FileMapRecordHold(tagfs_get_state()->files, p->fid, NULL);
    if (now) {
        rc = PlotGatherKept(p, now->tags, now->tag_count);
        FileRecordRelease(now);
        if (rc != OK)
            return rc;
    }

    /* The old ground of the window stays the record's until the new record
     * is inked; it is noted now, before the runs forget it. A move that goes
     * no further notes nothing: the new run is a Stake, and goes back. */
    uint32_t noted = p->freed_count;
    uint32_t count = g->last - g->first + 1;
    for (uint32_t i = g->first; i <= g->last && rc == OK; i++)
        if (p->runs[i].kind == STRIP_DATA)    /* a hole owns no ground to let go */
            rc = PlotLetGoLater(p, p->runs[i].start, p->runs[i].count);
    if (rc == OK)
        rc = PlotSplice(p, g->first, g->last, g->to, (uint32_t)g->blocks);
    if (rc != OK) {
        p->freed_count = noted;
        return rc;
    }
    g->laid = true;
    kprintf("[TagFS] file %llu: %u extent(s) of it, %llu block(s), gathered into one run at %llu "
            "— it now has %u\n", p->fid, count, (unsigned long long)g->blocks,
            (unsigned long long)g->to, p->run_count);
    return OK;
}

error_t PlotGatherStep(Plot *p, PlotGathering *g, bool *out_laid)
{
    error_t rc = OK;
    if (!g->laid) {
        rc = PlotGatherWindow(p, g);
        if (rc == OK && g->copied == g->blocks)
            rc = PlotGatherLay(p, g);
    }
    *out_laid = g->laid;
    return rc;
}

error_t PlotGather(Plot *p, uint64_t shed, uint64_t hint)
{
    PlotGathering g;
    bool          laid = false;
    error_t       rc   = PlotGatherBegin(p, shed, hint, &g);
    while (rc == OK && !laid)
        rc = PlotGatherStep(p, &g, &laid);
    return rc;
}
