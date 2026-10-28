#include "ribbon.h"
#include "book_mount.h"
#include "book_unit.h"
#include "errata.h"
#include "../box_hash/box_hash.h"
#include "boardroom.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/*
 * RibbonVerify — the data of the newest stitch held to its digests (spec_v2
 * §4.5 п.6, §3.9).
 *
 * A stitch sewn without DATA_FLUSHED was laid without its cycle's first
 * flush: its data may not have reached the medium when the stitch did. So
 * the mount reads every block of fresh data its Borns and Shapeds put in and
 * holds each to the digest the record carries; a file whose block does not
 * match is rolled back by its splice — its records of that stitch passed
 * over, as the Undone of the session's first stitch will say (§2.8). And on a medium
 * already caught lying, the newest two stitches are held so, DATA_FLUSHED or
 * not: a first flush lies as a last one does (M-2). A block that did not land
 * with a torn stitch after it is the medium not keeping what it confirmed —
 * a flush that lied, or what was written spoilt by the next write: that
 * medium lied (MEDIUM_LIED, kept in memory; the writer lays it in its
 * stitches).
 *
 * ‼ THE ROLL-BACK IS A VERDICT THE SESSION'S FIRST STITCH LAYS DOWN (H-1,
 * §16.5 п.4). A second mount would replay the record a roll-back passed over
 * unless an Undone says so, and a mount writes nothing: the verdict is held in
 * memory (errata.h) — the stitch, why, the file — and the first stitch of the
 * session carries it, right after its StitchHead. Until that stitch settles a
 * mount again finds the same stitch newest and comes to the same verdict.
 *
 * ‼ CLASP.FRESH BOUNDS THE READ BEFORE A BLOCK IS READ (§2.7). The fresh
 * blocks a stitch's records name are counted first and held to its Clasp: a
 * stitch whose Clasp says otherwise is one pass 2 does not apply
 * (RibbonFreshHeld), and none of its data is read. Of one that agrees, a block
 * is read only while its record has a digest for it — a CODEC run names more
 * blocks than it stores (§8.3), and those past its digests are counted, not
 * read — and a block past the volume's last is one the medium does not give:
 * not read, not landed. The data is read as data, not as the book's: no seq
 * of it is folded into the next one.
 *
 * What the spec bounds the read by in time — one D (§3.3, M-13) — is the
 * writer's to keep: a stitch sewn without DATA_FLUSHED is one whose check its
 * toll found within D, by the runs it reads and the pace the medium's reads
 * kept (settle/sewing.c, StitchToll). So the check reads a run the way the
 * toll prices it — a window the seat takes in one command at a time, each
 * block of it held to its digest — and not a command a block, which would
 * cost a seek a block. The DATA_FLUSHED stitches a lying medium has the mount
 * check too are not so bounded (§16.4 п.1), and no field of the book carries
 * D for a mount to hold them to: every block they name is read.
 *
 * Read the way volume-read reads it (tools/volumeread/bookmount.py _Verify).
 */

/* What one record's runs are held to: its digests, how many it has, the
 * next one to hold, a window of blocks to read into and how many it holds,
 * and what was found. */
typedef struct {
    const uint8_t* digests;
    uint64_t       ndigests;
    uint64_t       k;
    uint64_t       checked;
    uint64_t       bad;
    uint8_t*       window;
    uint32_t       blocks;
    BoxHashContext ctx;
} RibbonHeld;

/* A block of fresh data held to digest `want`: *matches whether it reads and
 * matches. OK, or the medium's refusal. */
static error_t RibbonBlockHeld(RibbonHeld* h, uint64_t block, uint64_t want, bool* matches)
{
    *matches = false;
    if (block > g_book.data_end)
        return OK;
    int rc = tagfs_volume_read(block * TAGFS_BLOCK_SECTORS, TAGFS_BLOCK_SECTORS, h->window);
    if (rc == (int)ERR_BAD_SECTOR)
        return OK;
    if (rc != 0)
        return BookUnitRefused(rc);
    *matches = BoxHashIntegrity(h->window, TAGFS_BLOCK_SIZE, &h->ctx) == want;
    return OK;
}

/* `n` blocks of fresh data from `block`, held to the digests from the `k`-th
 * on: read as one window, each block of it held to its digest — or, a window
 * the medium would not give whole, a block a read, so the block that does not
 * read is the one found. h->bad the first that does not match. OK, or the
 * medium's refusal. */
static error_t RibbonWindowHeld(RibbonHeld* h, uint64_t block, uint32_t n, uint64_t k)
{
    int rc = block + n - 1u > g_book.data_end
                 ? (int)ERR_BAD_SECTOR
                 : tagfs_volume_read(block * TAGFS_BLOCK_SECTORS, n * TAGFS_BLOCK_SECTORS, h->window);
    for (uint32_t b = 0; b < n && h->bad == UINT64_MAX; b++) {
        bool    matches = false;
        error_t held    = OK;
        if (rc == 0)
            matches = BoxHashIntegrity(h->window + (uint64_t)b * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, &h->ctx) ==
                      TallyDigestAt(h->digests, k + b);
        else if (rc == (int)ERR_BAD_SECTOR)
            held = RibbonBlockHeld(h, block + b, TallyDigestAt(h->digests, k + b), &matches);
        else
            return BookUnitRefused(rc);
        if (held != OK)
            return held;
        if (!matches)
            h->bad = block + b;
    }
    return OK;
}

/* The DATA runs not SHARED of a record's runs, read in order against its
 * digests until one does not match or the digests end — every fresh block
 * counted into `checked`. OK, or the medium's refusal. */
static error_t RibbonRunsHeld(const uint8_t* runs, uint32_t bytes, uint64_t n, uint64_t cursor, uint64_t seq,
                              RibbonHeld* h)
{
    uint32_t at = 0;
    for (uint64_t i = 0; i < n; i++) {
        TallyRun r;
        RibbonRunGet(runs, bytes, &at, &cursor, seq, &r);
        uint64_t fresh = RibbonRunFresh(&r);
        uint64_t take  = h->k < h->ndigests ? h->ndigests - h->k : 0;
        h->checked = fresh > UINT64_MAX - h->checked ? UINT64_MAX : h->checked + fresh;
        if (take > fresh)
            take = fresh;
        for (uint64_t b = 0; b < take && h->bad == UINT64_MAX;) {
            uint32_t n  = take - b < h->blocks ? (uint32_t)(take - b) : h->blocks;
            error_t  rc = RibbonWindowHeld(h, r.start + b, n, h->k + b);
            if (rc != OK)
                return rc;
            g_book.said.checked += n;
            b += n;
        }
        h->k += take;
    }
    return OK;
}

/*
 * One Born or Shaped of stitch `u` held to its data: the file rolled back by
 * its splice when a block did not land — said, put into `undone` (sorted) for
 * the replay, and into the verdict the session's first stitch lays down
 * (errata.h).
 *
 * ‼ A RECORD ALREADY PASSED OVER IS NOT HELD AGAIN (§2.8: one Undone a stitch
 * undone). On a medium that lied the newest two stitches are held, and once
 * the session's first stitch settled with its Undone, that stitch is one of
 * the two: the stitch its Undone names is held again — and its file, passed
 * over already by the Undone gathered first (RibbonUndones), would be found
 * not landed a second time, laid down a second time, and worn a second time.
 * So is a second record of a file this check rolled back in the same stitch:
 * its records there are passed over whole.
 */
static error_t RibbonRecordLanded(RibbonWalk* w, const RibbonUnit* u, const RibbonRecord* r, RibbonSkipSet* undone,
                                  RibbonHeld* h)
{
    RibbonBorn     b;
    RibbonShaped   d;
    RibbonSplice   s;
    uint32_t       used = 0, at = 0;
    uint64_t       cursor = 0, digests = 0;
    error_t        rc  = OK;
    const uint8_t* body = u->stream + r->body;
    bool born   = r->kind == RIBBON_BORN && RibbonBornGet(body, r->len, u->seq, &b, &used) != TALLY_BODY_BROKEN;
    bool shaped = !born && r->kind == RIBBON_SHAPED &&
                  RibbonShapedGet(body, r->len, u->seq, &d, &used) != TALLY_BODY_BROKEN;
    uint64_t       fid = born ? b.fid : shaped ? d.fid : 0;
    if ((!born && !shaped) || RibbonSkipHas(undone, u->seq, fid))
        return OK;
    h->k = h->checked = 0;
    h->bad = UINT64_MAX;
    h->digests  = born ? b.digests : d.digests;
    h->ndigests = born ? b.ndigests : d.ndigests;
    if (born)
        rc = RibbonRunsHeld(b.runs, b.runs_bytes, b.nruns, 0, u->seq, h);
    for (uint64_t i = 0; shaped && rc == OK && i < d.nsplice; i++) {
        RibbonSpliceNext(d.splices, d.splices_bytes, &at, &cursor, u->seq, &digests, &s);
        rc = RibbonRunsHeld(s.runs, s.runs_bytes, s.ninsert, s.cursor, u->seq, h);
    }
    if (rc != OK || h->bad == UINT64_MAX)
        return rc;
    rc = RibbonSkipPut(undone, u->seq, fid);
    if (rc == OK)
        rc = ErrataAdd(u->seq, UNDONE_NOT_LANDED, fid);
    bool torn = w->torn != NULL;
    w->lied |= torn;
    RibbonSay(w, RIBBON_CODE_NOT_LANDED, "fid %llu's data in stitch %llu did not land (block %llu of %llu checked does "
              "not match its digest): its record is rolled back by its splice — ERR_TAGFS_NOT_LANDED%s",
              (unsigned long long)fid, (unsigned long long)u->seq, (unsigned long long)h->bad,
              (unsigned long long)h->checked, torn ? "; a torn stitch lies after it, so the medium did not keep "
                                                    "what it confirmed: a flush that lied, or what was written "
                                                    "spoilt by the next write (M-2) — MEDIUM_LIED" : "");
    return rc;
}

/* Whether stitch `u`'s data is held to its digests: sound, sewn without its
 * first flush or on a medium that lied, and its Clasp counting the fresh
 * blocks its records name. */
static bool RibbonChecked(const RibbonWalk* w, const RibbonUnit* u)
{
    uint64_t named = 0;
    if (!u->sound || ((u->head.flags & STITCH_DATA_FLUSHED) && !w->lied))
        return false;
    return RibbonFreshNamed(u, &named) && named == u->clasp.fresh;
}

/* On a medium shown to lie before a block is read, the price of the check is
 * said first (§16.4 п.1): the fresh blocks the newest stitches name — every
 * one of them is read — counted from their Clasps, which bound them. Said in
 * the words volume-read predicts (bookmount.py _Verify, lied_check). */
static void RibbonVerifyPriceSaid(const RibbonWalk* w, uint32_t n)
{
    uint64_t blocks = 0;
    for (uint32_t s = w->nstitch - n; s < w->nstitch; s++)
        if (RibbonChecked(w, w->stitch[s]))
            blocks += w->stitch[s]->clasp.fresh;
    kprintf("[TagFS] the medium lied: %llu fresh block(s) of the newest %u stitch(es) are read and held to "
            "their digests before the ribbon is replayed — every one, however long that takes (§16.4)\n",
            (unsigned long long)blocks, n);
}

error_t RibbonVerify(RibbonWalk* w, RibbonSkipSet* undone)
{
    uint32_t    n      = w->nstitch < (w->lied ? 2u : 1u) ? w->nstitch : (w->lied ? 2u : 1u);
    uint32_t    blocks = BoardroomSeatRun(tagfs_get_seat()) / TAGFS_BLOCK_SECTORS;
    uint64_t    t0     = rdtsc();
    RibbonHeld* h      = kmalloc(sizeof(RibbonHeld));
    error_t     rc     = OK;
    if (h) {
        h->blocks = blocks ? blocks : 1u;
        h->window = kmalloc((uint64_t)h->blocks * TAGFS_BLOCK_SIZE);
    }
    if (!h || !h->window) {
        kfree(h);
        return ERR_NO_MEMORY;
    }
    if (w->lied)
        RibbonVerifyPriceSaid(w, n);
    BoxHashInit(&h->ctx, tagfs_get_state()->uuid, 16);
    for (uint32_t s = w->nstitch - n; rc == OK && s < w->nstitch; s++) {
        const RibbonUnit* u       = w->stitch[s];
        bool              checked = RibbonChecked(w, u);
        for (uint32_t i = 0; rc == OK && checked && i < u->nrecords; i++)
            rc = RibbonRecordLanded(w, u, &u->records[i], undone, h);
    }
    kfree(h->window);
    kfree(h);
    g_book.said.check_us += cpu_tsc_to_us(rdtsc() - t0);
    return rc;
}
