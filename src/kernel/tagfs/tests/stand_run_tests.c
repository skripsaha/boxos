#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../run/run.h"     /* TagFSRunBlocks, TagFSDataCost — the seat's run, and the trace */
#include "../file_map/file_map.h"  /* the file's record, its runs walked in it */
#include "../mount/book_mount.h" /* a book's data run (vol->book) */
#include "../helm/helm.h"   /* HelmWindowBytes — the run a write lays in one command */
#include "../block_map/block_map_derived.h" /* the run of free ground a book's write takes */
#include "write_job.h"      /* WriteJobSubmit, and the stand's own answer record */
#include "block_fail.h"     /* BLOCKFAIL — the commands counted at the door, and one refused */
#include "atomics.h"
#include "cpu_calibrate.h"
#include "tagfs_volume.h"

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
static const char *StandRunAside(TagFSVolume* vol, bool port)
{
    return port ? StandPortAside(vol, tagfs_get_seat(vol)) : StandStepsAsideFor(vol);
}

/* A file of a megabyte of zeros, laid by one span — one reservation, so the
 * volume gives it one run when it has one that long. */
static bool StandRunMake(TagFSVolume* vol, uint64_t *out_file_id)
{
    uint64_t file_id = 0;

    *out_file_id = 0;
    if (tagfs_create_file(vol, "test_file", NULL, 0, 0, &file_id) != 0)
        return false;
    if (tagfs_span_file(vol, file_id, STAND_RUN_BYTES) != OK) {
        tagfs_delete_file(vol, file_id, INK_UNWARRANTED);
        return false;
    }
    *out_file_id = file_id;
    return true;
}

/* Whether the file's megabyte lies in one run on the medium, read off its
 * record — never off the blocks, which would put them in a read-ahead window:
 * *out_first is the run's first block. */
static bool StandRunLiesInOneRun(TagFSVolume* vol, uint64_t file_id, uint64_t *out_first)
{
    uint32_t  got = 0;
    uint64_t  end = 0;
    bool      one = true;
    StripWalk walk;
    Strip     run;

    if (!tagfs_enter(vol))
        return false;
    FileRecord* rec = FileMapRecordHold(vol->state.files, file_id, NULL);
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
    tagfs_leave(vol);
    return one && got >= STAND_RUN_BLOCKS;
}

/* A program's write of `bytes` from `data` at the file's start — standing in
 * its call, or put on the port the way ObjWrite puts it, with the stand's
 * record in place of the program it would park — and whether it answered.
 * *out_running: the job is still out, and still holds the file and takes its
 * bytes from `data`. */
static bool StandRunWrite(TagFSVolume* vol, bool port, uint64_t file_id, const uint8_t *data, uint32_t bytes,
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
        StandPortTurn(vol, tagfs_get_seat(vol));
        cpu_pause();
    }
    out->rc           = s_port_answer.rc;
    out->written      = s_port_answer.written;
    out->size         = s_port_answer.size;
    out->through_port = s_port_answer.through_port;
    return true;
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

static TestResult StandRunMegabyte(TagFSVolume* vol, bool port)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        seat    = tagfs_get_seat(vol);
    uint64_t       file_id = 0;
    uint64_t       first   = 0;
    uint8_t       *data    = NULL;
    uint8_t       *back    = NULL;
    bool           armed   = false;
    bool           running = false;
    const uint32_t N       = TagFSRunBlocks(vol);
    const uint32_t want    = (STAND_RUN_BLOCKS + N - 1) / N;
    StandRunAnswer answer;
    TagFSDataCost  before, after;
    const char    *aside   = StandRunAside(vol, port);

    if (aside)
        return test_steps_aside(aside);

    data = kmalloc(STAND_RUN_BYTES);
    back = kmalloc(STAND_RUN_BYTES);
    TEST_ASSERT(data && back, "room for the megabyte and for reading it back");
    TestFillAt(data, STAND_RUN_BYTES, 0x4E1D0B00u + (port ? 1u : 0u), 0);
    TEST_ASSERT(StandRunMake(vol, &file_id), "a file of a megabyte is made by one span");
    if (!StandRunLiesInOneRun(vol, file_id, &first)) {
        verdict = test_steps_aside("the volume gave the megabyte more than one run, and a "
                                   "write across runs goes out a command a run");
        goto out;
    }
    TEST_ASSERT(!port || WriteJobSeatRunsAsync(seat),
                "a program's write on this seat goes through the port, as the path line said");

    BlockFailArm(seat, BLOCK_FAIL_WRITE, tagfs_block_to_sector(vol, vol->book.frame.data_block),
                 (vol->book.data_end - vol->book.frame.data_block) * TAGFS_BLOCK_SECTORS, BLOCK_FAIL_NEVER);
    armed = true;
    TagFSDataCostTake(vol, &before);
    bool answered = StandRunWrite(vol, port, file_id, data, STAND_RUN_BYTES, &answer, &running);
    TagFSDataCostTake(vol, &after);
    uint32_t seen = BlockFailSeen();
    BlockFailDisarm();
    armed = false;

    TEST_ASSERT(answered, "the write answers");
    TEST_ASSERT_EQ(answer.through_port, port, "on the road it was put on, to its end");
    TEST_ASSERT_OK(answer.rc, "the write is answered OK");
    TEST_ASSERT_EQ(answer.written, STAND_RUN_BYTES, "with every byte of it the file's");
    TEST_ASSERT_EQ(answer.size, STAND_RUN_BYTES, "and the size it was");
    if (!StandRunLiesInOneRun(vol, file_id, &first)) {
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

    TEST_ASSERT_EQ(TestFileRead(vol, file_id, back, STAND_RUN_BYTES), (int)STAND_RUN_BYTES,
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
        tagfs_delete_file(vol, file_id, INK_UNWARRANTED);
    kfree(data);
    kfree(back);
    return verdict;
}

// ----------------------------------------------------------------------------
// A book's write refused inside the run it takes answers exactly the blocks
// before the refused one
//
// W-ERR on fresh ground (helm_work.c, CR-13): a book lays every write on
// fresh ground, so the run refused inside is the one the write takes — for a
// window's worth of a file that holds nothing, the first run of that many
// free blocks (allotment.c, AllotmentBookPick: no run of its own to go on, no
// window kept), named before the write. The medium refusing its k-th block,
// the write answers the k-1 before it, the file is as long as they are and
// holds them, settled the same; the refused block and every one after it no
// record names.
// ----------------------------------------------------------------------------

static TestResult StandRunRefusedFresh(TagFSVolume* vol, bool port)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        seat    = tagfs_get_seat(vol);
    uint64_t       file_id = 0;
    uint64_t       first   = 0, block = 0;
    uint8_t       *data    = NULL;
    uint8_t       *back    = NULL;
    bool           armed   = false;
    bool           running = false;
    const uint32_t B       = TAGFS_BLOCK_SIZE;
    const uint32_t N       = (uint32_t)(HelmWindowBytes(vol, 0) / B);
    const uint32_t k       = N / 2 + 1;   /* the refused block's place in the run, from 1 */
    const uint32_t kept    = (k - 1) * B;
    StandRunAnswer answer;
    const char    *aside   = StandRunAside(vol, port);

    if (aside)
        return test_steps_aside(aside);
    if (N < 2)
        return test_steps_aside("the seat takes one block in a command: a run has no inside");

    data = kmalloc((uint64_t)N * B);
    back = kmalloc((uint64_t)N * B);
    TEST_ASSERT(data && back, "room for a window's worth and for reading it back");
    TestFillAt(data, N * B, 0x4E1D0E00u + (port ? 1u : 0u), 0);
    TEST_ASSERT_EQ(tagfs_create_file(vol, "test_file", NULL, 0, 0, &file_id), 0, "a file is made on the book");
    TEST_ASSERT(BlockMapDerivedRunFree(vol, 0, N, file_id, &first),
                "the first run of free ground a window's worth long is found: the write's to take");

    BlockFailArm(seat, BLOCK_FAIL_WRITE, tagfs_block_to_sector(vol, first + k - 1), TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    bool     answered = StandRunWrite(vol, port, file_id, data, N * B, &answer, &running);
    bool     fired    = BlockFailFired();
    uint32_t seen     = BlockFailSeen();
    BlockFailDisarm();
    armed = false;

    TEST_ASSERT(answered, "the write answers");
    TEST_ASSERT_EQ(answer.through_port, port, "on the road it was put on, to its end");
    TEST_ASSERT(fired, "the k-th block of the run it took was the one refused");
    TEST_ASSERT_EQ(seen, 1, "by the first command that met it: the run's own");
    TEST_ASSERT(answer.rc != OK, "the write answers the refusal");
    TestEvidence("blocks the refused write answered as landed", answer.written / B);
    TEST_ASSERT_EQ(answer.written, kept, "exactly the k-1 blocks of the run before the refused one");
    TEST_ASSERT_EQ(answer.size, kept, "and the file is as long as they are");
    TEST_ASSERT(StandBlockOf(vol, file_id, k - 2, &block) && block == first + k - 2,
                "the last of them where the run was taken");
    TEST_ASSERT_OK(TagFSSettle(vol), "a cycle settles them");
    TEST_ASSERT_EQ(TestFileRead(vol, file_id, back, kept), (int)kept,
                   "the file reads back, every block checked against its own digest");
    TEST_ASSERT_EQ(TestFirstDifference(back, data, kept), kept, "holding the blocks before the refused one");
    TEST_ASSERT(!StandBlockOf(vol, file_id, k - 1, &block), "and no record names the refused block or one after it");
out:
    if (running) {
        data    = NULL;
        file_id = 0;
    }
    if (armed)
        BlockFailDisarm();
    if (file_id)
        tagfs_delete_file(vol, file_id, INK_UNWARRANTED);
    kfree(data);
    kfree(back);
    return verdict;
}

static TestResult TestStandRunRefusedFresh(void)
{
    TagFSVolume* vol = TagFSHome();
    return StandRunRefusedFresh(vol, false);
}

static TestResult TestStandRunRefusedFreshThroughThePort(void)
{
    TagFSVolume* vol = TagFSHome();
    return StandRunRefusedFresh(vol, true);
}

static TestResult TestStandRunMegabyteInSeatRuns(void)
{
    TagFSVolume* vol = TagFSHome();
    return StandRunMegabyte(vol, false);
}

static TestResult TestStandRunMegabyteThroughThePort(void)
{
    TagFSVolume* vol = TagFSHome();
    return StandRunMegabyte(vol, true);
}

static const TestCase g_stand_run_tests[] = {
    { .name  = "stand_run_megabyte_in_seat_runs", .func = TestStandRunMegabyteInSeatRuns,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_run_megabyte_through_the_port",
      .func  = TestStandRunMegabyteThroughThePort, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_run_refused_inside_fresh", .func = TestStandRunRefusedFresh,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_run_refused_inside_fresh_through_the_port",
      .func  = TestStandRunRefusedFreshThroughThePort, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_run_suite = {
    .tests = g_stand_run_tests, .count = ARRAY_SIZE(g_stand_run_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
