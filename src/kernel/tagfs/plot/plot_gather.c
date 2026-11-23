#include "plot_internal.h"
#include "scribe.h"
#include "../run/run.h"
#include "../integrity/integrity.h"
#include "../file_map/file_map.h"
#include "../file_map/tail_slip.h"
#include "../ink/ink.h"   /* InkFileKept, InkTouchesBoot — the files whose blocks stay put */
#include "../block_map/block_map.h"
#include "../settle/sewing.h"
#include "../mount/away.h"   /* AwayDepartures — what a book's write is held to at its Ink */
#include "tagfs_volume.h"

/*
 * PlotGather — a stretch of a file's runs laid in one run of ground: a defrag
 * (plot.h).
 *
 * ‼ CONSECUTIVE RUNS, BECAUSE A RUN IS A PLACE IN THE FILE. Gathering runs
 * that are not neighbours in the file into one run on the medium saves
 * nothing: each still begins where the file's order puts it. Only a window of
 * neighbours laid in one run becomes one Strip.
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
 * The Plot's window is made the whole file first (PlotWindowWhole): the gathered run is fresh ground like any write's, taken
 * whole or not at all, a hole of the file laid in it as zeros, and every block
 * of it named with its digest; no Strip the record names is grown into it.
 */

/* B10, whatever the warrant: a warrant lets a call change a file the system
 * keeps, and never moves that file's blocks. Asked with the flags the file's
 * record carries and the runs the Plot names. */
static error_t PlotGatherKept(TagFSVolume* vol, const Plot *p, uint32_t flags)
{
    if (!InkFileKept(flags) && !InkTouchesBoot(vol, p->runs, p->run_count))
        return OK;
    kprintf("[TagFS] file %llu is one the system keeps — its record puts it in a role, or "
            "holds the kernel the Deed points the loader at — and its blocks are not "
            "moved\n", p->fid);
    return ERR_PERMISSION_DENIED;
}

/* The stretch of the file's own runs from run `from` on — the first run not
 * SHARED there and the runs in a row after it none of which is: [*first,
 * *last], and whether it lies in one run of ground. False past the last. */
static bool PlotStretchFrom(const Plot *p, uint32_t from, uint32_t *first, uint32_t *last, bool *one)
{
    uint32_t i = from;
    while (i < p->run_count && p->runs[i].kind == STRIP_SHARED)
        i++;
    if (i == p->run_count)
        return false;
    *first = *last = i;
    *one   = p->runs[i].kind == STRIP_DATA;
    while (*last + 1u < p->run_count && p->runs[*last + 1u].kind != STRIP_SHARED) {
        const Strip *r = &p->runs[++*last];
        *one = *one && r->kind == STRIP_DATA && r->start == p->runs[*last - 1u].start + p->runs[*last - 1u].count;
    }
    return true;
}

bool PlotStretchScattered(const Plot *p, uint32_t *first, uint32_t *last)
{
    bool one = true;
    for (uint32_t from = 0; PlotStretchFrom(p, from, first, last, &one); from = *last + 1u)
        if (!one)
            return true;
    return false;
}

/* The stretch [*first, *last] of runs the loaded record named a defrag lays
 * in one run — the first scattered, or the first of all — no more blocks than
 * one Strip counts, none of them SHARED. */
static bool PlotChooseWindow(const Plot *p, uint32_t *first, uint32_t *last, uint64_t *blocks)
{
    uint64_t all = 0, end = 0;
    bool     one = true;
    if (!PlotStretchScattered(p, first, last) && !PlotStretchFrom(p, 0, first, last, &one))
        return false;
    for (uint32_t i = 0; i <= *last; i++)
        end += p->runs[i].count;
    for (uint32_t i = *first; i <= *last; i++)
        all += p->runs[i].count;
    *blocks = all;
    return end <= p->blocks_before && all <= UINT32_MAX;
}

/* The runs with [first, last] replaced by the run [start, start + blocks) —
 * every other Strip as it was and the run one of its own. */
static error_t PlotSplice(Plot *p, uint32_t first, uint32_t last, uint64_t start, uint32_t blocks)
{
    uint64_t room = (uint64_t)p->run_count + 1;
    Strip   *runs = room <= UINT32_MAX ? kmalloc(sizeof(Strip) * room) : NULL;
    if (!runs)
        return ERR_NO_MEMORY;

    uint32_t n = 0;
    for (uint32_t i = 0; i < first; i++)
        runs[n++] = p->runs[i];
    runs[n++] = (Strip){ .start = start, .count = blocks, .kind = STRIP_DATA };
    for (uint32_t i = last + 1; i < p->run_count; i++)
        runs[n++] = p->runs[i];

    kfree(p->runs);
    p->runs      = runs;
    p->run_count = n;
    p->run_room  = (uint32_t)room;
    return OK;
}

/* One free run of exactly `blocks` near `hint` for the gather: from the
 * derived map past the reserve it keeps, whole (AllotmentTakeBookRun), what
 * the run may add to the next Tally in the reserve before it is asked
 * (PlotStakesCount) — counted as a Stake once staked, none if refused. */
static error_t PlotGatherTake(TagFSVolume* vol, Plot *p, uint64_t blocks, uint64_t hint, uint64_t *start)
{
    PlotStakesCount(vol, p, blocks);
    error_t rc = AllotmentTakeBookRun(vol, p->fid, (uint32_t)blocks, hint, EndpaperDataKeeps(vol, true), start);
    if (rc != OK)
        PlotStakesCount(vol, p, 0);
    return rc;
}

error_t PlotGatherBegin(TagFSVolume* vol, Plot *p, uint64_t hint, PlotGathering *g)
{
    memset(g, 0, sizeof(*g));
    error_t rc = PlotWindowWhole(p);
    if (rc == OK)
        rc = PlotGatherKept(vol, p, p->meta.flags);
    if (rc != OK)
        return rc;

    uint32_t first = 0, last = 0;
    uint64_t blocks = 0;
    if (!PlotChooseWindow(p, &first, &last, &blocks)) {
        kprintf("[TagFS] file %llu: no run of its %u extent(s) can be gathered\n", p->fid,
                p->run_count);
        return ERR_TAGFS_RECORD_FULL;
    }

    uint64_t start = 0;
    rc = PlotGatherTake(vol, p, blocks, hint, &start);
    /* A take that waits — room on its way, a Tally that gives some back
     * (allotment.c, AllotmentBookShort) — is no refusal, and says none. */
    if (rc != OK && rc != ERR_WOULD_BLOCK)
        kprintf("[TagFS] file %llu: gathering extents %u to %u of it needs one free run of %llu "
                "block(s), and the volume has none that long\n", p->fid, first, last,
                (unsigned long long)blocks);
    if (rc != OK)
        return rc;
    rc = PlotStake(p, start, (uint32_t)blocks);
    if (rc != OK)
        (void)PlotGiveBack(vol, p, start, (uint32_t)blocks);
    PlotStakesCount(vol, p, 0);
    if (rc != OK)
        return rc;
    /* The record the gather leaves is held its room on the ribbon before a
     * block is copied (P21); the run goes back with the Plot. */
    if ((rc = PlotWindowRoomAsked(vol, p)) != OK)
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
static error_t PlotGatherRead(TagFSVolume* vol, const Plot *p, uint64_t from, uint32_t n, uint8_t *buf, uint64_t to)
{
    error_t rc = TagFSReadRun(vol, from, n, buf);
    for (uint32_t j = 0; j < n && rc == OK; j++)
        if (!IntegrityVerify(vol, from + j, buf + (size_t)j * TAGFS_BLOCK_SIZE))
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
static error_t PlotGatherWindow(TagFSVolume* vol, Plot *p, PlotGathering *g)
{
    uint32_t most = TagFSRunBlocks(vol);
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
            rc = PlotGatherRead(vol, p, from, n, buf + (size_t)filled * TAGFS_BLOCK_SIZE, to + filled);
        if (rc != OK)
            break;
        filled    += n;
        g->within += n;
        if (g->within == e->count) {
            g->extent++;
            g->within = 0;
        }
    }
    /* The file's last block goes with the Slips over it laid on it (tail_slip.h): the run's record lets them go with the block they lay on. */
    uint64_t k = p->window_at;
    for (uint32_t i = 0; i < g->first; i++)
        k += p->runs[i].count;
    for (uint32_t j = 0; rc == OK && j < filled; j++)
        (void)TailSlipOver(p->rec, k + g->copied + j, buf + (size_t)j * TAGFS_BLOCK_SIZE);
    if (rc == OK && filled > 0) {
        uint32_t landed   = 0;
        uint64_t departed = AwayDepartures(vol);
        rc = TagFSWriteRun(vol, to, filled, buf, &landed);
        if (rc == OK)
            rc = PlotWindowLanded(vol, p, departed, to, buf, filled);
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
 * is now, since a role put on while the copy went on keeps the file's blocks
 * where they lie as much as one it held at the load, and the
 * Plot made to name the run. */
static error_t PlotGatherLay(TagFSVolume* vol, Plot *p, PlotGathering *g)
{
    error_t rc = VolumeFlushFor(vol, VolumeFlushNeed(vol));
    if (rc != OK)
        return rc;

    FileRecord *now = FileMapRecordHold(vol->state.files, p->fid, NULL);
    if (now) {
        rc = PlotGatherKept(vol, p, now->flags);
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
        if (StripOnGround(p->runs[i].kind))    /* a hole owns no ground to let go */
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

error_t PlotGatherStep(TagFSVolume* vol, Plot *p, PlotGathering *g, bool *out_laid)
{
    error_t rc = OK;
    if (!g->laid) {
        rc = PlotGatherWindow(vol, p, g);
        if (rc == OK && g->copied == g->blocks)
            rc = PlotGatherLay(vol, p, g);
    }
    *out_laid = g->laid;
    return rc;
}

error_t PlotGather(TagFSVolume* vol, Plot *p, uint64_t hint)
{
    PlotGathering g;
    bool          laid = false;
    error_t       rc   = PlotGatherBegin(vol, p, hint, &g);
    while (rc == OK && !laid)
        rc = PlotGatherStep(vol, p, &g, &laid);
    return rc;
}
