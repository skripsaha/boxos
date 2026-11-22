#include "stand_stitch.h"
#include "stand_fresh.h"
#include "../ink/ink.h"
#include "../ink/ink_internal.h"
#include "../ink/fallow.h"
#include "../ink/docket.h"
#include "../file_map/file_map.h"
#include "../integrity/gloss.h"
#include "../mount/book_mount.h"
#include "../plot/plot.h"
#include "block_fail.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_fresh_change suite: what a change of a file's data on a volume
// costs the stitch that names it, and the ground it gives up
//
// A stitch names what changed, and only that: a block written over in the
// middle of a run is one block in, the run's pieces on either side of it kept
// as the old run's — fresh in the Clasp is the blocks written, a cut is fresh
// in nothing and pays no flush ahead of its stitch — and so on a file mkfs laid,
// whose digests only the Tally holds. Appends a cycle takes go on the run the
// cycle's earlier appends began, one run in the record and in its stitch. The
// ribbon holds a write's claim from before its data to its Ink, and no more
// than the Ink's record after. A cut into a hole gives up no ground: a hole
// owns none. The Gloss makes room for a block below a root grown far above it.
// A Plot whose file was given up between two of its windows answers that the
// file is gone, and takes no ground.
// ============================================================================

/* The Strips of `fid`'s record, or 0 for none. */
static uint64_t StandChangeStrips(uint64_t fid)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    uint64_t    n   = rec ? rec->strip_count : 0;
    FileRecordRelease(rec);
    return n;
}

/* The cycle settled with every flush counted: OK and how many it paid, its
 * stitch's fresh blocks in *fresh. */
static bool StandChangeSettled(const StandStitch* s, uint32_t* flushes, uint64_t* fresh)
{
    uint64_t flags = 0;
    BlockFailArm(s->seat, BLOCK_FAIL_FLUSH, 0, 0, BLOCK_FAIL_NEVER);
    bool settled = TagFSSettle() == OK;
    *flushes     = BlockFailSeen();
    BlockFailDisarm();
    return settled && StandFreshLastStitch(&flags, fresh);
}

// ----------------------------------------------------------------------------
// A stitch names what changed, and only that
// ----------------------------------------------------------------------------

static TestResult TestStandChangeNamesOnlyNew(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0, runs = 0, claimed = 0, reserved = 0, fresh = 0, a = 0, b = 0, strips = 0;
    uint64_t        one = DocketClaimOfShaped(1, 1, 1, 0);
    uint32_t        flushes = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, 16u * TAGFS_BLOCK_SIZE), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-new.bin", NULL, 0, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 10u * TAGFS_BLOCK_SIZE, 1), 10 * TAGFS_BLOCK_SIZE, "ten blocks written");
    runs = StandFreshForcedRuns(fid, false);
    TEST_ASSERT(runs >= 1, "in one run, but where ground another owner holds breaks it (Allotment 1) — judged "
                           "before the cycle, which may give ground the take found held back to the free");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, m.size, TAGFS_BLOCK_SIZE, 2), TAGFS_BLOCK_SIZE, "a block is appended");
    RibbonClaimsStand(&claimed, &reserved);
    TestEvidence("bytes the ribbon holds claimed after one appended block", claimed);
    TEST_ASSERT(claimed == one && reserved == 0,
                "and the ribbon holds the claim of its Ink's record alone — nothing past it, nothing held");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, m.size, TAGFS_BLOCK_SIZE, 3), TAGFS_BLOCK_SIZE, "a second is appended");
    RibbonClaimsStand(&claimed, &reserved);
    TEST_ASSERT(claimed == 2u * one && reserved == 0, "and the ribbon holds two such claims, no more");
    strips = StandChangeStrips(fid);
    if (StandBlockOf(fid, 10, &a) && StandBlockOf(fid, 11, &b) && b == a + 1u)
        TEST_ASSERT_EQ(strips, runs + 1u,
                       "the second went on the run the first began: the settled runs, and one run of both");
    TEST_ASSERT(StandChangeSettled(&s, &flushes, &fresh) && fresh == 2, "settled, its stitch names two fresh blocks");
    TEST_ASSERT(StandStitchRemount() && StandChangeStrips(fid) == strips && StandFreshHolds(&m, fid),
                "and mounted again the record is the one memory held, Strip for Strip");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 5u * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 4), TAGFS_BLOCK_SIZE,
                   "block 5, in the middle of the run, is written over");
    TEST_ASSERT(StandChangeSettled(&s, &flushes, &fresh), "and settled");
    TestEvidence("fresh blocks a stitch names for one block written over in the middle of a run", fresh);
    TEST_ASSERT(fresh == 1 && flushes == 2, "its stitch names the one block written — the run's pieces kept");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 3u * TAGFS_BLOCK_SIZE), "the file is cut inside its run");
    TEST_ASSERT(StandChangeSettled(&s, &flushes, &fresh), "and settled");
    TEST_ASSERT(fresh == 0 && flushes == 1, "its stitch names no fresh block and pays no flush ahead of it");
    strips = StandChangeStrips(fid);
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid) && StandChangeStrips(fid) == strips,
                "mounted again, the file reads its bytes from the record memory held");
out:
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A file mkfs laid: its digests are the Tally's, never the Gloss's (§8.1)
// ----------------------------------------------------------------------------

#define STAND_CHANGE_LAID_MOST  (256u * 1024u)

/* Whether `rec` is a file mkfs laid that a call with no warrant may change:
 * no tag the system keeps, not the kernel's, a first run of three blocks and
 * more whose digests the Gloss does not hold, and no longer than the stand
 * holds. */
static bool StandChangeLaid(const FileRecord* rec)
{
    StripWalk w;
    Strip     first;
    uint64_t  digest = 0;
    StripWalkBegin(rec, &w);
    return rec->file_id != g_book.boot_fid && !InkFileKept(rec->flags) &&
           rec->size <= STAND_CHANGE_LAID_MOST && rec->size % TAGFS_BLOCK_SIZE != 0 && StripWalkNext(&w, &first) &&
           first.kind == STRIP_DATA && first.count >= 3 && !GlossFind(first.start + 1u, &digest);
}

/* The first file mkfs laid that the test may write: its id, or 0. */
static uint64_t StandChangeLaidFile(void)
{
    uint64_t ids[32], from = 0;
    uint32_t n;
    while ((n = FileMapFiles(tagfs_get_state()->files, from, ids, 32u)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec  = FileMapRecordHold(tagfs_get_state()->files, ids[i], NULL);
            bool        laid = rec && StandChangeLaid(rec);
            FileRecordRelease(rec);
            if (laid)
                return ids[i];
        }
        from = ids[n - 1u] + 1u;
    }
    return 0;
}

/* The file's bytes into the model, through a handle of its own. */
static bool StandChangeModelOf(StandFreshModel* m, uint64_t fid)
{
    TagFSFileHandle* h   = tagfs_open(fid, TAGFS_HANDLE_READ);
    FileRecord*      rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    uint64_t         got = 0;
    bool             read = h && rec && rec->size <= m->room && tagfs_read(h, m->bytes, rec->size, &got) == OK &&
                 got == rec->size;
    m->size = read ? rec->size : 0;
    FileRecordRelease(rec);
    if (h)
        tagfs_close(h);
    return read;
}

static TestResult TestStandChangeLaidFile(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = StandChangeLaidFile(), fresh = 0;
    uint32_t        flushes = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    if (!fid) {
        STAND_STITCH_PUT_BACK(&s);
        return test_steps_aside("no file mkfs laid here has a run of three blocks a write may change");
    }
    TEST_ASSERT(StandFreshModelMake(&m, STAND_CHANGE_LAID_MOST + 2u * TAGFS_BLOCK_SIZE), "room for the file's bytes");
    TEST_ASSERT(StandChangeModelOf(&m, fid), "a file mkfs laid is read whole, each block held to the Tally's digest");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 11), TAGFS_BLOCK_SIZE,
                   "its block 1, in the middle of its first run, is written over");
    TEST_ASSERT(StandChangeSettled(&s, &flushes, &fresh) && fresh == 1,
                "settled, its stitch names the one block written, and digests no block it did not write");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, m.size, 300u, 12), 300, "300 bytes are appended to its last block");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, m.size - TAGFS_BLOCK_SIZE), "the file is cut by a block");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid),
                "settled and mounted again, every byte reads, the blocks no write touched held to the Tally's digests");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A cut into a hole gives up no ground
// ----------------------------------------------------------------------------

static TestResult TestStandChangeCutIntoHole(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    TagFSState*     fs  = tagfs_get_state();
    uint64_t        fid = 0, free = 0, resting = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, 10u * TAGFS_BLOCK_SIZE), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-hole.bin", NULL, 0, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 21), TAGFS_BLOCK_SIZE, "a block is written");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 10u * TAGFS_BLOCK_SIZE), "the file is grown across a hole of nine blocks");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "and settled");
    free    = fs->free_blocks;
    resting = FallowBlocks();
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 5u * TAGFS_BLOCK_SIZE), "the file is cut into its hole");
    TestEvidence("blocks a cut into a hole set to rest", FallowBlocks() - resting);
    TEST_ASSERT(FallowBlocks() == resting && fs->free_blocks == free,
                "and no block rests, nor came free: a hole owns no ground to give up");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "settled and mounted again, the file is five blocks, one of data");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 0), "the file is cut to nothing");
    TEST_ASSERT_EQ(FallowBlocks(), resting + 1u, "and its one block of data rests");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "and settled");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The Gloss makes room for a block below a root grown far above it (§8.1)
// ----------------------------------------------------------------------------

#define STAND_CHANGE_FAR   ((1ull << 30) + 100u)
#define STAND_CHANGE_NEAR  256u      /* the Spine's: ground no file's data lies on */
#define STAND_CHANGE_NEARS 4u

static TestResult TestStandChangeGlossFar(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    uint64_t    digest  = 0;
    const char* aside   = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    GlossLetGo();
    TEST_ASSERT_EQ(GlossStandCount(), 0, "the Gloss let go of holds no digest");
    TEST_ASSERT_OK(GlossNote(STAND_CHANGE_FAR, 0xFA4u), "a digest is held for a block past 2^30");
    for (uint32_t i = 0; i < STAND_CHANGE_NEARS; i++)
        TEST_ASSERT_OK(GlossNote(STAND_CHANGE_NEAR + i, 0x100u + i), "a digest is held for a block below 4096");
    TEST_ASSERT(GlossFind(STAND_CHANGE_FAR, &digest) && digest == 0xFA4u, "the far one is found");
    for (uint32_t i = 0; i < STAND_CHANGE_NEARS; i++)
        TEST_ASSERT(GlossFind(STAND_CHANGE_NEAR + i, &digest) && digest == 0x100u + i,
                    "and each near one, under the root grown far above it");
    TEST_ASSERT_EQ(GlossStandCount(), STAND_CHANGE_NEARS + 1u, "the Gloss holds the five");
    GlossForget(STAND_CHANGE_FAR, 1);
    GlossForget(STAND_CHANGE_NEAR, STAND_CHANGE_NEARS);
    TEST_ASSERT_EQ(GlossStandCount(), 0, "and forgotten, none");
out:
    STAND_STITCH_PUT_BACK(&s);   /* mounted again: the Gloss the medium names */
    return verdict;
}

// ----------------------------------------------------------------------------
// A file given up between two windows of a Plot is gone to it
// ----------------------------------------------------------------------------

static TestResult TestStandChangeGivenUpBetween(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    Plot            p;
    TagFSState*     fs  = tagfs_get_state();
    uint64_t        fid = 0, at = 0, free = 0;
    bool            loaded = false;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    memset(&p, 0, sizeof(p));
    TEST_ASSERT(StandFreshModelMake(&m, TAGFS_BLOCK_SIZE), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-gone.bin", NULL, 0, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 31), TAGFS_BLOCK_SIZE, "and written");
    TEST_ASSERT_OK(PlotLoad(&p, fid, INK_UNWARRANTED), "a Plot of it is loaded");
    loaded = true;
    TEST_ASSERT_EQ(tagfs_delete_file(fid, INK_UNWARRANTED), 0, "the file is given up");
    InkBookPlotNext(&p);   /* as an Ink's next window holds the record again after it */
    free = fs->free_blocks;
    TEST_ASSERT_EQ(PlotBeginWrite(&p, 0, true, TAGFS_BLOCK_SIZE, &at), ERR_FILE_NOT_FOUND,
                   "the Plot's next window is refused: the file is not found");
    TEST_ASSERT_EQ(fs->free_blocks, free, "and no block was taken for it");
out:
    if (loaded)
        PlotRelease(&p);
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static const TestCase g_stand_fresh_change_tests[] = {
    { .name = "stand_fresh_change_names_only_new", .func = TestStandChangeNamesOnlyNew,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_change_laid_file", .func = TestStandChangeLaidFile,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_change_cut_into_hole", .func = TestStandChangeCutIntoHole,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_change_gloss_far", .func = TestStandChangeGlossFar,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_change_given_up_between", .func = TestStandChangeGivenUpBetween,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_fresh_change_suite = {
    g_stand_fresh_change_tests, sizeof(g_stand_fresh_change_tests) / sizeof(g_stand_fresh_change_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
