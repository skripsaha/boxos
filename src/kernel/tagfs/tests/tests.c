#include "tests.h"
#include "../tagfs.h"
#include "../plot/plot.h"
#include "../ink/fallow.h"
#include "../settle/settle.h"
#include "../settle/binder.h"
#include "../dedup/dedup.h"
#include "../keep/keep.h"
#include "../retire/retire.h"
#include "../mount/away.h"
#include "../integrity/integrity.h"
#include "boardroom.h"
#include "write_job.h"   /* WriteJobSeatRunsAsync — the road a program's write takes */
#include "amp.h"
#ifdef CONFIG_VOLUME_TESTS
#include "block_fail.h"
#include "stand_tests.h"
#include "stand_stitch.h"
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
//   files      the volume's file count;
//   held       blocks in use that are not the volume's own bookkeeping.
//
// Not the free count on its own, because the free count is not the test's
// alone to move. The book lays its Tallies and its ribbon on ground of its
// own, and what a file gave up rests in Fallow until the cycles after its
// record gave it back (ink/fallow.h) — all of it the book's, and none of it
// residue. So the book's own ground and the ground at rest are taken out of
// the count, and what is left is what files hold: a test that leaves a block
// behind, or hands back one that was never its own, moves it.
//
// Not the tag count: a tag is interned for good, so the tests keep to tags
// they have always used and the guard does not count tags.
// ============================================================================

#ifdef CONFIG_VOLUME_TESTS
/* The Quires of the grid of `q` blocks that [start, start + count) meets,
 * marked in `bit` (`nquire` of them). */
static void StandTallyQuireMark(uint64_t* bit, uint64_t nquire, uint64_t q, uint64_t start, uint64_t count)
{
    for (uint64_t at = start / q; count && at < nquire && at * q < start + count; at++)
        bit[at / 64u] |= 1ull << (at % 64u);
}

/* Whether the Quire `at` of the grid is marked in `bit` (`nquire` of them). */
static bool StandTallyQuireMarked(const uint64_t* bit, uint64_t nquire, uint64_t at)
{
    return at < nquire && (bit[at / 64u] & (1ull << (at % 64u)));
}

uint64_t StandTallyQuires(uint64_t q, uint64_t** marks, uint64_t* nquire)
{
    uint64_t n = (tagfs_get_state()->block_end + q - 1u) / q, blocks = 0;
    uint64_t* bit = kmalloc(sizeof(uint64_t) * (n / 64u + 1u));
    if (!bit)
        return UINT64_MAX;
    memset(bit, 0, sizeof(uint64_t) * (n / 64u + 1u));
    for (uint32_t i = 0; i < g_binder.nslots; i++) {
        const SlotGround* s = &g_binder.slot[i];
        for (uint32_t k = 0; s->holds && k < s->nground; k++)
            StandTallyQuireMark(bit, n, q, s->ground[k].start, s->ground[k].count);
        for (uint32_t k = 0; s->holds && k < s->nquires; k++)
            StandTallyQuireMark(bit, n, q, s->quires[k].start, s->quires[k].count);
    }
    StandTallyQuireMark(bit, n, q, g_binder.pair_a, g_binder.pair_room);
    StandTallyQuireMark(bit, n, q, g_binder.pair_b, g_binder.pair_room);
    for (uint64_t at = 0; at < n; at++)
        blocks += StandTallyQuireMarked(bit, n, at) ? q : 0;
    *marks  = bit;
    *nquire = n;
    return blocks;
}

/* A book's own ground, each block once, as its census holds it at a mount and
 * its Binder at a Tally: every Quire of the grid a slot
 * names a node in, and the current pair, whole (StandTallyQuires) — a slot a
 * mount read names its nodes alone, and the census holds the Quire each lies
 * in (mount/ground_census.c GroundQuiresHeld), the Binder gives it back whole
 * (settle/binder_slots.c QuiresGo) — and the ribbon's Quires the slots hold,
 * the one it is in and its next_quire, as they lie: a ribbon's Quire is taken
 * on its atom, not on that grid (settle/ribbon.c), so it is no Quire of it.
 * What else BinderStandGround names lies in the Quires marked. False when the
 * heap will not hold them. */
static bool BookOwnBlocks(uint64_t* own)
{
    const RibbonCursor* c      = RibbonCursorNow();
    uint64_t            q      = c->quire_blocks ? c->quire_blocks : 1u, nquire = 0;
    uint64_t*           marks  = NULL;
    uint64_t            blocks = StandTallyQuires(q, &marks, &nquire);
    uint32_t            room   = BinderStandGround(NULL, 0);
    Strip*              runs   = blocks != UINT64_MAX ? kmalloc(((uint64_t)room + 2u) * sizeof(Strip)) : NULL;
    if (!runs) {
        kfree(marks);
        return false;
    }
    uint32_t n = BinderStandGround(runs, room);
    n          = n < room ? n : room;
    runs[n++]  = (Strip){ .start = c->quire.block, .count = (uint32_t)c->quire.blocks, .kind = STRIP_DATA };
    runs[n++]  = (Strip){ .start = c->next.block, .count = (uint32_t)c->next.blocks, .kind = STRIP_DATA };
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t k = i; k > 0 && runs[k - 1].start > runs[k].start; k--) {
            Strip t     = runs[k];
            runs[k]     = runs[k - 1];
            runs[k - 1] = t;
        }
    uint64_t reach = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t hi = runs[i].start + runs[i].count;
        for (uint64_t at = runs[i].start > reach ? runs[i].start : reach; at < hi;) {
            uint64_t to = (at / q + 1u) * q < hi ? (at / q + 1u) * q : hi;
            blocks += StandTallyQuireMarked(marks, nquire, at / q) ? 0 : to - at;
            at = to;
        }
        reach = hi > reach ? hi : reach;
    }
    kfree(runs);
    kfree(marks);
    *own = blocks;
    return true;
}
#endif

bool VolumeTallyTake(VolumeTally* tally)
{
    TagFSState* fs = tagfs_get_state();
    if (!fs || !fs->initialized)
        return false;

    /* What is in use of the volume and not its own ground — its Tallies',
     * its ribbon's, and Fallow's, which gives back at a lag — is its files'
     * blocks, and any a write took and named nowhere. The front, the Spine's
     * slots, the far Deed and the ground held for rotten ranges are the same
     * before and after a test, and fall out of the difference the count is
     * read by. Across a cycle that settles a Tally may be cut, and the nodes
     * it laid in its pair of Tally-Quires are the book's, kept for the next
     * Tally to go on in (binder.c TallyEnded), and named by no slot — so the
     * runner reads `held` across a test only when no cycle settled within it
     * (`cycles`, VolumeResidueSaid). The book's ground is the stand's to name,
     * Quires whole as its census holds them (BookOwnBlocks): a build without
     * VOLUME_TESTS counts its files. */
    uint64_t own = 0;
#ifdef CONFIG_VOLUME_TESTS
    if (!BookOwnBlocks(&own))
        return false;
#endif
    TagFSLockTake(&fs->lock);
    uint64_t used = fs->block_end - fs->free_blocks;
    uint64_t rest = FallowBlocks();
    tally->files  = fs->total_files;
    TagFSLockDrop(&fs->lock);
    tally->held   = used - own - rest;
    tally->cycles = SettledCycles();
    return true;
}

/* Whether `held` tells across a test: only in a VOLUME_TESTS build, which
 * names the book's own ground, and when no cycle settled within the test
 * (VolumeTally). */
static bool VolumeHeldRead(const VolumeTally* before, const VolumeTally* after)
{
#ifdef CONFIG_VOLUME_TESTS
    return after->cycles == before->cycles;
#else
    (void)before;
    (void)after;
    return false;
#endif
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
    if (after.held != before->held && VolumeHeldRead(before, &after)) {
        kprintf("[TESTS] %s failed: it left residue — %llu block(s) in use "
                "outside the volume's own ground before it and %llu after\n",
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
#ifdef CONFIG_VOLUME_TESTS
    kprintf("[TESTS] on a book each test that writes is held to the files it leaves, and — when no cycle "
            "settled within it — to the blocks in use outside the book's own ground: a block a write took and "
            "named nowhere is free again at the next mount, which derives the map from the book, and only a "
            "count in this session finds it\n");
#else
    kprintf("[TESTS] on a book each test that writes is held to the files it leaves alone: a build without "
            "VOLUME_TESTS does not name the book's own ground, and the blocks a test leaves in use are "
            "counted by the stand's build\n");
#endif
}

/* SEEKLAG=<us>: the volume's seat seeks before every read from the first test
 * that writes on (block_fail.h, BlockFailReadLag) — a medium that seeks, for
 * the tests to meet as it is. */
static void TestSeekLag(void)
{
#ifdef CONFIG_SEEKLAG_US
    BlockFailReadLag(tagfs_get_seat(), CONFIG_SEEKLAG_US);
#endif
}

/*
 * A fault left armed.
 *
 * BLOCKFAIL and HEAPFAIL strike whatever meets them next. A test that came
 * back with one still standing — out through an assertion, or by forgetting —
 * would have it strike the runner's own count of the volume, the next test,
 * or the rest of the boot. So both are taken down after every test, before
 * anything is counted, and a test that left one standing has failed. So is
 * the wall's wait on a Tally held down (BinderStandNoRoomWait): left standing,
 * every test after would meet a full volume's wall with no Tally written for
 * room, and none would say so. So is Dedup's pass let go (DedupHold): left
 * so, it would lay the files of every test after on one another's blocks. So
 * is a medium remembered as one that lied (TagFSStandLiedForget): the stand's
 * volume is every test's, and every mount after would hold it to have lied.
 */
static bool TestLeftAFaultArmed(const TestCase* test)
{
#ifdef CONFIG_VOLUME_TESTS
    bool block   = BlockFailDisarm();
    bool heap    = HeapFailDisarm();
    bool wall    = BinderStandNoRoomWait(false);
    bool dedup   = !DedupHold(true);
    bool lied    = TagFSStandLiedForget();
    bool protect = BlockFailProtects(tagfs_get_seat());
    if (protect)
        BlockFailProtect(tagfs_get_seat(), false);
    if (!block && !heap && !wall && !dedup && !lied && !protect)
        return false;

    const char* left[6];
    uint32_t    n = 0;
    if (block)
        left[n++] = "BLOCKFAIL";
    if (heap)
        left[n++] = "HEAPFAIL";
    if (wall)
        left[n++] = "the wall's wait on a Tally held down";
    if (dedup)
        left[n++] = "Dedup's pass after a Tally let go";
    if (lied)
        left[n++] = "a medium remembered as one that lied";
    if (protect)
        left[n++] = "a protected medium";
    char   said[224];
    size_t at = 0;
    for (uint32_t i = 0; i < n; i++)
        at += (size_t)ksnprintf(said + at, sizeof(said) - at, "%s%s",
                                i == 0 ? "" : i + 1 == n ? " and " : ", ", left[i]);
    kprintf("[TESTS] %s failed: it left %s armed — taken down by the runner before anything else could meet "
            "it\n", test->name, said);
    return true;
#else
    (void)test;
    return false;
#endif
}

/*
 * The machine's Use Context a test of a book's writer set aside
 * (StandStitchAside) and stood aside past, never putting the ribbon back:
 * given back here, after the faults are taken down, so the next test and the
 * boot's own mounts meet the machine the boot had. A test it would not be set
 * again for has failed.
 */
static bool TestLeftContextAside(const TestCase* test)
{
#ifdef CONFIG_VOLUME_TESTS
    if (StandStitchContextBack())
        return false;
    kprintf("[TESTS] %s failed: the machine's Use Context it set aside would not be given back\n", test->name);
    return true;
#else
    (void)test;
    return false;
#endif
}

/*
 * On a book the cadence lays a Tally between cycles, and while the tests run
 * the cadence is held (SettleCadenceHold): a test meets only the cycles it
 * drives. So the Tally the cycles of the tests before left owed is laid here,
 * before the next test that writes, as the cadence would have laid it between
 * them — its Slips laid in their blocks first, as the cadence's Engross would.
 * Without it the ribbon of a whole stand runs on past every Tally — measured:
 * 10393 stitches over 39 Quires, read whole by every mount after them.
 */
static void TestTallyOwedLaid(const TestCase* test)
{
    if (!BinderEngrossOwed())
        return;
    error_t rc = BinderRunWhole();
    if (rc != OK)
        kprintf("[TESTS] the Tally owed before %s was not laid (error %u): the test meets the ribbon as "
                "it stands\n", test->name, (unsigned)rc);
}

/*
 * The tests that write begin on a book whose ribbon a Tally took in, whatever
 * a session before this boot left on it: a stitch of this mount settled, which
 * a Tally stands past (§4.2), its tails laid in their blocks (binder.c,
 * BinderRunWhole), and nothing its ribbon let go resting in Fallow at the
 * mounts after. A session leaves files whose tails ride Slips — each
 * holds every Tally back until the Helm's service, on another core, lays it —
 * and the ground its stitches gave up resting at every mount until a stitch of
 * that mount leaves the ribbon's Quire: past a test's cut of the free ground,
 * where it gives the ribbon a Quire the test weighed it without. Measured: a
 * second boot of the stand met two such tails and 453 blocks resting, and 25
 * tests of room, Tallies and Slips failed on them.
 */
static void TestBookTallied(void)
{
    error_t rc = TagFSSettle();
    if (rc == OK)
        rc = BinderRunWhole();
    if (rc != OK)
        kprintf("[TESTS] the Tally the tests that write begin on was not laid (error %u): they meet the book as it "
                "stands\n", (unsigned)rc);
}

static void TestRun(const TestCase* test, uint32_t of, TestStats* stats)
{
    bool        guarded = test->reach == TEST_WRITES_TO_VOLUME;
    VolumeTally before;

    if (guarded)
        TestTallyOwedLaid(test);
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

    /* Said as it begins where the tests write: some of those fill the volume
     * to its wall — a gigabyte on quill's stretched one, minutes of a loaded
     * host — and a harness waiting for the shell tells a boot still going
     * through them from one printing for ever only by this line (stand.sh
     * STAND_TEST_BEGUN). A plain build's tests only read, and its boot reads
     * on the owner's screen as it did. */
    if (g_volume_tests_allowed)
        kprintf("[TESTS] %u of %u: %s\n", stats->total_tests, of, test->name);
    TestResult seen = test->func();
    bool left_armed = TestLeftAFaultArmed(test);
    bool left_aside = TestLeftContextAside(test);

    /* Residue first: a test that left the volume changed has failed, and
     * whatever it would otherwise have been judged — held included — is not
     * said, so a count of held lines never includes one that failed. */
    if (guarded && VolumeResidueSaid(test->name, &before))
        stats->total_failed++;
    else if (left_armed || left_aside)
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
    uint32_t registered      = 0;

    for (uint32_t s = 0; s < ARRAY_SIZE(g_suites); s++)
        registered += g_suites[s]->count;

    /* No cycle begins on its own while the tests run: each meets the cycles
     * it drives, and a fault it arms is met by one of them (settle.h). Nor
     * does Dedup's pass after a Tally (dedup.h): a pass on another core would
     * lay one test's files on another's blocks under it — a test of Dedup lets
     * it go and holds it again. Nor does the Keep service look at the kept
     * files (keep.h): a look under way on another core is a caller inside the
     * door when a test lets the volume go and mounts it again, and the volume
     * waits for it to leave — nor the Retire service, for the same reason
     * (retire/retire.h). */
    SettleCadenceHold(true);
    (void)DedupHold(true);
    (void)KeepHold(true);
    (void)RetireHold(true);
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
                TestBookTallied();
                TestSeekLag();
                path_said = true;
            }
            TestRun(test, registered, stats);
        }
    }
    (void)RetireHold(false);
    (void)KeepHold(false);
    (void)DedupHold(false);
    SettleCadenceHold(false);

    /* Said with kprintf: "37 run, 35 passed" tells nobody which 37, and a
     * suite that quietly shrank is how a test stops covering anything. */
    if (skipped_writers > 0) {
        kprintf("[TESTS] %u test(s) that write to the volume were not run — "
                "this volume belongs to whoever is using this machine (build "
                "with VOLUME_TESTS=on to run them)\n", skipped_writers);
    }

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
