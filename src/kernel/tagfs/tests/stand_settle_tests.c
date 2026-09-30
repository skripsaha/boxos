#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../ink/ink.h"
#include "../metadata_pool/meta_pool.h"
#include "../settle/settle.h"
#include "amp.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "block_fail.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The cadence: metadata settles on its own (step 1.7)
//
// The runner holds the cadence off for every other test (settle.h,
// SettleCadenceHold); these let it go where the cadence is what they ask about.
// A test runs on a K-Core — or, on one core, inside a call with interrupts off
// — so it stands in for its own core's loop and calls the service point
// itself: on the committer's core that is where the cycle runs, anywhere else
// the call does nothing and the committer's own loop runs it.
// ============================================================================

static uint64_t StandNowUs(void)
{
    return cpu_tsc_to_us(rdtsc());
}

/* The file a test changes, given up and settled on the way out. */
static void StandSettleLetGo(uint32_t file_id)
{
    if (file_id) {
        (void)tagfs_delete_file(file_id);
        (void)TagFSSettle();
    }
}

/*
 * A change nobody anchors reaches the medium by itself: the Ink knocks, Lull
 * permits a cycle, and the cycle settles it — within the window the mount line
 * promises and the time the cycle itself takes on the medium. Watched for two
 * windows, so a cadence that never starts one fails here rather than hangs.
 */
static TestResult TestStandSettleWithoutAnchor(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   file_id = 0;

    SettleCadenceHold(false);
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5Au);
    TEST_ASSERT(StandMakeFile("settle_on_its_own", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written, and nothing anchors it");
    uint64_t gen   = InkedGen();
    uint64_t began = StandNowUs();
    while (SettledGen() < gen && StandNowUs() - began < 2u * SETTLE_WINDOW_US) {
        SettleServiceIfPending(false);
        cpu_pause();
    }
    uint64_t took = StandNowUs() - began;
    TEST_ASSERT(SettledGen() >= gen, "a cycle nobody asked for settled the change");
    TEST_ASSERT(took <= SETTLE_WINDOW_US + SettleLastDUs(),
                "within the window and the cycle's own time on the medium");
    TestEvidence("microseconds from the write to its settling:", took);
out:
    SettleCadenceHold(true);
    StandSettleLetGo(file_id);
    return verdict;
}

/* A waiter of the stand's own, as an anchor's is: what it was answered, and by
 * which settled cycle. */
typedef struct {
    SettleWaiter w;
    bool         answered;
    error_t      rc;
    uint64_t     cycle;
} StandSettleWaiter;

static void StandSettleAnswered(SettleWaiter* w, error_t rc)
{
    StandSettleWaiter* s = (StandSettleWaiter*)w;
    s->rc       = rc;
    s->cycle    = SettledCycles();
    s->answered = true;
}

/*
 * An anchor waits through its Chit and is answered by the cycle's S6 — not
 * before a cycle has settled its change and laid it in a block the volume never
 * writes again. The waiter stands linked while no cycle runs (the runner holds
 * the cadence), and the one cycle driven here answers it, OK.
 */
static TestResult TestStandSettleAnchorThroughChit(void)
{
    TestResult               verdict = TEST_PASS;
    uint32_t                 file_id = 0;
    static StandSettleWaiter waiter;

    memset(&waiter, 0, sizeof(waiter));
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0xA5u);
    TEST_ASSERT(StandMakeFile("settle_anchor_chit", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    uint64_t gen = InkedGen();
    TEST_ASSERT_EQ(SettleWaitNow(gen), ERR_WOULD_BLOCK, "its change waits for a cycle");

    SettleWaitLink(&waiter.w, gen, StandSettleAnswered);
    SettleServiceIfPending(false);
    TEST_ASSERT(!waiter.answered, "no cycle ran, and nobody answered the waiter");

    uint64_t cycles = SettledCycles();
    TEST_ASSERT_OK(TagFSSettle(), "a cycle settles");
    TEST_ASSERT(waiter.answered, "the cycle's S6 answered the waiter");
    TEST_ASSERT_OK(waiter.rc, "with OK");
    TEST_ASSERT_EQ(waiter.cycle, cycles + 1u, "and it was that cycle, the one after the link");
    TEST_ASSERT(PoolClosedSettledGen() >= gen,
                "the change lies in a closed block the volume never writes again");
    TEST_ASSERT_OK(SettleWaitNow(gen), "a wait for it now is answered at once");
out:
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    StandSettleLetGo(file_id);
    return verdict;
}

/*
 * RT-10. On one core the idle context is where a cycle runs, and a program that
 * never parks would never let it: a demand makes idle the debtor of the next
 * quantum until a cycle takes it up. Asked of the rule the scheduler asks from
 * the tick — the machine with the hog is the host's (PowerCut `hog`). On more
 * than one core the committer's K-Core runs cycles, and nobody owes idle.
 */
static TestResult TestStandSettleIdleOwed(void)
{
    TestResult verdict  = TEST_PASS;
    bool       one_core = g_amp.total_cores == 1;

    SettleCadenceHold(false);
    TEST_ASSERT_OK(TagFSSettle(), "the volume owes nothing to begin with");
    TEST_ASSERT(!TagFSIdleOwed(), "with nothing owed, idle is owed no quantum");
    SettleDemand(SETTLE_DEMAND_LEAVE);
    TEST_ASSERT_EQ(TagFSIdleOwed(), one_core,
                   "a demand makes idle the debtor on one core, and nowhere else");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle takes the demand up");
    TEST_ASSERT(!TagFSIdleOwed(), "and idle is owed nothing after it");
out:
    SettleCadenceHold(true);
    return verdict;
}

/*
 * RT-18. A medium that refuses a write as protected turns the volume read-only:
 * the cycle is refused, no change is taken any more — ERR_TAGFS_READ_ONLY —
 * and a waiter hears the same code rather than waiting for ever.
 */
static TestResult TestStandSettleWriteProtected(void)
{
    TestResult               verdict   = TEST_PASS;
    uint32_t                 file_id   = 0;
    uint8_t                  seat      = tagfs_get_seat();
    bool                     protected_ = false;
    InkReceipt               receipt;
    static StandSettleWaiter waiter;

    memset(&waiter, 0, sizeof(waiter));
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x3Cu);
    TEST_ASSERT(StandMakeFile("settle_protected", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    SettleWaitLink(&waiter.w, InkedGen(), StandSettleAnswered);

    BlockFailProtect(seat, true);
    protected_ = true;
    TEST_ASSERT(TagFSSettle() != OK, "the cycle is refused by the protected medium");
    TEST_ASSERT(TagFSReadOnly(), "and the volume takes no write from here");
    TEST_ASSERT(waiter.answered, "the waiter is answered, not left waiting");
    TEST_ASSERT_EQ(waiter.rc, ERR_TAGFS_READ_ONLY, "with ERR_TAGFS_READ_ONLY");
    TEST_ASSERT_EQ(InkQuitclaim(file_id, &receipt), ERR_TAGFS_READ_ONLY,
                   "an Ink is refused ERR_TAGFS_READ_ONLY");
    TEST_ASSERT_EQ(SettleWaitNow(InkedGen() + 1u), ERR_TAGFS_READ_ONLY,
                   "and so is a new wait for a cycle");
out:
    if (protected_) {
        BlockFailProtect(seat, false);
        InkReadOnlyLift();
    }
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    StandSettleLetGo(file_id);
    return verdict;
}

static const TestCase g_stand_settle_tests[] = {
    { .name = "stand_settle_without_anchor",     .func = TestStandSettleWithoutAnchor,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a file written without anchor has no name after a power cut" },
    { .name = "stand_settle_anchor_through_chit", .func = TestStandSettleAnchorThroughChit,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_idle_owed",          .func = TestStandSettleIdleOwed,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "on one core under a CPU hog nothing settles a write before the power goes" },
    { .name = "stand_settle_write_protected",    .func = TestStandSettleWriteProtected,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_settle_suite = {
    .tests = g_stand_settle_tests,
    .count = sizeof(g_stand_settle_tests) / sizeof(g_stand_settle_tests[0]),
};

#endif /* CONFIG_VOLUME_TESTS */
