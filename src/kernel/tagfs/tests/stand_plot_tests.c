#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../plot/plot.h"
#include "../file_map/file_map.h"    /* the file's record, its blocks counted in it */
#include "../ink/ink.h"              /* InkedGen — one Ink a window */
#include "boxos_decks.h"              /* the deck's span and its flag */

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// Plot — where a write may begin, how a file's extents grow, and a span
//
// Each test makes its own file, works it through the handle API and span as a
// program's request would, and deletes it: the runner's residue guard counts
// the volume before and after, and the next boot's roll call judges the map.
// ============================================================================

#define STAND_PLOT_RUN_BYTES     (3u << 20)   /* the file written 4 KiB at a time */

/* One write of `bytes` of the test buffer at `offset`, through a fresh handle:
 * the write's answer, and in *out_wrote the bytes it counted. */
static error_t StandPlotWriteAt(uint64_t file_id, uint64_t offset, uint32_t bytes,
                                uint64_t* out_wrote)
{
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    PlotSource       source;

    *out_wrote = 0;
    if (!handle)
        return ERR_FILE_NOT_FOUND;
    memset(&source, 0, sizeof(source));
    source.kernel  = g_test_buffer;
    source.bytes   = bytes;
    handle->offset = offset;
    error_t rc = tagfs_write(handle, &source, out_wrote);
    tagfs_close(handle);
    return rc;
}

/* The file's size and the blocks its record names. */
static bool StandPlotShape(uint64_t file_id, uint64_t* out_size, uint64_t* out_blocks,
                           uint32_t* out_extents)
{
    if (!tagfs_enter())
        return false;
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, file_id, NULL);
    if (rec) {
        *out_size   = rec->size;
        *out_blocks = rec->blocks;
        if (out_extents)
            *out_extents = (uint32_t)rec->strip_count;
    }
    bool held = rec != NULL;
    FileRecordRelease(rec);
    tagfs_leave();
    return held;
}

/*
 * W-OOB. A write that begins past a file's size is refused and changes
 * nothing: no block is taken for it and no byte of the file moves. Asked one
 * byte past the end, and 0xFFFF blocks past it, where a write once went into
 * whatever block of the volume a cut count pointed at. A write at the size itself still grows the file.
 */
static TestResult TestStandPlotBeyondSizeRefused(void)
{
    static const uint64_t past[2] = {
        TAGFS_BLOCK_SIZE + 1u,
        TAGFS_BLOCK_SIZE + 0xFFFFull * TAGFS_BLOCK_SIZE,
    };

    TestResult  verdict = TEST_PASS;
    uint64_t    file_id = 0;
    uint32_t    taken   = 0;
    VolumeTally before;
    VolumeTally after;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x70, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");
    TEST_ASSERT(VolumeTallyTake(&before), "the volume is counted");

    for (uint32_t i = 0; i < ARRAY_SIZE(past); i++) {
        uint64_t wrote = 0;
        error_t  rc    = StandPlotWriteAt(file_id, past[i], 1, &wrote);
        if (rc == OK || wrote != 0) {
            taken++;
            continue;
        }
        TEST_ASSERT_EQ(rc, ERR_TAGFS_BEYOND_SIZE, "a write that begins past the size is "
                                                  "refused as beginning beyond it");
    }
    TestEvidence("writes that began past the size and were taken", taken);
    if (taken) {
        verdict = TEST_DEFECT;
        goto out;
    }

    TEST_ASSERT(VolumeTallyTake(&after), "the volume is counted again");
    TEST_ASSERT_EQ(after.held, before.held, "and not a block was taken for either write");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 2 * TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "the file is as long as it was");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "and holds what it held");
    TEST_ASSERT_EQ(StandWriteAt(file_id, TAGFS_BLOCK_SIZE, g_test_buffer, 10), 10,
                   "a write that begins at the size grows the file");
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

/* The most extents a file grown one block at a time can have: its Allotment
 * windows, sized to the file each time it has none left — 1, 1, 2, 4, … — so
 * 1 + ceil(log2 blocks), and fewer wherever one window begins where the last
 * ended. */
static uint32_t StandPlotExtentCeiling(uint64_t blocks)
{
    uint32_t windows = 1;
    for (uint64_t covered = 1; covered < blocks; covered *= 2)
        windows++;
    return windows;
}

/*
 * T5. Three megabytes written 4 KiB at a time, one write after another, are
 * taken whole, and the file's runs merge: a record that took an extent for
 * every write refused its write at the 671st block, after 2748416 bytes.
 */
static TestResult TestStandPlotRunMerges(void)
{
    TestResult       verdict  = TEST_PASS;
    uint64_t         file_id  = 0;
    TagFSFileHandle* handle   = NULL;
    uint64_t         accepted = 0;
    uint64_t         size     = 0;
    uint64_t         blocks   = 0;
    uint32_t         extents  = 0;
    const char*      aside    = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, 0, &file_id), 0, "a file is created");
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle != NULL, "and opened");
    for (uint64_t at = 0; at < STAND_PLOT_RUN_BYTES; at += TAGFS_BLOCK_SIZE) {
        TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x71, at);
        if (TestHandleWrite(handle, g_test_buffer, TAGFS_BLOCK_SIZE) != (int)TAGFS_BLOCK_SIZE)
            break;
        accepted += TAGFS_BLOCK_SIZE;
    }
    tagfs_close(handle);
    handle = NULL;

    TEST_ASSERT(StandPlotShape(file_id, &size, &blocks, &extents), "the file's record reads");
    TestEvidence("extents the record names for the blocks written in a row", extents);
    if (accepted < STAND_PLOT_RUN_BYTES || extents > StandPlotExtentCeiling(blocks)) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(size, STAND_PLOT_RUN_BYTES, "the file is as long as what was written");
    TEST_ASSERT_EQ(blocks, STAND_PLOT_RUN_BYTES / TAGFS_BLOCK_SIZE,
                   "and names a block for every 4 KiB of it");

    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "it opens to be read");
    for (uint64_t at = 0; at < STAND_PLOT_RUN_BYTES; at += TAGFS_BLOCK_SIZE) {
        TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x71, at);
        TEST_ASSERT_EQ(TestHandleRead(handle, g_test_output, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                       "every block reads");
        TEST_ASSERT_EQ(at + TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE),
                       at + TAGFS_BLOCK_SIZE,
                       "every block holds its own bytes (first differing byte of the file)");
    }
out:
    if (handle)
        tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// span as a program asks it
// ----------------------------------------------------------------------------

/* The deck's span of the file to `size` with `flags`, its params as a
 * program's file_span lays them, asked by a caller in the kernel standing in
 * its call. The handler's answer. */
static int StandPlotDeckSpan(uint64_t file_id, uint64_t size, uint32_t flags)
{
    uint8_t params[20];
    memcpy(params, &file_id, 8);
    memcpy(params + 8, &size, 8);
    memcpy(params + 16, &flags, 4);
    return StandDeckAsk(STORAGE_OBJ_SPAN, params, sizeof(params), NULL, 0, NULL);
}

/*
 * B6, Z16-1. A write past the end through <cstdio> or <fstream> is refused
 * ERR_TAGFS_BEYOND_SIZE, and the library spans the file to where the write
 * begins and writes again. Between the refusal and the span another writer may
 * grow the file past that place; a span that made the file exactly that long
 * cut its bytes off, though it had been told they were the file's. The span it
 * asks now only grows (STORAGE_SPAN_GROW), decided under the Helm: a file
 * already longer is left as it is, and the write lays its bytes over the other
 * writer's and cuts nothing. Asked through the deck, its params as file_span
 * lays them, so the flag is the one a program sends.
 */
static TestResult TestStandPlotSpanGrowsOnly(void)
{
    TestResult     verdict = TEST_PASS;
    uint64_t       file_id = 0;
    uint64_t       wrote   = 0, size = 0, blocks = 0;
    uint64_t       cut     = 0;
    uint8_t*       expect  = NULL;
    uint8_t*       back    = NULL;
    const uint32_t made    = 100, at = 200, mine = 4, theirs = 300, grown = 1000;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    expect = kmalloc(grown);
    back   = kmalloc(grown);
    TEST_ASSERT(expect && back, "room for what the file should hold and for reading it back");
    memset(expect, 0, grown);
    TestFillAt(expect, made + theirs, 0x79, 0);
    TEST_ASSERT(StandMakeFile("test_file", expect, made, &file_id), "a file of 100 bytes is made");

    TestFillAt(g_test_buffer, mine, 0x7A, 0);
    TEST_ASSERT_EQ(StandPlotWriteAt(file_id, at, mine, &wrote), ERR_TAGFS_BEYOND_SIZE,
                   "one writer's write at 200 is refused as beginning past the size");
    TEST_ASSERT_EQ(StandWriteAt(file_id, made, expect + made, theirs), (int)theirs,
                   "another writer grows the file to 400 meanwhile");
    TEST_ASSERT_EQ(StandPlotDeckSpan(file_id, at, STORAGE_SPAN_GROW << 1), ERR_INVALID_ARGUMENT,
                   "the deck refuses a span flag it does not know");
    TEST_ASSERT_OK(StandPlotDeckSpan(file_id, at, STORAGE_SPAN_GROW),
                   "the first writer spans it to 200 through the deck, growing only");
    TEST_ASSERT(StandPlotShape(file_id, &size, &blocks, NULL), "the record reads");
    cut = size < made + theirs ? made + theirs - size : 0;
    TestEvidence("bytes another writer had added that the span cut off", cut);
    if (cut) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_OK(StandPlotWriteAt(file_id, at, mine, &wrote), "and writes its bytes at 200");
    memcpy(expect + at, g_test_buffer, mine);
    TEST_ASSERT_EQ(TestFileRead(file_id, back, grown), (int)(made + theirs),
                   "the file is 400 bytes still");
    TEST_ASSERT_EQ(TestFirstDifference(back, expect, made + theirs), made + theirs,
                   "the other writer's bytes are the file's, the first's laid over four of them");

    TEST_ASSERT_OK(StandPlotDeckSpan(file_id, grown, STORAGE_SPAN_GROW),
                   "a span to 1000, growing only, of a shorter file");
    TEST_ASSERT_EQ(TestFileRead(file_id, back, grown), (int)grown, "grows it to 1000 bytes");
    TEST_ASSERT_EQ(TestFirstDifference(back, expect, grown), grown, "zeros after the 400");
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    kfree(expect);
    kfree(back);
    return verdict;
}

static const TestCase g_stand_plot_tests[] = {
    { .name  = "stand_plot_beyond_size_refused", .func = TestStandPlotBeyondSizeRefused,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a write that began 0xFFFF blocks or more past a file's end went into a "
                       "block outside what it took, or answered OK having written nothing" },
    { .name  = "stand_plot_run_merges", .func = TestStandPlotRunMerges,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a file's record takes one extent per 4 KiB written and never merges "
                       "them, so a file written 4 KiB at a time is refused at about 2.6 MB" },
    { .name  = "stand_plot_span_grows_only", .func = TestStandPlotSpanGrowsOnly,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a write past the end through <cstdio> or <fstream> spans the file to "
                       "where it begins, and cuts off what another writer grew it by meanwhile" },
};

const TestSuite g_stand_plot_suite = {
    .tests = g_stand_plot_tests, .count = ARRAY_SIZE(g_stand_plot_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
