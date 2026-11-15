#include "stand_stitch.h"
#include "stand_fresh.h"
#include "volume_ribbon.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../file_map/file_map.h"
#include "../integrity/gloss.h"
#include "../tag_registry/tag_registry.h"
#include "read_job.h"
#include "write_job.h"
#include "block_fail.h"
#include "boardroom.h"
#include "crypto.h"
#include "cpu_calibrate.h"
#include "atomics.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_fresh suite: the data of a file on a volume, written on fresh
// ground
//
// A block the settled book names is never written: a write over it lays the
// new bytes on fresh ground and splices them in, the old block resting in
// Fallow — so a stitch that does not land leaves the file reading its old
// 4096 bytes, never a mix (C15). After every kind of change — a write, an
// append, a block written over whole or in part, synchronously or through the
// port, a cut, a growth across a hole — the cycle settles and the volume
// mounted again holds the same bytes and the same ground. A write the medium
// refuses names nothing (C1). A stitch naming fresh data whose check would
// cost the mount more than the last cycle cost pays the flush before its
// atoms and says so (DATA_FLUSHED), one naming none needs no flush ahead of
// it; and a fresh block read before any Tally names it is held to the
// digest its record carries, on every road a read takes. Every test puts the
// ribbon back as it found it (stand_stitch.h).
// ============================================================================

// ----------------------------------------------------------------------------
// The kit (stand_fresh.h)
// ----------------------------------------------------------------------------

bool StandFreshModelMake(StandFreshModel* m, uint64_t room)
{
    memset(m, 0, sizeof(*m));
    m->bytes = kmalloc(room);
    m->room  = room;
    if (m->bytes)
        memset(m->bytes, 0, room);
    return m->bytes != NULL;
}

void StandFreshModelLetGo(StandFreshModel* m)
{
    kfree(m->bytes);
    memset(m, 0, sizeof(*m));
}

int StandFreshWrite(StandFreshModel* m, uint64_t fid, uint64_t at, uint32_t bytes, uint64_t stream)
{
    if (at + bytes > m->room)
        return -1;
    TestFillAt(m->bytes + at, bytes, stream, at);
    int wrote = StandWriteAt(fid, at, m->bytes + at, bytes);
    if (wrote > 0 && at + (uint64_t)wrote > m->size)
        m->size = at + (uint64_t)wrote;
    return wrote;
}

error_t StandFreshSpan(StandFreshModel* m, uint64_t fid, uint64_t size)
{
    if (size > m->room)
        return ERR_INVALID_ARGUMENT;
    error_t rc = tagfs_span_file(fid, size);
    if (rc != OK)
        return rc;
    if (size > m->size)
        memset(m->bytes + m->size, 0, size - m->size);
    m->size = size;
    return OK;
}

/* The file's bytes from `at`, through a handle of its own, into `out`. */
static error_t StandFreshReadAt(uint64_t fid, uint64_t at, uint8_t* out, uint32_t bytes)
{
    TagFSFileHandle* h   = tagfs_open(fid, TAGFS_HANDLE_READ);
    uint64_t         got = 0;
    if (!h)
        return ERR_FILE_NOT_FOUND;
    h->offset  = at;
    error_t rc = tagfs_read(h, out, bytes, &got);
    tagfs_close(h);
    return rc == OK && got != bytes ? ERR_IO : rc;
}

bool StandFreshHolds(const StandFreshModel* m, uint64_t fid)
{
    FileRecord* rec  = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    bool        size = rec && rec->size == m->size;
    FileRecordRelease(rec);
    if (!size)
        return false;
    for (uint64_t at = 0; at < m->size; at += sizeof(g_test_output)) {
        uint32_t n = m->size - at < sizeof(g_test_output) ? (uint32_t)(m->size - at) : sizeof(g_test_output);
        if (StandFreshReadAt(fid, at, g_test_output, n) != OK || memcmp(g_test_output, m->bytes + at, n) != 0)
            return false;
    }
    return true;
}

uint32_t StandFreshPrint(uint64_t fid, uint64_t* runs)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    uint32_t    crc = 0;
    uint64_t    n   = 0, start = 0, blocks = 0;
    uint8_t     hole = 0;
    StripWalk   walk;
    Strip       s;
    if (!rec)
        return 0;
    crc = KCrc32cContinue(crc, (const uint8_t*)&rec->size, sizeof(rec->size));
    StripWalkBegin(rec, &walk);
    for (bool more = StripWalkNext(&walk, &s);; more = StripWalkNext(&walk, &s)) {
        uint8_t h = more && s.kind == STRIP_HOLE;
        if (more && blocks && h == hole && (h || start + blocks == s.start)) {
            blocks += s.count;
            continue;
        }
        if (blocks) {
            uint64_t row[3] = { hole, hole ? 0 : start, blocks };
            crc = KCrc32cContinue(crc, (const uint8_t*)row, sizeof(row));
            n += !hole;
        }
        if (!more)
            break;
        hole = h, start = s.start, blocks = s.count;
    }
    FileRecordRelease(rec);
    if (runs)
        *runs = n;
    return crc;
}

bool StandFreshSettledAndBack(const StandFreshModel* m, uint64_t fid)
{
    uint32_t before = StandFreshPrint(fid, NULL);
    return TagFSSettle() == OK && StandStitchRemount() && StandFreshPrint(fid, NULL) == before &&
           StandFreshHolds(m, fid);
}

// ----------------------------------------------------------------------------
// Every kind of change, settled and mounted again, holds the same bytes and
// the same ground (§2.8 Shaped)
// ----------------------------------------------------------------------------

static WriteJobStandAnswer s_fresh_answer;

error_t StandFreshPortWrite(StandFreshModel* m, uint64_t fid, uint64_t at, uint32_t bytes)
{
    WriteJobAsk ask;
    if (at + bytes > m->room)
        return ERR_INVALID_ARGUMENT;
    TestFillAt(m->bytes + at, bytes, 7u, at);
    memset(&ask, 0, sizeof(ask));
    memset(&s_fresh_answer, 0, sizeof(s_fresh_answer));
    ask.kind          = WRITE_JOB_WRITE;
    ask.file_id       = fid;
    ask.source.kernel = m->bytes + at;
    ask.source.bytes  = bytes;
    ask.offset        = at;
    ask.stand         = &s_fresh_answer;
    int     submit = WriteJobSubmit(&ask, NULL, NULL);
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_FRESH_PORT_MS);
    if (submit != ERR_WOULD_BLOCK && !__atomic_load_n(&s_fresh_answer.answered, __ATOMIC_ACQUIRE))
        return (error_t)submit;
    while (!__atomic_load_n(&s_fresh_answer.answered, __ATOMIC_ACQUIRE) && rdtsc() < bound) {
        StandPortTurn(tagfs_get_seat());
        cpu_pause();
    }
    if (!__atomic_load_n(&s_fresh_answer.answered, __ATOMIC_ACQUIRE))
        return ERR_TIMEOUT;
    if (s_fresh_answer.rc == OK && at + bytes > m->size)
        m->size = at + bytes;
    return s_fresh_answer.rc;
}

static TestResult TestStandFreshEachChange(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0, was = 0, now = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, 12u * TAGFS_BLOCK_SIZE), "room for the file's bytes as they must read");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-each.bin", NULL, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 3u * TAGFS_BLOCK_SIZE + 100u, 1), 3 * TAGFS_BLOCK_SIZE + 100,
                   "three blocks and a tail are written");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "settled and mounted again, the file reads them, on its ground");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, m.size, 5000u, 2), 5000, "5000 bytes are appended");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "and mounted again they are its");
    TEST_ASSERT(StandBlockOf(fid, 1, &was), "block 1 lies somewhere");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 3), TAGFS_BLOCK_SIZE,
                   "block 1 is written over whole");
    TEST_ASSERT(StandBlockOf(fid, 1, &now) && now != was, "on fresh ground: the block the book named is not written");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "and mounted again the file reads the new bytes");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 4000u, 300u, 4), 300, "300 bytes across blocks 0 and 1 are written");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "read, modified and written fresh, mounted again they hold");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 2u * TAGFS_BLOCK_SIZE + 10u), "the file is cut to two blocks and 10 bytes");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "and mounted again it is that long, its ground cut with it");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 6u * TAGFS_BLOCK_SIZE + 7u), "the file is grown across a hole");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "and mounted again the growth reads as zeros");
    if (!WriteJobSeatRunsAsync(tagfs_get_seat())) {
        kprintf("[TESTS] stand_fresh_each_change: the port's write stands aside — the volume's seat (%s) takes no "
                "write nobody stands over\n", BoardroomSeatName(tagfs_get_seat()));
        goto out;
    }
    TEST_ASSERT_OK(StandFreshPortWrite(&m, fid, 5000u, 1000u), "1000 bytes inside block 1 are written through the port");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "and mounted again the block holds them over its old bytes");
    TEST_ASSERT_OK(StandFreshPortWrite(&m, fid, m.size, 3u * TAGFS_BLOCK_SIZE), "three blocks are appended through it");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "and mounted again they are the file's");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A block written over is the old 4096 bytes or the new, never a mix (C15)
// ----------------------------------------------------------------------------

/* The file of two settled blocks, block 0 then written over by `write` —
 * whole or in part — and the volume let go of before any stitch names it:
 * mounted again, the file reads its old bytes on its old ground. */
static TestResult StandFreshNotLanded(uint32_t at, uint32_t bytes)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m, old;
    uint64_t        fid = 0, was = 0, now = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, 2u * TAGFS_BLOCK_SIZE) && StandFreshModelMake(&old, 2u * TAGFS_BLOCK_SIZE),
                "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-c15.bin", NULL, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 2u * TAGFS_BLOCK_SIZE, 11), 2 * TAGFS_BLOCK_SIZE, "two blocks written");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid) && StandBlockOf(fid, 0, &was), "settled, block 0 lies somewhere");
    memcpy(old.bytes, m.bytes, 2u * TAGFS_BLOCK_SIZE);
    old.size = m.size;
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, at, bytes, 12), (int)bytes, "block 0 is written over");
    TEST_ASSERT(StandFreshHolds(&m, fid) && StandBlockOf(fid, 0, &now) && now != was,
                "the file reads the new bytes from fresh ground");
    TEST_ASSERT(StandStitchRemount(), "the volume is let go of before a stitch names the write, and mounted again");
    TEST_ASSERT(StandFreshHolds(&old, fid), "the file reads its old bytes, every one of them");
    TEST_ASSERT(StandBlockOf(fid, 0, &now) && now == was, "from the block the book named");
out:
    StandFreshModelLetGo(&m);
    StandFreshModelLetGo(&old);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static TestResult TestStandFreshWholeNotLanded(void)
{
    return StandFreshNotLanded(0, TAGFS_BLOCK_SIZE);
}

static TestResult TestStandFreshPartNotLanded(void)
{
    return StandFreshNotLanded(50, 100);
}

// ----------------------------------------------------------------------------
// A write the medium refuses names nothing (C1)
// ----------------------------------------------------------------------------

static TestResult TestStandFreshRefusedNamesNothing(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    TagFSState*     fs  = tagfs_get_state();
    uint64_t        fid = 0, free = 0, resting = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, 4u * TAGFS_BLOCK_SIZE), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-c1.bin", NULL, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 21), TAGFS_BLOCK_SIZE, "a block is written");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "and settled");
    free    = fs->free_blocks;
    resting = FallowBlocks();
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, 0, 0, 1);
    TagFSFileHandle* h = tagfs_open(fid, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    TEST_ASSERT(h != NULL, "the file is opened");
    h->offset = TAGFS_BLOCK_SIZE;
    TestFillAt(g_test_buffer, 3u * TAGFS_BLOCK_SIZE, 22, 0);
    int wrote = TestHandleWrite(h, g_test_buffer, 3u * TAGFS_BLOCK_SIZE);
    tagfs_close(h);
    TEST_ASSERT(BlockFailDisarm() && BlockFailFired(), "the medium refused the write's first command");
    TEST_ASSERT(wrote < 0, "and the write is refused");
    TEST_ASSERT(fs->free_blocks == free && FallowBlocks() == resting,
                "every block it took went back, and nothing rests: no record named them");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "settled and mounted again, the file is as it was");
    TEST_ASSERT_EQ(fs->free_blocks, free, "and the volume's free ground the same");
out:
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// Fresh data whose check would cost more than a cycle is flushed before the
// stitch that names it (§3.3, §16.5 п.3)
// ----------------------------------------------------------------------------

bool StandFreshLastStitch(uint64_t* flags, uint64_t* fresh)
{
    RibbonUnit* u = NULL;
    if (!StandStitchRead(&u))
        return false;
    *flags = u->head.flags;
    *fresh = u->clasp.fresh;
    RibbonUnitLetGo(u);
    return true;
}

static TestResult TestStandFreshFlushesFirst(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    RibbonCursor    was, now;
    uint64_t        fid = 0, flags = 0, fresh = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, 4u * TAGFS_BLOCK_SIZE), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-flush.bin", NULL, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_OK(TagFSSettle(), "and its Born settled");
    BlockFailReadPace(true, 1, 1000000u);
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 2u * TAGFS_BLOCK_SIZE, 31), 2 * TAGFS_BLOCK_SIZE, "two blocks written");
    BlockFailArm(s.seat, BLOCK_FAIL_FLUSH, 0, 0, BLOCK_FAIL_NEVER);
    TEST_ASSERT_OK(TagFSSettle(), "the cycle that names them settles, their check named dearer than its D");
    TEST_ASSERT_EQ(BlockFailSeen(), 2, "paying two flushes: the data's before the stitch, and the one after it");
    TEST_ASSERT(StandFreshLastStitch(&flags, &fresh) && (flags & STITCH_DATA_FLUSHED) && fresh == 2,
                "its stitch says DATA_FLUSHED, and its Clasp the two fresh blocks it names");
    TEST_ASSERT_EQ(tagfs_rename_file(fid, "fresh-flush-named.bin"), 0, "the file is renamed");
    BlockFailArm(s.seat, BLOCK_FAIL_FLUSH, 0, 0, BLOCK_FAIL_NEVER);
    TEST_ASSERT_OK(TagFSSettle(), "the cycle that names the rename settles");
    TEST_ASSERT_EQ(BlockFailSeen(), 1, "paying the one flush after its stitch: it names no fresh data");
    TEST_ASSERT(StandFreshLastStitch(&flags, &fresh) && (flags & STITCH_DATA_FLUSHED) && fresh == 0,
                "and says DATA_FLUSHED, with nothing to flush ahead of it");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 32), TAGFS_BLOCK_SIZE, "block 0 is written over");
    RibbonCursorStand(&was);
    BlockFailArm(s.seat, BLOCK_FAIL_FLUSH, 0, 0, 1);
    TEST_ASSERT_OK(TagFSSettle(), "the cycle settles, its first flush refused");
    TEST_ASSERT(BlockFailDisarm() && BlockFailFired(), "the medium refused the flush before the stitch");
    RibbonCursorStand(&now);
    TEST_ASSERT(RibbonQuireSame(&now.sewn.quire, &was.next) && now.sewn.off == 0,
                "and the stitch was laid again at the start of the next_quire: no atom of it went before the flush");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "mounted again, the file holds the new bytes");
out:
    BlockFailReadPace(false, 0, 0);
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A fresh block is held to its digest before any Tally names it (§8.1)
// ----------------------------------------------------------------------------

static ReadJobStandAnswer s_fresh_read;

/* The port's read of the file's first `bytes`, answered. */
static error_t StandFreshPortRead(uint64_t fid, uint32_t bytes)
{
    TagFSFileHandle* h = tagfs_open(fid, TAGFS_HANDLE_READ);
    ReadJobAsk       ask;
    if (!h)
        return ERR_FILE_NOT_FOUND;
    memset(&ask, 0, sizeof(ask));
    memset(&s_fresh_read, 0, sizeof(s_fresh_read));
    ask.file_id     = fid;
    ask.sink.kernel = g_test_output;
    ask.sink.bytes  = bytes;
    ask.stand       = &s_fresh_read;
    if (ReadJobSubmit(h, &ask, NULL, NULL) != ERR_WOULD_BLOCK) {
        tagfs_close(h);
        return ERR_BUSY;
    }
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_FRESH_PORT_MS);
    while (!__atomic_load_n(&s_fresh_read.answered, __ATOMIC_ACQUIRE) && rdtsc() < bound) {
        StandPortTurn(tagfs_get_seat());
        cpu_pause();
    }
    return __atomic_load_n(&s_fresh_read.answered, __ATOMIC_ACQUIRE) ? s_fresh_read.rc : ERR_TIMEOUT;
}

/* The file's two blocks read by every road — one block alone, the run through
 * the read-ahead window, the port — each answering `rc`. */
static TestResult StandFreshThreeRoads(const StandFreshModel* m, uint64_t fid, error_t rc)
{
    TestResult verdict = TEST_PASS;
    TEST_ASSERT_EQ(StandFreshReadAt(fid, TAGFS_BLOCK_SIZE + 10u, g_test_output, 500), rc, "a read of block 1 alone");
    if (rc == OK)
        TEST_ASSERT(!memcmp(g_test_output, m->bytes + TAGFS_BLOCK_SIZE + 10u, 500), "gives its bytes");
    TEST_ASSERT_EQ(StandFreshReadAt(fid, 0, g_test_output, 2u * TAGFS_BLOCK_SIZE), rc,
                   "a read of the run through the window");
    if (!WriteJobSeatRunsAsync(tagfs_get_seat()))
        goto out;
    TEST_ASSERT_EQ(StandFreshPortRead(fid, 2u * TAGFS_BLOCK_SIZE), rc, "the port's read");
    if (rc == OK)
        TEST_ASSERT(!memcmp(g_test_output, m->bytes, 2u * TAGFS_BLOCK_SIZE), "gives every byte");
out:
    return verdict;
}

static TestResult TestStandFreshDigestHeld(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0, block = 0, digest = 0;
    uint8_t*        sector = g_test_buffer;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, 2u * TAGFS_BLOCK_SIZE), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-gloss.bin", NULL, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 2u * TAGFS_BLOCK_SIZE, 41), 2 * TAGFS_BLOCK_SIZE, "two blocks written");
    TEST_ASSERT(StandBlockOf(fid, 1, &block) && GlossFind(block, &digest), "the digest of its block 1 is held at once");
    verdict = StandFreshThreeRoads(&m, fid, OK);
    if (verdict != TEST_PASS)
        goto out;
    TEST_ASSERT(TagFSSettle() == OK && StandStitchRemount() && StandBlockOf(fid, 1, &block),
                "settled and mounted again, the block is named by the stitch alone");
    TEST_ASSERT(GlossFind(block, &digest), "and held to the digest the stitch carries");
    verdict = StandFreshThreeRoads(&m, fid, OK);
    if (verdict != TEST_PASS)
        goto out;
    TEST_ASSERT(BoardroomRead(s.seat, s.base + block * TAGFS_BLOCK_SECTORS, 1, sector) == 0, "its first sector read");
    sector[33] ^= 0x5A;
    TEST_ASSERT(BoardroomWrite(s.seat, s.base + block * TAGFS_BLOCK_SECTORS, 1, sector, NULL) == 0,
                "and a byte of it rots");
    tagfs_readahead_invalidate(block);
    verdict = StandFreshThreeRoads(&m, fid, ERR_CHECKSUM);
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A write interns nothing, and the volume takes files still
// ----------------------------------------------------------------------------

static TestResult TestStandFreshWritesInternNothing(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0;
    uint32_t        next;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, 2u * TAGFS_BLOCK_SIZE), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("fresh-intern.bin", NULL, 0, &fid), 0, "a file made on the book");
    next = tagfs_get_state()->registry->next_id;
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE + 1u, 51), TAGFS_BLOCK_SIZE + 1, "takes its bytes");
    TEST_ASSERT_EQ(tagfs_get_state()->registry->next_id, next, "and a write gives no tag an id");
    TEST_ASSERT(!TagFSReadOnly(), "the volume takes files still");
    TEST_ASSERT(StandFreshSettledAndBack(&m, fid), "and the bytes are the file's, mounted again");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static const TestCase g_stand_fresh_tests[] = {
    { .name = "stand_fresh_each_change", .func = TestStandFreshEachChange,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_whole_not_landed", .func = TestStandFreshWholeNotLanded,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_part_not_landed", .func = TestStandFreshPartNotLanded,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_refused_names_nothing", .func = TestStandFreshRefusedNamesNothing,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_flushes_first", .func = TestStandFreshFlushesFirst,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_digest_held", .func = TestStandFreshDigestHeld,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_fresh_writes_intern_nothing", .func = TestStandFreshWritesInternNothing,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_fresh_suite = {
    g_stand_fresh_tests, sizeof(g_stand_fresh_tests) / sizeof(g_stand_fresh_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
