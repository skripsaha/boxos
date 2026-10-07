#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../block_map/block_map.h"
#include "../file_table/file_table.h"
#include "../metadata_pool/meta_pool.h"
#include "../mount/away.h"
#include "../settle/settle.h"
#include "amp.h"
#include "baton.h"
#include "atomics.h"
#include "boardroom.h"
#include "cpu_calibrate.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"      /* SCHEDULER_DEFAULT_TICK_HZ */
#include "block_fail.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The cadence: metadata settles on its own (step 1.7)
//
// The runner holds the cadence off for every other test (settle.h,
// SettleCadenceHold); these let it go where the cadence is what they ask about.
// A test runs at boot on the boot core with its interrupts still off, however
// many cores there are — and the boot core is the drain core, whose batons
// carry the cadence's knock and whose tick fires the Lull's node. So a test
// that waits for the cadence stands in for its own core's loop: it knocks as
// the tick would, runs the batons the knock leaves there, and calls the
// service point. On one core the committer is this core, and the cycle runs
// in that call; on more it is a K-Core, which the knock wakes to look again,
// and its own loop runs the cycle.
// ============================================================================

static uint64_t StandNowUs(void)
{
    return cpu_tsc_to_us(rdtsc());
}

/* The file a test changes, given up and settled on the way out. */
static void StandSettleLetGo(uint32_t file_id)
{
    if (file_id) {
        (void)tagfs_delete_file(file_id, INK_UNWARRANTED);
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
        SettleKnock();
        BatonPump(amp_get_core_index());
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
    TEST_ASSERT_EQ(InkQuitclaim(file_id, INK_UNWARRANTED, &receipt), ERR_TAGFS_READ_ONLY,
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

// ----------------------------------------------------------------------------
// A refused cycle waits for its event (R2), and what each step's refusal is (R8)
// ----------------------------------------------------------------------------

/* The Ledger's copies, as sectors of the medium: a plan armed on them meets the
 * cycle at its S4, and only there. */
static void StandSettleArmLedger(uint32_t nth)
{
    TagFSState* fs     = tagfs_get_state();
    uint64_t    sector = tagfs_get_volume_base() +
                         (uint64_t)fs->layout.state_block * TAGFS_BLOCK_SECTORS;
    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_WRITE, sector,
                 (uint64_t)VOLUME_LEDGER_COPIES * TAGFS_BLOCK_SECTORS, nth);
}

/* One turn of the machine, done by hand: the knock the tick would give, the
 * batons it leaves on this core, the service point — the committer's own on
 * one core; on more, a K-Core's, which the knock wakes to look again. */
static void StandSettleTurn(void)
{
    SettleKnock();
    BatonPump(amp_get_core_index());
    SettleServiceIfPending(false);
    cpu_pause();
}

/*
 * R2 (Z01-2, Z10-1, Z10-6, F01-4). A cycle the medium refused — its Ledger
 * write — declares nothing, and the waiter stays in its Chit: no code of a
 * step is its answer. A bell the seat rang just before the cycle took its
 * count answered a refusal before it, and is no event of this one, however
 * late the room says it. With the cadence let go, no second cycle begins
 * before the seat says seat:ready — watched for two Lulls and a tick, within
 * which an eager change whose cycle is not parked begins it. The seat then
 * answers a command, which rings (B2), and the next cycle answers the waiter
 * OK. Red with the refusal of the medium parked on nothing: the cadence began
 * the refused cycle again at once, what the legacy channel did before the
 * bell; and red with the room's announcement counted as well as its bell.
 */
static TestResult TestStandSettleRefusalWaitsForTheBell(void)
{
    TestResult               verdict = TEST_PASS;
    uint8_t                  seat    = tagfs_get_seat();
    uint32_t                 file_id = 0;
    bool                     armed   = false;
    bool                     let_go  = false;
    static StandSettleWaiter waiter;
    const char*              aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT_OK(TagFSSettle(), "the volume owes nothing to begin with");
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x6Bu);
    TEST_ASSERT(StandMakeFile("settle_refused", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    SettleWaitLink(&waiter.w, InkedGen(), StandSettleAnswered);

    StandSettleArmLedger(1);
    armed = true;
    TagFSStandSettleRings(true);
    error_t refused = TagFSSettleAsCadence();
    bool    fired   = BlockFailFired();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT(fired, "the cycle's Ledger write was the one refused");
    TEST_ASSERT(refused != OK, "and the cycle is refused");
    TEST_ASSERT(!waiter.answered, "the waiter stays in its Chit: no code of a step is its answer");
    TEST_ASSERT(SettleParked(), "the cadence waits for the refusal's event");
    BoardroomAttendIfPending();
    TEST_ASSERT(SettleParked(), "and a bell rung before the cycle took its count, said after, "
                "is not that event");

    /* Counted and never refused: a second cycle would write the Ledger. */
    StandSettleArmLedger(BLOCK_FAIL_NEVER);
    armed = true;
    SettleCadenceHold(false);
    let_go = true;
    uint64_t watch = 2u * SettleLullUs() + 1000000ull / SCHEDULER_DEFAULT_TICK_HZ;
    uint64_t began = StandNowUs();
    while (StandNowUs() - began < watch)
        StandSettleTurn();
    uint32_t again = BlockFailSeen();
    BlockFailDisarm();
    armed = false;
    TestEvidence("Ledger writes of cycles begun before the event, of 0", again);
    TEST_ASSERT_EQ(again, 0, "the cadence began no second cycle before the event");
    TEST_ASSERT(!waiter.answered, "and the waiter is still in its Chit");

    /* Nightwatch's look, as the tick takes it a window later, and the look
     * after: a park its waiters have waited on longer than the window is said
     * once (the host counts the line). No tick runs while the tests do. */
    SettleWatchLook(StandNowUs() + SETTLE_WINDOW_US + 1u);
    SettleWatchLook(StandNowUs() + SETTLE_WINDOW_US + 2u);
    BatonPump(amp_get_core_index());

    TEST_ASSERT_OK(BoardroomRead(seat, tagfs_block_to_sector(0), TAGFS_BLOCK_SECTORS,
                                 g_test_output), "the seat answers a command");
    BoardroomAttendIfPending();
    TEST_ASSERT(!SettleParked(), "and rings seat:ready, which lifts the park");
    began = StandNowUs();
    while (!waiter.answered && StandNowUs() - began < 2u * SETTLE_WINDOW_US)
        StandSettleTurn();
    TEST_ASSERT(waiter.answered, "the next cycle answered the waiter");
    TEST_ASSERT_OK(waiter.rc, "with OK");
out:
    if (armed)
        BlockFailDisarm();
    if (let_go)
        SettleCadenceHold(true);
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    StandSettleLetGo(file_id);
    return verdict;
}

/*
 * X1-4. seat:ready that comes while a cycle runs — after the cycle took the
 * count its refusal is judged by, before the refusal is decided — is never
 * lost: the refused cycle parks on nothing, and the next one may go at once.
 * Red with the count read when the refusal is decided, after its event: the
 * cadence would wait for a seat:ready that had already come and would not come
 * twice.
 */
static TestResult TestStandSettleRingDuringTheCycle(void)
{
    TestResult               verdict = TEST_PASS;
    uint8_t                  seat    = tagfs_get_seat();
    uint32_t                 file_id = 0;
    bool                     armed   = false;
    static StandSettleWaiter waiter;
    const char*              aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT_OK(TagFSSettle(), "the volume owes nothing to begin with");
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x7Cu);
    TEST_ASSERT(StandMakeFile("settle_rings", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    SettleWaitLink(&waiter.w, InkedGen(), StandSettleAnswered);

    uint32_t rang = BoardroomSeatReadyGen(seat);
    StandSettleArmLedger(1);
    armed = true;
    TagFSStandSettleRings(false);
    error_t refused = TagFSSettleAsCadence();
    bool    fired   = BlockFailFired();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT(fired && refused != OK, "the cycle is refused at its Ledger");
    TEST_ASSERT_EQ(BoardroomSeatReadyGen(seat), rang + 1u,
                   "and its seat rang seat:ready while it ran, after its S0");
    TEST_ASSERT(!waiter.answered, "the waiter stays in its Chit");
    TEST_ASSERT(!SettleParked(), "the refusal parks on nothing: its event has come already");
    TEST_ASSERT_OK(TagFSSettleAsCadence(), "the next cycle goes, and settles");
    TEST_ASSERT(waiter.answered, "and answers the waiter");
    TEST_ASSERT_OK(waiter.rc, "OK");
out:
    if (armed)
        BlockFailDisarm();
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    StandSettleLetGo(file_id);
    return verdict;
}

/* Pages the stand holds while a cycle is refused for want of memory, and gives
 * back as the machine's memory coming back. */
#define STAND_SETTLE_HEAP_PAGES  16u

/*
 * R2 (Z07-9, Z10-1). A refusal that is not the medium's waits for its own
 * event, not for seat:ready, which nothing that refused would ever ring: a
 * step the machine had no memory for, for its pages to come back; a step whose
 * ground is on its way, for room to be asked for. A seat:ready meanwhile is
 * not the event either — and when it comes, the next cycle settles and answers
 * the waiter. Red before: every refusal waited for seat:ready, so one for
 * want of memory or room stood until a medium happened to refuse and answer
 * again — on a healthy medium, never.
 */
static TestResult TestStandSettleRefusalWaitsForItsOwnEvent(void)
{
    TestResult               verdict = TEST_PASS;
    uint8_t                  seat    = tagfs_get_seat();
    uint32_t                 file_id = 0;
    void*                    pages   = NULL;
    static StandSettleWaiter waiter;

    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT_OK(TagFSSettle(), "the volume owes nothing to begin with");
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x1Fu);
    TEST_ASSERT(StandMakeFile("settle_own_event", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    SettleWaitLink(&waiter.w, InkedGen(), StandSettleAnswered);

    pages = PmmTakeExact(STAND_SETTLE_HEAP_PAGES);
    TEST_ASSERT(pages != NULL, "the stand holds pages of the machine's");
    TagFSStandSettleRefuses(ERR_NO_MEMORY);
    TEST_ASSERT_EQ(TagFSSettleAsCadence(), ERR_NO_MEMORY, "a cycle is refused for want of memory");
    TEST_ASSERT(SettleParked(), "and waits");
    BoardroomNoteSeatReady(BoardroomSeatKind(seat), BoardroomSeatIndex(seat));
    BoardroomAttendIfPending();
    TEST_ASSERT(SettleParked(), "seat:ready is not what it waits for");
    PmmGiveExact(pages, STAND_SETTLE_HEAP_PAGES);
    pages = NULL;
    TagFSHeapCameBack();
    TEST_ASSERT(!SettleParked(), "the machine's memory coming back is");

    TagFSStandSettleRefuses(ERR_WOULD_BLOCK);
    TEST_ASSERT_EQ(TagFSSettleAsCadence(), ERR_WOULD_BLOCK,
                   "a cycle is refused with its ground on the way");
    TEST_ASSERT(SettleParked(), "and waits");
    BoardroomNoteSeatReady(BoardroomSeatKind(seat), BoardroomSeatIndex(seat));
    BoardroomAttendIfPending();
    TEST_ASSERT(SettleParked(), "seat:ready is not what it waits for");
    TEST_ASSERT(!waiter.answered, "the waiter stays in its Chit through both");
    SettleDemand(SETTLE_DEMAND_ROOM);
    TEST_ASSERT(!SettleParked(), "room asked for is");

    TEST_ASSERT_OK(TagFSSettleAsCadence(), "the next cycle settles");
    TEST_ASSERT(waiter.answered, "and answers the waiter");
    TEST_ASSERT_OK(waiter.rc, "OK");
out:
    TagFSStandSettleRefuses(OK);
    if (pages)
        PmmGiveExact(pages, STAND_SETTLE_HEAP_PAGES);
    if (!waiter.answered)
        (void)SettleWaitUnlink(&waiter.w);
    StandSettleLetGo(file_id);
    return verdict;
}

/*
 * F01-4. A block of the file table the medium refused is the medium's answer —
 * the very code it gives a block write — and not a metadata error, which says
 * the volume is damaged. Measured on the same block with the same plan, before
 * the cycle meets it.
 */
static TestResult TestStandSettleTableRefusalIsTheMediums(void)
{
    TestResult     verdict   = TEST_PASS;
    uint8_t        seat      = tagfs_get_seat();
    uint32_t       file_id   = 0;
    uint32_t       row_block = 0;
    FileTableEntry row       = { 0, 0 };
    bool           armed     = false;
    const char*    aside     = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_EQ(tagfs_create_file("settle_table_refused", NULL, 0, &file_id), 0,
                   "a file is created, so the table has a row to write");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle puts the file's row on the medium");
    TEST_ASSERT(StandTableRowOnMedium(file_id, &row, &row_block),
                "the block that carries the row is found on the medium");

    BlockFailArm(seat, BLOCK_FAIL_WRITE, tagfs_block_to_sector(row_block), TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    TEST_ASSERT_OK(TagFSMetaBlockRead(row_block, g_test_output), "the block is read as it stands");
    error_t medium = TagFSMetaBlockWrite(row_block, g_test_output);
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT(medium != OK, "the medium's answer to a refused write of the block");

    TagFSLockTake(&tagfs_get_state()->lock);
    int marked = file_table_update(file_id, row.meta_block, row.meta_offset);
    TagFSLockDrop(&tagfs_get_state()->lock);
    TEST_ASSERT_EQ(marked, 0, "the row is owed to the medium again");
    BlockFailArm(seat, BLOCK_FAIL_WRITE, tagfs_block_to_sector(row_block), TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    error_t refused = TagFSSettle();
    bool    fired   = BlockFailFired();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT(fired, "the cycle's write of the row's block was the one refused");
    TestEvidence("the cycle's answer, of the medium's", refused);
    TEST_ASSERT_EQ(refused, medium, "the cycle answers what the medium answered");
out:
    if (armed)
        BlockFailDisarm();
    if (TagFSSettle() != OK && verdict == TEST_PASS)
        verdict = TestFailedAt(__FILE__, __LINE__, "an unarmed cycle afterwards settles", 1, 0);
    if (file_id)
        (void)tagfs_delete_file(file_id, INK_UNWARRANTED);
    (void)TagFSSettle();
    return verdict;
}

/* Cycles until nothing rests in Fallow, and one more whose map says so on the
 * medium: a test that takes all of the ground, or the volume down, leaves no
 * ground at rest behind it (B1). */
#define STAND_SETTLE_BARE_CYCLES  8u

static bool StandSettleRested(void)
{
    TagFSState* fs = tagfs_get_state();
    for (uint32_t cycle = 0; cycle < STAND_SETTLE_BARE_CYCLES; cycle++) {
        if (TagFSSettle() != OK)
            return false;
        TagFSLockTake(&fs->lock);
        uint64_t resting = FallowBlocks();
        TagFSLockDrop(&fs->lock);
        if (resting == 0)
            return TagFSSettle() == OK;
    }
    return false;
}

/* Every free run of the volume, taken into a list that grows as it must, and
 * given back. */
typedef struct {
    uint32_t* at;
    uint32_t* n;
    uint32_t  count;
    uint32_t  cap;
} StandSettleRuns;

static bool StandSettleTakeAll(StandSettleRuns* runs)
{
    uint32_t all   = tagfs_get_state()->layout.data_blocks;
    uint32_t start = 0, got = 0;
    while (TagFSTakeRun(all, 0, &start, &got) == OK) {
        if (runs->count == runs->cap) {
            uint32_t  cap = runs->cap ? runs->cap * 2u : 64u;
            uint32_t* at  = kmalloc(sizeof(uint32_t) * cap);
            uint32_t* n   = kmalloc(sizeof(uint32_t) * cap);
            if (!at || !n) {
                kfree(at);
                kfree(n);
                (void)tagfs_free_blocks(start, got);
                return false;
            }
            if (runs->count) {
                memcpy(at, runs->at, sizeof(uint32_t) * runs->count);
                memcpy(n, runs->n, sizeof(uint32_t) * runs->count);
            }
            kfree(runs->at);
            kfree(runs->n);
            runs->at  = at;
            runs->n   = n;
            runs->cap = cap;
        }
        runs->at[runs->count] = start;
        runs->n[runs->count]  = got;
        runs->count++;
    }
    return true;
}

static void StandSettleGiveAll(StandSettleRuns* runs)
{
    for (uint32_t i = 0; i < runs->count; i++)
        (void)tagfs_free_blocks(runs->at[i], runs->n[i]);
    kfree(runs->at);
    kfree(runs->n);
    memset(runs, 0, sizeof(*runs));
}

/*
 * Z13-4. The file table's chain needs a block, and the volume has none: the
 * cycle settles all the same — the rows past the chain wait in memory, and the
 * pool's records name them — and the cycle after the ground came back grows
 * the chain. Red before: the cycle was refused at the file table, and so was
 * every cycle after it — the ground a short volume waits for comes back only
 * when a cycle settles.
 */
static TestResult TestStandSettleTableShortSettles(void)
{
    TestResult      verdict = TEST_PASS;
    TagFSState*     fs      = tagfs_get_state();
    StandChain      before  = { 0 };
    StandChain      after   = { 0 };
    StandSettleRuns runs    = { 0 };
    uint32_t        row     = 0;
    bool            named   = false;
    const char*     aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandSettleRested(), "every change is on the medium, and nothing rests");
    TEST_ASSERT(StandChainOnMedium(fs->layout.file_table_block, TAGFS_FILETBL_MAGIC, &before) &&
                !before.broken, "the file table's chain on the medium is whole");
    row = before.links * TAGFS_FTABLE_PER_BLOCK;
    TagFSLockTake(&fs->lock);
    named = file_table_update(row, 0, 0) == 0;
    TagFSLockDrop(&fs->lock);
    TEST_ASSERT(named, "a row past the chain makes the table need one more block");

    TEST_ASSERT(StandSettleTakeAll(&runs), "every free block of the volume is taken");
    error_t short_of_ground = TagFSSettle();
    TestEvidence("the cycle's answer with no ground for the table, of OK", short_of_ground);
    TEST_ASSERT_OK(short_of_ground, "the cycle settles without the block the rows need");
    TEST_ASSERT(StandChainOnMedium(fs->layout.file_table_block, TAGFS_FILETBL_MAGIC, &after) &&
                !after.broken, "the chain on the medium is whole");
    TEST_ASSERT_EQ(after.links, before.links, "and as long as it was");

    StandSettleGiveAll(&runs);
    TEST_ASSERT_OK(TagFSSettle(), "with the ground back, the next cycle settles");
    TEST_ASSERT(StandChainOnMedium(fs->layout.file_table_block, TAGFS_FILETBL_MAGIC, &after) &&
                !after.broken, "the chain on the medium is whole");
    TEST_ASSERT_EQ(after.links, before.links + 1u, "and one block longer, for the rows past it");
out:
    StandSettleGiveAll(&runs);
    if (named) {
        TagFSLockTake(&fs->lock);
        (void)file_table_delete(row);
        TagFSLockDrop(&fs->lock);
    }
    (void)TagFSSettle();
    return verdict;
}

// ----------------------------------------------------------------------------
// A program's debt is its own (R26)
// ----------------------------------------------------------------------------

/* Programs as the volume knows them: a record nobody runs, which a call served
 * on this core is noted against, and which leaves. */
static process_t s_stand_reader;
static process_t s_stand_writer;

/*
 * R26 (Z10-7 = Z16-3). A program owes the volume only its own Inks. A call
 * that read made none, and the program that leaves after it demands no cycle —
 * while a change it did not make waits for the window; a call that wrote made
 * one, and that program's leaving is a demand. Asked of the two steps the
 * executor takes around every call it serves (manifest_exec.c). Red before:
 * every call of the storage deck noted the volume's newest Ink against its
 * program, and a reader's exit started a cycle for somebody else's change.
 */
static TestResult TestStandSettleReaderOwesNoLeave(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   file_id = 0;

    memset(&s_stand_reader, 0, sizeof(s_stand_reader));
    memset(&s_stand_writer, 0, sizeof(s_stand_writer));
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x2Du);
    TEST_ASSERT(StandMakeFile("settle_reader", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    TEST_ASSERT_OK(TagFSSettle(), "and settled: no demand stands");
    TEST_ASSERT_EQ(StandWriteAt(file_id, 0, g_test_buffer, TAGFS_BLOCK_SIZE),
                   (int)TAGFS_BLOCK_SIZE, "a change nobody here made is inked");
    TEST_ASSERT(InkedGen() > SettledGen(), "and waits for its cycle");

    uint64_t inked = TagFSInkedHere();
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, TAGFS_BLOCK_SIZE), (int)TAGFS_BLOCK_SIZE,
                   "a program's call reads the file");
    TagFSNoteInkerSince(&s_stand_reader, inked);
    TagFSInkerLeaves(&s_stand_reader);
    TEST_ASSERT(!(TagFSStandSettleDemands() & SETTLE_DEMAND_LEAVE),
                "the reader leaves, and demands no cycle");

    inked = TagFSInkedHere();
    TEST_ASSERT_EQ(StandWriteAt(file_id, 0, g_test_buffer, TAGFS_BLOCK_SIZE),
                   (int)TAGFS_BLOCK_SIZE, "a program's call writes the file");
    TagFSNoteInkerSince(&s_stand_writer, inked);
    TagFSInkerLeaves(&s_stand_writer);
    TEST_ASSERT(TagFSStandSettleDemands() & SETTLE_DEMAND_LEAVE,
                "the writer leaves with its change unsettled, and demands the next cycle");
out:
    StandSettleLetGo(file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A stopping machine's last cycles leave no ground at rest (RT-12)
// ----------------------------------------------------------------------------

/*
 * Z09-4. A file deleted under a reader rests in Fallow for as long as the
 * reader is open — and a machine stops with its programs' handles open. The
 * halt's last cycles write that ground free in the map all the same, while
 * memory keeps it taken, so the reader still reads it and no other file is
 * handed it. Asked of the halt's own cycles (TagFSStandSettleForHalt), which
 * stop nothing here. Red before: the cycles ended with the block taken on the
 * medium and named by nothing, which the next mount's census keeps for good.
 */
static TestResult TestStandSettleHaltLeavesNoViewedGround(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0;
    uint32_t         block   = 0;
    TagFSFileHandle* reader  = NULL;
    bool             taken   = true;
    const char*      aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x4A17u, 0);
    TEST_ASSERT(StandMakeFile("settle_halt_viewed", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &block), "its block is found");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    reader = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(reader, "a reader opens it");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id, INK_UNWARRANTED), 0, "the file is deleted under it");
    file_id = 0;

    TEST_ASSERT_OK(TagFSStandSettleForHalt(), "a stopping machine's last cycles run");
    TEST_ASSERT(BlockMapMediumSays(block, &taken), "the map's sector is read off the medium");
    TEST_ASSERT(StandBlockInUse(block), "memory still keeps the reader's block taken");
    TEST_ASSERT_EQ(TestHandleRead(reader, g_test_output, TAGFS_BLOCK_SIZE), (int)TAGFS_BLOCK_SIZE,
                   "the reader reads the file it opened");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "byte for byte");

    TestEvidence("blocks the halt's last cycles left taken on the medium for an open reader",
                 taken ? 1u : 0u);
    if (taken)
        verdict = TEST_DEFECT;

    /* Asked right after the cycle's last step: the next cycle makes the
     * reserve the halt's cycles gave back again from the ground nearest the
     * volume's start, which may be this very block. */
    tagfs_close(reader);
    reader = NULL;
    TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle after the reader let go");
    TEST_ASSERT(!StandBlockInUse(block), "and the block goes back with it");
out:
    tagfs_close(reader);
    StandSettleLetGo(file_id);
    (void)TagFSSettle();
    return verdict;
}

/* The volume let go of and mounted again, with the runner's hold on its door
 * given back either way. True when it is up again. */
#define STAND_SETTLE_MOUNT_MS  10000u

static bool StandSettleRemount(void)
{
    TagFSState* fs     = tagfs_get_state();
    bool        let_go = TagFSStandLetGo();
    if (let_go) {
        uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_SETTLE_MOUNT_MS);
        TagFSAttendArrival();
        while (!(fs->initialized && TagFSStandMountIdle()) && rdtsc() < bound) {
            BoardroomAttendIfPending();
            TagFSServiceIfPending();
            BatonPump(amp_get_core_index());
            cpu_pause();
        }
    }
    TagFSStandHoldAgain();
    return let_go && fs->initialized;
}

/*
 * R26 (F02-2, F02-5). A program's change is of the mount it was made on. One
 * made on the volume mounted before owes this mount nothing; one made here is
 * what the program owes — though the old one's gen was the larger, since a
 * mount counts its Inks from the start again. And the new mount measures its
 * medium afresh: no cycle's cost carries over from the one before. Red before:
 * the larger gen of the mount before was kept, the program's change here was
 * never noted, and its leaving demanded nothing.
 */
static TestResult TestStandSettleRemountOwesItsOwn(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    file_id = 0;
    uint32_t    epoch   = tagfs_mount_epoch();
    bool        let_go  = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    memset(&s_stand_writer, 0, sizeof(s_stand_writer));
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x4Eu);
    TEST_ASSERT(StandMakeFile("settle_remount", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    uint64_t inked = TagFSInkedHere();
    TEST_ASSERT_EQ(StandWriteAt(file_id, 0, g_test_buffer, TAGFS_BLOCK_SIZE),
                   (int)TAGFS_BLOCK_SIZE, "a program's call writes it");
    TagFSNoteInkerSince(&s_stand_writer, inked);
    uint64_t gen_before = InkedGen();

    TEST_ASSERT(StandSettleRested(), "every change is on the medium, and nothing rests");
    let_go = true;
    TEST_ASSERT(StandSettleRemount(), "the volume is let go of and MOUNTED again");
    TEST_ASSERT_EQ(tagfs_mount_epoch(), epoch + 1u, "once");
    TEST_ASSERT_EQ(SettleLastDUs(), 0, "and no cycle's cost on its medium carries over");

    TagFSInkerLeaves(&s_stand_writer);
    TEST_ASSERT(!(TagFSStandSettleDemands() & SETTLE_DEMAND_LEAVE),
                "the program's change of the mount before is not owed here");
    inked = TagFSInkedHere();
    TEST_ASSERT_EQ(StandWriteAt(file_id, 0, g_test_buffer, TAGFS_BLOCK_SIZE),
                   (int)TAGFS_BLOCK_SIZE, "it writes the file on this mount");
    TestEvidence("the gen of its change here, below the mount before's", InkedGen());
    TEST_ASSERT(InkedGen() < gen_before, "with a gen below the one it wrote at before");
    TagFSNoteInkerSince(&s_stand_writer, inked);
    TagFSInkerLeaves(&s_stand_writer);
    TEST_ASSERT(TagFSStandSettleDemands() & SETTLE_DEMAND_LEAVE,
                "and leaves with that change unsettled: the next cycle is demanded");
out:
    if (let_go && !tagfs_get_state()->initialized)
        (void)StandSettleRemount();
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
    { .name = "stand_settle_refusal_waits_for_the_bell",
      .func = TestStandSettleRefusalWaitsForTheBell, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_ring_during_the_cycle", .func = TestStandSettleRingDuringTheCycle,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_refusal_waits_for_its_own_event",
      .func = TestStandSettleRefusalWaitsForItsOwnEvent, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_table_refusal_is_the_mediums",
      .func = TestStandSettleTableRefusalIsTheMediums, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_table_short_settles", .func = TestStandSettleTableShortSettles,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_reader_owes_no_leave", .func = TestStandSettleReaderOwesNoLeave,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_halt_leaves_no_viewed_ground",
      .func = TestStandSettleHaltLeavesNoViewedGround, .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a clean shutdown with a reader open on a deleted file leaves its blocks "
                       "taken on the medium and named by nothing, kept by the next mount for good" },
    /* Last: it takes the volume down and mounts it again. */
    { .name = "stand_settle_remount_owes_its_own", .func = TestStandSettleRemountOwesItsOwn,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_settle_suite = {
    .tests = g_stand_settle_tests,
    .count = sizeof(g_stand_settle_tests) / sizeof(g_stand_settle_tests[0]),
};

#endif /* CONFIG_VOLUME_TESTS */
