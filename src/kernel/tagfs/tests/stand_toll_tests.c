#include "tests.h"
#include "stand_tests.h"
#include "stand_stitch.h"
#include "stand_fresh.h"
#include "../tagfs.h"
#include "../ink/ink.h"
#include "../mount/book_mount.h"
#include "../settle/settle.h"
#include "../settle/sewing.h"
#include "../file_map/file_map.h"
#include "block_fail.h"
#include "atomics.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_toll suite: what a cycle pays before its stitch, and what a mount
// pays to believe it
//
// The hybrid: a stitch naming fresh data pays the flush of that data before
// its atoms only when the mount's check of it — its runs and its blocks, by
// the pace the medium's reads were seen to keep — would cost more than the
// last cycle did (StitchToll); a stitch it spares says so by not saying
// DATA_FLUSHED, and the mount reads its data back. The pace is named by the
// stand (BlockFailReadPace) where a test must meet one answer: QEMU's times
// drift. On a medium that seeks (BlockFailReadLag, SEEKLAG=) the check a
// spared stitch costs the mount stays within the D the writer judged it by —
// weighed by the stand's own clock, a seek for every read command it gives.
//
// And the window a frequent small writer waits for: five seconds.
// ============================================================================

#define STAND_TOLL_LAG_US    200u      /* a seek, when the build names none (SEEKLAG=) */
#ifdef CONFIG_SEEKLAG_US
#define STAND_TOLL_LAG_KEPT  CONFIG_SEEKLAG_US   /* the seek the build armed for every test (tests.c) */
#else
#define STAND_TOLL_LAG_KEPT  0u
#endif
#define STAND_TOLL_FILES     200u      /* the files a cycle bears, to cost the medium a D a seek fits in */
#define STAND_TOLL_FAST      (1ull << 50)

/* The flushes the cycle an asker runs pays, counted. */
static uint32_t StandTollFlushes(uint8_t seat, error_t* rc)
{
    BlockFailArm(seat, BLOCK_FAIL_FLUSH, 0, 0, BLOCK_FAIL_NEVER);
    *rc = TagFSSettle();
    uint32_t seen = BlockFailSeen();
    BlockFailDisarm();
    return seen;
}

/* A stitch naming two fresh blocks, its check named cheaper than the last
 * cycle: one flush, and no DATA_FLUSHED — the mount reads both blocks back.
 * Named dearer, or with no pace known: two flushes, DATA_FLUSHED. A stitch
 * naming none: one flush, DATA_FLUSHED — nothing to check. */
static TestResult TestStandTollBothWays(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    MountBookSaid   said;
    error_t         rc = OK;
    uint64_t        fid = 0, flags = 0, fresh = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, 4u * TAGFS_BLOCK_SIZE), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("toll.bin", NULL, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_OK(TagFSSettle(), "and its Born settled: the last cycle's D is measured");
    BlockFailReadPace(true, STAND_TOLL_FAST, 0);
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 2u * TAGFS_BLOCK_SIZE, 61), 2 * TAGFS_BLOCK_SIZE, "two blocks written");
    TEST_ASSERT_EQ(StandTollFlushes(s.seat, &rc), 1, "a check cheaper than D: the cycle pays the one flush after "
                   "its stitch");
    TEST_ASSERT_OK(rc, "and settles");
    TEST_ASSERT(StandFreshLastStitch(&flags, &fresh) && !(flags & STITCH_DATA_FLUSHED) && fresh == 2,
                "its stitch does not say DATA_FLUSHED, and its Clasp names the two fresh blocks");
    BlockFailReadPace(false, 0, 0);
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "mounted again, the file holds them");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.checked, 2, "the mount read both back and held them to their digests");
    TEST_ASSERT_EQ(tagfs_rename_file(fid, "toll-named.bin"), 0, "the file is renamed");
    TEST_ASSERT_EQ(StandTollFlushes(s.seat, &rc), 1, "a stitch naming no fresh data pays the one flush");
    TEST_ASSERT(rc == OK && StandFreshLastStitch(&flags, &fresh) && (flags & STITCH_DATA_FLUSHED) && fresh == 0,
                "and says DATA_FLUSHED: there is nothing to check");
    BlockFailReadPace(true, 1, 1000000u);
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 62), TAGFS_BLOCK_SIZE, "block 0 is written over");
    TEST_ASSERT_EQ(StandTollFlushes(s.seat, &rc), 2, "a check dearer than D: the data's flush, then the stitch's");
    TEST_ASSERT(rc == OK && StandFreshLastStitch(&flags, &fresh) && (flags & STITCH_DATA_FLUSHED) && fresh == 1,
                "and the stitch says DATA_FLUSHED");
    BlockFailReadPace(true, 0, 0);
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 63), TAGFS_BLOCK_SIZE,
                   "block 1 is written over");
    TEST_ASSERT_EQ(StandTollFlushes(s.seat, &rc), 2, "no pace known: the cycle pays both flushes");
    TEST_ASSERT(rc == OK && StandFreshLastStitch(&flags, &fresh) && (flags & STITCH_DATA_FLUSHED),
                "and says DATA_FLUSHED");
out:
    BlockFailReadPace(false, 0, 0);
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/*
 * M-13 on a medium that seeks: a stitch the writer spares its flush has the
 * mount's check of it cost the medium no more than the D it was judged by, by
 * the stand's own clock. Every read of the seat waits the stand's seek
 * (BlockFailReadLag) and the writer judges the check by that seek, the pace
 * named (BlockFailReadPace) — a block's bytes nothing beside it: the medium's
 * own measure is the worst read of a period, which one stall of the host
 * lengthens past any cycle. The seek is the build's (SEEKLAG=), or two thirds
 * of the D the cycle before cost where that is less, so one seek and a block
 * fit in that D whatever the host made of the cycle — no cycle is run again
 * until one is long enough. The check is weighed by the read commands the
 * mount gives the block, a seek each, and the block's bytes at that pace; its
 * microseconds, the host's, are evidence.
 */
static TestResult TestStandTollCheckWithinD(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    MountBookSaid   said;
    char            name[32];
    bool            up = false;
    uint64_t        fid = 0, other = 0, flags = 0, fresh = 0, d = 0, seek = 0, block = 0, reads = 0, cost = 0;
#ifdef CONFIG_SEEKLAG_US
    uint32_t        lag = CONFIG_SEEKLAG_US ? CONFIG_SEEKLAG_US : STAND_TOLL_LAG_US;
#else
    uint32_t        lag = STAND_TOLL_LAG_US;
#endif
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, TAGFS_BLOCK_SIZE), "room for the file's bytes");
    BlockFailReadLag(s.seat, lag);
    TEST_ASSERT(StandStitchRemount(), "the volume mounts on a medium that seeks");
    TEST_ASSERT_EQ(tagfs_create_file("toll-seek.bin", NULL, 0, &fid), 0, "a file is made");
    for (uint32_t i = 0; i < STAND_TOLL_FILES; i++) {
        ksnprintf(name, sizeof(name), "toll-seek-%u.bin", i);
        TEST_ASSERT_EQ(tagfs_create_file(name, NULL, 0, &other), 0, "and many more with it");
    }
    TEST_ASSERT_OK(TagFSSettle(), "one cycle bears them all");
    d    = SettleLastDUs();
    seek = d * 2u / 3u < lag ? d * 2u / 3u : lag;
    TestEvidence("D of the cycle that bore them, us", d);
    TEST_ASSERT(seek > 0, "a cycle bearing many files costs a D a seek fits in");
    BlockFailReadLag(s.seat, (uint32_t)seek);
    BlockFailReadPace(true, STAND_TOLL_FAST, seek);
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 64), TAGFS_BLOCK_SIZE, "a block is written");
    TEST_ASSERT_OK(TagFSSettle(), "and settles");
    TEST_ASSERT(StandFreshLastStitch(&flags, &fresh) && !(flags & STITCH_DATA_FLUSHED) && fresh == 1,
                "spared its flush: one seek and one block cost less than that D");
    BlockFailReadPace(false, 0, 0);
    TEST_ASSERT(StandBlockOf(fid, 0, &block), "the block's ground read off its record");
    BlockFailArm(s.seat, BLOCK_FAIL_READ, s.base + block * TAGFS_BLOCK_SECTORS, TAGFS_BLOCK_SECTORS,
                 BLOCK_FAIL_NEVER);
    up    = StandStitchRemount();
    reads = BlockFailSeen();
    BlockFailDisarm();
    TEST_ASSERT(up && StandFreshHolds(&m, fid), "mounted again, the file holds the block");
    MountBookStandSaid(&said);
    TestEvidence("the mount's check of it, us", said.check_us);
    TEST_ASSERT_EQ(said.checked, 1, "the mount read back the one block the writer counted");
    cost = reads * seek + said.checked * TAGFS_BLOCK_SIZE * 1000000ull / STAND_TOLL_FAST;
    TestEvidence("read commands the mount's check gave the block, a seek each", reads);
    TEST_ASSERT(reads >= 1, "off the medium");
    if (cost > d) {
        verdict = TestFailedAt(__FILE__, __LINE__, "and the check cost the medium no more than the D the writer "
                               "judged it by, by the stand's clock: a seek a read and the block's bytes at the "
                               "pace (cost, D) (M-13)", (int64_t)cost, (int64_t)d);
        goto out;
    }
out:
    BlockFailReadPace(false, 0, 0);
    BlockFailDisarm();
    BlockFailReadLag(s.seat, STAND_TOLL_LAG_KEPT);
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/*
 * What the toll weighs besides the bytes. Each case a pace named so that one
 * term alone decides, and D taken from the cycle before as the toll takes it:
 *
 *   the seeks — a pace that reads a block in no time and seeks in exactly D:
 *     one run spared, two paid (runs > (D − read) / seek);
 *   D without the flush of its data — after a cycle that paid it, a pace
 *     whose read of one block takes that cycle's whole time on the medium, to
 *     the microsecond below: the toll's D is that time less the flush, so the
 *     check costs more and the flush is paid (§16.5 п.1);
 *   the halt's CLEAN stitch — the cheapest pace, and the flush paid all the
 *     same: nobody checks a stopping machine's last stitch before the next
 *     mount.
 */
static TestResult TestStandTollWeighs(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m = { 0 }, n = { 0 };
    error_t         rc = OK;
    uint64_t        fid = 0, other = 0, flags = 0, fresh = 0, d = 0, rate = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, TAGFS_BLOCK_SIZE) && StandFreshModelMake(&n, TAGFS_BLOCK_SIZE),
                "room for two files' bytes");
    TEST_ASSERT(tagfs_create_file("toll-a.bin", NULL, 0, &fid) == 0 &&
                tagfs_create_file("toll-b.bin", NULL, 0, &other) == 0, "two files are made");
    TEST_ASSERT_OK(TagFSSettle(), "and settled with no fresh data: the toll's D is the whole of that cycle");
    d = SettleLastDUs();
    TEST_ASSERT(d > 0, "a cycle takes the medium some time");
    BlockFailReadPace(true, STAND_TOLL_FAST, d);
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 71), TAGFS_BLOCK_SIZE, "one file's block written");
    TEST_ASSERT_EQ(StandTollFlushes(s.seat, &rc), 1, "one run, a seek of D: within D — spared");
    TEST_ASSERT(rc == OK && StandFreshLastStitch(&flags, &fresh) && !(flags & STITCH_DATA_FLUSHED) && fresh == 1,
                "and its stitch does not say DATA_FLUSHED");
    d = SettleLastDUs();
    BlockFailReadPace(true, STAND_TOLL_FAST, d);
    TEST_ASSERT(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 72) == TAGFS_BLOCK_SIZE &&
                StandFreshWrite(&n, other, 0, TAGFS_BLOCK_SIZE, 73) == TAGFS_BLOCK_SIZE, "both files' blocks written");
    TEST_ASSERT_EQ(StandTollFlushes(s.seat, &rc), 2, "two runs, a seek of D each: past D — the data's flush paid");
    TEST_ASSERT(rc == OK && StandFreshLastStitch(&flags, &fresh) && (flags & STITCH_DATA_FLUSHED) && fresh == 2,
                "and its stitch says DATA_FLUSHED");
    d = SettleLastDUs();
    TestEvidence("the whole of the cycle that paid its data's flush, us", d);
    rate = ((uint64_t)TAGFS_BLOCK_SIZE * 1000000ull + d - 1u) / d;
    BlockFailReadPace(true, rate, 0);
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 74), TAGFS_BLOCK_SIZE, "a block written");
    TEST_ASSERT_EQ(StandTollFlushes(s.seat, &rc), 2, "its read takes that cycle's whole time: past its D, which is "
                   "that time less the flush of its data — paid");
    TEST_ASSERT(rc == OK && StandFreshLastStitch(&flags, &fresh) && (flags & STITCH_DATA_FLUSHED),
                "and its stitch says DATA_FLUSHED");
    BlockFailReadPace(true, STAND_TOLL_FAST, 0);
    TEST_ASSERT_EQ(StandFreshWrite(&n, other, 0, TAGFS_BLOCK_SIZE, 75), TAGFS_BLOCK_SIZE, "a block written");
    TEST_ASSERT_OK(TagFSStandSettleForHalt(), "a stopping machine's last cycle settles it");
    TEST_ASSERT(StandFreshLastStitch(&flags, &fresh) && (flags & STITCH_CLEAN) && (flags & STITCH_DATA_FLUSHED) &&
                fresh == 1, "its CLEAN stitch paid the flush of its data however cheap the check");
out:
    BlockFailReadPace(false, 0, 0);
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    StandFreshModelLetGo(&n);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// The window of a frequent small writer: five seconds
// ----------------------------------------------------------------------------

/* The cadence let run for `us`: whether a cycle settled anything meanwhile. */
static bool StandTollCycled(uint64_t us)
{
    uint64_t settled = SettledGen();
    uint64_t until   = rdtsc() + cpu_us_to_tsc(us);
    while (rdtsc() < until && SettledGen() == settled)
        StandSettleTurn();
    return SettledGen() != settled;
}

/*
 * The file's making let fall out of what its first append is judged by. Ink
 * counts a write that grew a file as a small append when its data took less
 * than the last cycle's D (ink.c InkPlotLazy), so whether the making counts —
 * a block written, then a settle shorter than a Lull — rests on the D the test
 * before left: none after a mount (CadenceMountReset), else a D the block's
 * write may beat. Counted, the first append is already the second small one,
 * lazy, and waits for the very window its check is bounded by. So the first
 * append comes past the horizon a frequent writer is judged by: the window.
 */
static void StandTollPastFrequent(uint64_t fid)
{
    FileRecord* rec   = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    uint64_t    since = rec ? rec->small_append_us : 0;
    FileRecordRelease(rec);
    while (since && cpu_tsc_to_us(rdtsc()) - since <= SETTLE_WINDOW_US)
        cpu_pause();
}

/*
 * A file appended to in small pieces: the second within a Lull of the first
 * makes its writer a frequent small one. Its third comes past a Lull and well
 * inside the window: it is lazy still — no cycle begins for it before the
 * window ends. The ribbon is put back as the test found it: the file given up
 * still leaves its stitches past the Tally and its blocks fallow, which every
 * later mount — the next tests', the next boot's of a book on a stick — would
 * replay as its own.
 */
static TestResult TestStandTollWindowBook(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    uint64_t    fid = 0, lull = 0, size = 0;
    bool        let_go = false;
    const char* aside  = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5Au);
    TEST_ASSERT(StandMakeFile("toll-small-book", g_test_buffer, TAGFS_BLOCK_SIZE, &fid),
                "a file is made and written");
    TEST_ASSERT_OK(TagFSSettle(), "and settled: the last cycle's D is measured");
    lull = SettleLullUs();
    if (4u * lull >= SETTLE_WINDOW_US) {
        verdict = test_steps_aside("a Lull of this medium is not well inside the window");
        goto out;
    }
    StandTollPastFrequent(fid);
    SettleCadenceHold(false);
    let_go = true;
    size   = TAGFS_BLOCK_SIZE;
    TEST_ASSERT_EQ(StandWriteAt(fid, size, g_test_buffer, 10), 10, "ten bytes are appended");
    TEST_ASSERT(StandTollCycled(SETTLE_WINDOW_US), "and settled by the cadence");
    size += 10u;
    TEST_ASSERT_EQ(StandWriteAt(fid, size, g_test_buffer, 10), 10, "ten more at once: a frequent small writer");
    size += 10u;
    TEST_ASSERT(!StandTollCycled(2u * lull), "whose change waits past a Lull");
    TEST_ASSERT_EQ(StandWriteAt(fid, size, g_test_buffer, 10), 10, "ten more, past a Lull since the last");
    TEST_ASSERT(!StandTollCycled(2u * lull), "its writer is lazy still: the window, not a Lull");
out:
    if (let_go)
        SettleCadenceHold(true);
    if (fid)
        (void)tagfs_delete_file(fid, INK_UNWARRANTED);
    (void)TagFSSettle();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static const TestCase g_stand_toll_tests[] = {
    { .name = "stand_toll_both_ways", .func = TestStandTollBothWays,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_toll_weighs", .func = TestStandTollWeighs,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_toll_check_within_d", .func = TestStandTollCheckWithinD,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_toll_window_book", .func = TestStandTollWindowBook,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_toll_suite = {
    g_stand_toll_tests, sizeof(g_stand_toll_tests) / sizeof(g_stand_toll_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
