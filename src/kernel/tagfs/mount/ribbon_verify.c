#include "ribbon.h"
#include "book_mount.h"
#include "book_unit.h"
#include "../box_hash/box_hash.h"

/*
 * RibbonVerify — the data of the newest stitch held to its digests (spec_v2
 * §4.5 п.6, §3.9).
 *
 * A stitch sewn without DATA_FLUSHED was laid without its cycle's first
 * flush: its data may not have reached the medium when the stitch did. So
 * the mount reads every block of fresh data its Borns and Shapeds put in and
 * holds each to the digest the record carries; a file whose block does not
 * match is rolled back by its splice — its records of that stitch passed
 * over, as an Undone of the next session will say (§2.8). And on a medium
 * already caught lying, the newest two stitches are held so, DATA_FLUSHED or
 * not: a first flush lies as a last one does (M-2). A block that did not land
 * with a torn stitch after it is the medium not keeping what it confirmed —
 * a flush that lied, or what was written spoilt by the next write: that
 * medium lied (MEDIUM_LIED, kept in memory; the writer lays it in its
 * stitches).
 *
 * ‼ THE ROLL-BACK IS A VERDICT, AND THIS KERNEL DOES NOT WRITE IT. A second
 * mount would replay the record a roll-back passed over unless an Undone in
 * the first stitch of the session says so (H-1), and the writer of Undone is
 * the next step's: the volume stands under glass for every such file, said.
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
 * What the spec bounds the read by in time — one D, two on a medium that lied
 * (§3.3) — is the writer's to keep: a stitch sewn without DATA_FLUSHED is one
 * whose check S0 found within D. The DATA_FLUSHED stitches a lying medium has
 * the mount check too are not so bounded, and no field of the book carries D
 * for a mount to hold them to: every block they name is read. In this tree
 * every stitch with fresh data is DATA_FLUSHED (§16.2 п.7), so a ribbon this
 * kernel's writer lays asks nothing of it but on a medium that lied.
 *
 * Read the way volume-read reads it (tools/volumeread/bookmount.py _Verify).
 */

/* What one record's runs are held to: its digests, how many it has, the
 * next one to hold, a block to read into, and what was found. */
typedef struct {
    const uint8_t* digests;
    uint64_t       ndigests;
    uint64_t       k;
    uint64_t       checked;
    uint64_t       bad;
    uint8_t*       block;
    BoxHashContext ctx;
} RibbonHeld;

/* A block of fresh data held to digest `want`: *matches whether it reads and
 * matches. OK, or the medium's refusal. */
static error_t RibbonBlockHeld(RibbonHeld* h, uint64_t block, uint64_t want, bool* matches)
{
    *matches = false;
    if (block > g_book.data_end)
        return OK;
    int rc = tagfs_volume_read(block * TAGFS_BLOCK_SECTORS, TAGFS_BLOCK_SECTORS, h->block);
    if (rc == (int)ERR_BAD_SECTOR)
        return OK;
    if (rc != 0)
        return BookUnitRefused(rc);
    *matches = BoxHashIntegrity(h->block, TAGFS_BLOCK_SIZE, &h->ctx) == want;
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
        for (uint64_t b = 0; b < take && h->bad == UINT64_MAX; b++) {
            bool    matches = false;
            error_t rc = RibbonBlockHeld(h, r.start + b, TallyDigestAt(h->digests, h->k + b), &matches);
            if (rc != OK)
                return rc;
            if (!matches)
                h->bad = r.start + b;
        }
        h->k += take;
    }
    return OK;
}

/* One Born or Shaped of stitch `u` held to its data: the file rolled back by
 * its splice when a block did not land — said, into `undone`, and the
 * volume under glass for the verdict. */
static error_t RibbonRecordLanded(RibbonWalk* w, const RibbonUnit* u, const RibbonRecord* r, RibbonSkipSet* undone,
                                  RibbonHeld* h)
{
    RibbonBorn     b;
    RibbonShaped   d;
    RibbonSplice   s;
    uint32_t       used = 0, at = 0;
    uint64_t       fid = 0, cursor = 0, digests = 0;
    error_t        rc  = OK;
    char           why[192];
    const uint8_t* body = u->stream + r->body;
    h->k = h->checked = 0;
    h->bad = UINT64_MAX;
    if (r->kind == RIBBON_BORN && RibbonBornGet(body, r->len, u->seq, &b, &used) != TALLY_BODY_BROKEN) {
        fid          = b.fid;
        h->digests   = b.digests;
        h->ndigests  = b.ndigests;
        rc = RibbonRunsHeld(b.runs, b.runs_bytes, b.nruns, 0, u->seq, h);
    } else if (r->kind == RIBBON_SHAPED && RibbonShapedGet(body, r->len, u->seq, &d, &used) != TALLY_BODY_BROKEN) {
        fid          = d.fid;
        h->digests   = d.digests;
        h->ndigests  = d.ndigests;
        for (uint64_t i = 0; rc == OK && i < d.nsplice; i++) {
            RibbonSpliceNext(d.splices, d.splices_bytes, &at, &cursor, u->seq, &digests, &s);
            rc = RibbonRunsHeld(s.runs, s.runs_bytes, s.ninsert, s.cursor, u->seq, h);
        }
    } else {
        return OK;
    }
    if (rc != OK || h->bad == UINT64_MAX)
        return rc;
    rc = RibbonSkipAdd(undone, u->seq, fid);
    bool torn = w->torn != NULL;
    w->lied |= torn;
    RibbonSay(w, RIBBON_CODE_NOT_LANDED, "fid %llu's data in stitch %llu did not land (block %llu of %llu checked does "
              "not match its digest): its record is rolled back by its splice — ERR_TAGFS_NOT_LANDED%s",
              (unsigned long long)fid, (unsigned long long)u->seq, (unsigned long long)h->bad,
              (unsigned long long)h->checked, torn ? "; a torn stitch lies after it, so the medium did not keep "
                                                    "what it confirmed: a flush that lied, or what was written "
                                                    "spoilt by the next write (M-2) — MEDIUM_LIED" : "");
    ksnprintf(why, sizeof(why), "fid %llu is rolled back by its splice, and a roll-back is a verdict an Undone "
              "carries, which this kernel does not write", (unsigned long long)fid);
    TagFSUnderGlass(why);
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
    uint32_t    n = w->nstitch < (w->lied ? 2u : 1u) ? w->nstitch : (w->lied ? 2u : 1u);
    RibbonHeld* h = kmalloc(sizeof(RibbonHeld));
    error_t     rc = OK;
    if (!h || !(h->block = kmalloc(TAGFS_BLOCK_SIZE))) {
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
    kfree(h->block);
    kfree(h);
    return rc;
}
