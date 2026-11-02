#include "tests.h"
#include "stand_tests.h"
#include "stand_binder.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../mount/away.h"
#include "../mount/book_mount.h"
#include "../mount/tally_read.h"
#include "../mount/book_unit.h"
#include "../block_map/block_map_derived.h"
#include "../settle/settle.h"
#include "../settle/binder.h"
#include "../read/read.h"
#include "../integrity/integrity.h"
#include "../integrity/gloss.h"
#include "read_job.h"
#include "write_job.h"
#include "../run/run.h"
#include "amp.h"
#include "baton.h"
#include "boardroom.h"
#include "block_fail.h"
#include "cpu_calibrate.h"
#include "atomics.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_tally suite: a format-v2 volume read off its book
//
// This kernel reads a format-v2 volume's book — the Spine's slots and the
// Tally of the newest one that reads — and writes into it stitches and the
// Tallies past them (settle/sewing.h, settle/binder.h): a change to a file's
// data it refuses. So these are asked of the volume the machine stands on,
// every one written for format v2 and standing aside on any other: every file
// it holds reads whole, each block held to its digest; a mount writes nothing,
// and two mounts are one state (C12); a node whose first copy rotted reads off
// its second, said (§2.5); a FILES leaf neither copy of which reads is a
// rotten range — mounted, its files locked, its blocks held (C19:
// stand_tally_range_tests.c); every road that would change a file is refused
// before a byte, and every door below the glass of a volume under it; a rotten data block is refused on the three roads a read
// takes, and a block with no digest as one that does not match; a medium that
// leaves and comes back has its book read again, with nothing read to judge
// it; and the live files are the records, with no table to agree with. And BLOCKFAIL's plan of bad sectors in
// two places at once, which C19 asks of it, for any format. And the block map derived from the owners'
// names: it holds to them, names a claim on a taken block and a release of a free one, and refuses the
// mount out loud when the heap will not hold its pages.
//
// What a test does to the medium it does by the Boardroom's own doors, beside
// TagFS: no cycle runs while it does, and the stand's hand is the medium's rot.
// Every sector it changes it puts back, and mounts again on it. The rotten
// range alone may write Tallies first — where no leaf below the root is one
// every slot names as it lies — and does so with the kit a test that writes
// Tallies is run with (stand_binder.h): the ring, the ground its slots hold and
// the ribbon put back as it found them.
// ============================================================================

#define STAND_TALLY_MOUNT_MS  10000u

/* The volume's medium arriving again, attended until the mount road has
 * nothing left to do — mounted or refused — bounded. */
void StandTallyArrive(void)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_TALLY_MOUNT_MS);
    TagFSAttendArrival();
    while (!TagFSStandMountIdle() && rdtsc() < bound) {
        BoardroomAttendIfPending();
        TagFSServiceIfPending();
        BatonPump(amp_get_core_index());
        cpu_pause();
    }
}

/* Whatever a test left: the plan down, the volume up again if it is not, and
 * the runner counted back in — once, here: a test that let the volume go and
 * met it again leaves the runner's hold to this. */
void StandTallyClose(void)
{
    BlockFailDisarm();
    if (!tagfs_get_state()->initialized)
        StandTallyArrive();
    TagFSStandHoldAgain();
}

/* Why a test written for a book stands aside here, or NULL. */
const char* StandTallyAside(void)
{
    if (tagfs_get_state()->format != DEED_FORMAT_V2)
        return "the volume is not format v2: there is no book to read";
    return StandStepsAsideFor();
}

/* What a mount found, folded into numbers two mounts of one volume share: how
 * many files, their bytes, and a sum over every file's id, size and name. */
typedef struct {
    uint64_t files;
    uint64_t bytes;
    uint64_t sum;
} StandTallyState;

static void StandTallyStateTake(StandTallyState* out)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    from = 0;
    uint64_t    ids[32];
    uint32_t    n;

    memset(out, 0, sizeof(*out));
    while ((n = FileMapRows(fs->files, from, ids, 32)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec = FileMapRecordHold(fs->files, ids[i], NULL);
            if (!rec)
                continue;
            out->files++;
            out->bytes += rec->size;
            out->sum = out->sum * 1000003u + ids[i] * 31u + rec->size;
            for (uint32_t c = 0; c < rec->name_len; c++)
                out->sum = out->sum * 131u + (uint8_t)rec->name[c];
            FileRecordRelease(rec);
        }
        from = ids[n - 1] + 1;
    }
}

/* One sector of the volume, read or written past TagFS — on the seat and
 * base the volume was mounted from, which stay what they are while it is
 * down. */
static bool StandTallySector(uint8_t seat, uint64_t base, uint64_t vlba, void* buf, bool write)
{
    return write ? BoardroomWrite(seat, base + vlba, 1, buf, NULL) == 0
                 : BoardroomRead(seat, base + vlba, 1, buf) == 0;
}

// ----------------------------------------------------------------------------
// Every file reads whole, each block held to its digest
// ----------------------------------------------------------------------------

/* The blocks of the live files by where a read finds their digests. */
typedef struct {
    uint64_t gloss;     /* the Gloss holds them */
    uint64_t tally;     /* the Tally's DIGESTS */
} StandTallyNamed;

/* What the Tally's DIGESTS said of a block the Gloss does not hold (tally_read.h). */
static const char* StandTallyDigestSaid(TallyDigestSays says)
{
    switch (says) {
    case TALLY_DIGEST_UNNAMED: return "the leaf that would name it reads, and no element of it does";
    case TALLY_DIGEST_ROTTEN:  return "that leaf, or its element, reads in no copy";
    case TALLY_DIGEST_UNREAD:  return "that leaf could not be read now";
    default:                   return "it is named";
    }
}

/*
 * Every DATA block of `rec` carries the digest it was written with, asked
 * where a read asks it (integrity.c, IntegrityVerifyBook; spec_v2 §8.1): the
 * Gloss first, then the Tally's DIGESTS. The Gloss holds what no Tally names
 * yet (gloss.h) — a block a stitch past the Tally put in among them, named by
 * that stitch alone until the next Tally: a used book holds such blocks
 * whenever the machine before it stopped with stitches sewn past its last
 * Tally — the mount says "the ribbon past the Tally: N stitch(es)"
 * (book_mount.c) — and they stay so until a Tally is laid, which the tests run
 * before this one may or may not owe. Asked of the Tally alone, the verdict
 * was the book's history, not its state. A block neither names is said with
 * the Tally's answer for it — no element, no copy, or no read now and why —
 * before the claim fails.
 */
static bool StandTallyDigested(const FileRecord* rec, StandTallyNamed* named)
{
    StripWalk walk;
    Strip     run;
    StripWalkBegin(rec, &walk);
    while (StripWalkNextGround(&walk, &run)) {
        for (uint64_t k = 0; k < run.count; k++) {
            uint64_t digest  = 0;
            error_t  refused = OK;
            if (GlossFind(run.start + k, &digest)) {
                named->gloss++;
                continue;
            }
            TallyDigestSays says = TallyDigestFind(run.start + k, &digest, &refused);
            if (says == TALLY_DIGEST_NAMED) {
                named->tally++;
                continue;
            }
            kprintf("[TESTS] stand_tally_files_read_whole: block %llu of file %llu — the Gloss holds no digest of "
                    "it, and of the Tally's DIGESTS %s (TallyDigestFind %d, refused %d)\n",
                    (unsigned long long)(run.start + k), (unsigned long long)rec->file_id,
                    StandTallyDigestSaid(says), (int)says, (int)refused);
            return false;
        }
    }
    return true;
}

/* The whole file read through a handle, a buffer at a time; its bytes. */
static error_t StandTallyReadWhole(uint64_t fid, uint64_t* got)
{
    TagFSFileHandle* h = tagfs_open(fid, TAGFS_HANDLE_READ);
    if (!h)
        return ERR_FILE_NOT_FOUND;
    error_t rc = OK;
    *got = 0;
    while (rc == OK && *got < h->file_size) {
        uint64_t n = 0;
        rc = tagfs_read(h, g_test_output, sizeof(g_test_output), &n);
        if (rc == OK && n == 0)
            rc = ERR_IO;
        *got += n;
    }
    tagfs_close(h);
    return rc;
}

static TestResult TestStandTallyFilesReadWhole(void)
{
    TestResult      verdict = TEST_PASS;
    TagFSState*     fs      = tagfs_get_state();
    const char*     aside   = StandTallyAside();
    StandTallyNamed named   = { 0, 0 };
    uint64_t        from = 0, files = 0, ids[16];
    uint32_t        n;

    if (aside)
        return test_steps_aside(aside);
    uint32_t errors = IntegrityErrorCount();
    while ((n = FileMapRows(fs->files, from, ids, 16)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec = FileMapRecordHold(fs->files, ids[i], NULL);
            TEST_ASSERT(rec != NULL, "every live file has its record");
            bool     digested = StandTallyDigested(rec, &named);
            uint64_t size     = rec->size;
            FileRecordRelease(rec);
            TEST_ASSERT(digested, "every block a live file names carries the digest it was written with — the "
                        "Gloss's, or the Tally's");
            uint64_t got = 0;
            TEST_ASSERT_OK(StandTallyReadWhole(ids[i], &got), "the file reads, every block of it");
            TEST_ASSERT_EQ(got, size, "to its last byte");
            files++;
        }
        from = ids[n - 1] + 1;
    }
    if (named.gloss)
        kprintf("[TESTS] stand_tally_files_read_whole: %llu block(s) of the live files are held to the digests "
                "the Gloss holds, %llu to the Tally's DIGESTS\n", (unsigned long long)named.gloss,
                (unsigned long long)named.tally);
    TEST_ASSERT(files > 0, "the book holds files");
    TEST_ASSERT_EQ(files, FileMapCount(fs->files), "and the walk met every one the mount holds");
    TEST_ASSERT_EQ(IntegrityErrorCount(), errors, "and no block read otherwise than its digest says");
out:
    return verdict;
}

// ----------------------------------------------------------------------------
// A mount writes nothing, and two mounts are one state (C12)
// ----------------------------------------------------------------------------

static TestResult TestStandTallyMountWritesNothing(void)
{
    TestResult      verdict = TEST_PASS;
    uint8_t         seat    = tagfs_get_seat();
    const char*     aside   = StandTallyAside();
    StandTallyState before, once, twice;
    bool            down    = false;

    if (aside)
        return test_steps_aside(aside);
    StandTallyStateTake(&before);
    BlockFailArm(seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    for (uint32_t m = 0; m < 2; m++) {
        down = TagFSStandLetGo();
        TEST_ASSERT(down, "the volume is let go of, writing nothing");
        StandTallyArrive();
        TagFSStandHoldAgain();
        down = false;
        TEST_ASSERT(tagfs_get_state()->initialized, "and is mounted again off its book");
        StandTallyStateTake(m ? &twice : &once);
    }
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "and not one write reached the medium across two mounts");
    TEST_ASSERT(!TagFSIsUnderGlass(), "and the book is one this kernel's writer goes on: no glass over it");
    TEST_ASSERT_EQ(once.files, before.files, "the first mount holds the files the boot's did");
    TEST_ASSERT_EQ(once.sum, before.sum, "the same files, by id, size and name");
    TEST_ASSERT_EQ(twice.files, once.files, "and the second the same");
    TEST_ASSERT_EQ(twice.bytes, once.bytes, "of the same bytes");
    TEST_ASSERT_EQ(twice.sum, once.sum, "by id, size and name");
out:
    if (down)
        StandTallyArrive();
    else
        TagFSStandLetGo();
    StandTallyClose();
    return verdict;
}

// ----------------------------------------------------------------------------
// A node whose first copy rotted is read off its second, and said (§2.5)
// ----------------------------------------------------------------------------

static TestResult TestStandTallyCopyOneRotten(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        seat    = tagfs_get_seat();
    uint64_t       base    = tagfs_get_volume_base();
    const char*    aside   = StandTallyAside();
    TallyPlace root;
    MountBookSaid  said;
    StandTallyState before, after;
    uint8_t*       was     = g_test_buffer;
    uint8_t*       rot     = g_test_buffer + TAGFS_SECTOR_SIZE;
    uint64_t       vlba    = 0;
    bool           rotten  = false;

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(TallyStandNodeWhere(TALLY_FILES, 0, &root), "the FILES root the mount read");
    TEST_ASSERT(root.ptr.copies >= 2, "is written in two copies");
    StandTallyStateTake(&before);

    vlba = root.ptr.block[0] * TAGFS_BLOCK_SECTORS + 3u;
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    TEST_ASSERT(StandTallySector(seat, base, vlba, was, false), "a sector of the root's first copy is read");
    memcpy(rot, was, TAGFS_SECTOR_SIZE);
    rot[200] ^= 0x5A;
    TEST_ASSERT(StandTallySector(seat, base, vlba, rot, true), "and a byte of it rots on the medium");
    rotten = true;
    StandTallyArrive();
    TagFSStandHoldAgain();

    TEST_ASSERT(tagfs_get_state()->initialized, "the volume mounts");
    MountBookStandSaid(&said);
    TEST_ASSERT_EQ(said.mended, 1, "and says one copy does not read, the second read in its place");
    TEST_ASSERT_EQ(said.rotten, 0, "and no range of it is lost");
    StandTallyStateTake(&after);
    TEST_ASSERT_EQ(after.files, before.files, "every file is there");
    TEST_ASSERT_EQ(after.sum, before.sum, "as it was");
out:
    if (rotten) {
        if (tagfs_get_state()->initialized)
            TagFSStandLetGo();
        if (!StandTallySector(seat, base, vlba, was, true))
            verdict = TestFailedAt(__FILE__, __LINE__, "the rotten sector would not take its bytes "
                                   "back (vlba, 0)", (int64_t)vlba, 0);
        StandTallyArrive();
        MountBookStandSaid(&said);
        if (said.mended != 0)
            verdict = TestFailedAt(__FILE__, __LINE__, "mounted on the sector put back, a copy still "
                                   "does not read (mended, 0)", said.mended, 0);
    }
    StandTallyClose();
    return verdict;
}

// ----------------------------------------------------------------------------
// A file's data is refused before a byte; a change of a file writes nothing
// until a stitch takes it
// ----------------------------------------------------------------------------

static TestResult TestStandTallyWritesRefused(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    const char* aside   = StandTallyAside();
    uint64_t    ids[1];

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(FileMapRows(tagfs_get_state()->files, 0, ids, 1) == 1, "a live file to change");
    BlockFailArm(seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x3C);
    TEST_ASSERT_EQ(TestFileWrite(ids[0], g_test_buffer, TAGFS_BLOCK_SIZE), -1,
                   "a file's bytes are not written");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "and not one write reached the medium");
    TEST_ASSERT(!TagFSReadOnly(), "the volume is not read-only: its book takes files, names and memory "
                "(stand_docket, stand_stitch)");
out:
    BlockFailDisarm();
    return verdict;
}

// ----------------------------------------------------------------------------
// A rotten data block, refused on every road a read takes
// ----------------------------------------------------------------------------

static ReadJobStandAnswer s_tally_answer;

/* A live file of three blocks or more in one run, and its first block. */
static bool StandTallyRunOfThree(uint64_t* fid, uint64_t* first)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    from = 0, ids[16];
    uint32_t    n;
    while ((n = FileMapRows(fs->files, from, ids, 16)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec = FileMapRecordHold(fs->files, ids[i], NULL);
            FileSpot    spot;
            bool        run = rec && FileRecordFind(rec, 0, &spot) && spot.kind == STRIP_DATA &&
                              spot.left >= 2;
            FileRecordRelease(rec);
            if (run) {
                *fid   = ids[i];
                *first = spot.block;
                return true;
            }
        }
        from = ids[n - 1] + 1;
    }
    return false;
}

/* The port's read of the file's first three blocks, answered — into a sink
 * of zeros, so the bytes in it are the ones the job landed. */
static error_t StandTallyPortRead(uint64_t fid)
{
    TagFSFileHandle* h = tagfs_open(fid, TAGFS_HANDLE_READ);
    ReadJobAsk       ask;
    if (!h)
        return ERR_FILE_NOT_FOUND;
    memset(&ask, 0, sizeof(ask));
    memset(&s_tally_answer, 0, sizeof(s_tally_answer));
    memset(g_test_output, 0, 3u * TAGFS_BLOCK_SIZE);
    ask.file_id     = fid;
    ask.sink.kernel = g_test_output;
    ask.sink.bytes  = 3u * TAGFS_BLOCK_SIZE;
    ask.stand       = &s_tally_answer;
    if (ReadJobSubmit(h, &ask, NULL, NULL) != ERR_WOULD_BLOCK) {
        tagfs_close(h);
        return ERR_BUSY;
    }
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_TALLY_MOUNT_MS);
    while (!__atomic_load_n(&s_tally_answer.answered, __ATOMIC_ACQUIRE) && rdtsc() < bound) {
        StandPortTurn(tagfs_get_seat());
        cpu_pause();
    }
    return __atomic_load_n(&s_tally_answer.answered, __ATOMIC_ACQUIRE) ? s_tally_answer.rc : ERR_TIMEOUT;
}

/* A read of `bytes` from `offset` of the file through a handle of its own. */
static error_t StandTallyReadAt(uint64_t fid, uint64_t offset, uint32_t bytes)
{
    TagFSFileHandle* h = tagfs_open(fid, TAGFS_HANDLE_READ);
    uint64_t         got = 0;
    if (!h)
        return ERR_FILE_NOT_FOUND;
    h->offset  = offset;
    error_t rc = tagfs_read(h, g_test_output, bytes, &got);
    tagfs_close(h);
    return rc;
}

static TestResult TestStandTallyRotThreePaths(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint64_t    base    = tagfs_get_volume_base();
    const char* aside   = StandTallyAside();
    uint64_t    fid = 0, first = 0, vlba = 0;
    uint8_t*    was     = g_test_buffer;
    uint8_t*    rot     = g_test_buffer + TAGFS_SECTOR_SIZE;
    uint8_t*    held    = g_test_buffer + TAGFS_BLOCK_SIZE;
    bool        rotten  = false;

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandTallyRunOfThree(&fid, &first), "a file of three blocks in one run");
    TEST_ASSERT_OK(StandTallyReadAt(fid, 0, TAGFS_BLOCK_SIZE), "its first block read as the file holds it");
    memcpy(held, g_test_output, TAGFS_BLOCK_SIZE);
    vlba = (first + 1u) * TAGFS_BLOCK_SECTORS;
    TEST_ASSERT(StandTallySector(seat, base, vlba, was, false), "its second block is read");
    memcpy(rot, was, TAGFS_SECTOR_SIZE);
    rot[17] ^= 0xA5;
    TEST_ASSERT(StandTallySector(seat, base, vlba, rot, true), "and rots on the medium");
    rotten = true;
    tagfs_readahead_invalidate(first + 1u);

    uint32_t errors = IntegrityErrorCount();
    TEST_ASSERT_EQ(StandTallyReadAt(fid, TAGFS_BLOCK_SIZE, 100), ERR_CHECKSUM,
                   "a read of that block alone answers that it does not hold what was written");
    TEST_ASSERT_EQ(StandTallyReadAt(fid, 0, 3u * TAGFS_BLOCK_SIZE), ERR_CHECKSUM,
                   "and a read of the run through the read-ahead window answers the same");
    TEST_ASSERT(IntegrityErrorCount() > errors, "the rot is counted");
    if (ReadJobSeatRunsAsync(seat)) {
        TEST_ASSERT_EQ(StandTallyPortRead(fid), ERR_CHECKSUM, "and the port's read answers the same");
        TEST_ASSERT_EQ(s_tally_answer.landed, TAGFS_BLOCK_SIZE,
                       "with the block before it landed, and not one byte of the rotten one");
        TEST_ASSERT_EQ(TestFirstDifference(g_test_output, held, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                       "as the file holds it");
    } else {
        kprintf("[TESTS] stand_tally_rot_three_paths: the port's road stands aside — the volume's seat "
                "(%s) takes no read nobody stands over, so no read of it goes through the port\n",
                BoardroomSeatName(seat));
    }
out:
    if (rotten) {
        if (!StandTallySector(seat, base, vlba, was, true))
            verdict = TestFailedAt(__FILE__, __LINE__, "the rotten sector would not take its bytes "
                                   "back (vlba, 0)", (int64_t)vlba, 0);
        tagfs_readahead_invalidate(first + 1u);
    }
    return verdict;
}

// ----------------------------------------------------------------------------
// A block no DIGESTS element names is refused, as the census says
// ----------------------------------------------------------------------------

/* The block before the far Deed: past every file the image lays, so no element
 * of DIGESTS names it. Every block format v2 writes carries its digest, so a
 * read that meets one with none is answered as one that does not match
 * (bookcensus.py _Digests: "a read of them answers ERR_CHECKSUM"). */
static TestResult TestStandTallyUndigestedRefused(void)
{
    TestResult  verdict = TEST_PASS;
    const char* aside   = StandTallyAside();
    uint64_t    block   = g_book.data_end - 1u, digest = 0;
    error_t     refused = OK;

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_EQ(TallyDigestFind(block, &digest, &refused), TALLY_DIGEST_UNNAMED,
                   "the block before the far Deed carries no digest in the Tally");
    uint32_t errors = IntegrityErrorCount();
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5A);
    TEST_ASSERT(!IntegrityVerify(block, g_test_buffer), "a read of it is refused, never answered unchecked");
    TEST_ASSERT(IntegrityErrorCount() > errors, "and counted as a block that does not hold");
out:
    return verdict;
}

// ----------------------------------------------------------------------------
// A pointer naming a longer unit than its node is a copy that does not hold,
// not a want of memory
// ----------------------------------------------------------------------------

/* The FILES root the mount read, read again at a length its pointer did not
 * name, the heap refusing the room for it: answered by the unit's own first
 * sector (book_unit.c, BookUnitFirstSays). */
static TestResult TestStandTallyLengthAsked(void)
{
    TestResult  verdict = TEST_PASS;
    const char* aside   = StandTallyAside();
    uint8_t     core    = amp_get_core_index();
    TallyPlace  root;
    BookUnit    u;
    bool        held = false;

    if (aside)
        return test_steps_aside(aside);
    memset(&u, 0, sizeof(u));
    TEST_ASSERT(TallyStandNodeWhere(TALLY_FILES, 0, &root), "the FILES root the mount read");
    uint64_t vlba  = root.ptr.block[0] * TAGFS_BLOCK_SECTORS;
    uint32_t count = (uint32_t)(root.ptr.sheets * TAGFS_BLOCK_SECTORS);
    HeapFailArm(core, 1);
    error_t rc = BookUnitRead(vlba, count + TAGFS_BLOCK_SECTORS, BOOK_UNIT_SHEET, root.ptr.seq, &u);
    held = true;
    HeapFailDisarm();
    TEST_ASSERT_EQ(rc, OK, "a Sheet more than the node, the heap refusing room for it, is answered");
    TEST_ASSERT(!u.whole, "as a copy that does not hold: its first sector begins a shorter unit");
    BookUnitLetGo(&u);
    HeapFailArm(core, 1);
    rc = BookUnitRead(vlba, count, BOOK_UNIT_SHEET, root.ptr.seq, &u);
    HeapFailDisarm();
    TEST_ASSERT_EQ(rc, ERR_NO_MEMORY, "and the node's own length, the heap refusing it, is a want of memory");
out:
    HeapFailDisarm();
    if (held)
        BookUnitLetGo(&u);
    return verdict;
}

// ----------------------------------------------------------------------------
// Every door below the glass refuses, though no road above lets a change by:
// the stand stands the volume under glass for the knock, and mounts it again
// ----------------------------------------------------------------------------

static TestResult TestStandTallyLowerDoorsRefused(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    const char* aside   = StandTallyAside();
    uint64_t    block   = g_book.data_end - 1u;
    uint32_t    landed  = 0, knocks = TagFSGlassKnocks();
    bool        glassed = false;

    if (aside)
        return test_steps_aside(aside);
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x6B);
    if (!TagFSIsUnderGlass()) {
        TagFSUnderGlass("the stand stands the volume under glass to knock at the doors below it, until it is "
                        "mounted again");
        glassed = true;
    }
    BlockFailArm(seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    TagFSGlassKnocking(true);
    TEST_ASSERT_EQ(tagfs_volume_write(block * TAGFS_BLOCK_SECTORS, 1, g_test_buffer), (int)ERR_TAGFS_READ_ONLY,
                   "the volume's sector door refuses");
    TEST_ASSERT_EQ(TagFSMetaBlockWrite(block, g_test_buffer), ERR_TAGFS_READ_ONLY,
                   "the metadata block door refuses");
    TEST_ASSERT_EQ(TagFSWriteRun(block, 1, g_test_buffer, &landed), ERR_TAGFS_READ_ONLY,
                   "the data run door refuses");
    TEST_ASSERT_EQ(landed, 0, "and lands nothing");
    TEST_ASSERT_EQ(WriteJobStandWriteDoor(block), ERR_TAGFS_READ_ONLY, "the port's write door refuses");
    TagFSGlassKnocking(false);
    TEST_ASSERT_EQ(TagFSGlassKnocks() - knocks, 4, "each of the four by the glass");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "and not one write reached the medium");
out:
    TagFSGlassKnocking(false);
    BlockFailDisarm();
    if (glassed) {
        TagFSStandLetGo();
        StandTallyClose();
    }
    return verdict;
}

// ----------------------------------------------------------------------------
// A book whose medium leaves and comes back as it was left is taken up,
// judged by what its medium holds (§3.8, §16.5 п.11)
// ----------------------------------------------------------------------------

static TestResult TestStandTallyMediumReturns(void)
{
    TestResult      verdict = TEST_PASS;
    uint8_t         seat    = tagfs_get_seat();
    const char*     aside   = StandTallyAside();
    StandTallyState before, after;
    bool            away = false;

    if (aside)
        return test_steps_aside(aside);
    StandTallyStateTake(&before);
    TagFSStandMediumLeaves();
    away = true;
    TEST_ASSERT(TagFSMediumLeft(), "the medium is said to have left");
    BlockFailArm(seat, BLOCK_FAIL_READ, 0, 0, BLOCK_FAIL_NEVER);
    AwayVerdict back = TagFSStandMediumReturns(NULL);
    away = false;
    TEST_ASSERT_EQ(back, AWAY_AS_LEFT, "its book is taken up: nothing changed it");
    TEST_ASSERT(BlockFailSeen() > 0, "judged by sectors read off the medium: its newest slot, the ribbon's end");
    BlockFailDisarm();
    TEST_ASSERT(tagfs_get_state()->initialized && tagfs_get_state()->format == DEED_FORMAT_V2 &&
                !TagFSMediumLeft(), "and the volume stands, its medium here again");
    TEST_ASSERT(!TagFSIsUnderGlass(), "a book its writer goes on, as it was");
    StandTallyStateTake(&after);
    TEST_ASSERT_EQ(after.files, before.files, "with the files it held");
    TEST_ASSERT_EQ(after.sum, before.sum, "the same, by id, size and name");
out:
    BlockFailDisarm();
    if (away)
        (void)TagFSStandMediumReturns(NULL);
    TagFSStandLetGo();
    StandTallyClose();
    return verdict;
}

// ----------------------------------------------------------------------------
// The live files are the records: a volume with no table has none to agree
// ----------------------------------------------------------------------------

static TestResult TestStandTallyLiveWithoutRows(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    const char* aside   = StandTallyAside();
    uint64_t    from = 0, files = 0, ids[32];
    uint32_t    n;

    if (aside)
        return test_steps_aside(aside);
    while ((n = FileMapRows(fs->files, from, ids, 32)) > 0) {
        for (uint32_t i = 0; i < n; i++)
            TEST_ASSERT(!FileMapRowGet(fs->files, ids[i], NULL, NULL), "a live file of a book has no row");
        files += n;
        from = ids[n - 1] + 1;
    }
    TEST_ASSERT_EQ(files, FileMapCount(fs->files), "and every file the map holds is live by its record");
out:
    return verdict;
}

// ----------------------------------------------------------------------------
// The block map, derived from the owners' names (§4.6): it holds to them, a
// claim on a taken block and a release of a free one are named, and a heap
// that will not hold its pages refuses the mount out loud
// ----------------------------------------------------------------------------

/* The volume's windows: the map covers [0, the far Deed's block). */
static uint32_t StandTallyWindows(void)
{
    return (uint32_t)((tagfs_get_state()->block_end + BLOCK_MAP_WINDOW_BLOCKS - 1u) / BLOCK_MAP_WINDOW_BLOCKS);
}

/* Every block a live file names reads taken on the map; *blocks how many. */
static bool StandTallyFilesTaken(uint64_t* blocks)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    from = 0, ids[16];
    uint32_t    n;
    while ((n = FileMapRows(fs->files, from, ids, 16)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec   = FileMapRecordHold(fs->files, ids[i], NULL);
            bool        taken = rec != NULL;
            StripWalk   walk;
            Strip       run;
            if (rec) {
                StripWalkBegin(rec, &walk);
                while (taken && StripWalkNextGround(&walk, &run))
                    for (uint64_t k = 0; taken && k < run.count; k++, (*blocks)++)
                        taken = !BlockMapDerivedIsFree(run.start + k);
                FileRecordRelease(rec);
            }
            if (!taken)
                return false;
        }
        from = ids[n - 1] + 1;
    }
    return true;
}

/* The longest run window w holds, as BlockMapDerivedRunIn finds it: the most
 * `want` it answers, and where that run starts. */
static uint32_t StandTallyLongest(uint32_t w, uint64_t* start)
{
    uint32_t lo = 0, hi = BLOCK_MAP_WINDOW_BLOCKS;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1u) / 2u;
        if (BlockMapDerivedRunIn(w, mid, start))
            lo = mid;
        else
            hi = mid - 1u;
    }
    if (lo)
        BlockMapDerivedRunIn(w, lo, start);
    return lo;
}

static TestResult TestStandTallyMapHolds(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    const char* aside   = StandTallyAside();
    uint64_t    free = 0, blocks = 0, start = 0, base;
    uint32_t    mixed = 0, w, longest;

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(BlockMapDerivedUp(), "the book's block map is up, derived from its owners at the mount");
    TEST_ASSERT(BlockMapDerivedStandHolds(&free, &mixed),
                "every window's summary holds to its page, and their sum to the volume's free count");
    TEST_ASSERT(free > 0 && mixed > 0, "the data run has free ground, and a window both free and taken");
    TEST_ASSERT(StandTallyFilesTaken(&blocks), "every block a live file names reads taken");
    TEST_ASSERT(blocks > 0, "and the files name blocks");
    TEST_ASSERT(!BlockMapDerivedIsFree(0), "the Deed's block reads taken: the front's");
    TEST_ASSERT(!BlockMapDerivedIsFree(fs->block_end), "and the far Deed's, past the map, is no free block");
    for (w = 0; w < StandTallyWindows() && !BlockMapDerivedRunIn(w, 1, &start); w++)
        ;
    TEST_ASSERT(w < StandTallyWindows(), "a window gives a free block");
    base = (uint64_t)w * BLOCK_MAP_WINDOW_BLOCKS;
    TEST_ASSERT(BlockMapDerivedIsFree(start), "the block it gives is free");
    TEST_ASSERT(start == base || !BlockMapDerivedIsFree(start - 1u), "and the first free one of its window");
    longest = StandTallyLongest(w, &start);
    TEST_ASSERT(longest > 0 && !BlockMapDerivedRunIn(w, longest + 1u, &start) &&
                BlockMapDerivedRunIn(w, longest, &start), "its longest run is the most a run in it is asked for");
    for (uint32_t k = 0; k < longest; k++)
        TEST_ASSERT(BlockMapDerivedIsFree(start + k), "every block of that run is free");
    TEST_ASSERT(start == base || !BlockMapDerivedIsFree(start - 1u), "and the block before it is not");
    TEST_ASSERT(start + longest == base + BLOCK_MAP_WINDOW_BLOCKS || !BlockMapDerivedIsFree(start + longest),
                "nor the block after it");
out:
    return verdict;
}

/* A file no live file is, whose claim the stand makes and lets go. */
#define STAND_TALLY_MAP_FID   0xFFFFFFF0u
#define STAND_TALLY_MAP_PAST  40u   /* releases of a free block: past the 32 the map names one by one */

static TestResult TestStandTallyMapClaimNamed(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    const char* aside   = StandTallyAside();
    uint64_t    before  = fs->ledger.free_blocks, start = 0, met = 0, free = 0;
    uint32_t    mixed = 0, w, k;
    bool        claimed = false;

    if (aside)
        return test_steps_aside(aside);
    /* The map is the mount's, and nothing changes it while the volume stands
     * under glass: the stand claims on it from one core, and lets go of all
     * it claimed. */
    for (w = 0; w < StandTallyWindows() && !BlockMapDerivedRunIn(w, 3, &start); w++)
        ;
    TEST_ASSERT(w < StandTallyWindows(), "a window holds three free blocks in a row");
    TEST_ASSERT_OK(BlockMapDerivedClaim(GROUND_FILE, STAND_TALLY_MAP_FID, start, 2, &met),
                   "two of them are claimed for a file");
    claimed = true;
    TEST_ASSERT_EQ(met, 0, "none of them was taken");
    TEST_ASSERT(!BlockMapDerivedIsFree(start) && !BlockMapDerivedIsFree(start + 1u) &&
                BlockMapDerivedIsFree(start + 2u), "the two read taken, the third free");
    TEST_ASSERT_EQ(fs->ledger.free_blocks, before - 2u, "and the volume's free count is two less");
    TEST_ASSERT_OK(BlockMapDerivedClaim(GROUND_TALLY, 1, start, 3, &met), "all three are claimed for a node");
    TEST_ASSERT_EQ(met, 2, "and the two the file names are met taken — two owners, named on the insertion");
    TEST_ASSERT_OK(BlockMapDerivedRelease(GROUND_TALLY, 1, start, 3, &met), "the three are let go");
    claimed = false;
    TEST_ASSERT_EQ(met, 0, "none of them was free");
    TEST_ASSERT_OK(BlockMapDerivedRelease(GROUND_FILE, STAND_TALLY_MAP_FID, start, 1, &met),
                   "the first is let go again");
    TEST_ASSERT_EQ(met, 1, "and is met free already, named");
    /* Past the bound a list said at mount keeps, the rest are counted — and
     * the first of them says so then, not only when the map is let go (the
     * stand's judge holds the line said once, and the count at the remount). */
    for (k = 0; k < STAND_TALLY_MAP_PAST; k++) {
        TEST_ASSERT_OK(BlockMapDerivedRelease(GROUND_FILE, STAND_TALLY_MAP_FID, start + 2u, 1, &met),
                       "the third, free, is let go once more");
        TEST_ASSERT_EQ(met, 1, "and is met free already");
    }
    TEST_ASSERT_EQ(fs->ledger.free_blocks, before, "the volume's free count is what it was");
    TEST_ASSERT(BlockMapDerivedStandHolds(&free, &mixed), "and every window's summary holds to its page");
out:
    if (claimed)
        BlockMapDerivedRelease(GROUND_TALLY, 1, start, 3, &met);
    return verdict;
}

static TestResult TestStandTallyMapShortOfMemory(void)
{
    TestResult  verdict = TEST_PASS;
    const char* aside   = StandTallyAside();
    uint64_t    free = 0, again = 0;
    uint32_t    mixed = 0, mixed_again = 0;
    bool        down = false;

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(BlockMapDerivedStandHolds(&free, &mixed) && mixed > 0,
                "the map holds a mixed window, whose page the heap may refuse");
    BlockMapDerivedStandRefuse(true);
    down = TagFSStandLetGo();
    TEST_ASSERT(down, "the volume is let go of");
    StandTallyArrive();
    HeapFailDisarm();
    TEST_ASSERT(!tagfs_get_state()->initialized,
                "and its mount, the heap refusing the map its first page, refuses the volume out loud");
    TEST_ASSERT(!BlockMapDerivedUp(), "leaving no map half made");
    TEST_ASSERT_EQ(BlockMapDerivedStandShort(), mixed, "the refusal names every mixed window the map wants");
    StandTallyArrive();
    TEST_ASSERT(tagfs_get_state()->initialized, "asked again, the volume is mounted");
    TagFSStandHoldAgain();
    down = false;
    TEST_ASSERT(BlockMapDerivedStandHolds(&again, &mixed_again), "its map holds");
    TEST_ASSERT_EQ(again, free, "with the free count it had");
    TEST_ASSERT_EQ(mixed_again, mixed, "and its mixed windows");
out:
    BlockMapDerivedStandRefuse(false);
    HeapFailDisarm();
    if (down)
        StandTallyArrive();
    else
        TagFSStandLetGo();
    StandTallyClose();
    return verdict;
}

// ----------------------------------------------------------------------------
// BLOCKFAIL: sectors that will not read, in two places at once
// ----------------------------------------------------------------------------

static TestResult TestStandBlockFailTwoRanges(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint64_t    at      = tagfs_get_volume_base() + 64u;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(!BlockFailAddBadSectors(seat, at, 1), "no range is added to a plan that does not stand");
    BlockFailArmBadSectors(seat, at, 2);
    TEST_ASSERT(BlockFailAddBadSectors(seat, at + 8u, 1), "a second range joins the plan");
    TEST_ASSERT(!BlockFailAddBadSectors((uint8_t)(seat + 1u), at + 16u, 1),
                "and none joins it for another seat");
    TEST_ASSERT_EQ(BoardroomRead(seat, at + 1u, 1, g_test_output), ERR_BAD_SECTOR,
                   "a sector of the first range will not read");
    TEST_ASSERT_EQ(BoardroomRead(seat, at + 8u, 1, g_test_output), ERR_BAD_SECTOR,
                   "nor one of the second");
    TEST_ASSERT_EQ(BoardroomRead(seat, at + 4u, 1, g_test_output), 0, "and one between them reads");
    TEST_ASSERT_EQ(BoardroomRead(seat, at + 9u, 1, g_test_output), 0, "as does one past the second");
    TEST_ASSERT_EQ(BlockFailSeen(), 2, "two reads met the plan");
out:
    BlockFailDisarm();
    return verdict;
}

static const TestCase g_stand_tally_tests[] = {
    { .name = "stand_tally_files_read_whole", .func = TestStandTallyFilesReadWhole,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_mount_writes_nothing", .func = TestStandTallyMountWritesNothing,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_copy_one_rotten", .func = TestStandTallyCopyOneRotten,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_rotten_leaf_locked", .func = TestStandTallyRottenLeafLocked,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_writes_refused", .func = TestStandTallyWritesRefused,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_rot_three_paths", .func = TestStandTallyRotThreePaths,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_undigested_refused", .func = TestStandTallyUndigestedRefused,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_length_asked", .func = TestStandTallyLengthAsked,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_lower_doors_refused", .func = TestStandTallyLowerDoorsRefused,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_medium_returns", .func = TestStandTallyMediumReturns,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_live_without_rows", .func = TestStandTallyLiveWithoutRows,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_map_holds", .func = TestStandTallyMapHolds,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_map_claim_named", .func = TestStandTallyMapClaimNamed,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_tally_map_short_of_memory", .func = TestStandTallyMapShortOfMemory,
      .reach = TEST_READS_ONLY, .format = DEED_FORMAT_V2 },
    { .name = "stand_blockfail_two_ranges", .func = TestStandBlockFailTwoRanges,
      .reach = TEST_READS_ONLY },
};

const TestSuite g_stand_tally_suite = {
    g_stand_tally_tests, sizeof(g_stand_tally_tests) / sizeof(g_stand_tally_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
