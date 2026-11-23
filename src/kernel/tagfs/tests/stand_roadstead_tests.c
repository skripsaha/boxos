#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "boardroom.h"
#include "roadstead.h"
#include "block_fail.h"   /* BLOCKFAIL — a read nobody stands over, refused */
#include "ahci.h"
#include "pmm.h"
#include "vmm.h"
#include "atomics.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The roadstead — a read that finds the medium busy waits for its turn
//
// A command that found every place on a medium taken used to be refused on
// the spot (ERR_BUSY, which the storage deck turned into ERR_IO for the
// program), or spun on. Now it moors in the medium's roadstead
// (core/boardroom/roadstead.h) and is called in when a place comes free. Each
// test here takes the places itself — the only slot of a port narrowed to one,
// or the whole port as a cache flush takes it — and asks for reads that must
// be taken, wait, and come back with the bytes an attended read brings.
//
// Every test reads sector 0 of the volume's seat and writes nothing.
// ============================================================================

/* The test's own bound on a wait that behaves in a few turns — it turns the
 * defect it stands for into a failure instead of a machine that stops. */
#define STAND_ROADSTEAD_BOUND_MS 2000u

typedef struct {
    RoadsteadBerth    berth;     /* the seat's from the submit until the answer */
    void*             dma;
    bool              taken;     /* the room took the read: its answer will come */
    volatile uint32_t answered;
    volatile error_t  status;
} StandRoadsteadRead;

static StandRoadsteadRead s_reads[2];

static void StandRoadsteadAnswered(uint8_t index, uint8_t slot, error_t status, void* ctx)
{
    (void)index;
    (void)slot;
    StandRoadsteadRead* r = (StandRoadsteadRead*)ctx;
    r->status = status;
    __atomic_store_n(&r->answered, 1u, __ATOMIC_RELEASE);
}

/* The AHCI port under the volume, or a reason there is none to ask. */
static const char* StandRoadsteadPort(TagFSVolume* vol, uint8_t* out_port)
{
    uint8_t seat = tagfs_get_seat(vol);
    if (BoardroomSeatKind(seat) != BOARD_AHCI)
        return "the volume's seat is not an AHCI port";
    *out_port = BoardroomSeatIndex(seat);
    return NULL;
}

/* A fresh read record with its page, answered by nobody yet. */
static bool StandRoadsteadReadReady(StandRoadsteadRead* r)
{
    memset(r, 0, sizeof(*r));
    r->dma = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    return r->dma != NULL;
}

static uint32_t StandRoadsteadAnswers(uint32_t n)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++)
        count += __atomic_load_n(&s_reads[i].answered, __ATOMIC_ACQUIRE);
    return count;
}

/* The read `r` asked of the seat, and taken when the room says so — by its
 * answer, never by the berth: the room fills the berth before it hands the
 * read on, taken or not. */
static error_t StandRoadsteadAsk(uint8_t seat, StandRoadsteadRead* r)
{
    error_t rc = BoardroomReadAsync(seat, 0, TAGFS_BLOCK_SECTORS, r->dma,
                                    StandRoadsteadAnswered, r, &r->berth);
    r->taken = rc == OK;
    return rc;
}

/* The reads of the first `n` the seat took. */
static uint32_t StandRoadsteadTaken(uint32_t n)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++)
        count += s_reads[i].taken;
    return count;
}

/* Wait for every read of the first `n` the seat took to be answered, turning
 * the port by hand so the wait depends on no other core and no interrupt. */
static bool StandRoadsteadAwait(uint8_t port, uint32_t n)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_ROADSTEAD_BOUND_MS);
    while (StandRoadsteadAnswers(n) < StandRoadsteadTaken(n)) {
        if (rdtsc() >= bound)
            return false;
        AhciPortTurn(port);
        cpu_pause();
    }
    return true;
}

/* A read that was taken and never answered owns its page — the medium may
 * still write into it — so the page is left to it. */
static void StandRoadsteadRelease(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        StandRoadsteadRead* r = &s_reads[i];
        if (r->dma && (!r->taken || r->answered))
            pmm_free(r->dma, 1);
        r->dma = NULL;
    }
}

/* The port narrowed to its one lowest slot, once it is quiet. The mask it had,
 * or 0 when it would not go quiet within the bound. */
static uint32_t StandRoadsteadNarrow(uint8_t port)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_ROADSTEAD_BOUND_MS);
    for (;;) {
        ahci_port_t* state = ahci_get_port_state(port);
        if (!state)
            return 0;
        uint32_t had = AhciPortStandNarrow(port, state->slot_mask & (0u - state->slot_mask));
        if (had || rdtsc() >= bound)
            return had;
        AhciPortTurn(port);
        cpu_pause();
    }
}

/* A port without NCQ has exactly one slot, and on it any two commands meet
 * (N7). Two reads asked for while a caller stands over that slot are both
 * taken — never refused — wait, and are answered in turn with the bytes the
 * attended read brought. */
static TestResult TestStandRoadsteadOneSlotTwoReads(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat(vol);
    uint8_t     port    = 0;
    uint32_t    had     = 0;
    int         held    = -1;
    const char* none    = StandRoadsteadPort(vol, &port);

    if (none)
        return test_steps_aside(none);

    memset(s_reads, 0, sizeof(s_reads));
    TEST_ASSERT_EQ(BoardroomRead(seat, 0, TAGFS_BLOCK_SECTORS, g_test_output), 0,
                   "the attended read of the block both reads ask for");
    TEST_ASSERT(StandRoadsteadReadReady(&s_reads[0]) && StandRoadsteadReadReady(&s_reads[1]),
                "a page for each read");

    had = StandRoadsteadNarrow(port);
    TEST_ASSERT(had != 0, "the port went quiet and was narrowed to one slot");
    held = AhciStandForSlot(port);
    TEST_ASSERT(held >= 0, "a caller stands over the port's only slot");

    for (uint32_t i = 0; i < 2; i++) {
        TEST_ASSERT_OK(StandRoadsteadAsk(seat, &s_reads[i]),
                       "a read that finds the only slot taken is taken, not refused");
    }
    TEST_ASSERT_EQ(StandRoadsteadAnswers(2), 0, "and neither is answered while the slot is held");

    AhciLeaveSlot(port, (uint8_t)held);
    held = -1;
    TEST_ASSERT(StandRoadsteadAwait(port, 2), "both are answered once the slot comes free");
    for (uint32_t i = 0; i < 2; i++) {
        TEST_ASSERT_OK(s_reads[i].status, "each answered by the medium, not refused");
        TEST_ASSERT_EQ(TestFirstDifference(vmm_phys_to_virt((uintptr_t)s_reads[i].dma),
                                           g_test_output, TAGFS_BLOCK_SIZE),
                       TAGFS_BLOCK_SIZE, "with the bytes the attended read brought");
    }
out:
    if (held >= 0)
        AhciLeaveSlot(port, (uint8_t)held);
    (void)StandRoadsteadAwait(port, 2);
    if (had)
        AhciPortStandNarrow(port, had);
    StandRoadsteadRelease(2);
    return verdict;
}

/* A cache flush takes the whole port (AHCI 1.3.1 §1.7), and a read asked for
 * while it holds the door used to be refused — the read beside an anchor that
 * a program saw fail. It is taken, waits for the door, and is answered with
 * the bytes the attended read brought. */
static TestResult TestStandRoadsteadReadWaitsForTheDoor(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat(vol);
    uint8_t     port    = 0;
    uint8_t     slot    = 0;
    bool        door    = false;
    const char* none    = StandRoadsteadPort(vol, &port);

    if (none)
        return test_steps_aside(none);

    memset(s_reads, 0, sizeof(s_reads));
    TEST_ASSERT_EQ(BoardroomRead(seat, 0, TAGFS_BLOCK_SECTORS, g_test_output), 0,
                   "the attended read of the block the read asks for");
    TEST_ASSERT(StandRoadsteadReadReady(&s_reads[0]), "a page for the read");

    TEST_ASSERT_EQ(ahci_port_take_whole(port, CONFIG_AHCI_IO_TIMEOUT_MS, &slot), AHCI_TAKE_GOT,
                   "the whole port is taken, as a cache flush takes it");
    door = true;
    TEST_ASSERT_OK(StandRoadsteadAsk(seat, &s_reads[0]),
                   "a read that finds the door shut is taken, not refused");
    TEST_ASSERT_EQ(StandRoadsteadAnswers(1), 0, "and is not answered while the door is shut");

    ahci_port_give_back(port);
    door = false;
    TEST_ASSERT(StandRoadsteadAwait(port, 1), "it is answered once the door opens");
    TEST_ASSERT_OK(s_reads[0].status, "by the medium, not refused");
    TEST_ASSERT_EQ(TestFirstDifference(vmm_phys_to_virt((uintptr_t)s_reads[0].dma),
                                       g_test_output, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "with the bytes the attended read brought");
out:
    if (door)
        ahci_port_give_back(port);
    (void)StandRoadsteadAwait(port, 1);
    StandRoadsteadRelease(1);
    return verdict;
}

/* The order the roadstead keeps (N5): served as moored; a flush at the head
 * wants the whole port, and says so, for as long as it is at the head; a
 * command a fault interrupted and the port proved innocent goes back in
 * front of it. */
static TestResult TestStandRoadsteadFlushWantsTheWholePort(void)
{
    TestResult     verdict = TEST_PASS;
    Roadstead      r;
    RoadsteadBerth read1, flush, read2, innocent;

    memset(&r, 0, sizeof(r));
    memset(&read1, 0, sizeof(read1));
    memset(&flush, 0, sizeof(flush));
    memset(&read2, 0, sizeof(read2));
    memset(&innocent, 0, sizeof(innocent));
    read1.want = read2.want = innocent.want = BERTH_ONE;
    flush.want = BERTH_WHOLE;

    RoadsteadMoor(&r, &read1);
    RoadsteadMoor(&r, &flush);
    RoadsteadMoor(&r, &read2);
    TEST_ASSERT(RoadsteadHead(&r) == &read1, "served in the order moored");
    TEST_ASSERT(!r.door_wanted, "a read at the head wants no door");

    RoadsteadCastOff(&r);
    TEST_ASSERT(RoadsteadHead(&r) == &flush, "the flush comes next");
    TEST_ASSERT(r.door_wanted, "and at the head it wants the whole port");

    RoadsteadMoorFirst(&r, &innocent);
    TEST_ASSERT(RoadsteadHead(&r) == &innocent, "an innocent command goes back in front");
    TEST_ASSERT(!r.door_wanted, "and the door is not wanted while it is at the head");

    RoadsteadCastOff(&r);
    TEST_ASSERT(r.door_wanted, "the flush at the head again wants the door again");
    RoadsteadCastOff(&r);
    TEST_ASSERT(RoadsteadHead(&r) == &read2 && !r.door_wanted,
                "and the read behind it is served after it");
    RoadsteadCastOff(&r);
    TEST_ASSERT(RoadsteadIsEmpty(&r) && r.tail == NULL, "and nothing is left");
out:
    return verdict;
}

/*
 * The bell on the road nobody stands on (B2). A read nobody stands over that
 * the medium refuses, then one it answers: the seat counts seat:ready once,
 * for the refusal — the room hears a command's answer on this road as it does
 * on the attended one, because it stands between every seat and the caller.
 * Red before: the room never saw an answer on this road at all, and a seat
 * busy only with programs' reads never rang.
 */
static TestResult TestStandRoadsteadRefusalRingsOnce(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat(vol);
    uint8_t     port    = 0;
    bool        armed   = false;
    const char* none    = StandRoadsteadPort(vol, &port);

    if (none)
        return test_steps_aside(none);

    memset(s_reads, 0, sizeof(s_reads));
    TEST_ASSERT(StandRoadsteadReadReady(&s_reads[0]) && StandRoadsteadReadReady(&s_reads[1]),
                "a page for each read");

    uint32_t gen = BoardroomSeatReadyGen(seat);
    BlockFailArm(seat, BLOCK_FAIL_READ, 0, TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    TEST_ASSERT_OK(StandRoadsteadAsk(seat, &s_reads[0]), "a read nobody stands over is taken");
    TEST_ASSERT(StandRoadsteadAwait(port, 1), "and answered");
    TEST_ASSERT(BlockFailFired(), "by BLOCKFAIL, in the medium's place");
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT_EQ(s_reads[0].status, ERR_IO, "with a refusal");
    TEST_ASSERT_EQ(BoardroomSeatReadyGen(seat), gen, "which rings nothing");

    TEST_ASSERT_OK(StandRoadsteadAsk(seat, &s_reads[1]), "the next read nobody stands over");
    TEST_ASSERT(StandRoadsteadAwait(port, 2), "is answered");
    TEST_ASSERT_OK(s_reads[1].status, "by the medium");
    TEST_ASSERT_EQ(BoardroomSeatReadyGen(seat), gen + 1u,
                   "and the seat counts seat:ready once, for the refusal before it");
out:
    if (armed)
        BlockFailDisarm();
    (void)StandRoadsteadAwait(port, 2);
    StandRoadsteadRelease(2);
    return verdict;
}

static const TestCase g_stand_roadstead_tests[] = {
    { .name = "stand_roadstead_one_slot_two_reads", .func = TestStandRoadsteadOneSlotTwoReads,
      .reach = TEST_READS_ONLY },
    { .name = "stand_roadstead_read_waits_for_the_door",
      .func = TestStandRoadsteadReadWaitsForTheDoor, .reach = TEST_READS_ONLY },
    { .name = "stand_roadstead_flush_wants_the_whole_port",
      .func = TestStandRoadsteadFlushWantsTheWholePort, .reach = TEST_READS_ONLY },
    { .name = "stand_roadstead_refusal_rings_once",
      .func = TestStandRoadsteadRefusalRingsOnce, .reach = TEST_READS_ONLY },
};

const TestSuite g_stand_roadstead_suite = {
    .tests = g_stand_roadstead_tests, .count = ARRAY_SIZE(g_stand_roadstead_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
