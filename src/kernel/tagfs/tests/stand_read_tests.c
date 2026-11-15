#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../read/read.h"
#include "../run/run.h"
#include "../integrity/integrity.h"
#include "read_job.h"      /* ReadJobSubmit, and the stand's own answer record */
#include "block_fail.h"    /* BLOCKFAIL that refuses nothing — a count of the reads of a block */
#include "touch.h"
#include "boardroom.h"
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
static int StandReadSubmit(TagFSFileHandle* handle, uint64_t file_id, uint64_t offset,
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
static bool StandReadMakeRun(uint32_t blocks, uint32_t stream, uint64_t* file_id,
                             uint64_t* first)
{
    TestFillAt(g_test_buffer, blocks * TAGFS_BLOCK_SIZE, stream, 0);
    if (!StandMakeFile("test_file", g_test_buffer, blocks * TAGFS_BLOCK_SIZE, file_id))
        return false;
    if (blocks > 1 && tagfs_defrag_file(*file_id) != OK)
        return false;
    for (uint32_t k = 0; k < blocks; k++) {
        uint64_t b = 0;
        if (!StandBlockOf(*file_id, k, &b))
            return false;
        if (k == 0)
            *first = b;
        else if (b != *first + k)
            return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// A hole inside the size, on both roads (S2)
// ----------------------------------------------------------------------------

static TestResult TestStandReadHoleRefused(void)
{
    TestResult       verdict = TEST_PASS;
    uint64_t         file_id = 0;
    uint64_t         first   = 0;
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
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

static TestResult TestStandReadHoleRefusedThroughPort(void)
{
    TestResult       verdict = TEST_PASS;
    uint8_t          seat    = tagfs_get_seat();
    uint64_t         file_id = 0;
    uint64_t         first   = 0;
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
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// A book's read beside a write serves the version its handle opened (N1)
//
// A book never writes a block a version names: the write over lays fresh
// ground, and the ground the version an open handle reads stays held until it
// is closed (MP-D16). So what the window and a fetch that lands after the
// write hold is that version's, and is what the handle reads; a handle opened
// after the write reads the write.
// ----------------------------------------------------------------------------

static TestResult TestStandReadAheadReadsItsVersion(void)
{
    TestResult       verdict = TEST_PASS;
    uint64_t         file_id = 0;
    uint64_t         first   = 0, again = 0;
    TagFSFileHandle* handle  = NULL;
    TagFSFileHandle* later   = NULL;
    const char*      aside   = StandStepsAsideFor();
    uint8_t*         newer   = g_test_buffer + STAND_READ_BLOCKS * TAGFS_BLOCK_SIZE;
    uint64_t         got     = 0;

    if (aside)
        return test_steps_aside(aside);
    if (TagFSRunBlocks() < STAND_READ_BLOCKS)
        return test_steps_aside("the medium hands over fewer than three blocks at a time, so "
                                "no window holds the run this test fetches");
    TEST_ASSERT(StandReadMakeRun(STAND_READ_BLOCKS, 0x76, &file_id, &first),
                "a three-block file is made in one run");
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "and opened to read");

    TEST_ASSERT_OK(tagfs_read(handle, g_test_output, 2 * TAGFS_BLOCK_SIZE, &got),
                   "its first two blocks are read in one fetch");
    TEST_ASSERT(ReadAheadServe(handle, first + 1) != NULL, "and the window holds the second");
    TestFillAt(newer, TAGFS_BLOCK_SIZE, 0x77, TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(StandWriteAt(file_id, TAGFS_BLOCK_SIZE, newer, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "the second block is written over whole");
    TEST_ASSERT(StandBlockOf(file_id, 1, &again) && again != first + 1, "on fresh ground");
    handle->offset = 0;
    TEST_ASSERT_OK(tagfs_read(handle, g_test_output, 2 * TAGFS_BLOCK_SIZE, &got),
                   "the two blocks are read again through the handle opened before it");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, 2 * TAGFS_BLOCK_SIZE),
                   2 * TAGFS_BLOCK_SIZE, "and are the version it opened");

    uint32_t count = STAND_READ_BLOCKS;
    uint64_t gen   = ReadAheadBegin(handle, first, &count);
    TEST_ASSERT(gen != 0 && count == STAND_READ_BLOCKS, "a fetch of the three is named");
    TEST_ASSERT_OK(TagFSReadRun(first, count, ReadAheadBuffer(handle)),
                   "and reads the medium before the write lands");
    TestFillAt(newer, TAGFS_BLOCK_SIZE, 0x78, 2 * TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(StandWriteAt(file_id, 2 * TAGFS_BLOCK_SIZE, newer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "the third block is written over whole meanwhile");
    (void)ReadAheadLand(handle, gen, count);
    handle->offset = 2 * TAGFS_BLOCK_SIZE;
    TEST_ASSERT_OK(tagfs_read(handle, g_test_output, TAGFS_BLOCK_SIZE, &got),
                   "the third block is read through the same handle");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer + 2 * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "and is its version's, the fetch that landed late or the medium alike");

    later = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(later != NULL, "a handle opened after the writes");
    later->offset = TAGFS_BLOCK_SIZE;
    TEST_ASSERT_OK(tagfs_read(later, g_test_output, 2 * TAGFS_BLOCK_SIZE, &got), "reads the two blocks");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output + TAGFS_BLOCK_SIZE, newer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "the third what its write left");
    TestFillAt(newer, TAGFS_BLOCK_SIZE, 0x77, TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, newer, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "and the second what its write left");
out:
    tagfs_close(later);
    tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
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
// A partial write over a rotten block is refused; a whole one is not (RT-22)
//
// The block rots on the medium behind its digest's back — a byte flipped by
// the Boardroom's own doors — and a read of it
// says so. Sixteen bytes written into it, which would read the rest of the
// block and lay a fresh digest over the rot, are refused; a write of the whole
// block reads nothing of it and is taken. Where the whole block lands is the
// volume's to choose: over the old one, or on fresh ground (a book, §3.1). A
// block the file no longer names is given its own bytes back, so no rot the
// test made is left on the medium either way.
// ----------------------------------------------------------------------------

/* `bytes` laid on `block` of the medium by the Boardroom's door, behind every
 * digest's back. */
static bool StandReadBlockPut(uint64_t block, const uint8_t* bytes)
{
    return BoardroomWrite(tagfs_get_seat(), tagfs_block_to_sector(block), TAGFS_BLOCK_SECTORS, bytes,
                          NULL) == 0;
}

static TestResult TestStandReadRmwRefusesRot(void)
{
    TestResult       verdict = TEST_PASS;
    uint8_t          seat    = tagfs_get_seat();
    uint64_t         file_id = 0;
    uint64_t         first   = 0, now = 0, got = 0;
    bool             rotten  = false;
    uint8_t*         was     = g_test_buffer;
    uint8_t*         rot     = g_test_buffer + TAGFS_BLOCK_SIZE;
    uint8_t*         whole   = g_test_buffer + 2 * TAGFS_BLOCK_SIZE;
    TagFSFileHandle* handle  = NULL;
    const char*      aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandReadMakeRun(1, 0x76, &file_id, &first), "a one-block file is made");
    TEST_ASSERT_OK(BoardroomRead(seat, tagfs_block_to_sector(first), TAGFS_BLOCK_SECTORS, was),
                   "its block is read off the medium");
    memcpy(rot, was, TAGFS_BLOCK_SIZE);
    rot[100] ^= 0xFF;
    TEST_ASSERT(StandReadBlockPut(first, rot), "and rots there, behind its digest's back");
    rotten = true;
    tagfs_readahead_invalidate(first);
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "the file is opened to read");
    TEST_ASSERT_EQ(tagfs_read(handle, g_test_output, TAGFS_BLOCK_SIZE, &got), ERR_CHECKSUM,
                   "a read of the block answers that it does not hold what was written");
    tagfs_close(handle);
    handle = NULL;

    TEST_ASSERT(StandWriteAt(file_id, 100, "sixteen bytes...", 16) < 0,
                "sixteen bytes laid over the rotten block are refused");
    TestFillAt(whole, TAGFS_BLOCK_SIZE, 0x77, 0);
    TEST_ASSERT_EQ(StandWriteAt(file_id, 0, whole, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "a write that covers the block whole is taken: it reads nothing of it");
    TEST_ASSERT(StandBlockOf(file_id, 0, &now), "the file's block is found again");
    if (now != first)
        TEST_ASSERT(StandReadBlockPut(first, was), "the block the file left is given its own bytes back");
    rotten = false;
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "and the file's block reads again");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, whole, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "holding what the whole write left");
out:
    tagfs_close(handle);
    if (rotten) {
        if (!StandReadBlockPut(first, was))
            verdict = TestFailedAt(__FILE__, __LINE__,
                                   "the rotten block would not take its bytes back — the medium and "
                                   "its digest disagree over it (block, 0)", first, 0);
        tagfs_readahead_invalidate(first);
    }
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// A record that names holes, on both roads
//
// The record read here is the test's own, built in memory and put on a handle
// opened onto a two-block file in place of the one the open found: three
// blocks of hole, the file's two blocks of ground, two of hole. Both roads
// land zeros, the file's bytes and zeros — all seven blocks — and ask the
// medium for no block of a hole: BLOCKFAIL, counting and refusing nothing,
// sees no read of block 0, which a hole's start reads as.
// ----------------------------------------------------------------------------

#define STAND_READ_HOLED_BLOCKS 7u

/* The open handle's record made the test's: holes around the file's run. */
static bool StandReadHoled(TagFSFileHandle* handle, uint64_t first)
{
    Strip runs[3] = {
        { .start = 0,     .count = 3, .kind = STRIP_HOLE },
        { .start = first, .count = 2, .kind = STRIP_DATA },
        { .start = 0,     .count = 2, .kind = STRIP_HOLE },
    };
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.file_id      = handle->file_id;
    meta.filename     = "test_file";
    meta.extents      = runs;
    meta.extent_count = ARRAY_SIZE(runs);
    FileRecord* rec = FileRecordBuild(&meta);
    if (!rec)
        return false;
    FileRecordRelease(handle->rec);
    handle->rec       = rec;
    handle->file_size = STAND_READ_HOLED_BLOCKS * TAGFS_BLOCK_SIZE;
    return true;
}

/* Whether `sink` holds the seven blocks the record says: zeros, the file's
 * two blocks as g_test_buffer holds them, zeros. */
static bool StandReadHoledLanded(const uint8_t* sink)
{
    const uint32_t B = TAGFS_BLOCK_SIZE;
    for (uint32_t i = 0; i < STAND_READ_HOLED_BLOCKS * B; i++)
        if ((i < 3u * B || i >= 5u * B) && sink[i] != 0)
            return false;
    return TestFirstDifference(sink + 3u * B, g_test_buffer, 2u * B) == 2u * B;
}

/* A read of block 0 counted, never refused: what a hole's start would ask. */
static void StandReadHoleWatch(void)
{
    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_READ, tagfs_block_to_sector(0), TAGFS_BLOCK_SECTORS,
                 BLOCK_FAIL_NEVER);
}

static TestResult TestStandReadHoles(void)
{
    TestResult       verdict = TEST_PASS;
    uint64_t         file_id = 0, first = 0, got = 0;
    TagFSFileHandle* handle  = NULL;
    bool             armed   = false;
    uint8_t*         sink    = kmalloc(STAND_READ_HOLED_BLOCKS * TAGFS_BLOCK_SIZE);
    const char*      aside   = StandStepsAsideFor();

    if (aside) {
        kfree(sink);
        return test_steps_aside(aside);
    }
    TEST_ASSERT(sink != NULL, "room for the seven blocks");
    memset(sink, 0xA5, STAND_READ_HOLED_BLOCKS * TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandReadMakeRun(2, 0x78, &file_id, &first), "a two-block file is made in one run");
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL && StandReadHoled(handle, first),
                "it is opened to read, its record holes around its run");
    StandReadHoleWatch();
    armed = true;
    error_t rc = tagfs_read(handle, sink, STAND_READ_HOLED_BLOCKS * TAGFS_BLOCK_SIZE, &got);
    uint32_t asked = BlockFailSeen();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT_OK(rc, "the read that stands in its call is answered");
    TEST_ASSERT_EQ(got, STAND_READ_HOLED_BLOCKS * TAGFS_BLOCK_SIZE, "with all seven blocks");
    TEST_ASSERT(StandReadHoledLanded(sink), "zeros, the file's two blocks, zeros");
    TEST_ASSERT_EQ(asked, 0, "and no read of a hole reached the medium");
out:
    if (armed)
        BlockFailDisarm();
    tagfs_close(handle);
    kfree(sink);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

static TestResult TestStandReadHolesThroughPort(void)
{
    TestResult       verdict = TEST_PASS;
    uint8_t          seat    = tagfs_get_seat();
    uint64_t         file_id = 0, first = 0;
    TagFSFileHandle* handle  = NULL;
    bool             armed   = false, submitted = false;
    uint8_t*         sink    = kmalloc(STAND_READ_HOLED_BLOCKS * TAGFS_BLOCK_SIZE);
    const char*      aside   = StandReadPortAside(seat);
    ReadJobAsk       ask;

    memset(&ask, 0, sizeof(ask));
    memset(&s_read_answer, 0, sizeof(s_read_answer));
    if (aside) {
        kfree(sink);
        return test_steps_aside(aside);
    }
    TEST_ASSERT(sink != NULL, "room for the seven blocks");
    memset(sink, 0xA5, STAND_READ_HOLED_BLOCKS * TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandReadMakeRun(2, 0x79, &file_id, &first), "a two-block file is made in one run");
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL && StandReadHoled(handle, first),
                "it is opened to read, its record holes around its run");
    ask.file_id     = file_id;
    ask.sink.kernel = sink;
    ask.sink.bytes  = STAND_READ_HOLED_BLOCKS * TAGFS_BLOCK_SIZE;
    ask.stand       = &s_read_answer;
    StandReadHoleWatch();
    armed = true;
    TEST_ASSERT_EQ(ReadJobSubmit(handle, &ask, NULL, NULL), ERR_WOULD_BLOCK,
                   "the read goes through the port");
    handle    = NULL;                                /* the job's, and closed by it */
    submitted = true;
    TEST_ASSERT(StandReadAnswered(seat), "and is answered");
    uint32_t asked = BlockFailSeen();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT_OK(s_read_answer.rc, "the read through the port lands");
    TEST_ASSERT_EQ(s_read_answer.landed, STAND_READ_HOLED_BLOCKS * TAGFS_BLOCK_SIZE,
                   "all seven blocks");
    TEST_ASSERT(StandReadHoledLanded(sink), "zeros, the file's two blocks, zeros");
    TEST_ASSERT_EQ(asked, 0, "and no read of a hole reached the medium");
out:
    if (armed)
        BlockFailDisarm();
    tagfs_close(handle);
    bool answered = __atomic_load_n(&s_read_answer.answered, __ATOMIC_ACQUIRE) != 0;
    if (!submitted || answered)
        kfree(sink);                                 /* a job unanswered still lands in it */
    if (file_id && (!submitted || answered))
        tagfs_delete_file(file_id, INK_UNWARRANTED);
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
    { .name = "stand_readahead_reads_its_version", .func = TestStandReadAheadReadsItsVersion,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_read_holes", .func = TestStandReadHoles, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_read_holes_through_port", .func = TestStandReadHolesThroughPort,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_read_rmw_refuses_rot", .func = TestStandReadRmwRefusesRot,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a partial write over a rotten block lays its bytes over the rot and "
                       "writes it back under a fresh digest, so the rot is the file's for good" },
};

const TestSuite g_stand_read_suite = {
    .tests = g_stand_read_tests, .count = ARRAY_SIZE(g_stand_read_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
