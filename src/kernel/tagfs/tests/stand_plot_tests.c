#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../plot/plot.h"
#include "../file_map/file_map.h"    /* the file's record, its blocks counted in it */
#include "../helm/helm.h"            /* a span's and a defrag's windows, one call each */
#include "../ink/ink.h"              /* InkedGen — one Ink a window */
#include "../run/run.h"              /* TagFSRunBlocks, and a previous owner's bytes laid */
#include "../block_map/block_map.h"  /* the ground a file keeps in hand, and a block taken between runs */
#include "write_job.h"               /* a span and a defrag as a program asks them */
#include "boxos_decks.h"              /* the deck's span and its flag */
#include "atomics.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// Plot — where a write may begin, what a gap reads as, how a file's extents
// grow, and span and defrag
//
// Each test makes its own file, works it through the handle API and span as a
// program's request would, and deletes it: the runner's residue guard counts
// the volume before and after, and the next boot's roll call judges the map.
// ============================================================================

#define STAND_PLOT_RUN_BYTES     (3u << 20)   /* the file written 4 KiB at a time */
#define STAND_PLOT_GAP_BLOCKS    4u           /* the ground a span takes, a previous owner's bytes on it */
#define STAND_PLOT_KEPT_FILE     8u           /* the blocks of a file whose next growth keeps ground in hand */
#define STAND_PLOT_SPAN_TAIL     17u          /* bytes past a block a span ends on */
#define STAND_PLOT_SPAN_WINDOWS  6u           /* the whole windows a span grows by, its tail after them */
#define STAND_PLOT_SCATTER_RUNS  3u           /* the runs a file to gather lies in, a window each */
#define STAND_PLOT_JOB_BOUND_MS  20000u       /* the test's own bound on a job it pumps by hand */

/* The stand's record of a job's answer: static, since a job still running when
 * a test gives up must not write into a frame that is gone. */
static WriteJobStandAnswer s_plot_answer;

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
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

/*
 * A gap reads as zeros, never as the bytes the ground's previous owner left.
 * The ground the span grows onto must hold such bytes for certain, or the test
 * proves nothing (T-A-3): a deleted file's ground rests in Fallow, out of every
 * take, until cycles the runner holds back give it back, so a span after a
 * delete took clean ground and a span that laid no zeros passed. So the ground
 * is the file's own in hand: a file of eight blocks grown by one has its
 * Allotment take a window sized to it and keep the seven it did not use
 * (block_map.h), and the test lays a pattern on four of them, as a previous
 * owner would have left it. A span across three blocks and a tail takes its
 * four from the front of that window — checked off the record — and every
 * byte of the gap must read zero.
 */
static TestResult TestStandPlotGapReadsZeros(void)
{
    TestResult     verdict = TEST_PASS;
    uint64_t       file_id = 0;
    uint64_t       start   = 0;
    uint32_t       count   = 0, landed = 0;
    uint32_t       leaked  = 0;
    uint8_t*       data    = NULL;
    uint8_t*       back    = NULL;
    const uint32_t B       = TAGFS_BLOCK_SIZE;
    const uint32_t grown   = STAND_PLOT_KEPT_FILE + 1;
    const uint64_t span    = (uint64_t)(grown + STAND_PLOT_GAP_BLOCKS - 1) * B + STAND_PLOT_SPAN_TAIL;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    data = kmalloc((size_t)grown * B);
    back = kmalloc((size_t)span);
    TEST_ASSERT(data && back, "room for the file and for reading it back");
    TestFillAt(data, grown * B, 0x72, 0);
    TEST_ASSERT(StandMakeFile("test_file", data, STAND_PLOT_KEPT_FILE * B, &file_id),
                "a file of eight blocks is made");
    TEST_ASSERT_EQ(StandWriteAt(file_id, STAND_PLOT_KEPT_FILE * B, data + STAND_PLOT_KEPT_FILE * B, B),
                   (int)B, "and grown by one");
    TEST_ASSERT(AllotmentHeld(file_id, &start, &count) && count >= STAND_PLOT_GAP_BLOCKS,
                "its Allotment keeps ground in hand, four blocks of it at least");

    TestFillAt(g_test_buffer, STAND_PLOT_GAP_BLOCKS * B, 0x72, 1);
    TEST_ASSERT_OK(TagFSWriteRun(start, STAND_PLOT_GAP_BLOCKS, g_test_buffer, &landed),
                   "a previous owner's bytes are laid on the front of it");
    TEST_ASSERT_EQ(landed, STAND_PLOT_GAP_BLOCKS, "on all four blocks");

    TEST_ASSERT_OK(tagfs_span_file(file_id, span), "the file is spanned across four more blocks");
    for (uint32_t i = 0; i < STAND_PLOT_GAP_BLOCKS; i++) {
        uint64_t block = 0;
        TEST_ASSERT(StandBlockOf(file_id, grown + i, &block) && block == start + i,
                    "the span grew onto the ground the bytes lie on, from the front of the window");
    }

    memset(back, 0xA5, (size_t)span);
    TEST_ASSERT_EQ(TestFileRead(file_id, back, (uint32_t)span), (int)span,
                   "it reads as long as it was spanned");
    TEST_ASSERT_EQ(TestFirstDifference(back, data, grown * B), grown * B,
                   "the nine blocks it had are as they were");
    leaked = StandPlotNonZero(back + (size_t)grown * B, (uint32_t)(span - (uint64_t)grown * B));
    TestEvidence("bytes of the gap that did not read zero", leaked);
    if (leaked)
        verdict = TEST_DEFECT;
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    kfree(data);
    kfree(back);
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
    uint64_t    file_id = 0;
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
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// span and defrag by windows (R24)
//
// A span grew a file in one piece: every block of the growth reserved before
// the first zero moved, and its holder's core held for all of it — a K-Core,
// and on one core the whole machine with interrupts off — while a span to past
// what the volume has took its free ground before it was refused; and a defrag
// copied the whole file in one piece the same way. Now each goes a window at a
// time, the holder free to give its core back between two, as a write does.
// ----------------------------------------------------------------------------

static uint64_t StandPlotBlocksFor(uint64_t bytes)
{
    return (bytes + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;
}

/* A span or a defrag of the file asked as a program's call asks it: answered
 * to the stand's record, the job giving its core back between two windows as
 * a program's does. The submit's answer. */
static int StandPlotJob(WriteJobKind kind, uint64_t file_id, uint64_t size)
{
    WriteJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    memset(&s_plot_answer, 0, sizeof(s_plot_answer));
    ask.kind    = kind;
    ask.file_id = file_id;
    ask.size    = size;
    ask.stand   = &s_plot_answer;
    return WriteJobSubmit(&ask, NULL, NULL);
}

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

/* The machine turned by hand until the job answered the stand's record — its
 * windows run at the service point here or on another core — or the bound. */
static bool StandPlotAnswered(void)
{
    uint8_t  seat  = tagfs_get_seat();
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_PLOT_JOB_BOUND_MS);
    while (!__atomic_load_n(&s_plot_answer.answered, __ATOMIC_ACQUIRE)) {
        if (rdtsc() >= bound)
            return false;
        StandPortTurn(seat);
        cpu_pause();
    }
    return true;
}

/* A file of STAND_PLOT_SCATTER_RUNS runs of `blocks` blocks each, its bytes
 * from `data`: after each run but the last, the block after the file's end is
 * taken by the test, so the next run cannot follow on. The blocks taken go in
 * walls[], counted in *out_walled, for the caller to give back. False when a
 * step did not answer; what it made is the caller's to give back then too. */
static bool StandPlotScatter(const uint8_t* data, uint32_t blocks, uint64_t* out_file_id,
                             uint64_t walls[STAND_PLOT_SCATTER_RUNS - 1], uint32_t* out_walled)
{
    const uint32_t bytes = blocks * TAGFS_BLOCK_SIZE;

    *out_walled = 0;
    if (!StandMakeFile("test_file", data, bytes, out_file_id))
        return false;
    for (uint32_t r = 1; r < STAND_PLOT_SCATTER_RUNS; r++) {
        uint64_t last = 0;
        if (!StandBlockOf(*out_file_id, r * blocks - 1, &last) ||
            BlockMapTake(1, last + 1, &walls[r - 1], __func__) != OK)
            return false;
        (*out_walled)++;
        if (StandWriteAt(*out_file_id, (uint64_t)r * bytes, data + (size_t)r * bytes, bytes) !=
            (int)bytes)
            return false;
    }
    return true;
}

static void StandPlotWallsDown(const uint64_t* walls, uint32_t walled)
{
    for (uint32_t i = 0; i < walled; i++)
        (void)tagfs_free_blocks(walls[i], 1);
}

/*
 * A span grows its file a window at a time. The test holds the Helm as a span
 * does and asks for the span's windows one call at a time: a span of six
 * windows and a tail is seven calls, each growing the file by no more than a
 * window and inking it, and after each the volume holds for the file exactly
 * the blocks its size needs — nothing staked past them, so the span never
 * holds more ground unnamed than a write's window does.
 */
static TestResult TestStandPlotSpanByWindows(void)
{
    TestResult       verdict  = TEST_PASS;
    uint64_t         file_id  = 0;
    TagFSFileHandle* handle   = NULL;
    HelmWaiter       here;
    bool             holding  = false;
    bool             ended    = false;
    uint8_t*         back     = NULL;
    uint32_t         turns    = 0;
    uint32_t         past     = 0;
    uint64_t         most     = 0;
    uint64_t         found    = 0;
    error_t          rc       = OK;
    VolumeTally      base;
    VolumeTally      now;
    const uint64_t   W        = HelmWindowBytes(0);
    const uint64_t   size     = STAND_PLOT_SPAN_WINDOWS * W + STAND_PLOT_SPAN_TAIL;
    const char*      aside    = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    back = kmalloc((size_t)size);
    TEST_ASSERT(back, "room to read the file back");
    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0, "an empty file is created");
    TEST_ASSERT(VolumeTallyTake(&base), "the volume is counted");
    handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle, "a handle opens it to write");
    TEST_ASSERT_OK(HelmTakeNow(handle, &here), "and holds its Helm, as the span's job does");
    holding = true;

    uint64_t inked = InkedGen();
    while (!ended && turns <= STAND_PLOT_SPAN_WINDOWS + 1) {
        uint64_t at = handle->file_size;
        rc = HelmSpanWindow(handle, size, false, &found, &ended);
        turns++;
        if (rc != OK)
            break;
        if (handle->file_size - at > most)
            most = handle->file_size - at;
        TEST_ASSERT(VolumeTallyTake(&now), "the volume is counted after the window");
        if (now.held - base.held != StandPlotBlocksFor(handle->file_size))
            past++;
    }
    HelmLetGo(handle->ofe, &here);
    holding = false;
    TEST_ASSERT_OK(rc, "every window of the span is taken");
    TestEvidence("calls a span of six windows and a tail took, one window each", turns);
    TEST_ASSERT_EQ(turns, STAND_PLOT_SPAN_WINDOWS + 1,
                   "one window a call: the holder had its core back between two");
    TEST_ASSERT(most <= W, "and no window grew the file by more than a window");
    TEST_ASSERT_EQ(past, 0, "after each window the volume held for the file the blocks its size "
                            "needs, and nothing staked past them");
    TEST_ASSERT(InkedGen() - inked >= turns, "one Ink a window, each its own record");

    memset(back, 0xA5, (size_t)size);
    TEST_ASSERT_EQ(TestFileRead(file_id, back, (uint32_t)size), (int)size,
                   "the file reads as long as it was spanned");
    TEST_ASSERT_EQ(StandPlotNonZero(back, (uint32_t)size), 0, "and reads zero, every byte");
out:
    if (holding)
        HelmLetGo(handle->ofe, &here);
    tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    kfree(back);
    return verdict;
}

/*
 * A defrag copies its file a window at a time, and inks once. A file of three
 * runs of a window each, a block of the test's between them, is gathered with
 * the test holding the Helm as the defrag does, one call a window: the run it
 * gathers into is taken at the first, the file's length and no more; every
 * call but the last leaves the record naming the old runs; and the last inks
 * the file in one run, the old ground resting and the bytes where they belong.
 */
static TestResult TestStandPlotDefragByWindows(void)
{
    TestResult       verdict = TEST_PASS;
    uint64_t         file_id = 0;
    uint64_t         walls[STAND_PLOT_SCATTER_RUNS - 1];
    uint32_t         walled  = 0;
    TagFSFileHandle* handle  = NULL;
    HelmWaiter       here;
    HelmDefragging   d;
    bool             holding = false;
    bool             ended   = false;
    uint8_t*         data    = NULL;
    uint8_t*         back    = NULL;
    uint32_t         turns   = 0;
    uint32_t         early   = 0;
    uint64_t         staked  = 0;
    uint64_t         size    = 0, blocks = 0;
    uint32_t         extents = 0;
    uint64_t         first   = 0;
    error_t          rc      = OK;
    VolumeTally      base;
    VolumeTally      now;
    const uint32_t   R       = TagFSRunBlocks();
    const uint32_t   all     = STAND_PLOT_SCATTER_RUNS * R;
    const uint32_t   bytes   = all * TAGFS_BLOCK_SIZE;
    const char*      aside   = StandStepsAsideFor();

    memset(&d, 0, sizeof(d));
    if (aside)
        return test_steps_aside(aside);

    data = kmalloc(bytes);
    back = kmalloc(bytes);
    TEST_ASSERT(data && back, "room for the file and for reading it back");
    TestFillAt(data, bytes, 0x75, 0);
    TEST_ASSERT(StandPlotScatter(data, R, &file_id, walls, &walled),
                "a file of three runs of a window each is made");
    TEST_ASSERT(StandPlotShape(file_id, &size, &blocks, &extents) && extents > 1,
                "it lies in more than one run");
    TEST_ASSERT(StandBlockOf(file_id, 0, &first), "its first block is found");
    const uint32_t extents_before = extents;
    const uint64_t first_before   = first;
    TEST_ASSERT(VolumeTallyTake(&base), "the volume is counted");

    handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle, "a handle opens it to write");
    TEST_ASSERT_OK(HelmTakeNow(handle, &here), "and holds its Helm, as the defrag's job does");
    holding = true;
    while (!ended && turns <= all) {
        rc = HelmDefragWindow(handle, &d, &ended);
        turns++;
        if (rc != OK || ended)
            break;
        TEST_ASSERT(StandPlotShape(file_id, &size, &blocks, &extents) &&
                    StandBlockOf(file_id, 0, &first), "the record reads between two windows");
        if (extents != extents_before || first != first_before)
            early++;
        TEST_ASSERT(VolumeTallyTake(&now), "the volume is counted between two windows");
        if (now.held - base.held > staked)
            staked = now.held - base.held;
    }
    HelmDefragLetGo(&d);
    HelmLetGo(handle->ofe, &here);
    holding = false;
    TEST_ASSERT_OK(rc, "every window of the defrag is taken");
    TestEvidence("calls a defrag of three windows' blocks took, one window each", turns);
    TEST_ASSERT(turns >= STAND_PLOT_SCATTER_RUNS,
                "a window of its copy a call: the holder had its core back between two");
    TEST_ASSERT_EQ(early, 0, "until the last window the record named the runs it had");
    TEST_ASSERT_EQ(staked, all, "and the run it gathers into was held, the file's length");

    TEST_ASSERT(StandPlotShape(file_id, &size, &blocks, &extents) && extents == 1,
                "the file lies in one run");
    TEST_ASSERT(VolumeTallyTake(&now), "the volume is counted");
    TEST_ASSERT_EQ(now.held, base.held, "its old ground rests, and nothing is staked");
    TEST_ASSERT_EQ(TestFileRead(file_id, back, bytes), (int)bytes, "it reads back");
    TEST_ASSERT_EQ(TestFirstDifference(back, data, bytes), bytes, "whole, where it lies now");
out:
    HelmDefragLetGo(&d);
    if (holding)
        HelmLetGo(handle->ofe, &here);
    tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    StandPlotWallsDown(walls, walled);
    kfree(data);
    kfree(back);
    return verdict;
}

/*
 * A defrag whose file is given up between two of its windows (CR-9). A delete
 * takes no Helm, and a defrag of many windows is now long enough for one to
 * land in it: the next window finds the file given up and answers so, nothing
 * of it named, and the run it had taken goes back when it is let go of.
 */
static TestResult TestStandPlotDefragGivenUpBetweenWindows(void)
{
    TestResult       verdict = TEST_PASS;
    uint64_t         file_id = 0;
    uint64_t         walls[STAND_PLOT_SCATTER_RUNS - 1];
    uint32_t         walled  = 0;
    TagFSFileHandle* handle  = NULL;
    HelmWaiter       here;
    HelmDefragging   d;
    bool             holding = false;
    bool             ended   = false;
    uint8_t*         data    = NULL;
    VolumeTally      base;
    VolumeTally      now;
    const uint32_t   R       = TagFSRunBlocks();
    const char*      aside   = StandStepsAsideFor();

    memset(&d, 0, sizeof(d));
    if (aside)
        return test_steps_aside(aside);

    data = kmalloc((size_t)STAND_PLOT_SCATTER_RUNS * R * TAGFS_BLOCK_SIZE);
    TEST_ASSERT(data, "room for the file");
    TestFillAt(data, STAND_PLOT_SCATTER_RUNS * R * TAGFS_BLOCK_SIZE, 0x76, 0);
    TEST_ASSERT(VolumeTallyTake(&base), "the volume is counted");
    TEST_ASSERT(StandPlotScatter(data, R, &file_id, walls, &walled),
                "a file of three runs of a window each is made");

    handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle, "a handle opens it to write");
    TEST_ASSERT_OK(HelmTakeNow(handle, &here), "and holds its Helm, as the defrag's job does");
    holding = true;
    TEST_ASSERT_OK(HelmDefragWindow(handle, &d, &ended), "the defrag's first window is taken");
    TEST_ASSERT(!ended, "and more are left");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id, INK_UNWARRANTED), 0,
                   "the file is given up between two windows, waiting for nothing");
    file_id = 0;
    TEST_ASSERT_EQ(HelmDefragWindow(handle, &d, &ended), ERR_FILE_NOT_FOUND,
                   "the next window finds it given up and says so");
    HelmDefragLetGo(&d);
    HelmLetGo(handle->ofe, &here);
    holding = false;
    StandPlotWallsDown(walls, walled);
    walled = 0;
    TEST_ASSERT(VolumeTallyTake(&now), "the volume is counted again");
    TEST_ASSERT_EQ(now.held, base.held,
                   "the run the defrag had taken went back, and the file's ground rests");
out:
    HelmDefragLetGo(&d);
    if (holding)
        HelmLetGo(handle->ofe, &here);
    tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    StandPlotWallsDown(walls, walled);
    kfree(data);
    return verdict;
}

/*
 * A span and a defrag asked as a program asks them give their core back. The
 * job runs its first window in its caller's call and then lists itself for a
 * service point, its answer to come: the call answers ERR_WOULD_BLOCK where a
 * job in one piece answered the work's own end. The rest runs where the
 * machine turns, and the answer is the span's size and the defrag's one run.
 */
static TestResult TestStandPlotJobsGiveTheCoreBack(void)
{
    TestResult     verdict = TEST_PASS;
    uint64_t       spanned = 0, file_id = 0;
    uint64_t       walls[STAND_PLOT_SCATTER_RUNS - 1];
    uint32_t       walled  = 0;
    bool           pending = false;
    uint8_t*       data    = NULL;
    uint8_t*       back    = NULL;
    uint64_t       size    = 0, blocks = 0;
    uint32_t       extents = 0;
    const uint32_t R       = TagFSRunBlocks();
    const uint32_t bytes   = STAND_PLOT_SCATTER_RUNS * R * TAGFS_BLOCK_SIZE;
    const uint64_t span    = 3 * HelmWindowBytes(0) + STAND_PLOT_SPAN_TAIL;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &spanned), 0, "an empty file is created");
    uint64_t inked = InkedGen();
    int      asked = StandPlotJob(WRITE_JOB_SPAN, spanned, span);
    pending        = asked == ERR_WOULD_BLOCK;
    TEST_ASSERT_EQ(asked, ERR_WOULD_BLOCK,
                   "a span of three windows and a tail gave its core back after its first window");
    TEST_ASSERT(StandPlotAnswered(), "and is answered where the machine turns");
    pending = false;
    TEST_ASSERT_OK(s_plot_answer.rc, "OK");
    TEST_ASSERT_EQ(s_plot_answer.size, span, "the file as long as it was spanned");
    TEST_ASSERT(InkedGen() - inked >= 4, "one Ink a window");

    data = kmalloc(bytes);
    back = kmalloc(bytes);
    TEST_ASSERT(data && back, "room for a second file and for reading it back");
    TestFillAt(data, bytes, 0x77, 0);
    TEST_ASSERT(StandPlotScatter(data, R, &file_id, walls, &walled),
                "a file of three runs of a window each is made");
    asked   = StandPlotJob(WRITE_JOB_DEFRAG, file_id, 0);
    pending = asked == ERR_WOULD_BLOCK;
    TEST_ASSERT_EQ(asked, ERR_WOULD_BLOCK, "its defrag gave its core back after its first window");
    TEST_ASSERT(StandPlotAnswered(), "and is answered where the machine turns");
    pending = false;
    TEST_ASSERT_OK(s_plot_answer.rc, "OK");
    TEST_ASSERT(StandPlotShape(file_id, &size, &blocks, &extents) && extents == 1,
                "the file lies in one run");
    TEST_ASSERT_EQ(TestFileRead(file_id, back, bytes), (int)bytes, "it reads back");
    TEST_ASSERT_EQ(TestFirstDifference(back, data, bytes), bytes, "whole, where it lies now");
out:
    if (pending && !StandPlotAnswered()) {
        spanned = 0;      /* the job still holds a file's Helm: both are left to it */
        file_id = 0;
        data    = NULL;
    }
    if (spanned)
        tagfs_delete_file(spanned, INK_UNWARRANTED);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    StandPlotWallsDown(walls, walled);
    kfree(data);
    kfree(back);
    return verdict;
}

/*
 * Z07-5. A span past every block the volume could give is refused before it
 * takes one. It took the volume's free ground first — every other writer told
 * there was none meanwhile — and gave it back on its refusal; grown a window at
 * a time, it would have filled the volume with zeros before the refusal came.
 */
static TestResult TestStandPlotSpanPastTheVolume(void)
{
    TestResult     verdict = TEST_PASS;
    uint64_t       file_id = 0;
    uint64_t       size    = 0, blocks = 0;
    uint64_t       took    = 0;
    VolumeTally    base;
    VolumeTally    now;
    const uint32_t B       = TAGFS_BLOCK_SIZE;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, B, 0x78, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, B, &file_id), "a one-block file is made");
    TEST_ASSERT(VolumeTallyTake(&base), "the volume is counted");
    uint64_t past = ((uint64_t)tagfs_get_state()->layout.data_blocks + 1) * B;
    TEST_ASSERT_EQ(tagfs_span_file(file_id, past), ERR_TAGFS_NO_SPACE,
                   "a span past every block of the volume is refused as no room");
    TEST_ASSERT(VolumeTallyTake(&now), "the volume is counted again");
    took = now.held - base.held;
    TestEvidence("blocks the refused span left the file", took);
    if (took) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT(StandPlotShape(file_id, &size, &blocks, NULL) && size == B && blocks == 1,
                "the file is as long as it was");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 2 * B), (int)B, "it reads back");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, B), B, "as it was");
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
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
    { .name  = "stand_plot_gap_reads_zeros", .func = TestStandPlotGapReadsZeros,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a gap in a file reads as the bytes its ground's previous owner left",
      .format = DEED_FORMAT_V1, .analog = "stand_slip_cut_over_hole" },
    { .name  = "stand_plot_span_grows_and_shrinks", .func = TestStandPlotSpanGrowsAndShrinks,
      .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_slip_cut_over_data" },
    { .name  = "stand_plot_span_by_windows", .func = TestStandPlotSpanByWindows,
      .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_fresh_ground_big_write" },
    { .name  = "stand_plot_defrag_by_windows", .func = TestStandPlotDefragByWindows,
      .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_fresh_ground_appends_keep_one_run" },
    { .name  = "stand_plot_defrag_given_up_between_windows",
      .func  = TestStandPlotDefragGivenUpBetweenWindows, .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_fresh_change_given_up_between" },
    { .name  = "stand_plot_jobs_give_the_core_back", .func = TestStandPlotJobsGiveTheCoreBack,
      .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_fresh_ground_big_write" },
    { .name  = "stand_plot_span_past_the_volume", .func = TestStandPlotSpanPastTheVolume,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a span past every block the volume could give took the volume's free "
                       "ground before it was refused",
      .format = DEED_FORMAT_V1, .analog = "stand_fresh_ground_fill_and_empty" },
    { .name  = "stand_plot_span_grows_only", .func = TestStandPlotSpanGrowsOnly,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a write past the end through <cstdio> or <fstream> spans the file to "
                       "where it begins, and cuts off what another writer grew it by meanwhile" },
};

const TestSuite g_stand_plot_suite = {
    .tests = g_stand_plot_tests, .count = ARRAY_SIZE(g_stand_plot_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
