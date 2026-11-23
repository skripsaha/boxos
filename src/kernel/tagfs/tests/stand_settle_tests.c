#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../block_map/block_map.h"
#include "../mount/away.h"
#include "../settle/settle.h"
#include "../helm/helm.h"
#include "system_halt.h"
#include "amp.h"
#include "baton.h"
#include "atomics.h"
#include "boardroom.h"
#include "cpu_calibrate.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"      /* SCHEDULER_DEFAULT_TICK_HZ */
#include "block_fail.h"
#include "tagfs_volume.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The cadence: metadata settles on its own (step 1.7)
//
// The runner holds the cadence off for every other test (settle.h,
// SettleCadenceHold); these let it go where the cadence is what they ask about.
// A test runs at boot on the boot core with its interrupts still off, however
// many cores there are — and the boot core is the drain core, whose batons
// carry what its seat answers, and whose tick fires the Lull's node. So a test
// that waits for the cadence stands in for its own core's loop: it knocks as
// the tick would, runs the batons left there, and calls the service point. On
// one core the committer is this core, and the cycle runs in that call; on
// more it is a K-Core, which the knock wakes to look again, and its own loop
// runs the cycle.
// ============================================================================

static uint64_t StandNowUs(void)
{
    return cpu_tsc_to_us(rdtsc());
}

/* The file a test changes, given up and settled on the way out. */
static void StandSettleLetGo(TagFSVolume* vol, uint64_t file_id)
{
    if (file_id) {
        (void)tagfs_delete_file(vol, file_id, INK_UNWARRANTED);
        (void)TagFSSettle(vol);
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
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    uint64_t   file_id = 0;

    SettleCadenceHold(vol, false);
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5Au);
    TEST_ASSERT(StandMakeFile(vol, "settle_on_its_own", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written, and nothing anchors it");
    uint64_t gen   = InkedGen(vol);
    uint64_t began = StandNowUs();
    while (SettledGen(vol) < gen && StandNowUs() - began < 2u * SETTLE_WINDOW_US) {
        SettleKnock();
        BatonPump(amp_get_core_index());
        SettleServiceIfPending(vol);
        cpu_pause();
    }
    uint64_t took = StandNowUs() - began;
    TEST_ASSERT(SettledGen(vol) >= gen, "a cycle nobody asked for settled the change");
    TEST_ASSERT(took <= SETTLE_WINDOW_US + SettleLastDUs(vol),
                "within the window and the cycle's own time on the medium");
    TestEvidence("microseconds from the write to its settling:", took);
out:
    SettleCadenceHold(vol, true);
    StandSettleLetGo(vol, file_id);
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
    TagFSVolume* vol = TagFSHome();
    StandSettleWaiter* s = (StandSettleWaiter*)w;
    s->rc       = rc;
    s->cycle    = SettledCycles(vol);
    s->answered = true;
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
    TagFSVolume* vol = TagFSHome();
    TestResult verdict  = TEST_PASS;
    bool       one_core = g_amp.total_cores == 1;

    SettleCadenceHold(vol, false);
    TEST_ASSERT_OK(TagFSSettle(vol), "the volume owes nothing to begin with");
    TEST_ASSERT(!TagFSIdleOwed(vol), "with nothing owed, idle is owed no quantum");
    SettleDemand(vol, SETTLE_DEMAND_LEAVE);
    TEST_ASSERT_EQ(TagFSIdleOwed(vol), one_core,
                   "a demand makes idle the debtor on one core, and nowhere else");
    TEST_ASSERT_OK(TagFSSettle(vol), "a cycle takes the demand up");
    TEST_ASSERT(!TagFSIdleOwed(vol), "and idle is owed nothing after it");
out:
    SettleCadenceHold(vol, true);
    return verdict;
}

/*
 * RT-18. A medium that refuses a write as protected turns the volume read-only:
 * the cycle is refused, no change is taken any more — ERR_TAGFS_READ_ONLY —
 * and a waiter hears the same code rather than waiting for ever.
 */
static TestResult TestStandSettleWriteProtected(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult               verdict   = TEST_PASS;
    uint64_t                 file_id   = 0;
    uint8_t                  seat      = tagfs_get_seat(vol);
    bool                     protected_ = false;
    InkReceipt               receipt;
    static StandSettleWaiter waiter;

    memset(&waiter, 0, sizeof(waiter));
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x3Cu);
    TEST_ASSERT(StandMakeFile(vol, "settle_protected", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    SettleWaitLink(vol, &waiter.w, InkedGen(vol), StandSettleAnswered);

    BlockFailProtect(seat, true);
    protected_ = true;
    TEST_ASSERT(TagFSSettle(vol) != OK, "the cycle is refused by the protected medium");
    TEST_ASSERT(TagFSReadOnly(vol), "and the volume takes no write from here");
    TEST_ASSERT(waiter.answered, "the waiter is answered, not left waiting");
    TEST_ASSERT_EQ(waiter.rc, ERR_TAGFS_READ_ONLY, "with ERR_TAGFS_READ_ONLY");
    TEST_ASSERT_EQ(InkQuitclaim(vol, file_id, INK_UNWARRANTED, &receipt), ERR_TAGFS_READ_ONLY,
                   "an Ink is refused ERR_TAGFS_READ_ONLY");
    TEST_ASSERT_EQ(SettleWaitNow(vol, InkedGen(vol) + 1u), ERR_TAGFS_READ_ONLY,
                   "and so is a new wait for a cycle");
out:
    if (protected_) {
        BlockFailProtect(seat, false);
        InkReadOnlyLift(vol);
    }
    if (!waiter.answered)
        (void)SettleWaitUnlink(vol, &waiter.w);
    StandSettleLetGo(vol, file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A refused cycle waits for its event (R2), and what each step's refusal is (R8)
// ----------------------------------------------------------------------------

/* One turn of the machine, done by hand: the knock the tick would give, the
 * batons left on this core, the service point — the committer's own on one
 * core; on more, a K-Core's, which the knock wakes to look again. */
void StandSettleTurn(TagFSVolume* vol)
{
    SettleKnock();
    BatonPump(amp_get_core_index());
    SettleServiceIfPending(vol);
    cpu_pause();
}

/*
 * R2 on a book (Z01-2, Z10-1, Z10-6, F01-4). A write a book's medium refuses
 * is taken again on the ribbon (M-6), but a medium that says it is not ready
 * refuses the cycle (sewing.c, StitchRefused): the stand answers the stitch's
 * making so. It declares nothing, and the waiter stays in its Chit. A bell
 * the seat rang just before the stitch's TAKE took its count answered a
 * refusal before it, and is no event of this one, however late the room says
 * it. With the cadence let go, no second cycle begins before the seat says
 * seat:ready — watched for two Lulls and a tick, within which a cycle a waiter
 * asks for begins at once when nothing parks it. The seat then says
 * seat:ready, and the next cycle answers the waiter OK. Red with no bell rung
 * at the stitch's TAKE (TagFSStandSettleRings, sewing.c): the cycle's seat
 * never rings.
 */
static TestResult TestStandSettleStitchRefusalWaitsForTheBell(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult               verdict = TEST_PASS;
    uint8_t                  seat    = tagfs_get_seat(vol);
    uint64_t                 file_id = 0;
    bool                     let_go  = false;
    static StandSettleWaiter waiter;
    const char*              aside   = StandStepsAsideFor(vol);

    if (aside)
        return test_steps_aside(aside);

    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT_OK(TagFSSettle(vol), "the volume owes nothing to begin with");
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x6Du);
    TEST_ASSERT(StandMakeFile(vol, "settle_stitch_refused", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    SettleWaitLink(vol, &waiter.w, InkedGen(vol), StandSettleAnswered);

    uint32_t rang = BoardroomSeatReadyGen(seat);
    TagFSStandSettleRefuses(ERR_DEVICE_NOT_READY);
    TagFSStandSettleRings(true);
    TEST_ASSERT_EQ(TagFSSettleAsCadence(vol), ERR_DEVICE_NOT_READY,
                   "the cycle is refused as a medium that is not ready refuses it");
    TEST_ASSERT_EQ(BoardroomSeatReadyGen(seat), rang + 1u,
                   "its seat rang seat:ready just before the stitch's TAKE took its count");
    TEST_ASSERT(!waiter.answered, "the waiter stays in its Chit: no code of a step is its answer");
    TEST_ASSERT(SettleParked(vol), "the cadence waits for the refusal's event");
    BoardroomAttendIfPending();
    TEST_ASSERT(SettleParked(vol), "and a bell rung before the cycle took its count, said after, "
                "is not that event");

    uint64_t steps = SettleStepsTaken(vol);
    SettleCadenceHold(vol, false);
    let_go = true;
    uint64_t watch = 2u * SettleLullUs(vol) + 1000000ull / SCHEDULER_DEFAULT_TICK_HZ;
    uint64_t began = StandNowUs();
    while (StandNowUs() - began < watch)
        StandSettleTurn(vol);
    TestEvidence("steps of cycles begun before the event, of 0", SettleStepsTaken(vol) - steps);
    TEST_ASSERT_EQ(SettleStepsTaken(vol), steps, "the cadence began no second cycle before the event");
    TEST_ASSERT(!waiter.answered, "and the waiter is still in its Chit");

    /* Nightwatch's look, as the tick takes it a window later, and the look
     * after: a park its waiters have waited on longer than the window is said
     * once. No tick runs while the tests do. */
    SettleWatchLook(vol, StandNowUs() + SETTLE_WINDOW_US + 1u);
    SettleWatchLook(vol, StandNowUs() + SETTLE_WINDOW_US + 2u);
    BatonPump(amp_get_core_index());

    BoardroomNoteSeatReady(BoardroomSeatKind(seat), BoardroomSeatIndex(seat));
    BoardroomAttendIfPending();
    TEST_ASSERT(!SettleParked(vol), "the seat says seat:ready, which lifts the park");
    began = StandNowUs();
    while (!waiter.answered && StandNowUs() - began < 2u * SETTLE_WINDOW_US)
        StandSettleTurn(vol);
    TEST_ASSERT(waiter.answered, "the next cycle answered the waiter");
    TEST_ASSERT_OK(waiter.rc, "with OK");
out:
    TagFSStandSettleRefuses(OK);
    if (let_go)
        SettleCadenceHold(vol, true);
    if (!waiter.answered)
        (void)SettleWaitUnlink(vol, &waiter.w);
    StandSettleLetGo(vol, file_id);
    return verdict;
}

/*
 * X1-4 on a book. seat:ready that comes while a cycle runs — after the
 * stitch's TAKE took the count its refusal is judged by, before the refusal is
 * decided — is never lost: the cycle refused as a medium that is not ready
 * refuses it parks on nothing, and the next one may go at once. Red with no
 * bell rung at the stitch's TAKE (TagFSStandSettleRings, sewing.c): the
 * cycle's seat never rings, and the refusal parks.
 */
static TestResult TestStandSettleStitchRingDuringTheCycle(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult               verdict = TEST_PASS;
    uint8_t                  seat    = tagfs_get_seat(vol);
    uint64_t                 file_id = 0;
    static StandSettleWaiter waiter;
    const char*              aside   = StandStepsAsideFor(vol);

    if (aside)
        return test_steps_aside(aside);

    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT_OK(TagFSSettle(vol), "the volume owes nothing to begin with");
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x7Du);
    TEST_ASSERT(StandMakeFile(vol, "settle_stitch_rings", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    SettleWaitLink(vol, &waiter.w, InkedGen(vol), StandSettleAnswered);

    uint32_t rang = BoardroomSeatReadyGen(seat);
    TagFSStandSettleRefuses(ERR_DEVICE_NOT_READY);
    TagFSStandSettleRings(false);
    TEST_ASSERT_EQ(TagFSSettleAsCadence(vol), ERR_DEVICE_NOT_READY,
                   "the cycle is refused as a medium that is not ready refuses it");
    TEST_ASSERT_EQ(BoardroomSeatReadyGen(seat), rang + 1u,
                   "and its seat rang seat:ready while it ran, after the stitch's TAKE");
    TEST_ASSERT(!waiter.answered, "the waiter stays in its Chit");
    TEST_ASSERT(!SettleParked(vol), "the refusal parks on nothing: its event has come already");
    TEST_ASSERT_OK(TagFSSettleAsCadence(vol), "the next cycle goes, and settles");
    TEST_ASSERT(waiter.answered, "and answers the waiter");
    TEST_ASSERT_OK(waiter.rc, "OK");
out:
    TagFSStandSettleRefuses(OK);
    if (!waiter.answered)
        (void)SettleWaitUnlink(vol, &waiter.w);
    StandSettleLetGo(vol, file_id);
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
    TagFSVolume* vol = TagFSHome();
    TestResult               verdict = TEST_PASS;
    uint8_t                  seat    = tagfs_get_seat(vol);
    uint64_t                 file_id = 0;
    void*                    pages   = NULL;
    static StandSettleWaiter waiter;

    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT_OK(TagFSSettle(vol), "the volume owes nothing to begin with");
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x1Fu);
    TEST_ASSERT(StandMakeFile(vol, "settle_own_event", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    SettleWaitLink(vol, &waiter.w, InkedGen(vol), StandSettleAnswered);

    pages = PmmTakeExact(STAND_SETTLE_HEAP_PAGES);
    TEST_ASSERT(pages != NULL, "the stand holds pages of the machine's");
    TagFSStandSettleRefuses(ERR_NO_MEMORY);
    TEST_ASSERT_EQ(TagFSSettleAsCadence(vol), ERR_NO_MEMORY, "a cycle is refused for want of memory");
    TEST_ASSERT(SettleParked(vol), "and waits");
    BoardroomNoteSeatReady(BoardroomSeatKind(seat), BoardroomSeatIndex(seat));
    BoardroomAttendIfPending();
    TEST_ASSERT(SettleParked(vol), "seat:ready is not what it waits for");
    PmmGiveExact(pages, STAND_SETTLE_HEAP_PAGES);
    pages = NULL;
    TagFSHeapCameBack(vol);
    TEST_ASSERT(!SettleParked(vol), "the machine's memory coming back is");

    TagFSStandSettleRefuses(ERR_WOULD_BLOCK);
    TEST_ASSERT_EQ(TagFSSettleAsCadence(vol), ERR_WOULD_BLOCK,
                   "a cycle is refused with its ground on the way");
    TEST_ASSERT(SettleParked(vol), "and waits");
    BoardroomNoteSeatReady(BoardroomSeatKind(seat), BoardroomSeatIndex(seat));
    BoardroomAttendIfPending();
    TEST_ASSERT(SettleParked(vol), "seat:ready is not what it waits for");
    TEST_ASSERT(!waiter.answered, "the waiter stays in its Chit through both");
    SettleDemand(vol, SETTLE_DEMAND_ROOM);
    TEST_ASSERT(!SettleParked(vol), "room asked for is");

    TEST_ASSERT_OK(TagFSSettleAsCadence(vol), "the next cycle settles");
    TEST_ASSERT(waiter.answered, "and answers the waiter");
    TEST_ASSERT_OK(waiter.rc, "OK");
out:
    TagFSStandSettleRefuses(OK);
    if (pages)
        PmmGiveExact(pages, STAND_SETTLE_HEAP_PAGES);
    if (!waiter.answered)
        (void)SettleWaitUnlink(vol, &waiter.w);
    StandSettleLetGo(vol, file_id);
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
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    uint64_t   file_id = 0;

    memset(&s_stand_reader, 0, sizeof(s_stand_reader));
    memset(&s_stand_writer, 0, sizeof(s_stand_writer));
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x2Du);
    TEST_ASSERT(StandMakeFile(vol, "settle_reader", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    TEST_ASSERT_OK(TagFSSettle(vol), "and settled: no demand stands");
    TEST_ASSERT_EQ(StandWriteAt(vol, file_id, 0, g_test_buffer, TAGFS_BLOCK_SIZE),
                   (int)TAGFS_BLOCK_SIZE, "a change nobody here made is inked");
    TEST_ASSERT(InkedGen(vol) > SettledGen(vol), "and waits for its cycle");

    uint64_t inked = TagFSInkedHere(vol);
    TEST_ASSERT_EQ(TestFileRead(vol, file_id, g_test_output, TAGFS_BLOCK_SIZE), (int)TAGFS_BLOCK_SIZE,
                   "a program's call reads the file");
    TagFSNoteInkerSince(vol, &s_stand_reader, inked);
    TagFSInkerLeaves(vol, &s_stand_reader);
    TEST_ASSERT(!(TagFSStandSettleDemands(vol) & SETTLE_DEMAND_LEAVE),
                "the reader leaves, and demands no cycle");

    inked = TagFSInkedHere(vol);
    TEST_ASSERT_EQ(StandWriteAt(vol, file_id, 0, g_test_buffer, TAGFS_BLOCK_SIZE),
                   (int)TAGFS_BLOCK_SIZE, "a program's call writes the file");
    TagFSNoteInkerSince(vol, &s_stand_writer, inked);
    TagFSInkerLeaves(vol, &s_stand_writer);
    TEST_ASSERT(TagFSStandSettleDemands(vol) & SETTLE_DEMAND_LEAVE,
                "the writer leaves with its change unsettled, and demands the next cycle");
out:
    StandSettleLetGo(vol, file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A stopping machine's last cycles leave no ground at rest (RT-12)
// ----------------------------------------------------------------------------

/*
 * H1 (logcheck haltcycle). The drain's measure, fed what the drain saw on four
 * cores when it gave up on work that was moving: the committer's cycle ended,
 * the halt's own cycles took their steps with the work owed standing still,
 * and a writer's window took the medium's answers while it held its file's
 * Helm. Each of these moves the work as a new low does; a look at which none
 * of them happened does not. Red before: the halt's own steps counted for
 * nothing, nor did a write the medium answered, and the glass turned as the
 * committer's cycle ended ran out on the writer's window.
 */
static TestResult TestStandHaltDrainCountsWhatMoves(void)
{
    TestResult    verdict = TEST_PASS;
    HaltDrainSeen seen    = { .Best = UINT64_MAX, .Steps = UINT64_MAX, .Writes = UINT64_MAX };
    uint64_t      steps   = 4837;
    uint64_t      writes  = 900;

    TEST_ASSERT(HaltDrainMoved(&seen, 13, steps, writes), "the first look moves");
    TEST_ASSERT(!HaltDrainMoved(&seen, 13, steps, writes), "a look at which nothing moved does not");
    TEST_ASSERT(HaltDrainMoved(&seen, 12, steps, writes), "a new low moves");
    TEST_ASSERT(HaltDrainMoved(&seen, 12, steps + 1, writes),
                "a step of the committer's cycle moves, the work owed standing still");
    TEST_ASSERT(HaltDrainMoved(&seen, 1, steps + 4, writes + 2), "its cycle's end moves");
    TEST_ASSERT(!HaltDrainMoved(&seen, 1, steps + 4, writes + 2), "and then nothing moves");
    TEST_ASSERT(HaltDrainMoved(&seen, 1, steps + 30, writes + 2),
                "the steps of the halt's own cycles move, the work owed standing still");
    TEST_ASSERT(HaltDrainMoved(&seen, 2, steps + 30, writes + 3),
                "a write of the volume the medium answered moves, a window of a job that "
                "holds its file's Helm, the work owed above its low");
    TEST_ASSERT(!HaltDrainMoved(&seen, 2, steps + 30, writes + 3), "and then nothing moves");
    TEST_ASSERT(HaltDrainMoved(&seen, 0, steps + 30, writes + 3), "the job's end is a new low");
out:
    return verdict;
}

/* The volume let go of and mounted again, with the runner's hold on its door
 * given back either way. True when it is up again. */
#define STAND_SETTLE_MOUNT_MS  10000u

static bool StandSettleRemount(TagFSVolume* vol)
{
    TagFSState* fs     = &vol->state;
    bool        let_go = TagFSStandLetGo(vol);
    if (let_go) {
        uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_SETTLE_MOUNT_MS);
        TagFSAttendArrival(vol);
        while (!(fs->initialized && TagFSStandMountIdle(vol)) && rdtsc() < bound) {
            BoardroomAttendIfPending();
            TagFSServiceIfPending(vol);
            BatonPump(amp_get_core_index());
            cpu_pause();
        }
    }
    TagFSStandHoldAgain(vol);
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
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    uint64_t    file_id = 0;
    uint32_t    epoch   = tagfs_mount_epoch(vol);
    bool        let_go  = false;
    const char* aside   = StandStepsAsideFor(vol);

    if (aside)
        return test_steps_aside(aside);

    memset(&s_stand_writer, 0, sizeof(s_stand_writer));
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x4Eu);
    TEST_ASSERT(StandMakeFile(vol, "settle_remount", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    uint64_t inked = TagFSInkedHere(vol);
    TEST_ASSERT_EQ(StandWriteAt(vol, file_id, 0, g_test_buffer, TAGFS_BLOCK_SIZE),
                   (int)TAGFS_BLOCK_SIZE, "a program's call writes it");
    TagFSNoteInkerSince(vol, &s_stand_writer, inked);
    uint64_t gen_before = InkedGen(vol);

    TEST_ASSERT(StandSettledBare(vol), "every change is on the medium, and nothing rests");
    let_go = true;
    TEST_ASSERT(StandSettleRemount(vol), "the volume is let go of and MOUNTED again");
    TEST_ASSERT_EQ(tagfs_mount_epoch(vol), epoch + 1u, "once");
    TEST_ASSERT_EQ(SettleLastDUs(vol), 0, "and no cycle's cost on its medium carries over");

    TagFSInkerLeaves(vol, &s_stand_writer);
    TEST_ASSERT(!(TagFSStandSettleDemands(vol) & SETTLE_DEMAND_LEAVE),
                "the program's change of the mount before is not owed here");
    inked = TagFSInkedHere(vol);
    TEST_ASSERT_EQ(StandWriteAt(vol, file_id, 0, g_test_buffer, TAGFS_BLOCK_SIZE),
                   (int)TAGFS_BLOCK_SIZE, "it writes the file on this mount");
    TestEvidence("the gen of its change here, below the mount before's", InkedGen(vol));
    TEST_ASSERT(InkedGen(vol) < gen_before, "with a gen below the one it wrote at before");
    TagFSNoteInkerSince(vol, &s_stand_writer, inked);
    TagFSInkerLeaves(vol, &s_stand_writer);
    TEST_ASSERT(TagFSStandSettleDemands(vol) & SETTLE_DEMAND_LEAVE,
                "and leaves with that change unsettled: the next cycle is demanded");
out:
    if (let_go && !vol->state.initialized)
        (void)StandSettleRemount(vol);
    StandSettleLetGo(vol, file_id);
    return verdict;
}

static const TestCase g_stand_settle_tests[] = {
    { .name = "stand_settle_without_anchor",     .func = TestStandSettleWithoutAnchor,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a file written without anchor has no name after a power cut" },
    { .name = "stand_settle_idle_owed",          .func = TestStandSettleIdleOwed,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "on one core under a CPU hog nothing settles a write before the power goes" },
    { .name = "stand_settle_write_protected",    .func = TestStandSettleWriteProtected,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_stitch_refusal_waits_for_the_bell",
      .func = TestStandSettleStitchRefusalWaitsForTheBell, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_stitch_ring_during_the_cycle", .func = TestStandSettleStitchRingDuringTheCycle,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_refusal_waits_for_its_own_event",
      .func = TestStandSettleRefusalWaitsForItsOwnEvent, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_settle_reader_owes_no_leave", .func = TestStandSettleReaderOwesNoLeave,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_halt_drain_counts_what_moves",
      .func = TestStandHaltDrainCountsWhatMoves, .reach = TEST_READS_ONLY },
    /* Last: it takes the volume down and mounts it again. */
    { .name = "stand_settle_remount_owes_its_own", .func = TestStandSettleRemountOwesItsOwn,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_settle_suite = {
    .tests = g_stand_settle_tests,
    .count = sizeof(g_stand_settle_tests) / sizeof(g_stand_settle_tests[0]),
};

#endif /* CONFIG_VOLUME_TESTS */
