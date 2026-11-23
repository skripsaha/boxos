#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../helm/helm.h"
#include "../ink/ink.h"
#include "write_job.h"      /* WriteJobSubmit, and the stand's own answer record */
#include "block_fail.h"     /* BLOCKFAIL — a block's write the medium refuses */
#include "boardroom.h"
#include "ahci.h"           /* AhciPortTurn — an AHCI port's answers brought in by hand */
#include "xhci_interrupt.h" /* xhci_process_events — a USB disk's, the same way */
#include "xhci_msd.h"
#include "baton.h"
#include "amp.h"
#include "atomics.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// A program's write through the port (steps 1.5 and 1.5c)
//
// A program's write on a seat that takes commands nobody stands over goes
// through the port, the WriteJob parked on each command's answer — an AHCI
// port's slots, and since step 1.5c a USB disk's queue too, where it had only
// ever stood over every block on the drain core. These tests put a job on that
// road the way ObjWrite does, with the stand's own record in place of the
// program it would park (WriteJobStandAnswer), and hold it to what the road
// promises: its blocks went through the port to the end, it inked a window at
// a time, and a refused block's write part way answers the one contract every
// write answers — the bytes the file holds of it, and the size.
//
// The tests run before the machine is running: the boot core has not opened
// its interrupts, so what the tick, the interrupt and the K-Core loop would do
// is done here, by hand — the seat's answers brought in, the drain core's
// batons pumped, the service point run. The record is static: a job still
// running when a test gives up must not write into a frame that is gone.
// ============================================================================

/* The test's own bound on a job of three windows. A USB disk under emulation
 * answers a block in well under a millisecond; this turns a job that never
 * answers into a failure instead of a machine that stops. */
#define STAND_PORT_BOUND_MS 20000u

static WriteJobStandAnswer s_answer;

/* Why this machine has no port road for a program's write to walk: one core,
 * where the program stands in its call and no K-Core pumps, or a legacy
 * channel, which has no completion to be told about. NULL when it has one. */
const char* StandPortAside(TagFSVolume* vol, uint8_t seat)
{
    if (g_amp.total_cores <= 1)
        return "one core: a program's write stands in its call there, and no K-Core would "
               "pump a job through the port";
    BoardKind kind = BoardroomSeatKind(seat);
    if (kind != BOARD_AHCI && kind != BOARD_USB)
        return "the volume's seat is a legacy channel, whose processor moves every byte: "
               "no write goes through a port there";
    return StandStepsAsideFor(vol);
}

/* One turn of the machine the job continues in: the seat's answers brought in,
 * the drain core's batons pumped (an AHCI job's steps, a USB command's), and
 * the service point run (a USB job's steps). */
void StandPortTurn(TagFSVolume* vol, uint8_t seat)
{
    switch (BoardroomSeatKind(seat)) {
    case BOARD_AHCI:
        AhciPortTurn(BoardroomSeatIndex(seat));
        break;
    case BOARD_USB:
        xhci_process_events();
        xhci_msd_watchdog();
        break;
    default:
        break;
    }
    BatonPump(amp_get_core_index());
    HelmRunReady(vol);
}

static bool StandPortAnswered(TagFSVolume* vol, uint8_t seat, uint32_t bound_ms)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(bound_ms);
    for (;;) {
        if (__atomic_load_n(&s_answer.answered, __ATOMIC_ACQUIRE))
            return true;
        if (rdtsc() >= bound)
            return false;
        StandPortTurn(vol, seat);
        cpu_pause();
    }
}

/* The job for a write of `bytes` from `data` into the file, at `offset` or
 * appended, answered to the stand's record. */
static int StandPortSubmit(uint64_t file_id, const uint8_t* data, uint32_t bytes,
                           uint64_t offset, bool append)
{
    WriteJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    memset(&s_answer, 0, sizeof(s_answer));
    ask.kind          = WRITE_JOB_WRITE;
    ask.file_id       = file_id;
    ask.source.kernel = data;
    ask.source.bytes  = bytes;
    ask.offset        = offset;
    ask.append        = append;
    ask.stand         = &s_answer;
    return WriteJobSubmit(&ask, NULL, NULL);
}

/* Whether the job the submit answered for was taken: its answer is to come, or
 * it already came inside the call. Anything else is a refusal before any job
 * was made, and no answer will ever come. */
static bool StandPortTaken(int submit)
{
    return submit == ERR_WOULD_BLOCK || __atomic_load_n(&s_answer.answered, __ATOMIC_ACQUIRE);
}

// ----------------------------------------------------------------------------
// A program's write goes through the port, a window at a time
//
// Step 1.5c. On a USB disk every block of a program's write was stood over on
// the drain core — the one road the WriteJob had there — while the same write
// on an AHCI port parked its program. Now the job is chosen for the port on
// either seat when more than one core pumps (WriteJobSeatRunsAsync, the road
// the path line at the head of the writing tests names). Here a half-block
// file has two windows appended: the block it ends in is read through the port
// before the write is laid over it, every block of the write goes through the
// port, one Ink lands a window, and the file reads back whole.
// ----------------------------------------------------------------------------

static TestResult TestStandPortWriteGoesThroughThePort(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult     verdict = TEST_PASS;
    uint8_t        seat    = tagfs_get_seat(vol);
    uint64_t       file_id = 0;
    uint8_t*       data    = NULL;
    uint8_t*       expect  = NULL;
    uint8_t*       back    = NULL;
    bool           asked   = false;
    const uint32_t half    = TAGFS_BLOCK_SIZE / 2;
    const uint64_t W       = HelmWindowBytes(vol, 0);
    const uint32_t bytes   = (uint32_t)(2 * W);   /* windows of W - half, W and half */
    const uint32_t size    = half + bytes;
    const char*    aside   = StandPortAside(vol, seat);

    if (aside)
        return test_steps_aside(aside);

    data   = kmalloc(bytes);
    expect = kmalloc(size);
    back   = kmalloc(size);
    TEST_ASSERT(data && expect && back, "room for the write and for reading the file back");
    TestFillAt(expect, half, 0x4E1D0700u, 0);
    TestFillAt(data, bytes, 0x4E1D0701u, 0);
    memcpy(expect + half, data, bytes);
    TEST_ASSERT(StandMakeFile(vol, "test_file", expect, half, &file_id), "a half-block file is made");

    TEST_ASSERT(WriteJobSeatRunsAsync(seat),
                "a program's write on this seat goes through the port, as the path line said");

    uint64_t inked = InkedGen(vol);
    asked = StandPortTaken(StandPortSubmit(file_id, data, bytes, 0, true));
    TEST_ASSERT(asked, "the job is taken, its answer to come");
    TEST_ASSERT(StandPortAnswered(vol, seat, STAND_PORT_BOUND_MS), "and it answers");
    asked = false;
    TEST_ASSERT(s_answer.through_port, "its blocks went through the port to its end");
    TEST_ASSERT_OK(s_answer.rc, "the write is answered OK");
    TEST_ASSERT_EQ(s_answer.written, bytes, "with every byte of it the file's");
    TEST_ASSERT_EQ(s_answer.size, size, "and the size it grew to");
    TestEvidence("Inks the write of three windows made", InkedGen(vol) - inked);
    TEST_ASSERT(InkedGen(vol) - inked >= 3, "one Ink a window, each its own record");
    TEST_ASSERT_EQ(TestFileRead(vol, file_id, back, size), (int)size, "the file reads back");
    TEST_ASSERT_EQ(TestFirstDifference(back, expect, size), size,
                   "its first half-block kept, and the write laid after it");
out:
    if (asked && !StandPortAnswered(vol, seat, STAND_PORT_BOUND_MS)) {
        data    = NULL;   /* the job may still take its bytes from here: left to it */
        file_id = 0;      /* ... and it holds the file's Helm */
    }
    if (file_id)
        tagfs_delete_file(vol, file_id, INK_UNWARRANTED);
    kfree(data);
    kfree(expect);
    kfree(back);
    return verdict;
}

static const TestCase g_stand_port_tests[] = {
    { .name  = "stand_port_write_goes_through_the_port",
      .func  = TestStandPortWriteGoesThroughThePort, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_port_suite = {
    .tests = g_stand_port_tests, .count = ARRAY_SIZE(g_stand_port_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
