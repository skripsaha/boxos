#include "stand_binder.h"
#include "stand_fresh.h"
#include "../settle/binder.h"
#include "../settle/endpaper_fact.h"
#include "../settle/foliation.h"
#include "../file_map/file_map.h"
#include "tagfs_volume.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_endpaper suite: the measure of the next Tally the book's reserve
// keeps — what the last Tally laid and what changed since
//
// What Ink laid in memory and no stitch settled yet is in the measure, so a
// stitch that settles never raises it. Data a write puts in an old file is
// born at the stitch that carries it, and grows DIGESTS's measure, though a
// rename after the write in the same cycle makes the file's change a Labeled.
// The mount measures by the Tally it stands on what that Tally's writer
// measured, and counts the files the stitches it replays changed; the stitches
// that settle between a Tally's slice and its DONE are counted as that Tally's
// growth. Each time, the next Tally laid takes no more than the measure said.
// The measure a write of data keeps, with the blocks handed since, is never
// less than one taken afresh.
// Every test puts the volume back as it found it.
// ============================================================================

#define STAND_ENDPAPER_FILES   2400u   /* files of one name: nineteen leaves of FILES or so */
#define STAND_ENDPAPER_BATCH   1200u
#define STAND_ENDPAPER_BLOCKS  512u    /* a write's blocks: past a window of DIGESTS */
#define STAND_ENDPAPER_STRIDED 1600u   /* blocks written one apart: a Strip each, a hole between */
#define STAND_ENDPAPER_MANY    7200u   /* empty files: FILES by the last Tally well under the measure from above */

typedef struct {
    uint64_t files;
    uint64_t digests;
    uint64_t tags;
} StandEndpaper;

static void StandEndpaperOf(TagFSVolume* vol, StandEndpaper* m, const char* when)
{
    char words[96];
    EndpaperFactMost(vol, &m->files, &m->digests, &m->tags);
    ksnprintf(words, sizeof(words), "blocks the measure gives FILES %s:", when);
    TestEvidence(words, m->files);
    ksnprintf(words, sizeof(words), "blocks the measure gives DIGESTS %s:", when);
    TestEvidence(words, m->digests);
}

static bool StandEndpaperSame(const StandEndpaper* a, const StandEndpaper* b)
{
    return a->files == b->files && a->digests == b->digests && a->tags == b->tags;
}

/* `n` files of one name made, a batch settled at a time: the first id into
 * *first unless it holds one. */
static TestResult StandEndpaperMake(TagFSVolume* vol, uint32_t n, uint64_t* first)
{
    TestResult verdict = TEST_PASS;
    uint64_t   fid     = 0;
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQ(tagfs_create_file(vol, "endpaper.n", NULL, 0, 0, &fid), 0, "a file is made");
        *first = *first ? *first : fid;
        if (i % STAND_ENDPAPER_BATCH == STAND_ENDPAPER_BATCH - 1u)
            TEST_ASSERT_OK(TagFSSettle(vol), "the files settle a batch at a time");
    }
    TEST_ASSERT_OK(TagFSSettle(vol), "every file settled");
out:
    return verdict;
}

/* The files [first, first + n) given up and settled. */
static TestResult StandEndpaperGiveUp(TagFSVolume* vol, uint64_t first, uint64_t n)
{
    TestResult verdict = TEST_PASS;
    for (uint64_t f = first; first && f < first + n; f++)
        TEST_ASSERT_EQ(tagfs_delete_file(vol, f, INK_UNWARRANTED), 0, "a file is given up");
    TEST_ASSERT_OK(TagFSSettle(vol), "and settled");
out:
    return verdict;
}

// ----------------------------------------------------------------------------
// What memory holds and no stitch settled
// ----------------------------------------------------------------------------

static TestResult TestStandEndpaperOwed(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    StandEndpaper   before, owed, settled;
    uint64_t        data = 0, first = 0, fid = 0;
    const char*     aside = StandStitchAside(vol, &s);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_ENDPAPER_BLOCKS * TAGFS_BLOCK_SIZE), "room for the data");
    TEST_ASSERT_EQ(tagfs_create_file(vol, "endpaper-owed.bin", NULL, 0, 0, &data), 0, "a file for the data is made");
    TEST_ASSERT_OK(TagFSSettle(vol), "and settled");
    StandEndpaperOf(vol, &before, "before");
    TEST_ASSERT(before.files != UINT64_MAX && before.digests != UINT64_MAX,
                "the volume stands on a Tally the measure knows");
    for (uint32_t i = 0; i < STAND_ENDPAPER_BATCH; i++) {
        TEST_ASSERT_EQ(tagfs_create_file(vol, "endpaper-owed.n", NULL, 0, 0, &fid), 0, "a file is made");
        first = first ? first : fid;
    }
    TEST_ASSERT_EQ(StandFreshWrite(vol, &m, data, 0, STAND_ENDPAPER_BLOCKS * TAGFS_BLOCK_SIZE, 7u),
                   (int)(STAND_ENDPAPER_BLOCKS * TAGFS_BLOCK_SIZE), "the data is written");
    StandEndpaperOf(vol, &owed, "with them inked, no stitch settled");
    TEST_ASSERT_OK(TagFSSettle(vol), "the files and the data settle");
    StandEndpaperOf(vol, &settled, "once they settled");
    TEST_ASSERT(settled.files > before.files && settled.digests > before.digests,
                "the measure grew by the files and the data that settled");
    TEST_ASSERT(owed.files >= settled.files && owed.digests >= settled.digests,
                "and held them before they settled: a stitch that settles never raises the measure");
    TEST_ASSERT(StandEndpaperGiveUp(vol, first, STAND_ENDPAPER_BATCH) == TEST_PASS &&
                StandEndpaperGiveUp(vol, data, 1u) == TEST_PASS, "the files and the data are given up");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A Labeled after a write: the data it carries is born and counted
// ----------------------------------------------------------------------------

static TestResult TestStandEndpaperLabeledCarries(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    StandEndpaper   settled, carried;
    FileSpot        spot = { 0 };
    uint64_t        fid = 0, seq = 0, born = 0;
    const char*     aside = StandStitchAside(vol, &s);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, (1u + STAND_ENDPAPER_BLOCKS) * TAGFS_BLOCK_SIZE), "room for the data");
    TEST_ASSERT_EQ(tagfs_create_file(vol, "endpaper-lab.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(vol, &m, fid, 0, TAGFS_BLOCK_SIZE, 11u), TAGFS_BLOCK_SIZE, "a block of it written");
    TEST_ASSERT_OK(TagFSSettle(vol), "and settled");
    StandEndpaperOf(vol, &settled, "with the file settled");
    TEST_ASSERT_EQ(StandFreshWrite(vol, &m, fid, TAGFS_BLOCK_SIZE, STAND_ENDPAPER_BLOCKS * TAGFS_BLOCK_SIZE, 12u),
                   (int)(STAND_ENDPAPER_BLOCKS * TAGFS_BLOCK_SIZE), "more of it written");
    TEST_ASSERT_EQ(tagfs_rename_file(vol, fid, "endpaper-lab.txt"), 0,
                   "and the file renamed in the same cycle: its Jot a Labeled, a Shaped in it");
    TEST_ASSERT_OK(TagFSSettle(vol), "the stitch carrying both settles");
    seq = RibbonCursorNow(vol)->last;
    StandEndpaperOf(vol, &carried, "once it settled");
    FileRecord* rec   = FileMapRecordHold(vol->state.files, fid, NULL);
    bool        found = rec && FileRecordFind(rec, 1u, &spot) && spot.kind == STRIP_DATA;
    FileRecordRelease(rec);
    TEST_ASSERT(found, "the block written in the second cycle is the file's");
    born = BirthsOf(vol, spot.block);
    TestEvidence("the stitch that carried the write and the rename:", seq);
    TestEvidence("the birth its ground was noted at:", born);
    TEST_ASSERT_EQ(born, seq, "the ground is born at the stitch that put it in");
    TEST_ASSERT(carried.digests > settled.digests, "and its digests grow DIGESTS's measure");
    TEST_ASSERT(StandFreshHolds(vol, &m, fid), "the file reads as it was written");
    TEST_ASSERT(StandEndpaperGiveUp(vol, fid, 1u) == TEST_PASS, "the file is given up");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The mount's measure, and the stitches between a slice and its DONE
// ----------------------------------------------------------------------------

/* The next Tally written: no more than the measure taken just before it. */
static TestResult StandEndpaperNextFits(TagFSVolume* vol, const char* what)
{
    TestResult      verdict = TEST_PASS;
    BinderStandSaid said;
    uint64_t        cost = TallyCostBlocks(vol);
    TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "the next Tally is written");
    BinderStand(vol, &said);
    TestEvidence("blocks the reserve measured the next Tally at:", cost);
    TestEvidence("blocks a copy of it took:", said.tally_blocks);
    TEST_ASSERT(said.tally_blocks <= cost, what);
out:
    return verdict;
}

static TestResult TestStandEndpaperMount(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandEndpaper      laid, stood, live, again;
    uint64_t           first = 0, more = 0;
    const char*        aside = StandBinderAside(vol, &b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(vol) && TagFSSettle(vol) == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandEndpaperMake(vol, STAND_ENDPAPER_BATCH, &first) == TEST_PASS, "files are made");
    TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "a Tally of them is written");
    StandEndpaperOf(vol, &laid, "by the Tally its writer laid");
    TEST_ASSERT(StandStitchRemount(vol), "the volume mounted again on it");
    StandEndpaperOf(vol, &stood, "by the Tally the mount stands on");
    TEST_ASSERT(StandEndpaperSame(&laid, &stood), "the mount measures what the Tally's writer measured");
    TEST_ASSERT(StandEndpaperMake(vol, STAND_ENDPAPER_FILES, &more) == TEST_PASS, "more files made past the Tally");
    StandEndpaperOf(vol, &live, "as their stitches settled");
    TEST_ASSERT(StandStitchRemount(vol), "the volume mounted again, their stitches replayed");
    StandEndpaperOf(vol, &again, "past the stitches the mount replayed");
    TEST_ASSERT(again.files > stood.files, "the files the replayed stitches made are the mount's growth");
    TEST_ASSERT(again.files <= live.files && again.digests <= live.digests && again.tags <= live.tags,
                "and no more than the writer counted as they settled");
    TEST_ASSERT_OK(TagFSSettle(vol), "a stitch of the mount settled");
    TEST_ASSERT(StandEndpaperNextFits(vol, "the next Tally takes no more than the mount's measure") == TEST_PASS,
                "the next Tally fits the measure");
    TEST_ASSERT(StandEndpaperGiveUp(vol, first, STAND_ENDPAPER_BATCH) == TEST_PASS &&
                StandEndpaperGiveUp(vol, more, STAND_ENDPAPER_FILES) == TEST_PASS, "the files are given up");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

static TestResult TestStandEndpaperPending(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    uint64_t           first = 0;
    const char*        aside = StandBinderAside(vol, &b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(vol) && TagFSSettle(vol) == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(BinderTurn(vol) && BinderOwed(vol), "a Tally is begun, its slice taken in its first turn");
    TEST_ASSERT(StandEndpaperMake(vol, STAND_ENDPAPER_FILES, &first) == TEST_PASS,
                "files made, their stitches settled between the slice and the Tally's DONE");
    TEST_ASSERT_OK(BinderRunWhole(vol), "the Tally runs to its DONE");
    TEST_ASSERT(StandEndpaperNextFits(vol, "the next Tally, the files in it, takes no more than the measure the "
                                      "last one's DONE left") == TEST_PASS, "the next Tally fits the measure");
    TEST_ASSERT(StandEndpaperGiveUp(vol, first, STAND_ENDPAPER_FILES) == TEST_PASS, "the files are given up");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/*
 * ‼ A LEAF NOTHING WENT IN IS THE LEAF IT WAS. A Tally of empty files, nothing
 * changed since: no item of an empty file widens with the next Tally's seq —
 * only a Parcel's runs do — so the next Tally's FILES is measured at the
 * leaves the last one laid, the leaves its runs lie in recut, and never every
 * leaf recut as though a run lay in each (endpaper_fact.c, FilesMost): less
 * than twice the leaves it laid, where every leaf recut is three times them.
 * The reserve keeps the measure for each slot of the ring besides
 * (BinderRingShort), so a measure three times the book filled a volume of
 * empty files with reserve long before its ground ran out. The next Tally
 * fits the measure.
 */
static TestResult TestStandEndpaperEmptyFiles(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandEndpaper      laid;
    uint64_t           first = 0;
    uint32_t           leaves = 0;
    const char*        aside = StandBinderAside(vol, &b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(vol) && TagFSSettle(vol) == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandEndpaperMake(vol, STAND_ENDPAPER_MANY, &first) == TEST_PASS, "empty files are made");
    TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "a Tally of them is written");
    TEST_ASSERT(FoliationLeaves(vol, TALLY_FILES, &leaves) && leaves > 0, "the leaves of FILES it laid are known");
    TestEvidence("leaves of FILES the Tally laid:", leaves);
    StandEndpaperOf(vol, &laid, "by the Tally of empty files, nothing changed since");
    TEST_ASSERT(laid.files < 2u * (uint64_t)leaves,
                "FILES is measured at the leaves laid and the leaves the runs lie in, not every leaf recut");
    TEST_ASSERT(StandEndpaperNextFits(vol, "the next Tally takes no more than the measure") == TEST_PASS,
                "the next Tally fits the measure");
    TEST_ASSERT(StandEndpaperGiveUp(vol, first, STAND_ENDPAPER_MANY) == TEST_PASS, "the files are given up");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The measure a write keeps
// ----------------------------------------------------------------------------

/* A file of STAND_ENDPAPER_STRIDED blocks written one apart, settled. */
static TestResult StandEndpaperStrided(TagFSVolume* vol, StandFreshModel* m, uint64_t* fid)
{
    TestResult verdict = TEST_PASS;
    TEST_ASSERT(StandFreshModelMake(m, 2u * STAND_ENDPAPER_STRIDED * TAGFS_BLOCK_SIZE), "room for the file");
    TEST_ASSERT_EQ(tagfs_create_file(vol, "endpaper-kept.bin", NULL, 0, 0, fid), 0, "a file is made");
    TEST_ASSERT_OK(StandFreshSpan(vol, m, *fid, 2u * STAND_ENDPAPER_STRIDED * TAGFS_BLOCK_SIZE), "spanned over a hole");
    for (uint64_t k = 0; k < STAND_ENDPAPER_STRIDED; k++)
        TEST_ASSERT_EQ(StandFreshWrite(vol, m, *fid, 2u * k * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 40u + k),
                       TAGFS_BLOCK_SIZE, "every other block written");
    TEST_ASSERT_OK(TagFSSettle(vol), "and settled");
out:
    return verdict;
}

static TestResult TestStandEndpaperKept(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    uint64_t           fid = 0, first = 0, kept = 0, fresh = 0;
    const char*        aside = StandBinderAside(vol, &b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(vol) && TagFSSettle(vol) == OK, "the volume mounted afresh, a stitch of it settled");
    TEST_ASSERT(StandEndpaperMake(vol, STAND_ENDPAPER_MANY, &first) == TEST_PASS, "empty files are made");
    TEST_ASSERT(StandEndpaperStrided(vol, &m, &fid) == TEST_PASS, "and a file of many runs");
    TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "a Tally of them is written");
    /* Kept twice: a measure is kept with the data handed when the one before
     * it began (endpaper.c), so the second counts from the first. */
    (void)EndpaperDataKeeps(vol, true);
    (void)EndpaperDataKeeps(vol, true);
    TEST_ASSERT_EQ(StandFreshWrite(vol, &m, fid, TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 9u), TAGFS_BLOCK_SIZE,
                   "a block of a hole of the file written past the measure kept: one block handed since, the "
                   "file changed since the Tally and counted whole");
    kept  = TallyKeptBlocks(vol);
    fresh = TallyCostBlocks(vol);
    TestEvidence("blocks the next Tally takes by the measure kept and the data handed since:", kept);
    TestEvidence("blocks it takes by a measure taken now:", fresh);
    TEST_ASSERT(kept != UINT64_MAX, "a measure is kept");
    TEST_ASSERT(kept >= fresh, "the measure kept, with the blocks handed since, is no less than one taken now");
    TEST_ASSERT_OK(TagFSSettle(vol), "the write settled");
    TEST_ASSERT(StandFreshHolds(vol, &m, fid), "the file reads as it was written");
    TEST_ASSERT(StandEndpaperGiveUp(vol, first, STAND_ENDPAPER_MANY) == TEST_PASS &&
                StandEndpaperGiveUp(vol, fid, 1u) == TEST_PASS, "the files are given up");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------

static const TestCase g_stand_endpaper_tests[] = {
    { .name = "stand_endpaper_owed", .func = TestStandEndpaperOwed,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_endpaper_labeled_carries", .func = TestStandEndpaperLabeledCarries,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_endpaper_mount", .func = TestStandEndpaperMount,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_endpaper_pending", .func = TestStandEndpaperPending,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_endpaper_kept", .func = TestStandEndpaperKept,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_endpaper_empty_files", .func = TestStandEndpaperEmptyFiles,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_endpaper_suite = {
    g_stand_endpaper_tests, sizeof(g_stand_endpaper_tests) / sizeof(g_stand_endpaper_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
