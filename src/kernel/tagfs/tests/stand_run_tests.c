#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../run/run.h"     /* TagFSRunBlocks, TagFSDataCost — the seat's run, and the trace */
#include "../file_map/file_map.h"  /* the file's record, its runs walked in it */
#include "../mount/book_mount.h" /* a book's data run (g_book) */
#include "write_job.h"      /* WriteJobSubmit, and the stand's own answer record */
#include "block_fail.h"     /* BLOCKFAIL — the commands counted at the door, and one refused */
#include "atomics.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The data in runs (step 1.11)
//
// A file's write went to the medium a block a command on both roads, so ten
// megabytes on a flash drive that takes sixteen blocks in one command were two
// thousand five hundred and sixty commands. Now a write goes out a stretch a
// command: the part of it that lies in one run on the medium, as much of it as
// the seat takes at once (BoardroomSeatRun). These tests hold both roads — the
// write standing in its call, and the write through the port — to what that
// promises: a megabyte that lies in one run goes out in ceil(256 / N) commands
// on a seat that takes N blocks in one, counted at the Boardroom's door and by
// the trace the performance dump reads; and a medium that refuses the k-th
// block of a run answers exactly the k-1 blocks before it as landed, each
// under its own digest, and nothing from it on.
// ============================================================================

#define STAND_RUN_BYTES  (1024u * 1024u)
#define STAND_RUN_BLOCKS (STAND_RUN_BYTES / TAGFS_BLOCK_SIZE)

/* The test's own bound on a job through the port: a megabyte on an emulated
 * disk answers in well under a second, and this turns a job that never
 * answers into a failure instead of a machine that stops. */
#define STAND_RUN_BOUND_MS 20000u

/* What a write answered, on either road: the one contract every write answers
 * (W-ERR) — the reason, the bytes of the write the file's inked record holds,
 * the size the file is — and whether it went through the port. */
typedef struct {
    error_t  rc;
    uint64_t written;
    uint64_t size;
    bool     through_port;
} StandRunAnswer;

/* The port road's record. Static: a job still running when a test gives up
 * must not write into a frame that is gone. */
static WriteJobStandAnswer s_port_answer;

/* Why the road cannot be walked on this machine, or NULL. */
static const char *StandRunAside(bool port)
{
    return port ? StandPortAside(tagfs_get_seat()) : StandStepsAsideFor();
}

/* A file of a megabyte of zeros, laid by one span — one reservation, so the
 * volume gives it one run when it has one that long. */
static bool StandRunMake(uint64_t *out_file_id)
{
    uint64_t file_id = 0;

    *out_file_id = 0;
    if (tagfs_create_file("test_file", NULL, 0, &file_id) != 0)
        return false;
    if (tagfs_span_file(file_id, STAND_RUN_BYTES) != OK) {
        tagfs_delete_file(file_id, INK_UNWARRANTED);
        return false;
    }
    *out_file_id = file_id;
    return true;
}

/* Whether the file's megabyte lies in one run on the medium, read off its
 * record — never off the blocks, which would put them in a read-ahead window:
 * *out_first is the run's first block. */
static bool StandRunLiesInOneRun(uint64_t file_id, uint64_t *out_first)
{
    uint32_t  got = 0;
    uint64_t  end = 0;
    bool      one = true;
    StripWalk walk;
    Strip     run;

    if (!tagfs_enter())
        return false;
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, file_id, NULL);
    if (rec) {
        StripWalkBegin(rec, &walk);
        while (got < STAND_RUN_BLOCKS && one && StripWalkNext(&walk, &run)) {
            if (got == 0)
                *out_first = run.start;
            else
                one = run.start == end;
            end  = run.start + run.count;
            got += run.count;
        }
    }
    FileRecordRelease(rec);
    tagfs_leave();
    return one && got >= STAND_RUN_BLOCKS;
}

/* A program's write of `bytes` from `data` at the file's start — standing in
 * its call, or put on the port the way ObjWrite puts it, with the stand's
 * record in place of the program it would park — and whether it answered.
 * *out_running: the job is still out, and still holds the file and takes its
 * bytes from `data`. */
static bool StandRunWrite(bool port, uint64_t file_id, const uint8_t *data, uint32_t bytes,
                          StandRunAnswer *out, bool *out_running)
{
    uint64_t    stats[2] = { UINT64_MAX, UINT64_MAX };
    Crate       crate    = { .addr = (uint64_t)(uintptr_t)stats, .size = 0,
                             .capacity = sizeof(stats) };
    WriteJobAsk ask;

    memset(&ask, 0, sizeof(ask));
    memset(out, 0, sizeof(*out));
    *out_running      = false;
    ask.kind          = WRITE_JOB_WRITE;
    ask.file_id       = file_id;
    ask.source.kernel = data;
    ask.source.bytes  = bytes;

    if (!port) {
        ask.out_crate = &crate;
        out->rc       = WriteJobSubmit(&ask, NULL, NULL);
        out->written  = stats[0];
        out->size     = stats[1];
        return crate.size == sizeof(stats);
    }

    memset(&s_port_answer, 0, sizeof(s_port_answer));
    ask.stand  = &s_port_answer;
    int submit = WriteJobSubmit(&ask, NULL, NULL);
    if (submit != ERR_WOULD_BLOCK && !__atomic_load_n(&s_port_answer.answered, __ATOMIC_ACQUIRE))
        return false;   /* refused before a job was made: no answer will come */

    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_RUN_BOUND_MS);
    while (!__atomic_load_n(&s_port_answer.answered, __ATOMIC_ACQUIRE)) {
        if (rdtsc() >= bound) {
            *out_running = true;
            return false;
        }
        StandPortTurn(tagfs_get_seat());
        cpu_pause();
    }
    out->rc           = s_port_answer.rc;
    out->written      = s_port_answer.written;
    out->size         = s_port_answer.size;
    out->through_port = s_port_answer.through_port;
    return true;
}

/* Where the first byte that is not zero is, or `n`. */
static uint32_t StandRunFirstNonZero(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (p[i])
            return i;
    }
    return n;
}

// ----------------------------------------------------------------------------
// A megabyte in one run goes out in as few commands as the seat takes
//
// The design's count: a megabyte written aligned over a file that lies in one
// run on the medium, on a seat that takes N blocks in one command, is
// ceil(256 / N) data commands — sixteen on a flash drive, eight on a SATA
// port — where it was two hundred and fifty-six. Counted at the Boardroom's
// door (BLOCKFAIL armed over the file's sectors, refusing none), and by the
// trace the performance dump reads, which must agree with the door. Each
// block still has its own digest: the file reads back through them whole. A
// book lays a write over a file on fresh ground (§3.6), so there BLOCKFAIL
// counts over the whole data run — no other write of the volume goes out
// while the cadence is held — and the run counted is the one the file lies in
// after the write.
// ----------------------------------------------------------------------------

static TestResult StandRunMegabyte(bool port)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        seat    = tagfs_get_seat();
    uint64_t       file_id = 0;
    uint64_t       first   = 0;
    uint8_t       *data    = NULL;
    uint8_t       *back    = NULL;
    bool           armed   = false;
    bool           running = false;
    const uint32_t N       = TagFSRunBlocks();
    const uint32_t want    = (STAND_RUN_BLOCKS + N - 1) / N;
    const bool     book    = tagfs_get_state()->format == DEED_FORMAT_V2;
    StandRunAnswer answer;
    TagFSDataCost  before, after;
    const char    *aside   = StandRunAside(port);

    if (aside)
        return test_steps_aside(aside);

    data = kmalloc(STAND_RUN_BYTES);
    back = kmalloc(STAND_RUN_BYTES);
    TEST_ASSERT(data && back, "room for the megabyte and for reading it back");
    TestFillAt(data, STAND_RUN_BYTES, 0x4E1D0B00u + (port ? 1u : 0u), 0);
    TEST_ASSERT(StandRunMake(&file_id), "a file of a megabyte is made by one span");
    if (!StandRunLiesInOneRun(file_id, &first)) {
        verdict = test_steps_aside("the volume gave the megabyte more than one run, and a "
                                   "write across runs goes out a command a run");
        goto out;
    }
    TEST_ASSERT(!port || WriteJobSeatRunsAsync(seat),
                "a program's write on this seat goes through the port, as the path line said");

    if (book)
        BlockFailArm(seat, BLOCK_FAIL_WRITE, tagfs_block_to_sector(g_book.frame.data_block),
                     (g_book.data_end - g_book.frame.data_block) * TAGFS_BLOCK_SECTORS, BLOCK_FAIL_NEVER);
    else
        BlockFailArm(seat, BLOCK_FAIL_WRITE, tagfs_block_to_sector(first),
                     (uint64_t)STAND_RUN_BLOCKS * TAGFS_BLOCK_SECTORS, BLOCK_FAIL_NEVER);
    armed = true;
    TagFSDataCostTake(&before);
    bool answered = StandRunWrite(port, file_id, data, STAND_RUN_BYTES, &answer, &running);
    TagFSDataCostTake(&after);
    uint32_t seen = BlockFailSeen();
    BlockFailDisarm();
    armed = false;

    TEST_ASSERT(answered, "the write answers");
    TEST_ASSERT_EQ(answer.through_port, port, "on the road it was put on, to its end");
    TEST_ASSERT_OK(answer.rc, "the write is answered OK");
    TEST_ASSERT_EQ(answer.written, STAND_RUN_BYTES, "with every byte of it the file's");
    TEST_ASSERT_EQ(answer.size, STAND_RUN_BYTES, "and the size it was");
    if (book && !StandRunLiesInOneRun(file_id, &first)) {
        verdict = test_steps_aside("the book laid the megabyte's fresh ground in more than one run, and a "
                                   "write across runs goes out a command a run");
        goto out;
    }
    TestEvidence("blocks the seat takes in one command", N);
    TestEvidence("medium commands the megabyte went out in", seen);
    TEST_ASSERT_EQ(seen, want, "ceil(256 / the blocks the seat takes in one command)");
    TEST_ASSERT_EQ(after.commands - before.commands, want,
                   "and the trace the performance dump reads counts the same commands");
    TEST_ASSERT_EQ(after.bytes - before.bytes, STAND_RUN_BYTES, "... the megabyte's bytes");
    TEST_ASSERT_EQ(after.writes - before.writes, 1, "... and one write");

    TEST_ASSERT_EQ(TestFileRead(file_id, back, STAND_RUN_BYTES), (int)STAND_RUN_BYTES,
                   "the file reads back, every block checked against its own digest");
    TEST_ASSERT_EQ(TestFirstDifference(back, data, STAND_RUN_BYTES), STAND_RUN_BYTES,
                   "byte for byte");
out:
    if (running) {
        data    = NULL;   /* the job may still take its bytes from here: left to it */
        file_id = 0;      /* ... and it holds the file's Helm */
    }
    if (armed)
        BlockFailDisarm();
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    kfree(data);
    kfree(back);
    return verdict;
}

// ----------------------------------------------------------------------------
// The k-th block of a run refused answers exactly the k-1 before it
//
// W-ERR, inside a run. When every block went out alone, a refused block's
// write was a command of its own and the blocks before it had landed by their
// own. Now they go out in one command with it, and a medium that names the
// sector it failed at keeps what came before (BLOCKFAIL refuses the way one
// does): the answer counts exactly the k-1 blocks of the run before the
// refused one, their digests are recorded, and the refused block and every
// one after it hold what they held.
// ----------------------------------------------------------------------------

static TestResult StandRunRefusedInside(bool port)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        seat    = tagfs_get_seat();
    uint64_t       file_id = 0;
    uint64_t       first   = 0;
    uint8_t       *data    = NULL;
    uint8_t       *back    = NULL;
    bool           armed   = false;
    bool           running = false;
    const uint32_t B       = TAGFS_BLOCK_SIZE;
    const uint32_t N       = TagFSRunBlocks();
    const uint32_t k       = N / 2 + 1;   /* the refused block's place in the run, from 1 */
    const uint32_t kept    = (k - 1) * B;
    StandRunAnswer answer;
    const char    *aside   = StandRunAside(port);

    if (aside)
        return test_steps_aside(aside);
    if (N < 2)
        return test_steps_aside("the seat takes one block in a command: a run has no inside");

    data = kmalloc(STAND_RUN_BYTES);
    back = kmalloc(STAND_RUN_BYTES);
    TEST_ASSERT(data && back, "room for the megabyte and for reading it back");
    TestFillAt(data, STAND_RUN_BYTES, 0x4E1D0C00u + (port ? 1u : 0u), 0);
    TEST_ASSERT(StandRunMake(&file_id), "a file of a megabyte is made by one span");
    if (!StandRunLiesInOneRun(file_id, &first)) {
        verdict = test_steps_aside("the volume gave the megabyte more than one run, so its "
                                   "first run may end before the refused block");
        goto out;
    }

    BlockFailArm(seat, BLOCK_FAIL_WRITE, tagfs_block_to_sector(first + k - 1),
                 TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    bool answered = StandRunWrite(port, file_id, data, STAND_RUN_BYTES, &answer, &running);
    bool fired    = BlockFailFired();
    uint32_t seen = BlockFailSeen();
    BlockFailDisarm();
    armed = false;

    TEST_ASSERT(answered, "the write answers");
    TEST_ASSERT_EQ(answer.through_port, port, "on the road it was put on, to its end");
    TEST_ASSERT(fired, "the k-th block of the first run was the one refused");
    TEST_ASSERT_EQ(seen, 1, "by the first command that met it: the run's own");
    TEST_ASSERT(answer.rc != OK, "the write answers the refusal");
    TestEvidence("blocks the refused run answered as landed", answer.written / B);
    TEST_ASSERT_EQ(answer.written, kept, "exactly the k-1 blocks of the run before the refused one");
    TEST_ASSERT_EQ(answer.size, STAND_RUN_BYTES, "and the size the file still is");

    TEST_ASSERT_EQ(TestFileRead(file_id, back, STAND_RUN_BYTES), (int)STAND_RUN_BYTES,
                   "the file reads back, every block checked against its own digest");
    TEST_ASSERT_EQ(TestFirstDifference(back, data, kept), kept,
                   "the blocks before the refused one hold the write");
    TEST_ASSERT_EQ(StandRunFirstNonZero(back + kept, STAND_RUN_BYTES - kept),
                   STAND_RUN_BYTES - kept,
                   "and the refused block and every one after it the zeros they held");
out:
    if (running) {
        data    = NULL;
        file_id = 0;
    }
    if (armed)
        BlockFailDisarm();
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    kfree(data);
    kfree(back);
    return verdict;
}

static TestResult TestStandRunMegabyteInSeatRuns(void)
{
    return StandRunMegabyte(false);
}

static TestResult TestStandRunMegabyteThroughThePort(void)
{
    return StandRunMegabyte(true);
}

static TestResult TestStandRunRefusedInsideARun(void)
{
    return StandRunRefusedInside(false);
}

static TestResult TestStandRunRefusedInsideARunThroughThePort(void)
{
    return StandRunRefusedInside(true);
}

static const TestCase g_stand_run_tests[] = {
    { .name  = "stand_run_megabyte_in_seat_runs", .func = TestStandRunMegabyteInSeatRuns,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_run_megabyte_through_the_port",
      .func  = TestStandRunMegabyteThroughThePort, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_run_refused_inside_a_run", .func = TestStandRunRefusedInsideARun,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_run_refused_inside_a_run_through_the_port",
      .func  = TestStandRunRefusedInsideARunThroughThePort, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_run_suite = {
    .tests = g_stand_run_tests, .count = ARRAY_SIZE(g_stand_run_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
