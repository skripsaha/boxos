#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../read/read.h"
#include "../integrity/integrity.h"
#include "read_job.h"      /* ReadJobSubmit, and the stand's own answer record */
#include "touch.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The read path (step 1.10)
//
// Both roads a program's read takes — the one that stands in its call
// (TagFSReadInto) and the one through the port (ReadJob) — answer a part of
// the file inside its size that no extent covers, and a block that does not
// match its digest, the same way: the real reason, and the bytes that landed
// before it. A handle's read-ahead window never serves a block a write has
// landed on since it was fetched — not even when the write lands while the
// fetch is on its way. A partial write over a rotten block is refused; a
// write that covers the block whole is not.
//
// A hole is made in the reader's own view: the handle's size is lengthened
// past its extents after the open, which is what a record that describes more
// file than there are blocks hands a reader — and the volume's record is left
// as it is. A rotten block is written on the medium behind the integrity map's
// back, and its bytes and digest are made to agree again before the test lets
// it go.
// ============================================================================

#define STAND_READ_BLOCKS  3u
#define STAND_READ_BOUND_MS 20000u

static ReadJobStandAnswer s_read_answer;

/* Why this machine has no port road for a read, or NULL. */
static const char* StandReadPortAside(uint8_t seat)
{
    if (!ReadJobSeatRunsAsync(seat))
        return "a read on this seat stands in its call: one core, or a seat that answers "
               "only a read somebody stands over";
    return StandStepsAsideFor();
}

/* The read job's answer, the machine turned by hand until it comes. */
static bool StandReadAnswered(uint8_t seat)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_READ_BOUND_MS);
    for (;;) {
        if (__atomic_load_n(&s_read_answer.answered, __ATOMIC_ACQUIRE))
            return true;
        if (rdtsc() >= bound)
            return false;
        StandPortTurn(seat);
        cpu_pause();
    }
}

/* A read of `bytes` from `offset` of the open handle through the port, into
 * g_test_output, answered to the stand's record. The job owns the handle from
 * a taken submit on, and closes it; a submit not taken leaves it the caller's. */
static int StandReadSubmit(TagFSFileHandle* handle, uint32_t file_id, uint64_t offset,
                           uint32_t bytes)
{
    ReadJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    memset(&s_read_answer, 0, sizeof(s_read_answer));
    handle->offset   = offset;
    ask.file_id      = file_id;
    ask.sink.kernel  = g_test_output;
    ask.sink.bytes   = bytes;
    ask.stand        = &s_read_answer;
    return ReadJobSubmit(handle, &ask, NULL, NULL);
}

/* A file of `blocks` blocks of stream `stream` in one run on the medium. */
static bool StandReadMakeRun(uint32_t blocks, uint32_t stream, uint32_t* file_id,
                             uint32_t* first)
{
    TestFillAt(g_test_buffer, blocks * TAGFS_BLOCK_SIZE, stream, 0);
    if (!StandMakeFile("test_file", g_test_buffer, blocks * TAGFS_BLOCK_SIZE, file_id))
        return false;
    if (blocks > 1 && tagfs_defrag_file(*file_id, 0) != OK)
        return false;
    for (uint32_t k = 0; k < blocks; k++) {
        uint32_t b = 0;
        if (!StandBlockOf(*file_id, k, &b))
            return false;
        if (k == 0)
            *first = b;
        else if (b != *first + k)
            return false;
    }
    return true;
}

/* Block `block` rots on the medium behind the map's back: `was` with one byte
 * flipped. */
static bool StandReadRot(uint32_t block, const uint8_t* was)
{
    uint8_t* rot = g_test_buffer + 3 * TAGFS_BLOCK_SIZE;
    memcpy(rot, was, TAGFS_BLOCK_SIZE);
    rot[100] ^= 0xFF;
    uint64_t vlba = ((uint64_t)tagfs_get_state()->layout.data_block + block) * TAGFS_BLOCK_SECTORS;
    return tagfs_volume_write(vlba, TAGFS_BLOCK_SECTORS, rot) == 0;
}

// ----------------------------------------------------------------------------
// A hole inside the size, on both roads (S2)
// ----------------------------------------------------------------------------

static TestResult TestStandReadHoleRefused(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0, first = 0;
    TagFSFileHandle* handle  = NULL;
    const char*      aside   = StandStepsAsideFor();
    uint64_t         got     = 0;

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandReadMakeRun(2, 0x71, &file_id, &first), "a two-block file is made in one run");
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "and opened to read");
    handle->file_size = STAND_READ_BLOCKS * TAGFS_BLOCK_SIZE;   /* the record says a block more */

    error_t rc = tagfs_read(handle, g_test_output, STAND_READ_BLOCKS * TAGFS_BLOCK_SIZE, &got);
    TEST_ASSERT_EQ(rc, ERR_METADATA_CORRUPTED,
                   "a read into the block no extent covers is refused as corrupted metadata");
    TEST_ASSERT_EQ(got, 2 * TAGFS_BLOCK_SIZE, "with the two blocks before it landed");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, 2 * TAGFS_BLOCK_SIZE),
                   2 * TAGFS_BLOCK_SIZE, "as the file holds them");
    TEST_ASSERT_EQ(handle->offset, 2 * TAGFS_BLOCK_SIZE, "and the handle moved by them");
out:
    tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

static TestResult TestStandReadHoleRefusedThroughPort(void)
{
    TestResult       verdict = TEST_PASS;
    uint8_t          seat    = tagfs_get_seat();
    uint32_t         file_id = 0, first = 0;
    TagFSFileHandle* handle  = NULL;
    const char*      aside   = StandReadPortAside(seat);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandReadMakeRun(2, 0x72, &file_id, &first), "a two-block file is made in one run");
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "and opened to read");
    handle->file_size = STAND_READ_BLOCKS * TAGFS_BLOCK_SIZE;

    int submit = StandReadSubmit(handle, file_id, 0, STAND_READ_BLOCKS * TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(submit, ERR_WOULD_BLOCK, "the read goes through the port");
    handle = NULL;                                   /* the job's, and closed by it */
    TEST_ASSERT(StandReadAnswered(seat), "and is answered");
    TEST_ASSERT_EQ(s_read_answer.rc, ERR_METADATA_CORRUPTED,
                   "refused as corrupted metadata, as the read that stands answers it");
    TEST_ASSERT_EQ(s_read_answer.landed, 2 * TAGFS_BLOCK_SIZE, "with the two blocks before it landed");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, 2 * TAGFS_BLOCK_SIZE),
                   2 * TAGFS_BLOCK_SIZE, "as the file holds them");
out:
    tagfs_close(handle);
    if (file_id && __atomic_load_n(&s_read_answer.answered, __ATOMIC_ACQUIRE))
        tagfs_delete_file(file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A read beside a write never serves a stale read-ahead block (N1)
// ----------------------------------------------------------------------------

static TestResult TestStandReadAheadForgetsAWrite(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0, first = 0, again = 0;
    TagFSFileHandle* handle  = NULL;
    const char*      aside   = StandStepsAsideFor();
    uint8_t*         newer   = g_test_buffer + STAND_READ_BLOCKS * TAGFS_BLOCK_SIZE;
    uint64_t         got     = 0;

    if (aside)
        return test_steps_aside(aside);
    if (ReadAheadDepth() < STAND_READ_BLOCKS)
        return test_steps_aside("the medium hands over fewer than three blocks at a time, so "
                                "no window holds the run this test fetches");
    TEST_ASSERT(StandReadMakeRun(STAND_READ_BLOCKS, 0x73, &file_id, &first),
                "a three-block file is made in one run");
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "and opened to read");

    /* The write lands between two reads of the window. */
    TEST_ASSERT_OK(tagfs_read(handle, g_test_output, 2 * TAGFS_BLOCK_SIZE, &got),
                   "its first two blocks are read in one fetch");
    TEST_ASSERT(ReadAheadServe(handle, first + 1) != NULL, "and the window holds the second");
    TestFillAt(newer, TAGFS_BLOCK_SIZE, 0x74, TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(StandWriteAt(file_id, TAGFS_BLOCK_SIZE, newer, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "the second block is written over whole");
    TEST_ASSERT(StandBlockOf(file_id, 1, &again) && again == first + 1, "in place");
    TEST_ASSERT(ReadAheadServe(handle, first + 1) == NULL, "and the window lets it go");
    handle->offset = 0;
    TEST_ASSERT_OK(tagfs_read(handle, g_test_output, 2 * TAGFS_BLOCK_SIZE, &got),
                   "the two blocks are read again through the same handle");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output + TAGFS_BLOCK_SIZE, newer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "and the second is what the write left");

    /* The write lands while a fetch over its block is on its way in. */
    uint32_t count = STAND_READ_BLOCKS;
    uint64_t gen   = ReadAheadBegin(handle, first, &count);
    TEST_ASSERT(gen != 0 && count == STAND_READ_BLOCKS, "a fetch of the three is named");
    TEST_ASSERT_OK(TagFSReadRun(first, count, ReadAheadBuffer(handle)),
                   "and reads the medium before the write lands");
    TestFillAt(newer, TAGFS_BLOCK_SIZE, 0x75, 2 * TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(StandWriteAt(file_id, 2 * TAGFS_BLOCK_SIZE, newer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "the third block is written over whole meanwhile");
    TEST_ASSERT(!ReadAheadLand(handle, gen, count), "the fetch that landed late is dropped");
    TEST_ASSERT(ReadAheadServe(handle, first + 2) == NULL, "and the window holds nothing of it");
    handle->offset = 2 * TAGFS_BLOCK_SIZE;
    TEST_ASSERT_OK(tagfs_read(handle, g_test_output, TAGFS_BLOCK_SIZE, &got),
                   "the third block is read through the same handle");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, newer, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "and is what the write left, not what the late fetch read");
out:
    tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A block out of the read-ahead window, verified as one off the medium is
// (CW-D8), and a rotten block's read answering the verdict (Integrity)
//
// A three-block run is read whole: the read fetches the run in one command
// and hands every block out of the window. Its middle block rotted on the
// medium behind the map's back first, so the read is refused ERR_CHECKSUM with
// the first block landed, and the block is named on the "integrity" tag.
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t block;
    uint32_t heard;
} StandRotEar;

static StandRotEar s_rot_ear;

static void StandRotHeard(TouchTag tag_id, const void* payload, uint32_t plen,
                          uint32_t source_pid, void* ctx)
{
    (void)tag_id;
    (void)source_pid;
    StandRotEar* ear = (StandRotEar*)ctx;
    uint32_t     block;
    if (!payload || plen < sizeof(block))
        return;
    memcpy(&block, payload, sizeof(block));
    if (block == ear->block)
        __atomic_add_fetch(&ear->heard, 1u, __ATOMIC_RELAXED);
}

static TestResult TestStandReadAheadHitIsVerified(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0, first = 0;
    TagFSFileHandle* handle  = NULL;
    TouchWatch*      ear     = NULL;
    bool             rotten  = false;
    uint8_t*         was     = g_test_buffer;
    const char*      aside   = StandStepsAsideFor();
    uint64_t         got     = 0;

    if (aside)
        return test_steps_aside(aside);
    if (!IntegrityIsInitialized())
        return test_steps_aside("verify-on-read is not armed on this volume, so no rot "
                                "can be caught");
    TouchTag tag = tagfs_tag_lookup("integrity");
    if (tag == TOUCH_TAG_INVALID)
        return test_steps_aside("this volume has no \"integrity\" tag to name rot on");

    TEST_ASSERT(StandReadMakeRun(STAND_READ_BLOCKS, 0x6A, &file_id, &first),
                "a three-block file is made in one run");
    TEST_ASSERT(StandReadRot(first + 1, was + TAGFS_BLOCK_SIZE),
                "its middle block rots on the medium, behind the map's back");
    rotten = true;
    memset(&s_rot_ear, 0, sizeof(s_rot_ear));
    s_rot_ear.block = first + 1;
    ear = TouchWatchSet(tag, StandRotHeard, &s_rot_ear);
    TEST_ASSERT(ear != NULL, "an ear on the integrity tag");

    uint32_t errors_before = IntegrityErrorCount();
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "the file is opened to read");
    error_t rc = tagfs_read(handle, g_test_output, STAND_READ_BLOCKS * TAGFS_BLOCK_SIZE, &got);
    uint32_t caught = IntegrityErrorCount() - errors_before;
    TestEvidence("rotten blocks a read caught out of the read-ahead window, of 1", caught);
    TEST_ASSERT_EQ(rc, ERR_CHECKSUM, "the read of the rotten block answers the verdict");
    TEST_ASSERT_EQ(got, TAGFS_BLOCK_SIZE, "with the block before it landed");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, was, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "as the file holds it");
    TEST_ASSERT_EQ(caught, 1, "the rotten block is caught once");
    TEST_ASSERT_EQ(__atomic_load_n(&s_rot_ear.heard, __ATOMIC_RELAXED), 1,
                   "and named once on the integrity tag");
out:
    if (ear)
        TouchWatchClear(ear);
    tagfs_close(handle);
    if (rotten && tagfs_write_block(first + 1, was + TAGFS_BLOCK_SIZE) != OK)
        verdict = TestFailedAt(__FILE__, __LINE__,
                               "the rotten block would not take its bytes back — the map and "
                               "the medium disagree over it (block, 0)", first + 1, 0);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

/* The same rot through the port: the job checks each block before it lands. */
static TestResult TestStandReadRotThroughPort(void)
{
    TestResult       verdict = TEST_PASS;
    uint8_t          seat    = tagfs_get_seat();
    uint32_t         file_id = 0, first = 0;
    TagFSFileHandle* handle  = NULL;
    bool             rotten  = false;
    uint8_t*         was     = g_test_buffer;
    const char*      aside   = StandReadPortAside(seat);

    if (aside)
        return test_steps_aside(aside);
    if (!IntegrityIsInitialized())
        return test_steps_aside("verify-on-read is not armed on this volume, so no rot "
                                "can be caught");

    TEST_ASSERT(StandReadMakeRun(STAND_READ_BLOCKS, 0x6B, &file_id, &first),
                "a three-block file is made in one run");
    TEST_ASSERT(StandReadRot(first + 1, was + TAGFS_BLOCK_SIZE),
                "its middle block rots on the medium, behind the map's back");
    rotten = true;
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "the file is opened to read");

    int submit = StandReadSubmit(handle, file_id, 0, STAND_READ_BLOCKS * TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(submit, ERR_WOULD_BLOCK, "the read goes through the port");
    handle = NULL;
    TEST_ASSERT(StandReadAnswered(seat), "and is answered");
    TEST_ASSERT_EQ(s_read_answer.rc, ERR_CHECKSUM, "with the verdict on the rotten block");
    TEST_ASSERT_EQ(s_read_answer.landed, TAGFS_BLOCK_SIZE, "and the block before it landed");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, was, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "as the file holds it");
out:
    tagfs_close(handle);
    if (rotten && tagfs_write_block(first + 1, was + TAGFS_BLOCK_SIZE) != OK)
        verdict = TestFailedAt(__FILE__, __LINE__,
                               "the rotten block would not take its bytes back — the map and "
                               "the medium disagree over it (block, 0)", first + 1, 0);
    if (file_id && __atomic_load_n(&s_read_answer.answered, __ATOMIC_ACQUIRE))
        tagfs_delete_file(file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A partial write over a rotten block is refused; a whole one is not (RT-22)
// ----------------------------------------------------------------------------

static TestResult TestStandReadRmwRefusesRot(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    file_id = 0, first = 0;
    bool        rotten  = false;
    uint8_t*    was     = g_test_buffer;
    uint8_t*    whole   = g_test_buffer + TAGFS_BLOCK_SIZE;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);
    if (!IntegrityIsInitialized())
        return test_steps_aside("verify-on-read is not armed on this volume, so no rot "
                                "can be caught");

    TEST_ASSERT(StandReadMakeRun(1, 0x76, &file_id, &first), "a one-block file is made");
    TEST_ASSERT(StandReadRot(first, was), "its block rots on the medium, behind the map's back");
    rotten = true;

    TEST_ASSERT(StandWriteAt(file_id, 100, "sixteen bytes...", 16) < 0,
                "sixteen bytes laid over the rotten block are refused");
    TestFillAt(whole, TAGFS_BLOCK_SIZE, 0x77, 0);
    TEST_ASSERT_EQ(StandWriteAt(file_id, 0, whole, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "a write that covers the block whole is taken: it reads nothing of it");
    rotten = false;
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "and the block reads again");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, whole, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "holding what the whole write left");
out:
    if (rotten && tagfs_write_block(first, was) != OK)
        verdict = TestFailedAt(__FILE__, __LINE__,
                               "the rotten block would not take its bytes back — the map and "
                               "the medium disagree over it (block, 0)", first, 0);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

static const TestCase g_stand_read_tests[] = {
    { .name = "stand_read_hole_refused", .func = TestStandReadHoleRefused,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a part of a file inside its size that no extent covers was read "
                       "as a short success on the road through the port" },
    { .name = "stand_read_hole_refused_through_port", .func = TestStandReadHoleRefusedThroughPort,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a part of a file inside its size that no extent covers was read "
                       "as a short success on the road through the port" },
    { .name = "stand_readahead_forgets_a_write", .func = TestStandReadAheadForgetsAWrite,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a read-ahead fetch that lands after a write to its block fills the "
                       "cache with what the block held before, and a later read serves it" },
    { .name  = "stand_readahead_hit_is_verified", .func = TestStandReadAheadHitIsVerified,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a block read out of the read-ahead cache is handed out without "
                       "verify-on-read, so a rotten block is caught only when it is the "
                       "first of the run a read prefetched" },
    { .name = "stand_read_rot_through_port", .func = TestStandReadRotThroughPort,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a read through the port never checks a block against its digest, "
                       "and a rotten block reaches the program as data" },
    { .name = "stand_read_rmw_refuses_rot", .func = TestStandReadRmwRefusesRot,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a partial write over a rotten block lays its bytes over the rot and "
                       "writes it back under a fresh digest, so the rot is the file's for good" },
};

const TestSuite g_stand_read_suite = {
    .tests = g_stand_read_tests, .count = ARRAY_SIZE(g_stand_read_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
