#include "plot_internal.h"
#include "../file_map/file_map.h"
#include "../block_map/block_map.h"
#include "../settle/sewing.h"
#include "../box_hash/box_hash.h"
#include "../ink/docket.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../run/run.h"
#include "../file_map/tail_slip.h"
#include "../mount/away.h"

/*
 * PlotWindow — the Plot (plot.h): fresh ground, and a window instead of a copy
 * of the file.
 *
 * ‼ DATA IS NEVER WRITTEN INTO A BLOCK THE SETTLED BOOK NAMES. A block written
 * over gets a new block of fresh ground — a new Stake on the same place in the
 * file (PlotRestake) — and the record's splice names it; the old block rests
 * in Fallow once the Ink is laid. A write the medium refuses, or a stitch that
 * does not land, leaves the file on its old blocks, whole: a block written
 * over reads its old 4096 bytes or its new ones, never a mix. A block written
 * in part is read first — from the OLD block, held to its digest — and the
 * whole block laid on the new one.
 *
 * ‼ THE PLOT HOLDS THE RECORD, NOT A COPY OF ITS RUNS (§2.9, H-5). A file of
 * a million runs copied whole for every 4 KiB written is 24 MB a write. So the
 * Plot holds a reference on the record it loaded, finds a block of it where
 * the record holds it (FileRecordFind), and copies into `runs` only the window
 * of whole Strips its change works on — the Strips the write covers, or the
 * file's tail where it grows or is cut. The Ink splices the window back into
 * the record (PlotConveyance): the record copies the Parcels around it, and
 * shares every other.
 *
 * Where fresh ground lies (Allotment, block_map.h): the version that fills a
 * block goes on the block that continues the file's run before it, when that
 * one is free (rule 1), and the blocks one call writes over are taken as one
 * run (rule 2) — a plan of fresh ground for them, `fresh`, taken before a byte
 * moves and named, the part that landed, when the write ends; a block that
 * will not be full after the write is put aside, so the continuation stays
 * free for the version that fills it. Data never takes the book's reserve
 * (EndpaperDataKeeps), and the room its record takes on the ribbon is asked
 * before a byte moves (P21). A growth across a gap is a hole, written nowhere.
 */

#define PLOT_BLOCK TAGFS_BLOCK_SIZE

static uint64_t PlotWindowBlocksFor(uint64_t bytes)
{
    return (bytes + PLOT_BLOCK - 1) / PLOT_BLOCK;
}

/* The blocks the window's runs hold now. */
static uint64_t PlotWindowHeld(const Plot *p)
{
    return p->blocks - (p->rec->blocks - p->window_was);
}

void PlotWindowLoad(Plot *p, FileRecord *rec)
{
    p->rec           = rec;
    p->blocks        = rec->blocks;
    p->blocks_before = rec->blocks;
    p->window_at     = rec->blocks;
    p->window_was    = 0;
    p->windowed      = false;
}

/* The record, the plan and the digests let go — the two arrays whatever the
 * Plot still holds, a record or none — and the ribbon's room held for the
 * change, while the mounting it was held of stands. */
void PlotWindowLetGo(Plot *p)
{
    PlotSlipLetGo(p);
    FileRecordRelease(p->rec);
    kfree(p->fresh);
    kfree(p->digests);
    if (p->ribbon_held && p->mount_epoch == tagfs_mount_epoch())
        InkBookHeldLetGo(p->ribbon_held);
    p->rec         = NULL;
    p->fresh       = NULL;
    p->digests     = NULL;
    p->ribbon_held = 0;
}

#ifdef CONFIG_VOLUME_TESTS
static volatile uint64_t s_measure_fid;
static volatile uint64_t s_measure_most;

void PlotWindowMeasure(uint64_t fid)
{
    __atomic_store_n(&s_measure_most, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_measure_fid, fid, __ATOMIC_RELEASE);
}

uint64_t PlotWindowMeasured(void)
{
    return __atomic_load_n(&s_measure_most, __ATOMIC_ACQUIRE);
}

/* The window's Strips, every one a copy of the record's, noted for the file
 * measured. */
static void PlotWindowMeasureNote(const Plot *p)
{
    uint64_t most = __atomic_load_n(&s_measure_most, __ATOMIC_RELAXED);
    if (p->fid != __atomic_load_n(&s_measure_fid, __ATOMIC_ACQUIRE))
        return;
    while (p->run_count > most &&
           !__atomic_compare_exchange_n(&s_measure_most, &most, p->run_count, false, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED))
        ;
}
#endif

/* `count` Strips of the record from the walk `w` put after the window's runs
 * (`after`) or before them: OK or ERR_NO_MEMORY. */
static error_t PlotWindowTake(Plot *p, StripWalk *w, uint64_t blocks, bool after)
{
    Strip   *got = NULL;
    uint32_t n = 0, room = 0;
    uint64_t taken = 0;
    Strip    s;
    while (taken < blocks && StripWalkNext(w, &s)) {
        if (!PlotRoom((void **)&got, &room, n, (uint64_t)n + 1u, sizeof(Strip))) {
            kfree(got);
            return ERR_NO_MEMORY;
        }
        got[n++] = s;
        taken += s.count;
    }
    if (!PlotRoom((void **)&p->runs, &p->run_room, p->run_count, (uint64_t)p->run_count + n, sizeof(Strip))) {
        kfree(got);
        return ERR_NO_MEMORY;
    }
    if (after) {
        memcpy(p->runs + p->run_count, got, sizeof(Strip) * n);
    } else {
        memmove(p->runs + n, p->runs, sizeof(Strip) * p->run_count);
        memcpy(p->runs, got, sizeof(Strip) * n);
        p->window_at -= taken;
    }
    p->run_count  += n;
    p->window_was += taken;
    kfree(got);
#ifdef CONFIG_VOLUME_TESTS
    PlotWindowMeasureNote(p);
#endif
    return OK;
}

/*
 * The window made to cover the file's blocks [lo, hi) of the record — whole
 * Strips, `hi` past the record's end the record's end: opened where it was not,
 * and grown on either side by the record's own Strips, which the window's
 * changes never touched. An empty window stands at the record's end.
 */
error_t PlotWindowCover(Plot *p, uint64_t lo, uint64_t hi)
{
    const FileRecord *rec = p->rec;
    StripWalk         w;
    uint64_t          begins = 0;
    if (!rec)
        return ERR_FILE_NOT_FOUND;
    hi = hi < rec->blocks ? hi : rec->blocks;
    lo = lo < hi ? lo : hi;
    if (!p->windowed) {
        p->windowed  = true;
        p->window_at = rec->blocks;
        if (lo >= hi || !StripWalkAt(rec, lo, &w, &begins))
            return OK;
        p->window_at = begins;
        return PlotWindowTake(p, &w, hi - begins, true);
    }
    uint64_t end = p->window_at + p->window_was;
    if (lo < p->window_at && StripWalkAt(rec, lo, &w, &begins)) {
        error_t rc = PlotWindowTake(p, &w, p->window_at - begins, false);
        if (rc != OK)
            return rc;
    }
    if (hi > end && StripWalkAt(rec, end, &w, &begins))
        return PlotWindowTake(p, &w, hi - end, true);
    return OK;
}

error_t PlotWindowWhole(Plot *p)
{
    return PlotWindowCover(p, 0, p->rec ? p->rec->blocks : 0);
}

/* Where block k of the file lies as the Plot leaves it — not the plan of
 * fresh ground for blocks written over, which are not the file's until the
 * write ends: in the window's runs, or in the record before or after it. */
static bool PlotWindowSpot(const Plot *p, uint64_t k, FileSpot *out)
{
    uint64_t held = PlotWindowHeld(p);
    if (k >= p->window_at && k < p->window_at + held) {
        uint64_t base = p->window_at;
        for (uint32_t i = 0; i < p->run_count; i++) {
            if (k < base + p->runs[i].count) {
                uint64_t off = k - base;
                out->kind  = p->runs[i].kind;
                out->left  = (uint32_t)(p->runs[i].count - off - 1u);
                out->block = StripOnGround(p->runs[i].kind) ? p->runs[i].start + off : 0;
                return true;
            }
            base += p->runs[i].count;
        }
        return false;
    }
    if (k >= p->window_at + held)
        k = k - held + p->window_was;
    return FileRecordFind(p->rec, k, out);
}

/* The plan's block for file block k: true and *block, *left the plan's blocks
 * after it on the same run. */
static bool PlotFreshSpot(const Plot *p, uint64_t k, uint64_t *block, uint32_t *left)
{
    if (k < p->fresh_at || k >= p->fresh_at + p->fresh_blocks)
        return false;
    return PlotFind(p->fresh, p->fresh_count, k - p->fresh_at, block, left);
}

// ----------------------------------------------------------------------------
// Fresh ground
// ----------------------------------------------------------------------------

/* The block that continues the file's run before block k: the one after the
 * block k - 1 lies on — the plan's, where it has one — and 0 when k - 1 is a
 * hole or there is none. */
static uint64_t PlotContinues(const Plot *p, uint64_t k)
{
    uint64_t block = 0;
    uint32_t left  = 0;
    FileSpot spot;
    if (k == 0)
        return 0;
    if (PlotFreshSpot(p, k - 1u, &block, &left))
        return block + 1u;
    if (!PlotWindowSpot(p, k - 1u, &spot) || !StripOnGround(spot.kind))
        return 0;
    return spot.block + 1u;
}

/* Whether the file's blocks [k, k + n) take in the block its Slips lie over:
 * the one promised to it (allotment.c), theirs to have. */
static bool PlotLaysPromised(const Plot *p, uint64_t k, uint64_t n)
{
    uint64_t last = p->rec && p->rec->size ? (p->rec->size - 1u) / PLOT_BLOCK : 0;
    return TailSlipOn(p->rec, last) && k <= last && last - k < n;
}

/* The blocks a take of `want` can be handed now, past what the book's reserve
 * keeps: what it may add to the next Tally is counted for every measure
 * before it is asked (PlotStakesCount) — for these, not for `want`, or a write
 * at a full volume's wall keeps every other measure, the ribbon's next Quire
 * among them, as far past the ground left as a take a hundred times what the
 * wall gives it. The take itself asks `want`: one short of it is a piece, the
 * write's wait for the rest no take had whole (allotment.c, AllotmentBookHad).
 * Counted by the measure the take is judged by: the kept one, or — when it
 * leaves less than `want`, and the take refused past it is asked again of a
 * fresh one (AllotmentTakeBook) — the lesser of the two; counted by the kept
 * one alone, a take the fresh one handed was in no measure until its Stake,
 * and a full volume's reserve, counted then, stood past the free. Handed more
 * than counted only when the free grew since — ground come back — and the
 * count is made exact the moment the take is had. */
static uint64_t PlotMayPast(uint64_t free, uint64_t keep, bool own)
{
    return (free > keep ? free - keep : 0) + (own ? 1u : 0u);
}

static uint32_t PlotTakeMost(uint32_t want, bool own)
{
    uint64_t keep = EndpaperDataKeeps(false), free = AllotmentFreeUnpromised();
    uint64_t may  = PlotMayPast(free, keep, own);
    if (may < want) {
        uint64_t fresh = EndpaperDataKeeps(true);
        if (fresh < keep)
            may = PlotMayPast(free, fresh, own);
    }
    return may >= want ? want : (uint32_t)may;
}

/*
 * `n` blocks of fresh ground for the file's blocks from k on, near where its
 * run goes on — each run taken a Stake, and handed to `put` with its first
 * file block. The book's reserve is the measure kept with the data handed
 * since (endpaper.c), what the take may add to the next Tally in it before
 * it is asked (PlotTakeMost); a take short of room past it is asked again of
 * a fresh one before it is refused (allotment.c). The block promised to the
 * file's Slips is the first run's to have, once.
 */
static error_t PlotFreshTake(Plot *p, uint64_t k, uint64_t n, bool aside,
                             error_t (*put)(Plot *, uint64_t, uint64_t, uint32_t))
{
    bool own = PlotLaysPromised(p, k, n);
    while (n > 0) {
        uint64_t start = 0;
        uint32_t got   = 0;
        uint32_t want  = n > BLOCK_MAP_TAKE_MOST ? BLOCK_MAP_TAKE_MOST : (uint32_t)n;
        PlotStakesCount(p, PlotTakeMost(want, own));
        error_t rc = AllotmentTakeBook(p->fid, want, p->blocks, PlotContinues(p, k), aside, EndpaperDataKeeps(false),
                                       own, &start, &got);
        if (rc == OK && (rc = PlotStake(p, start, got)) != OK)
            (void)PlotGiveBack(p, start, got);
        PlotStakesCount(p, 0);
        if (rc != OK)
            return rc;
        own = false;
        if ((rc = put(p, k, start, got)) != OK)
            return rc;
        k += got;
        n -= got;
    }
    return OK;
}

/* A run of the plan put after the ones before it. */
static error_t PlotFreshPut(Plot *p, uint64_t k, uint64_t start, uint32_t count)
{
    if (p->fresh_blocks == 0)
        p->fresh_at = k;
    if (!PlotRoom((void **)&p->fresh, &p->fresh_room, p->fresh_count, (uint64_t)p->fresh_count + 1u,
                  sizeof(Strip)))
        return ERR_NO_MEMORY;
    PlotRunsPut(p->fresh, &p->fresh_count, start, count);
    p->fresh_blocks += count;
    return OK;
}

/* A run the file grows by put after its last. */
static error_t PlotGrowPut(Plot *p, uint64_t k, uint64_t start, uint32_t count)
{
    (void)k;
    return PlotAppendRun(p, start, count);
}

/* Fresh ground for the file's blocks [k, k + n): one run where it can be had
 * (Allotment 2), the last block apart and aside when the write leaves it
 * short of full — its version does not fill it — so the block that continues
 * the run stays free for the version that will (Allotment 1). */
static error_t PlotFreshFor(Plot *p, uint64_t k, uint64_t n, bool short_last,
                            error_t (*put)(Plot *, uint64_t, uint64_t, uint32_t))
{
    error_t rc = OK;
    if (n > (short_last ? 1u : 0u))
        rc = PlotFreshTake(p, k, n - (short_last ? 1u : 0u), false, put);
    if (rc == OK && short_last)
        rc = PlotFreshTake(p, k + n - 1u, 1, true, put);
    return rc;
}

// ----------------------------------------------------------------------------
// A write
// ----------------------------------------------------------------------------

/* The most bytes the record of the window's change takes on the ribbon: a
 * Shaped of one splice a run the window ends with, a digest a block of fresh
 * ground, and the Slip the write rides — what the Ink claims (docket.h,
 * DocketClaimOfShaped). */
static uint64_t PlotWindowRibbonMost(const Plot *p)
{
    uint64_t fresh = p->fresh_blocks;
    for (uint32_t i = 0; i < p->stake_count; i++)
        fresh += p->stakes[i].count;
    return DocketClaimOfShaped((uint64_t)p->run_count + p->fresh_count + 2u, 1, fresh, PlotSlipMost(p));
}

/*
 * ‼ THE ROOM A CHANGE'S RECORD TAKES IS HELD FROM BEFORE ITS DATA TO ITS INK
 * (P21, §3.6). Asked and let go before the data, the room could be taken by
 * another change — or a TAKE could carry the claims it was judged against —
 * while the data moves, and the Ink's own claim be refused after its bytes had
 * landed. So the most the record can take is held on the ribbon now
 * (RibbonReserve), grown where a later window of the same change asks more,
 * and the Ink takes the hold for its claim (RibbonClaimHeld) — or the Plot lets
 * it go (PlotWindowLetGo).
 */
error_t PlotWindowRoomAsked(Plot *p)
{
    TagFSState *fs   = tagfs_get_state();
    uint64_t    most = PlotWindowRibbonMost(p);
    if (most <= p->ribbon_held)
        return OK;
    TagFSLockTake(&fs->lock);
    error_t rc = RibbonReserve(most - p->ribbon_held);
    if (rc == OK)
        p->ribbon_held = most;
    TagFSLockDrop(&fs->lock);
    rc = RibbonClaimSay(rc);
    return rc;
}

error_t PlotWindowBegin(Plot *p, uint64_t at, uint64_t bytes)
{
    bool    slipped = false;
    error_t chose   = PlotSlipBegin(p, at, bytes, &slipped);
    if (chose != OK || slipped)
        return chose;
    uint64_t had   = p->blocks_before;
    uint64_t k0    = at / PLOT_BLOCK;
    uint64_t k1    = PlotWindowBlocksFor(at + bytes);
    uint64_t over  = k1 < had ? k1 : had;
    uint64_t size  = at + bytes > p->size ? at + bytes : p->size;
    bool     short_last = size % PLOT_BLOCK != 0;
    error_t  rc    = PlotWindowCover(p, k0 < had ? k0 : had, k1 < had ? k1 : had);
    /* A full last block with Slips over it, which a write past it leaves
     * untouched, stops being the last: it goes into a block with them first
     * (§2.8 — they lie on the last block alone). */
    if (rc == OK && k0 >= had && had && TailSlipOn(p->rec, had - 1u) &&
        (rc = PlotWindowCover(p, had - 1u, had)) == OK)
        rc = PlotWindowTail(p, had - 1u, PLOT_BLOCK);
    if (rc == OK && k0 < over)
        rc = PlotFreshFor(p, k0, over - k0, short_last && over == PlotWindowBlocksFor(size), PlotFreshPut);
    if (rc == OK && k1 > p->blocks)
        rc = PlotFreshFor(p, p->blocks, k1 - p->blocks, short_last, PlotGrowPut);
    return rc == OK ? PlotWindowRoomAsked(p) : rc;
}

/* The block the rest of block k is read from, and what is done with it: the
 * old block it lay on — a hole reads as zeros, never read — or none for a
 * block the file did not have. */
static PlotFill PlotWindowFill(const Plot *p, uint64_t k, uint32_t offset, uint32_t bytes, uint64_t *from)
{
    FileSpot spot;
    PlotFill fill = PlotFillFor(p, k, offset, bytes);
    if (fill != PLOT_FILL_READ)
        return fill;
    if (!PlotWindowSpot(p, k, &spot) || !StripOnGround(spot.kind))
        return PLOT_FILL_FRESH;
    *from = spot.block;
    return PLOT_FILL_READ;
}

bool PlotWindowStretchAt(const Plot *p, uint64_t pos, uint64_t left, uint32_t most, PlotStretch *out)
{
    uint64_t k     = pos / PLOT_BLOCK;
    uint64_t block = 0;
    uint32_t after = 0;
    FileSpot spot;
    if (left == 0 || most == 0)
        return false;
    if (!PlotFreshSpot(p, k, &block, &after)) {
        if (!PlotWindowSpot(p, k, &spot) || spot.kind != STRIP_DATA)
            return false;
        block = spot.block;
        after = spot.left;
    }
    uint32_t head  = (uint32_t)(pos % PLOT_BLOCK);
    uint64_t count = PlotWindowBlocksFor(head + left);
    count = count < (uint64_t)after + 1u ? count : (uint64_t)after + 1u;
    count = count < most ? count : most;
    uint64_t carried = count * PLOT_BLOCK - head;
    out->block      = block;
    out->count      = (uint32_t)count;
    out->k          = k;
    out->head       = head;
    out->bytes      = carried < left ? carried : left;
    out->from_first = block;
    out->from_last  = block + count - 1u;
    uint64_t in_first = PLOT_BLOCK - head < out->bytes ? PLOT_BLOCK - head : out->bytes;
    out->first = PlotWindowFill(p, k, head, (uint32_t)in_first, &out->from_first);
    out->last  = out->first;
    if (count > 1)
        out->last = PlotWindowFill(p, k + count - 1u, 0,
                                   (uint32_t)(head + out->bytes - (count - 1u) * PLOT_BLOCK), &out->from_last);
    else
        out->from_last = out->from_first;
    return true;
}

error_t PlotWindowLanded(Plot *p, uint64_t departed, uint64_t block, const uint8_t *data, uint32_t count)
{
    BoxHashContext ctx;
    if (count == 0)
        return OK;
    if (!PlotRoom((void **)&p->digests, &p->digest_room, p->digest_count, (uint64_t)p->digest_count + count,
                  sizeof(PlotDigest)))
        return ERR_NO_MEMORY;
    if (p->digest_count == 0)
        p->departed = departed;
    BoxHashInit(&ctx, tagfs_get_state()->uuid, 16);
    for (uint32_t i = 0; i < count; i++)
        p->digests[p->digest_count++] =
            (PlotDigest){ block + i, BoxHashIntegrity(data + (size_t)i * PLOT_BLOCK, PLOT_BLOCK, &ctx) };
    return OK;
}

// ----------------------------------------------------------------------------
// The plan named: what landed restaked, the rest given back
// ----------------------------------------------------------------------------

/* Of the Strip `s`, `len` blocks from its `off`-th. */
static Strip PlotPiece(const Strip *s, uint64_t off, uint64_t len)
{
    Strip piece = *s;
    piece.count = (uint32_t)len;
    if (StripOnGround(s->kind))
        piece.start = s->start + off;
    return piece;
}

/* The plan's Strips for file blocks [k, k + n) put into `out` from *m. */
static void PlotFreshPieces(const Plot *p, uint64_t k, uint64_t n, Strip *out, uint32_t *m)
{
    uint64_t base = p->fresh_at;
    for (uint32_t i = 0; i < p->fresh_count; i++) {
        uint64_t lo = base, hi = base + p->fresh[i].count;
        base = hi;
        if (hi <= k || lo >= k + n)
            continue;
        uint64_t a = k > lo ? k : lo, b = k + n < hi ? k + n : hi;
        out[(*m)++] = PlotPiece(&p->fresh[i], a - lo, b - a);
    }
}

/*
 * PlotRestake — a new Stake on the same place in the file over blocks it had:
 * the plan's ground for file blocks [k, k + n) put in the window where the old
 * runs lay, cut where they were cut, and the old ground let go — to rest in
 * Fallow once the Ink is laid (a Stake of this call's own goes back now).
 */
static error_t PlotRestake(Plot *p, uint64_t k, uint64_t n)
{
    uint64_t a = k - p->window_at, b = a + n, pos = 0;
    uint32_t m = 0;
    bool     placed = false;
    Strip   *out = kmalloc(sizeof(Strip) * ((uint64_t)p->run_count + p->fresh_count + 2u));
    error_t  rc  = out ? OK : ERR_NO_MEMORY;
    for (uint32_t i = 0; rc == OK && i < p->run_count; i++) {
        const Strip *r  = &p->runs[i];
        uint64_t     lo = pos, hi = pos + r->count;
        pos = hi;
        if (hi <= a || lo >= b) {
            if (lo >= b && !placed) {
                PlotFreshPieces(p, k, n, out, &m);
                placed = true;
            }
            out[m++] = *r;
            continue;
        }
        if (lo < a)
            out[m++] = PlotPiece(r, 0, a - lo);
        if (!placed) {
            PlotFreshPieces(p, k, n, out, &m);
            placed = true;
        }
        uint64_t cut_lo = a > lo ? a : lo, cut_hi = b < hi ? b : hi;
        if (StripOnGround(r->kind))
            rc = PlotLetGo(p, r->start + (cut_lo - lo), (uint32_t)(cut_hi - cut_lo));
        if (hi > b)
            out[m++] = PlotPiece(r, b - lo, hi - b);
    }
    if (rc == OK && !placed)
        PlotFreshPieces(p, k, n, out, &m);
    if (rc != OK) {
        kfree(out);
        return rc;
    }
    kfree(p->runs);
    p->runs      = out;
    p->run_count = m;
    p->run_room  = (uint32_t)((uint64_t)p->run_count + p->fresh_count + 2u);
    return OK;
}

/* The plan let go of: every run of it the file did not take goes back. */
static void PlotFreshLetGo(Plot *p, uint64_t from)
{
    uint64_t base = p->fresh_at;
    for (uint32_t i = 0; i < p->fresh_count; i++) {
        uint64_t lo = base, hi = base + p->fresh[i].count;
        base = hi;
        if (hi <= from)
            continue;
        uint64_t a = from > lo ? from : lo;
        (void)PlotLetGo(p, p->fresh[i].start + (a - lo), (uint32_t)(hi - a));
    }
    p->fresh_count  = 0;
    p->fresh_blocks = 0;
}

error_t PlotWindowEnd(Plot *p, uint64_t at, uint64_t done)
{
    uint64_t kdone = PlotWindowBlocksFor(at + done);
    uint64_t named = 0;
    error_t  rc    = OK;
    if (p->fresh_blocks && kdone > p->fresh_at) {
        named = kdone - p->fresh_at < p->fresh_blocks ? kdone - p->fresh_at : p->fresh_blocks;
        rc    = PlotRestake(p, p->fresh_at, named);
    }
    if (p->fresh_blocks)
        PlotFreshLetGo(p, p->fresh_at + named);
    return rc;
}

// ----------------------------------------------------------------------------
// A cut and a growth
// ----------------------------------------------------------------------------

error_t PlotWindowTrim(Plot *p, uint64_t keep_blocks)
{
    if (!p->rec)
        return ERR_FILE_NOT_FOUND;
    return PlotWindowCover(p, keep_blocks ? keep_blocks - 1u : 0, p->rec->blocks);
}

/* A hole of `count` blocks after the file's last run — the hole it ends with
 * grown, where it ends with one: a hole owns no ground, and naming more of it
 * gives up none. */
error_t PlotAppendHole(Plot *p, uint64_t count)
{
    while (count > 0) {
        uint32_t n = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
        Strip   *last = p->run_count ? &p->runs[p->run_count - 1u] : NULL;
        if (last && last->kind == STRIP_HOLE && (uint64_t)last->count + n <= UINT32_MAX) {
            last->count += n;
        } else {
            if (!PlotRoom((void **)&p->runs, &p->run_room, p->run_count, (uint64_t)p->run_count + 1u,
                          sizeof(Strip)))
                return ERR_NO_MEMORY;
            p->runs[p->run_count++] = (Strip){ .start = 0, .count = n, .kind = STRIP_HOLE };
        }
        p->blocks += n;
        count     -= n;
    }
    return OK;
}

/* The file's last block laid again on fresh ground — for a growth that makes
 * it whole, or a tail its Slips leave (Engross): its base, the Slips over it,
 * zeros past the file's `tail` bytes of it — what lay past them there, a cut
 * tail or a previous owner's bytes, is not the file's. */
error_t PlotWindowTail(Plot *p, uint64_t k, uint32_t tail)
{
    FileSpot spot;
    uint8_t *buf = NULL;
    uint64_t block = 0, departed = 0;
    uint32_t landed = 0, left = 0;
    error_t  rc = PlotFreshFor(p, k, 1, false, PlotFreshPut);
    if (rc == OK && (!PlotWindowSpot(p, k, &spot) || !PlotFreshSpot(p, k, &block, &left)))
        rc = ERR_TAGFS_EXTENT_ERROR;
    if (rc == OK)
        rc = PlotWindowRoomAsked(p);
    if (rc == OK && !(buf = kmalloc(PLOT_BLOCK)))
        rc = ERR_NO_MEMORY;
    if (rc == OK && !StripOnGround(spot.kind))
        memset(buf, 0, PLOT_BLOCK);
    else if (rc == OK && (rc = tagfs_read_block(spot.block, buf)) != OK)
        kprintf("[TagFS] file %llu: its last block %llu answered %u to its read, so it is not laid again with "
                "its %llu bytes\n", p->fid, (unsigned long long)spot.block, (unsigned)rc, (unsigned long long)p->size);
    if (rc == OK) {
        (void)TailSlipOver(p->rec, k, buf);
        memset(buf + tail, 0, PLOT_BLOCK - tail);
        departed = AwayDepartures();
        rc       = TagFSWriteRun(block, 1, buf, &landed);
    }
    if (rc == OK)
        rc = PlotWindowLanded(p, departed, block, buf, 1);
    if (rc == OK)
        rc = PlotWindowEnd(p, (uint64_t)k * PLOT_BLOCK, PLOT_BLOCK);
    else if (p->fresh_blocks)
        PlotFreshLetGo(p, p->fresh_at);
    kfree(buf);
    return rc;
}

/* Whether a growth of the file to `size` lays its last block again: a DATA
 * block short of full holds, past the size, bytes that are not the file's; and
 * a block with Slips over it that stops being the last — they lie on the last
 * block alone — goes into a block with them (§2.8). A hole short of full
 * grown inside itself reads zeros past its Slips: nothing is laid. */
static bool PlotWindowTailOwed(const Plot *p, uint64_t size)
{
    uint64_t had = PlotWindowBlocksFor(p->size);
    FileSpot spot;
    if (had == 0 || !PlotWindowSpot(p, had - 1u, &spot))
        return false;
    if (TailSlipOn(p->rec, had - 1u) && PlotWindowBlocksFor(size) > had)
        return true;
    return p->size % PLOT_BLOCK && StripOnGround(spot.kind);
}

error_t PlotWindowGrow(Plot *p, uint64_t size)
{
    uint64_t had  = PlotWindowBlocksFor(p->size);
    uint32_t tail = (uint32_t)(p->size % PLOT_BLOCK);
    if (!p->rec)
        return ERR_FILE_NOT_FOUND;
    error_t  rc   = PlotWindowCover(p, had ? had - 1u : 0, p->rec->blocks);
    if (rc == OK)
        rc = PlotTrimTo(p, had);       /* ground past the size holds no byte of the file */
    if (rc == OK && PlotWindowTailOwed(p, size))
        rc = PlotWindowTail(p, had - 1u, tail ? tail : PLOT_BLOCK);
    if (rc == OK)
        rc = PlotAppendHole(p, PlotWindowBlocksFor(size) - p->blocks);
    if (rc == OK) {
        p->size      = size;
        p->data_need = p->digest_count ? VolumeFlushNeed() : p->data_need;
    }
    return rc;
}

error_t PlotWindowRoomFor(const Plot *p, uint64_t end)
{
    if (!p->rec)
        return ERR_FILE_NOT_FOUND;
    uint64_t need = end > p->size && PlotWindowTailOwed(p, end) ? 1u : 0u;
    if (need == 0)
        return OK;
    TagFSState *fs   = tagfs_get_state();
    uint64_t    keep = EndpaperDataKeeps(false);
    bool        own  = PlotLaysPromised(p, PlotWindowBlocksFor(p->size) - 1u, 1);
    TagFSLockTake(&fs->lock);
    uint64_t may = AllotmentBookMay(fs->free_blocks + FallowBlocks(), keep, own);
    TagFSLockDrop(&fs->lock);
    if (may >= need)
        return OK;
    kprintf("[TagFS] file %llu: growing it to %llu bytes lays its last block again on fresh ground, and the volume "
            "has no block for it past the %llu its book keeps and the %llu promised to the tails Slips lie over — "
            "refused\n", p->fid, (unsigned long long)end, (unsigned long long)keep,
            (unsigned long long)AllotmentPromised());
    return ERR_TAGFS_NO_SPACE;
}
