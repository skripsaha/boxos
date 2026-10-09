#include "tests.h"
#include "../tagfs.h"
#include "../plot/plot.h"
#include "../metadata_pool/meta_pool.h"
#include "../ink/fallow.h"
#include "../settle/settle.h"
#include "../integrity/integrity.h"
#include "boardroom.h"
#include "write_job.h"   /* WriteJobSeatRunsAsync — the road a program's write takes */
#include "amp.h"
#ifdef CONFIG_VOLUME_TESTS
#include "block_fail.h"
#include "stand_tests.h"
#endif

// ============================================================================
// The runner, the judge, and what the suites share. The tests themselves live
// in the suite files beside this one (TEST_SUITES in tests.h).
// ============================================================================

static bool g_tests_initialized = false;

uint8_t g_test_buffer[16384];
uint8_t g_test_output[16384];

/*
 * Whether this build is allowed to write to the volume it is testing.
 *
 * A build switch rather than a runtime guess: "is this medium somebody's" is
 * not a question a kernel can answer by looking at it, and guessing wrong in
 * the permissive direction costs somebody their files.
 */
#ifdef CONFIG_VOLUME_TESTS
static const bool g_volume_tests_allowed = true;
#else
static const bool g_volume_tests_allowed = false;
#endif

// ============================================================================
// What the test running now has said
//
// A test hands back one verdict and nothing else, so everything it has to say
// beside it travels here: why it stood down, whether an assertion already
// failed out loud, and the evidence a caveat needs. The runner clears all of it
// before every test, so nothing can outlive the test that said it.
// ============================================================================

static const char* g_current_test      = NULL;
static const char* g_step_aside_reason = NULL;
static bool        g_failure_said      = false;

static struct {
    bool     given;
    char     words[96];
    uint64_t n;
} g_evidence;

TestResult TestFailedAt(const char* file, int line, const char* what,
                        int64_t seen, int64_t wanted)
{
    kprintf("[TESTS] %s failed at %s:%d: %s (%lld, %lld)\n",
            g_current_test ? g_current_test : "(no test running)",
            file, line, what, (long long)seen, (long long)wanted);
    g_failure_said = true;
    return TEST_FAIL;
}

void TestEvidence(const char* words, uint64_t n)
{
    size_t len = words ? strlen(words) : 0;
    if (len >= sizeof(g_evidence.words))
        len = sizeof(g_evidence.words) - 1;
    memcpy(g_evidence.words, words, len);
    g_evidence.words[len] = '\0';
    g_evidence.n     = n;
    g_evidence.given = true;
}

/*
 * Why a test that RAN decided not to run.
 *
 * The reason used to be said into debug_printf, which is compiled away in the
 * build that goes on the board. The count then closed without the names: "19
 * skipped" against seventeen explained, and the other two unnameable in front
 * of the machine. The runner reads it back beside the verdict instead.
 */
TestResult test_steps_aside(const char* why)
{
    g_step_aside_reason = why;
    return TEST_SKIP;
}

// ============================================================================
// Helpers the suites share
// ============================================================================

void fill_random(void* buffer, uint32_t size)
{
    uint8_t* buf = (uint8_t*)buffer;
    for (uint32_t i = 0; i < size; i++)
        buf[i] = (uint8_t)(i ^ (i >> 3) ^ (i >> 7));
}

void fill_pattern(void* buffer, uint32_t size, uint8_t pattern)
{
    memset(buffer, pattern, size);
}

/* fill_random repeats itself every block, so two blocks of one file written
 * with it are indistinguishable and a block swap reads back as a pass. Here
 * every group of four bytes is mixed from its absolute offset and the stream,
 * so no two blocks — of one file or of two — hold the same bytes. */
void TestFillAt(void* buffer, uint32_t size, uint64_t stream, uint64_t offset)
{
    uint8_t* buf = (uint8_t*)buffer;
    for (uint32_t i = 0; i < size; i++) {
        uint64_t at = offset + i;
        uint64_t x  = ((at >> 2) + 1) * 0x9E3779B97F4A7C15ull;
        x ^= (stream + 1) * 0xC2B2AE3D27D4EB4Full;
        x ^= x >> 29;
        buf[i] = (uint8_t)(x >> ((at & 3) * 8));
    }
}

uint32_t TestFirstDifference(const void* a, const void* b, uint32_t n)
{
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (uint32_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return i;
    return n;
}

int TestHandleWrite(TagFSFileHandle* handle, const void* data, uint32_t bytes)
{
    PlotSource source;
    uint64_t   written = 0;

    memset(&source, 0, sizeof(source));
    source.kernel = (const uint8_t*)data;
    source.bytes  = bytes;
    return tagfs_write(handle, &source, &written) == OK ? (int)written : -1;
}

int TestHandleRead(TagFSFileHandle* handle, void* buffer, uint32_t capacity)
{
    uint64_t got = 0;
    return tagfs_read(handle, buffer, capacity, &got) == OK ? (int)got : -1;
}

int TestFileWrite(uint64_t file_id, const void* data, uint32_t bytes)
{
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    if (!handle)
        return -1;
    int written = TestHandleWrite(handle, data, bytes);
    tagfs_close(handle);
    return written;
}

int TestFileRead(uint64_t file_id, void* buffer, uint32_t capacity)
{
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    if (!handle)
        return -1;
    int got = TestHandleRead(handle, buffer, capacity);
    tagfs_close(handle);
    return got;
}

// ============================================================================
// ‼ WHAT A TEST THAT WRITES LEAVES BEHIND
//
// A test that writes is a guest on the volume, and it leaves the volume as it
// found it or it fails — by the runner's count, not by its own word. Taken
// before and after every such test:
//
//   files      the Ledger's file count;
//   held       blocks in use that are not the volume's own bookkeeping.
//
// Not the free count on its own, because the free count is not the test's
// alone to move. The metadata pool takes Spares and closes its tail onto them
// (metadata_pool/), and the file table and the registry chain theirs at a
// Settle cycle — all of it for good, and none of it residue — and what a file
// gave up rests in Fallow until the cycles after its record gave it back
// (ink/fallow.h). So the registry's and the table's chains are walked on the
// medium, the way the mount's roll call walks them; the pool's chain and its
// Spares are counted in memory, which holds the pool's newest links before any
// cycle wrote them; and they, the ground at rest, the integrity map and the
// snapshot manifest an earlier kernel may have laid down are taken out of the
// count. No test can take a snapshot (a format-v1 volume refuses one), so what
// is left is what files hold, and a test that leaves a block behind, or hands
// back one that was never its own, moves it.
//
// Not the tag count: a tag is interned for good (every created file interns
// the stem of its name), so the tests keep to names they have always used and
// the guard does not count tags.
// ============================================================================

static uint8_t  g_chain_block[TAGFS_BLOCK_SIZE];

/* Blocks on one metadata chain, walked on the medium from its head (a volume
 * block, as the Deed names it) by the magic every link carries. */
static uint32_t VolumeChainLength(uint32_t head_volume_block, uint32_t magic)
{
    TagFSState* fs = tagfs_get_state();
    uint32_t block = head_volume_block - fs->layout.data_block;
    uint32_t links = 0;

    while (links < fs->layout.data_blocks) {
        if (TagFSMetaBlockRead(block, g_chain_block) != OK)
            break;
        uint32_t says_magic, next;
        memcpy(&says_magic, g_chain_block, sizeof(says_magic));
        memcpy(&next, g_chain_block + sizeof(says_magic), sizeof(next));
        if (says_magic != magic)
            break;
        links++;
        if (next == 0)
            break;
        block = next;
    }
    return links;
}

bool VolumeTallyTake(VolumeTally* tally)
{
    TagFSState* fs = tagfs_get_state();
    if (!fs || !fs->initialized)
        return false;

    uint64_t chains =
        VolumeChainLength(fs->layout.tag_registry_block,  TAGFS_REGISTRY_MAGIC) +
        VolumeChainLength(fs->layout.file_table_block,    TAGFS_FILETBL_MAGIC);

    TagFSLockTake(&fs->lock);
    uint64_t used = fs->layout.data_blocks - fs->ledger.free_blocks;
    uint64_t own  = chains + PoolBlocksHeld() + FallowBlocks() + UseMemoryBlocksHeld() +
                    fs->ledger.integrity_map_blocks + IntegrityLayBlocks() +
                    (fs->ledger.cow_manifest_block != 0);
    tally->files = fs->ledger.total_files;
    TagFSLockDrop(&fs->lock);

    tally->held = used - own;
    return true;
}

/* True when the test left the volume other than it found it — every
 * difference said, with both numbers. */
static bool VolumeResidueSaid(const char* name, const VolumeTally* before)
{
    VolumeTally after;
    if (!VolumeTallyTake(&after)) {
        kprintf("[TESTS] %s failed: the volume could not be counted after it, "
                "so what it left behind is unknown\n", name);
        return true;
    }

    bool residue = false;
    if (after.files != before->files) {
        kprintf("[TESTS] %s failed: it left residue — the volume held %llu "
                "file(s) before it and %llu after\n", name,
                (unsigned long long)before->files,
                (unsigned long long)after.files);
        residue = true;
    }
    if (after.held != before->held) {
        kprintf("[TESTS] %s failed: it left residue — %llu block(s) in use "
                "outside the volume's own chains before it and %llu after\n",
                name, (unsigned long long)before->held,
                (unsigned long long)after.held);
        residue = true;
    }
    return residue;
}

// ============================================================================
// The judge
//
//   caveated  seen     verdict
//   yes       DEFECT   held — a known defect, reproduced on purpose
//   yes       PASS     FAIL: the caveat was lifted early
//   no        DEFECT   FAIL: the defect it stands for is back
//   either    FAIL     FAIL
//   no        PASS     PASS
//
// Plus three things no test gets to decide for itself: a caveated test that
// gave no evidence has proven nothing either way, a test that can see a defect
// but does not name it cannot say what came back, and a test whose assertion
// failed out loud has failed whatever it returned afterwards.
// ============================================================================

static void TestJudge(const TestCase* test, TestResult seen, TestStats* stats)
{
    bool caveated = test->caveat_step != NULL;

    if (seen == TEST_FAIL || g_failure_said) {
        stats->total_failed++;
        if (!g_failure_said)
            kprintf("[TESTS] %s failed and did not say where\n", test->name);
        else if (seen != TEST_FAIL)
            kprintf("[TESTS] %s failed: an assertion failed inside it and it "
                    "still ended as if it had not\n", test->name);
        return;
    }

    if (seen == TEST_SKIP) {
        stats->total_skipped++;
        kprintf("[TESTS] %s ran and stood down: %s\n", test->name,
                g_step_aside_reason
                    ? g_step_aside_reason
                    : "it gave no reason, which leaves this run covering less "
                      "than its own count admits");
        return;
    }

    if (seen != TEST_PASS && seen != TEST_DEFECT) {
        stats->total_failed++;
        kprintf("[TESTS] %s failed: it ended with %u, which is no verdict\n",
                test->name, (unsigned)seen);
        return;
    }

    if ((caveated || seen == TEST_DEFECT) && !g_evidence.given) {
        stats->total_failed++;
        kprintf("[TESTS] %s failed: it gave no evidence — a test that %s must "
                "say what it counted, and the count\n", test->name,
                caveated ? "holds a caveat" : "sees its defect");
        return;
    }

    if ((caveated || seen == TEST_DEFECT) && !test->caveat_defect) {
        stats->total_failed++;
        kprintf("[TESTS] %s failed: its TestCase names no defect — a test that "
                "can see one says what it stands for in caveat_defect, and keeps "
                "it after its caveat is lifted (%s %llu)\n", test->name,
                g_evidence.words, (unsigned long long)g_evidence.n);
        return;
    }

    if (caveated && seen == TEST_DEFECT) {
        stats->total_held++;
        kprintf("[TESTS] caveat held: %s — %s — discharged by step %s (%s %llu)\n",
                test->name, test->caveat_defect, test->caveat_step,
                g_evidence.words, (unsigned long long)g_evidence.n);
        return;
    }

    if (caveated) {
        stats->total_failed++;
        kprintf("[TESTS] caveat lifted early: %s — the test does not test what "
                "it claims, or step %s already landed — lift the caveat (%s %llu)\n",
                test->name, test->caveat_step, g_evidence.words,
                (unsigned long long)g_evidence.n);
        return;
    }

    if (seen == TEST_DEFECT) {
        stats->total_failed++;
        kprintf("[TESTS] %s failed: the defect it stands for is back — %s (%s %llu)\n",
                test->name, test->caveat_defect, g_evidence.words,
                (unsigned long long)g_evidence.n);
        return;
    }

    stats->total_passed++;
    debug_printf("[TESTS] %s passed\n", test->name);
}

// ============================================================================
// The runner
// ============================================================================

#define TEST_SUITE_WALKED(name) &g_##name##_suite,
static const TestSuite* const g_suites[] = { TEST_SUITES(TEST_SUITE_WALKED) };

/* Said once, before the first test that writes. A red test is only half a
 * report until it says which road its writes took: these write from the boot
 * core as a caller in the kernel, which stands in its call — all but the
 * stand_port suite's and stand_run's through the port, which take the road a
 * program's write takes on this seat (WriteJobSeatRunsAsync): through the
 * port, parked on each answer, on an AHCI port or a USB disk with more than
 * one core. */
static void TestSayWritePath(void)
{
    uint8_t seat = tagfs_get_seat();
    kprintf("[TESTS] the tests that write go through seat %u (%s), on the "
            "synchronous path — as a caller in the kernel, standing in its call; "
            "a program's write there, and the stand_port suite's and stand_run's "
            "through the port, goes %s, on %u core(s)\n",
            seat, BoardroomSeatName(seat),
            WriteJobSeatRunsAsync(seat) ? "through the port, parked on each answer"
                                        : "the synchronous path too",
            (unsigned)g_amp.total_cores);
}

/*
 * A fault left armed.
 *
 * BLOCKFAIL and HEAPFAIL strike whatever meets them next. A test that came
 * back with one still standing — out through an assertion, or by forgetting —
 * would have it strike the runner's own count of the volume, the next test,
 * or the rest of the boot. So both are taken down after every test, before
 * anything is counted, and a test that left one standing has failed.
 */
static bool TestLeftAFaultArmed(const TestCase* test)
{
#ifdef CONFIG_VOLUME_TESTS
    bool block   = BlockFailDisarm();
    bool heap    = HeapFailDisarm();
    bool protect = BlockFailProtects(tagfs_get_seat());
    if (protect)
        BlockFailProtect(tagfs_get_seat(), false);
    if (!block && !heap && !protect)
        return false;

    const char* left[3] = { "", "", "" };
    uint32_t    n = 0;
    if (block)
        left[n++] = "BLOCKFAIL";
    if (heap)
        left[n++] = "HEAPFAIL";
    if (protect)
        left[n++] = "a protected medium";
    kprintf("[TESTS] %s failed: it left %s%s%s%s%s armed — taken down by the runner "
            "before anything else could meet it\n", test->name, left[0],
            n == 3 ? ", " : n == 2 ? " and " : "", n > 1 ? left[1] : "",
            n == 3 ? " and " : "", n == 3 ? left[2] : "");
    return true;
#else
    (void)test;
    return false;
#endif
}

static void TestRun(const TestCase* test, TestStats* stats)
{
    bool        guarded = test->reach == TEST_WRITES_TO_VOLUME;
    VolumeTally before;

    if (guarded && !VolumeTallyTake(&before)) {
        stats->total_failed++;
        kprintf("[TESTS] %s was not run: the volume could not be counted before "
                "it, so nothing it left behind could have been seen\n", test->name);
        return;
    }

    g_current_test      = test->name;
    g_step_aside_reason = NULL;
    g_failure_said      = false;
    g_evidence.given    = false;

    debug_printf("[TESTS] running %s\n", test->name);
    TestResult seen = test->func();
    bool left_armed = TestLeftAFaultArmed(test);

    /* Residue first: a test that left the volume changed has failed, and
     * whatever it would otherwise have been judged — held included — is not
     * said, so a count of held lines never includes one that failed. */
    if (guarded && VolumeResidueSaid(test->name, &before))
        stats->total_failed++;
    else if (left_armed)
        stats->total_failed++;
    else
        TestJudge(test, seen, stats);

    g_current_test = NULL;
}

error_t TagFS_TestsInit(void)
{
    if (g_tests_initialized)
        return ERR_ALREADY_INITIALIZED;
    g_tests_initialized = true;
    return OK;
}

void TagFS_TestsShutdown(void)
{
    g_tests_initialized = false;
}

error_t TagFS_RunAllTests(TestStats* stats)
{
    if (!g_tests_initialized)
        return ERR_NOT_INITIALIZED;

    memset(stats, 0, sizeof(*stats));
    uint32_t skipped_writers = 0;
    bool     path_said       = false;

    /* No cycle begins on its own while the tests run: each meets the cycles
     * it drives, and a fault it arms is met by one of them (settle.h). */
    SettleCadenceHold(true);
    for (uint32_t s = 0; s < ARRAY_SIZE(g_suites); s++) {
        const TestSuite* suite = g_suites[s];
        for (uint32_t i = 0; i < suite->count; i++) {
            const TestCase* test = &suite->tests[i];
            stats->total_tests++;

            /*
             * A test that writes goes nowhere near a volume somebody is using.
             *
             * The mounted volume belongs to whoever switched the machine on,
             * and these tests create files, allocate blocks and write the
             * journal on it. That was found by reading the owner's flash drive
             * back: his own files with test_file, test_rw and stress_file_0..2
             * among them. Built with VOLUME_TESTS=on they run, which is what
             * the test images in QEMU are for; otherwise they are skipped and
             * said to have been skipped.
             */
            if (test->reach == TEST_WRITES_TO_VOLUME && !g_volume_tests_allowed) {
                skipped_writers++;
                stats->total_skipped++;
                continue;
            }
            if (test->reach == TEST_WRITES_TO_VOLUME && !path_said) {
                TestSayWritePath();
                path_said = true;
            }
            TestRun(test, stats);
        }
    }
    SettleCadenceHold(false);

    /* Said with kprintf: "37 run, 35 passed" tells nobody which 37, and a
     * suite that quietly shrank is how a test stops covering anything. */
    if (skipped_writers > 0) {
        kprintf("[TESTS] %u test(s) that write to the volume were not run — "
                "this volume belongs to whoever is using this machine (build "
                "with VOLUME_TESTS=on to run them)\n", skipped_writers);
    }
#ifdef CONFIG_VOLUME_TESTS
    if (path_said)
        StandSayChainsLeft();
#endif

    return stats->total_failed == 0 ? OK : ERR_INTERNAL;
}

/* The runner's door, called once at boot from main.c (CONFIG_RUN_STARTUP_TESTS),
 * before any program runs: the tests run inside the volume, and the ones that
 * take the whole volume's ground, issue the last file id, swap the well-known
 * tags, narrow a port, change the machine's Use Context or hold the cadence
 * are safe only because nobody else is using it yet. kprintf, not debug_printf:
 * without these numbers a run where every single test SKIPPED still printed
 * "All tests PASSED". A test held under a caveat is a known defect reproduced
 * on purpose — counted on its own, neither passed nor failed nor skipped, and
 * it does not fail the run. */
error_t TagFS_RunTests(void)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    TestStats stats;
    error_t   result = TagFS_RunAllTests(&stats);
    kprintf("[TESTS] TagFS: %u run, %u passed, %u failed, %u skipped, "
            "%u held under a caveat\n",
            stats.total_tests, stats.total_passed, stats.total_failed,
            stats.total_skipped, stats.total_held);
    tagfs_leave();
    return result;
}
