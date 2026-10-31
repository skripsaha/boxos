#include "tests.h"
#include "stand_tests.h"
#include "stand_ribbon.h"
#include "stand_stitch.h"
#include "volume_ribbon.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../mount/away.h"
#include "../mount/book_mount.h"
#include "../mount/errata.h"
#include "../mount/under_glass.h"
#include "../block_map/block_map_derived.h"
#include "../settle/sewing.h"
#include "../settle/settle.h"
#include "atomics.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_errata suite: a mount's verdict, held in memory and laid down by
// the session's first stitch (spec_v2 §3.9, §2.8 Undone, §16.5 п.4-7)
//
// Each test sews, by the stand's hand (stand_ribbon.h), a ribbon whose newest
// stitch a mount must roll back — its data never landed, sewn without its
// first flush (C3), on a medium that lied (C6), or a record that does not fit
// its file (the rule of rot) — and holds the mount to the verdict: the volume
// written on, not under glass; the files rolled back by their splices, a file
// born there never born and its id never given again; the verdict the same
// on a mount before any cycle ran (C16, the cut before the Undone settled).
// Then the session's first cycle runs, and its stitch is held to the law: an
// Undone of each stitch undone right after its StitchHead, before any other
// record, and every file rolled back wearing settle:undone, which a query by
// the tag finds; mounted again on it, the verdict is the Undone's and the
// state the same (C16, the Undone settled). Everything laid is put back.
// ============================================================================

#define STAND_ERRATA_WIDE_FID  ((1ull << 32) + 5u)

static const TagSpelling s_undone_tag = { .key = "settle", .key_len = 6, .value = "undone", .value_len = 6 };

/* A Shaped of `fid` over its gen `base` stitches back, its one block put in
 * at `block` with `digest` — the digest its data would have, or not. */
static void StandErrataShaped(StandRibbonRecords* r, uint64_t fid, uint64_t base, uint64_t block, uint64_t digest)
{
    uint8_t  body[128], splices[48], dig[8];
    uint32_t n      = 0;
    uint64_t cursor = 0;
    TallyRun run    = { .start = block, .count = 1 };
    n += BookVarintPut(splices + n, 0);
    n += BookVarintPut(splices + n, 1);
    n += BookVarintPut(splices + n, 1);
    n += RibbonRunPut(splices + n, &run, &cursor);
    TallyLePut(dig, digest, 8);
    RibbonShaped sh = { .fid = fid, .base = base, .size = TAGFS_BLOCK_SIZE, .mtime_ns = 2, .nsplice = 1,
                        .splices = splices, .splices_bytes = n, .ndigests = 1, .digests = dig };
    StandRibbonPut(r, RIBBON_SHAPED, body, RibbonShapedBodyPut(body, &sh));
    r->fresh += 1;
}

/* The block file `fid` holds first, or 0 when it is not live. */
static uint64_t StandErrataBlock(uint64_t fid)
{
    uint64_t block = 0;
    return StandBlockOf(fid, 0, &block) ? block : 0;
}

/* Whether file `fid` wears settle:undone, and a query by the tag finds it. */
static bool StandErrataWears(uint64_t fid)
{
    TagFSState* fs = tagfs_get_state();
    uint32_t    id = TagRegistryLookup(fs->registry, &s_undone_tag);
    uint64_t    found[8];
    TagFSPage   page;
    if (id == TAGFS_INVALID_TAG_ID || !TagBitmapHas(fs->bitmap_index, id, fid))
        return false;
    memset(&page, 0, sizeof(page));
    if (TagBitmapQueryPage(fs->bitmap_index, &id, 1, NULL, 0, NULL, 0, 0, found, 8, false, &page) != 0)
        return false;
    for (uint32_t i = 0; i < page.delivered; i++)
        if (found[i] == fid)
            return true;
    return false;
}

/* The `k`-th record of the stitch the session's first cycle sewed, read as an
 * Undone: the seq it names, why, and its fids into `fids` (room for `room`).
 * False when that record is no Undone or does not read. */
static bool StandErrataUndoneAt(const RibbonUnit* u, uint32_t k, uint64_t* seq, uint64_t* why, uint64_t* fids,
                                uint32_t room, uint64_t* nfid)
{
    RibbonUndone d;
    uint32_t     used = 0, at = 0;
    if (k >= u->nrecords || u->records[k].kind != RIBBON_UNDONE ||
        RibbonUndoneGet(u->stream + u->records[k].body, u->records[k].len, &d, &used) == TALLY_BODY_BROKEN)
        return false;
    *seq  = u->seq - d.seq_back;
    *why  = d.why;
    *nfid = d.nfid;
    for (uint64_t i = 0; i < d.nfid; i++) {
        uint64_t id = 0;
        if (!TallyIdNext(d.fids, d.fids_bytes, &at, &id, i == 0))
            return false;
        if (i < room)
            fids[i] = id;
    }
    return true;
}

/* The session's first cycle, run by an asker: the block it lies on kept for
 * the put-back, and the stitch it sewed read back (*out, the caller's). */
static bool StandErrataFirstCycle(StandRibbon* w, RibbonUnit** out)
{
    RibbonCursor c;
    RibbonCursorStand(&c);
    if (w->nlaid == STAND_RIBBON_LAID)
        return false;
    w->laid[w->nlaid++] = (c.quire.block + c.off / TAGFS_BLOCK_SECTORS) * TAGFS_BLOCK_SECTORS;
    return TagFSSettle() == OK && StandStitchRead(out);
}

/* The one Undone a first stitch must carry: of `seq`, `why`, naming `fid`
 * alone, as its first record — before any Coined. */
static bool StandErrataCarries(const RibbonUnit* u, uint64_t seq, uint64_t why, uint64_t fid)
{
    uint64_t got_seq = 0, got_why = 0, nfid = 0, fids[2] = { 0, 0 };
    return StandErrataUndoneAt(u, 0, &got_seq, &got_why, fids, 2, &nfid) && got_seq == seq && got_why == why &&
           nfid == 1 && fids[0] == fid;
}

// ----------------------------------------------------------------------------
// C3 and C16: a Shaped whose data never landed, rolled back by its splice
// ----------------------------------------------------------------------------

/* Stitch top+1 bears file F on the first free block; stitch top+2, sewn
 * without its first flush, puts the second in for it with a digest its bytes
 * do not hold — its data never landed. Mounted, F stands as stitch top+1
 * left it, the second block neither a file's nor free, the volume written on;
 * mounted again before any cycle, the same. The first cycle carries the
 * Undone, and F settle:undone; mounted on it, the same F — said by the
 * Undone now, the newest stitch no longer the one undone. */
static TestResult TestStandErrataNotLanded(void)
{
    TestResult          verdict = TEST_PASS;
    StandRibbon         w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid       said;
    RibbonUnit*         u = NULL;
    uint64_t            digests[2];
    const char*         aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    StandRibbonBorn(r1, w.fid, TAGFS_BLOCK_SIZE, w.free, 1, digests, 0, 0, 0);
    StandErrataShaped(r2, w.fid, 1, w.free + 1u, digests[1] ^ 1u);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    h2.flags      = 0;
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "a Born, and a Shaped sewn without its first flush whose data never landed, are laid");
    for (uint32_t pass = 0; pass < 2; pass++) {
        TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
        MountBookStandSaid(&said);
        TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_NOT_LANDED], 1, "the data that did not land is said");
        TEST_ASSERT(!TagFSIsUnderGlass(), "and the volume is written on: the verdict is held in memory");
        TEST_ASSERT_EQ(StandErrataBlock(w.fid), w.free, "the file stands as its Born left it");
        TEST_ASSERT(!BlockMapDerivedIsFree(w.free + 1u), "the block the undone record put in is nobody's");
        if (!pass)
            TEST_ASSERT(TagFSStandLetGo(), "let go of before any cycle ran: the cut before the Undone settled");
    }
    TEST_ASSERT(StandErrataFirstCycle(&w, &u), "the session's first cycle settles");
    TEST_ASSERT(StandErrataCarries(u, w.top + 2u, UNDONE_NOT_LANDED, w.fid),
                "its stitch carries the Undone of the stitch undone, why 1, first — before any Coined");
    TEST_ASSERT(StandErrataWears(w.fid), "and the file rolled back wears settle:undone, found by the tag");
    RibbonUnitLetGo(u);
    u = NULL;
    TEST_ASSERT(TagFSStandLetGo() && StandRibbonUp(), "the volume mounts on the Undone");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.ribbon[RIBBON_CODE_UNDONE] == 1 && said.ribbon[RIBBON_CODE_NOT_LANDED] == 0,
                "the Undone is the verdict now; no data is checked again");
    TEST_ASSERT(!TagFSIsUnderGlass() && StandErrataBlock(w.fid) == w.free && StandErrataWears(w.fid),
                "the file stands where it stood, wearing settle:undone, the volume written on");
out:
    RibbonUnitLetGo(u);
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

/* Whether a cycle holds the Settle — from its S0 to its Give. */
static bool StandErrataCycling(void)
{
    return __atomic_load_n(&g_settle_scribe.holder, __ATOMIC_ACQUIRE) != 0;
}

/*
 * The session's first cycle is asked at the door's opening (SETTLE_DEMAND_
 * ERRATA): no change is owed, no program asks, and the cadence alone lays the
 * verdict down — at once, not a window on: with nothing inked, nothing else
 * would ever begin it. The stand holds the cadence, so it is let go here, and
 * the machine turned by hand until a cycle holds the Settle — well inside the
 * window; held again at once, the cycle in flight runs on to its end — on the
 * committer's core, after the ribbon's cursor has moved past its stitch — and
 * no other begins behind it, and the machine is turned until it gave the
 * Settle back, a window of its own to do it in. What the test reads and puts
 * back is the medium as that one cycle left it: the cycle leaves the volume's
 * door only after its Give (settle.c, SettleRunEnd), and the put-back waits
 * that out before it lays a byte (StandRibbonLetGo).
 */
static TestResult TestStandErrataAsked(void)
{
    TestResult          verdict = TEST_PASS;
    StandRibbon         w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    RibbonCursor        c, now;
    RibbonUnit*         u = NULL;
    uint64_t            digests[2], until = 0;
    bool                let_go = false, began = false, moved = false;
    const char*         aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    StandRibbonBorn(r1, w.fid, TAGFS_BLOCK_SIZE, w.free, 1, digests, 0, 0, 0);
    StandErrataShaped(r2, w.fid, 1, w.free + 1u, digests[1] ^ 1u);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    h2.flags      = 0;
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "a Born, and a Shaped whose data never landed, are laid");
    TEST_ASSERT(StandRibbonUp() && !TagFSIsUnderGlass(), "the volume mounts on them, holding a verdict");
    RibbonCursorStand(&c);
    TEST_ASSERT(w.nlaid < STAND_RIBBON_LAID, "room to put back the stitch the cadence lays");
    w.laid[w.nlaid++] = (c.quire.block + c.off / TAGFS_BLOCK_SECTORS) * TAGFS_BLOCK_SECTORS;
    SettleCadenceHold(false);
    let_go = true;
    until  = rdtsc() + cpu_us_to_tsc(SETTLE_WINDOW_US / 2u);
    while (!began && !moved && rdtsc() < until) {
        StandSettleTurn();
        began = StandErrataCycling();
        RibbonCursorStand(&now);
        moved = now.quire.block != c.quire.block || now.off != c.off;
    }
    SettleCadenceHold(true);
    let_go = false;
    TEST_ASSERT(began || moved, "the cadence began a cycle by itself, well inside the window");
    until = rdtsc() + cpu_us_to_tsc(SETTLE_WINDOW_US);
    while (StandErrataCycling() && rdtsc() < until)
        StandSettleTurn();
    RibbonCursorStand(&now);
    moved = now.quire.block != c.quire.block || now.off != c.off;
    TEST_ASSERT(moved && !StandErrataCycling(), "and the cycle sewed a stitch and ended");
    TEST_ASSERT(StandStitchRead(&u) && StandErrataCarries(u, w.top + 2u, UNDONE_NOT_LANDED, w.fid),
                "the session's first, carrying the verdict");
    TEST_ASSERT(StandErrataWears(w.fid), "and the file rolled back wears settle:undone");
out:
    if (let_go)
        SettleCadenceHold(true);
    RibbonUnitLetGo(u);
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

// ----------------------------------------------------------------------------
// A Born undone: no file, its id burned (§16.5 п.7)
// ----------------------------------------------------------------------------

static TestResult TestStandErrataBornUndone(void)
{
    TestResult          verdict = TEST_PASS;
    StandRibbon         w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid       said;
    RibbonUnit*         u = NULL;
    uint64_t            digests[2], fid = 0, wrong[1];
    const char*         aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    wrong[0] = digests[0] ^ 1u;
    StandRibbonEmpty(r1, w.fid);
    StandRibbonBorn(r2, w.fid + 1u, TAGFS_BLOCK_SIZE, w.free, 1, wrong, 0, 0, 0);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    h2.flags      = 0;
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "an empty file, then a Born whose data never landed, are laid");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_NOT_LANDED], 1, "the data that did not land is said");
    TEST_ASSERT(!TagFSIsUnderGlass(), "the volume is written on");
    TEST_ASSERT(StandRibbonLive(w.fid, NULL) && !StandRibbonLive(w.fid + 1u, NULL),
                "the file born in the undone stitch does not exist — no empty shell of it either");
    TEST_ASSERT(StandErrataFirstCycle(&w, &u), "the session's first cycle settles");
    TEST_ASSERT(StandErrataCarries(u, w.top + 2u, UNDONE_NOT_LANDED, w.fid + 1u),
                "its stitch carries the Undone of the Born, first");
    RibbonUnitLetGo(u);
    u = NULL;
    TEST_ASSERT_EQ(tagfs_create_file("errata-after.bin", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT(fid > w.fid + 1u, "with an id past the one burned: an id is never given twice");
out:
    RibbonUnitLetGo(u);
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

// ----------------------------------------------------------------------------
// The rule of rot: a record that does not fit its file (why 2)
// ----------------------------------------------------------------------------

static TestResult TestStandErrataRot(void)
{
    TestResult          verdict = TEST_PASS;
    StandRibbon         w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid       said;
    RibbonUnit*         u = NULL;
    uint8_t             body[64];
    uint64_t            digests[2];
    const char*         aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    StandRibbonBorn(r1, w.fid, TAGFS_BLOCK_SIZE, w.free, 1, digests, 0, 0, 0);
    RibbonLabeled l = { .fid = w.fid, .base = 5, .what = LABEL_NAME, .name = (const uint8_t*)"rot", .name_len = 3 };
    StandRibbonPut(r2, RIBBON_LABELED, body, RibbonLabeledBodyPut(body, &l));
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "a Born, and a Labeled over a gen the file never had, are laid");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_BASE_MISMATCH], 1, "the record that does not fit is said");
    TEST_ASSERT(!TagFSIsUnderGlass(), "and the volume is written on: the rule of rot is a verdict too");
    TEST_ASSERT_EQ(StandErrataBlock(w.fid), w.free, "the file stays as it last matched");
    TEST_ASSERT(StandErrataFirstCycle(&w, &u), "the session's first cycle settles");
    TEST_ASSERT(StandErrataCarries(u, w.top + 2u, UNDONE_ROT, w.fid), "its stitch carries the Undone, why 2, first");
    TEST_ASSERT(StandErrataWears(w.fid), "and the file wears settle:undone");
    RibbonUnitLetGo(u);
    u = NULL;
    TEST_ASSERT(TagFSStandLetGo() && StandRibbonUp(), "the volume mounts on the Undone");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.ribbon[RIBBON_CODE_UNDONE] == 1 && said.ribbon[RIBBON_CODE_BASE_MISMATCH] == 0,
                "the record undone is passed over, not held to its file again");
    TEST_ASSERT(!TagFSIsUnderGlass() && StandErrataBlock(w.fid) == w.free, "and the file stands where it stood");
out:
    RibbonUnitLetGo(u);
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

/*
 * The rule of rot at a file's second record of a stitch (C13: a Coined lost
 * with a sector of a closed unit): a Shaped of F that fits, then a Labeled of
 * F putting on a tag no Coined names. The mount rolls F back to where the
 * stitch found it — the Shaped passed over too, its block nobody's — exactly
 * as the next mount leaves it once the Undone passes over every record of F
 * in that stitch (§2.8, H-1).
 */
static TestResult TestStandErrataRotLate(void)
{
    TestResult          verdict = TEST_PASS;
    StandRibbon         w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid       said;
    RibbonUnit*         u = NULL;
    uint8_t             body[64], add[12];
    uint64_t            digests[2];
    const char*         aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    StandRibbonBorn(r1, w.fid, TAGFS_BLOCK_SIZE, w.free, 1, digests, 0, 0, 0);
    StandErrataShaped(r2, w.fid, 1, w.free + 1u, digests[1]);
    RibbonLabeled l = { .fid = w.fid, .base = 0, .what = LABEL_TAGS, .nadd = 1, .add = add,
                        .add_bytes = BookVarintPut(add, (uint64_t)TAGFS_MAX_TAG_ID + 1u) };
    StandRibbonPut(r2, RIBBON_LABELED, body, RibbonLabeledBodyPut(body, &l));
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "a Born, then a Shaped that fits and a Labeled wearing a tag nobody coined, are laid");
    for (uint32_t pass = 0; pass < 2; pass++) {
        TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
        MountBookStandSaid(&said);
        TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_BASE_MISMATCH], 1, "the Labeled that does not fit is said");
        TEST_ASSERT(!TagFSIsUnderGlass(), "and the volume is written on");
        TEST_ASSERT_EQ(StandErrataBlock(w.fid), w.free, "the file stands where the stitch found it: its Shaped is "
                       "passed over with the Labeled");
        TEST_ASSERT(!BlockMapDerivedIsFree(w.free + 1u), "the block the Shaped put in is nobody's");
        if (!pass)
            TEST_ASSERT(TagFSStandLetGo(), "let go of before any cycle ran");
    }
    TEST_ASSERT(StandErrataFirstCycle(&w, &u), "the session's first cycle settles");
    TEST_ASSERT(StandErrataCarries(u, w.top + 2u, UNDONE_ROT, w.fid), "its stitch carries the Undone, why 2, first");
    RibbonUnitLetGo(u);
    u = NULL;
    TEST_ASSERT(TagFSStandLetGo() && StandRibbonUp(), "the volume mounts on the Undone");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.ribbon[RIBBON_CODE_UNDONE] == 1 && said.ribbon[RIBBON_CODE_BASE_MISMATCH] == 0,
                "both records of the file are passed over by the Undone");
    TEST_ASSERT(!TagFSIsUnderGlass() && StandErrataBlock(w.fid) == w.free && StandErrataWears(w.fid),
                "and the file stands where the first mount left it, wearing settle:undone");
out:
    RibbonUnitLetGo(u);
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

// ----------------------------------------------------------------------------
// C6: a medium that lied, two stitches undone — an Undone each
// ----------------------------------------------------------------------------

static TestResult TestStandErrataLiedTwo(void)
{
    TestResult          verdict = TEST_PASS;
    StandRibbon         w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid       said;
    RibbonUnit*         u = NULL;
    uint64_t            digests[2], wrong[2], seq[2] = { 0, 0 }, why = 0, nfid = 0, fids[2];
    const char*         aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    wrong[0] = digests[0] ^ 1u;
    wrong[1] = digests[1] ^ 1u;
    StandRibbonBorn(r1, w.fid, TAGFS_BLOCK_SIZE, w.free, 1, wrong, 0, 0, 0);
    StandRibbonBorn(r2, w.fid + 1u, TAGFS_BLOCK_SIZE, w.free + 1u, 1, wrong + 1, 0, 0, 0);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    h1.flags     |= STITCH_MEDIUM_LIED;
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    h2.flags     |= STITCH_MEDIUM_LIED;
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "two Borns on a medium that lied, DATA_FLUSHED, neither one's data landed, are laid");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
    TEST_ASSERT(!TagFSIsUnderGlass(), "the volume is written on");
    TEST_ASSERT(!StandRibbonLive(w.fid, NULL) && !StandRibbonLive(w.fid + 1u, NULL),
                "neither file is born: a first flush lies as a last one does (M-2)");
    TEST_ASSERT(StandErrataFirstCycle(&w, &u), "the session's first cycle settles");
    TEST_ASSERT(StandErrataUndoneAt(u, 0, &seq[0], &why, fids, 2, &nfid) && why == UNDONE_NOT_LANDED &&
                nfid == 1 && StandErrataUndoneAt(u, 1, &seq[1], &why, fids + 1, 1, &nfid) && nfid == 1,
                "its stitch carries two Undones, first, one for each stitch undone");
    TEST_ASSERT(seq[0] == w.top + 1u && seq[1] == w.top + 2u && fids[0] == w.fid && fids[1] == w.fid + 1u,
                "in the order of the stitches they undo");
    RibbonUnitLetGo(u);
    u = NULL;
    /* The stitch that laid them lies, MEDIUM_LIED, after the second undone:
     * the next mount holds the newest two again — that one among them. */
    TEST_ASSERT(TagFSStandLetGo() && StandRibbonUp(), "the volume mounts on the Undones");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.ribbon[RIBBON_CODE_UNDONE] == 2 && said.ribbon[RIBBON_CODE_NOT_LANDED] == 0,
                "the stitch an Undone names is not held again: no data said not landed, no verdict to lay twice");
    TEST_ASSERT(!StandRibbonLive(w.fid, NULL) && !StandRibbonLive(w.fid + 1u, NULL), "and neither file is born");
out:
    RibbonUnitLetGo(u);
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

// ----------------------------------------------------------------------------
// An Undone of a file id past 32 bits (§16.5 п.5: fids are u64)
// ----------------------------------------------------------------------------

static TestResult TestStandErrataWideFid(void)
{
    TestResult          verdict = TEST_PASS;
    StandRibbon         w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    RibbonUnit*         u = NULL;
    uint64_t            digests[2];
    const char*         aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    StandRibbonBorn(r1, STAND_ERRATA_WIDE_FID, TAGFS_BLOCK_SIZE, w.free, 1, digests, 0, 0, 0);
    StandErrataShaped(r2, STAND_ERRATA_WIDE_FID, 1, w.free + 1u, digests[1] ^ 1u);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    h2.flags      = 0;
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "a file of id 2^32 + 5 is born, and a Shaped of it whose data never landed laid");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
    TEST_ASSERT(!TagFSIsUnderGlass() && StandErrataBlock(STAND_ERRATA_WIDE_FID) == w.free,
                "the file is rolled back, the volume written on");
    TEST_ASSERT(StandErrataFirstCycle(&w, &u), "the session's first cycle settles");
    TEST_ASSERT(StandErrataCarries(u, w.top + 2u, UNDONE_NOT_LANDED, STAND_ERRATA_WIDE_FID),
                "its Undone names the file by all 64 bits of its id");
out:
    RibbonUnitLetGo(u);
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

/* The verdict's first cycle where the mount gave the ribbon no next_quire
 * (RibbonNextGivenAtMount, refused): the cycle asks for one at its TAKE before
 * the verdict's settle:undone claims room past it (StitchRunStep) — so the
 * first cycle settles, carrying the Undone, and is not refused for want of
 * room a window on, every cycle after the same. */
static TestResult TestStandErrataNextRefused(void)
{
    TestResult          verdict = TEST_PASS;
    StandRibbon         w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid       said;
    RibbonUnit*         u = NULL;
    RibbonCursor        c;
    uint64_t            digests[2];
    bool                armed = false;
    const char*         aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    StandRibbonBorn(r1, w.fid, TAGFS_BLOCK_SIZE, w.free, 1, digests, 0, 0, 0);
    StandErrataShaped(r2, w.fid, 1, w.free + 1u, digests[1] ^ 1u);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    h2.flags       = 0;
    h2.from_block  = w.quire;
    h2.from_blocks = w.quire_blocks;
    h2.next_block  = 0;
    h2.next_blocks = 0;
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.next, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "a Born, and at the start of the next_quire a Shaped whose data never landed, naming none past "
                "it, are laid");
    RibbonStandNextRefused(true);
    armed = true;
    TEST_ASSERT(StandRibbonUp() && !TagFSIsUnderGlass(), "the volume mounts on them, written on");
    armed = RibbonStandNextRefused(false);
    TEST_ASSERT(!armed, "the mount asked the map for a next_quire, and the map refused it");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.ribbon[RIBBON_CODE_NOT_LANDED] == 1 && ErrataHeld(),
                "the data that did not land is said, and the verdict is held");
    RibbonCursorStand(&c);
    TEST_ASSERT(c.quire.block == w.next && !c.next.blocks,
                "the ribbon stands in the Quire it went on into, with no next_quire past it");
    TEST_ASSERT(StandErrataFirstCycle(&w, &u),
                "the session's first cycle settles: it asks for a next_quire before the tag claims room past it");
    TEST_ASSERT(StandErrataCarries(u, w.top + 2u, UNDONE_NOT_LANDED, w.fid),
                "its stitch carries the Undone of the stitch undone, first");
    TEST_ASSERT(StandErrataWears(w.fid), "and the file rolled back wears settle:undone");
out:
    if (armed)
        (void)RibbonStandNextRefused(false);
    RibbonUnitLetGo(u);
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

static const TestCase g_stand_errata_tests[] = {
    { .name = "stand_errata_not_landed", .func = TestStandErrataNotLanded,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_errata_born_undone", .func = TestStandErrataBornUndone,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_errata_asked", .func = TestStandErrataAsked,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_errata_rot_late", .func = TestStandErrataRotLate,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_errata_rot", .func = TestStandErrataRot,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_errata_lied_two", .func = TestStandErrataLiedTwo,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_errata_wide_fid", .func = TestStandErrataWideFid,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_errata_next_refused", .func = TestStandErrataNextRefused,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
};

const TestSuite g_stand_errata_suite = {
    g_stand_errata_tests, sizeof(g_stand_errata_tests) / sizeof(g_stand_errata_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
