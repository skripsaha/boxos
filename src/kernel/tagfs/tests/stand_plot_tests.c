#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../plot/plot.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// Plot — where a write may begin, what a gap reads as, how a file's extents
// grow, and span
//
// Each test makes its own file, works it through the handle API and span as a
// program's request would, and deletes it: the runner's residue guard counts
// the volume before and after, and the next boot's roll call judges the map.
// ============================================================================

#define STAND_PLOT_RUN_BYTES  (3u << 20)   /* the file written 4 KiB at a time */
#define STAND_PLOT_GAP_BLOCKS 4u           /* the ground a deleted file leaves, and a span takes */
#define STAND_PLOT_SPAN_TAIL  17u          /* bytes past a block a span ends on */

/* One write of `bytes` of the test buffer at `offset`, through a fresh handle:
 * the write's answer, and in *out_wrote the bytes it counted. */
static error_t StandPlotWriteAt(uint32_t file_id, uint64_t offset, uint32_t bytes,
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
static bool StandPlotShape(uint32_t file_id, uint64_t* out_size, uint64_t* out_blocks,
                           uint32_t* out_extents)
{
    TagFSMetadata meta;

    memset(&meta, 0, sizeof(meta));
    if (tagfs_get_metadata(file_id, &meta) != 0)
        return false;
    uint64_t blocks = 0;
    for (uint32_t e = 0; e < meta.extent_count && meta.extents; e++)
        blocks += meta.extents[e].block_count;
    *out_size   = meta.size;
    *out_blocks = blocks;
    if (out_extents)
        *out_extents = meta.extent_count;
    tagfs_metadata_free(&meta);
    return true;
}

/* How many of `n` bytes are not zero. */
static uint32_t StandPlotNonZero(const uint8_t* bytes, uint32_t n)
{
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n; i++)
        seen += bytes[i] != 0;
    return seen;
}

/*
 * W-OOB. A write that begins past a file's size is refused and changes
 * nothing: no block is taken for it and no byte of the file moves. Asked one
 * byte past the end, and 0xFFFF blocks past it — the gap an extent's count
 * could not say, where a write once went into whatever block of the volume
 * the cut count pointed at. A write at the size itself still grows the file.
 */
static TestResult TestStandPlotBeyondSizeRefused(void)
{
    static const uint64_t past[2] = {
        TAGFS_BLOCK_SIZE + 1u,
        TAGFS_BLOCK_SIZE + (uint64_t)PLOT_RUN_MOST * TAGFS_BLOCK_SIZE,
    };

    TestResult  verdict = TEST_PASS;
    uint32_t    file_id = 0;
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
        tagfs_delete_file(file_id);
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
    uint32_t         file_id  = 0;
    TagFSFileHandle* handle   = NULL;
    uint64_t         accepted = 0;
    uint64_t         size     = 0;
    uint64_t         blocks   = 0;
    uint32_t         extents  = 0;
    const char*      aside    = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0, "a file is created");
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
        tagfs_delete_file(file_id);
    return verdict;
}

/*
 * A gap reads as zeros, never as the bytes the ground's previous owner left.
 * A file of four blocks of a pattern is deleted, so its ground is free again
 * still holding the pattern; a second file is spanned across three blocks and
 * a tail — four blocks, the best fit for that ground — and every byte of it
 * must read zero, whatever ground it was given.
 */
static TestResult TestStandPlotGapReadsZeros(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    first   = 0;
    uint32_t    file_id = 0;
    uint32_t    leaked  = 0;
    uint64_t    span    = (STAND_PLOT_GAP_BLOCKS - 1) * TAGFS_BLOCK_SIZE + STAND_PLOT_SPAN_TAIL;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, STAND_PLOT_GAP_BLOCKS * TAGFS_BLOCK_SIZE, 0x72, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, STAND_PLOT_GAP_BLOCKS * TAGFS_BLOCK_SIZE,
                              &first),
                "a file of four blocks of a pattern is made");
    TEST_ASSERT_EQ(tagfs_delete_file(first), 0, "and deleted, its ground free again");
    first = 0;

    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0, "a second file is created");
    TEST_ASSERT_OK(tagfs_span_file(file_id, span), "and spanned across four blocks");

    memset(g_test_output, 0xA5, STAND_PLOT_GAP_BLOCKS * TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, STAND_PLOT_GAP_BLOCKS * TAGFS_BLOCK_SIZE),
                   span, "it reads as long as it was spanned");
    leaked = StandPlotNonZero(g_test_output, (uint32_t)span);
    TestEvidence("bytes of the gap that did not read zero", leaked);
    if (leaked)
        verdict = TEST_DEFECT;
out:
    if (first)
        tagfs_delete_file(first);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

/*
 * span, both ways. Two blocks of a pattern cut to 100 bytes give one block
 * back; spanned out again to three blocks and a tail, the file keeps its 100
 * bytes and reads zero after them — the old bytes past 100 in its first block
 * included, which a cut left where they lay; spanned to nothing it names no
 * block. Each step counted on the volume.
 */
static TestResult TestStandPlotSpanGrowsAndShrinks(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    file_id = 0;
    uint64_t    size    = 0;
    uint64_t    blocks  = 0;
    uint64_t    grown   = 3u * TAGFS_BLOCK_SIZE + STAND_PLOT_SPAN_TAIL;
    VolumeTally base;
    VolumeTally now;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    memset(&base, 0, sizeof(base));
    memset(&now, 0, sizeof(now));
    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0, "a file is created");
    TEST_ASSERT(VolumeTallyTake(&base), "the volume is counted with the file empty");
    TestFillAt(g_test_buffer, 2 * TAGFS_BLOCK_SIZE, 0x73, 0);
    TEST_ASSERT_EQ(TestFileWrite(file_id, g_test_buffer, 2 * TAGFS_BLOCK_SIZE), 2 * TAGFS_BLOCK_SIZE,
                   "two blocks of a pattern are written");

    TEST_ASSERT_OK(tagfs_span_file(file_id, 100), "the file is spanned to 100 bytes");
    TEST_ASSERT(StandPlotShape(file_id, &size, &blocks, NULL), "its record reads");
    TEST_ASSERT_EQ(size, 100, "it is 100 bytes long");
    TEST_ASSERT_EQ(blocks, 1, "in one block");
    TEST_ASSERT(VolumeTallyTake(&now), "the volume is counted");
    TEST_ASSERT_EQ(now.held - base.held, 1, "and the block it cut is given back");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 2 * TAGFS_BLOCK_SIZE), 100,
                   "it reads 100 bytes");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, 100), 100,
                   "the pattern's first 100");

    TEST_ASSERT_OK(tagfs_span_file(file_id, grown), "the file is spanned to three blocks and a tail");
    TEST_ASSERT(StandPlotShape(file_id, &size, &blocks, NULL), "its record reads");
    TEST_ASSERT_EQ(size, grown, "it is as long as it was spanned");
    TEST_ASSERT_EQ(blocks, 4, "in four blocks");
    TEST_ASSERT(VolumeTallyTake(&now), "the volume is counted");
    TEST_ASSERT_EQ(now.held - base.held, 4, "and holds the four");
    memset(g_test_output, 0xA5, 4 * TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 4 * TAGFS_BLOCK_SIZE), grown,
                   "it reads as long as it was spanned");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, 100), 100,
                   "its first 100 bytes are the pattern's still");
    TEST_ASSERT_EQ(StandPlotNonZero(g_test_output + 100, (uint32_t)(grown - 100)), 0,
                   "and every byte after them reads zero");

    TEST_ASSERT_OK(tagfs_span_file(file_id, 0), "the file is spanned to nothing");
    TEST_ASSERT(StandPlotShape(file_id, &size, &blocks, NULL), "its record reads");
    TEST_ASSERT_EQ(size, 0, "it is empty");
    TEST_ASSERT_EQ(blocks, 0, "and names no block");
    TEST_ASSERT(VolumeTallyTake(&now), "the volume is counted");
    TEST_ASSERT_EQ(now.held, base.held, "every block it held is given back");
out:
    if (file_id)
        tagfs_delete_file(file_id);
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
    { .name  = "stand_plot_gap_reads_zeros", .func = TestStandPlotGapReadsZeros,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a gap in a file reads as the bytes its ground's previous owner left" },
    { .name  = "stand_plot_span_grows_and_shrinks", .func = TestStandPlotSpanGrowsAndShrinks,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_plot_suite = {
    .tests = g_stand_plot_tests, .count = ARRAY_SIZE(g_stand_plot_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
