#include "tests.h"
#include "stand_tests.h"
#include "volume_ribbon.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../mount/away.h"
#include "../mount/book_mount.h"
#include "../mount/book_unit.h"
#include "../integrity/integrity.h"
#include "../integrity/gloss.h"
#include "../box_hash/box_hash.h"
#include "../block_map/block_map_derived.h"
#include "../settle/sewing.h"
#include "read_job.h"
#include "amp.h"
#include "baton.h"
#include "boardroom.h"
#include "block_fail.h"
#include "cpu_calibrate.h"
#include "crypto.h"
#include "atomics.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_ribbon suite: stitches sewn past the Tally of the volume the
// machine stands on, and the mount that reads them
//
// Each test lays stitches with the ribbon's own codec (volume_ribbon.h) where
// the ribbon goes on — past the last stitch the slot's Tally took in, or the
// kernel's own writer sewed since (settle/sewing.h) — past TagFS, by the
// Boardroom's doors, as the stand's hand; mounts the volume again and holds what the
// mount made of them to the rule (spec_v2 §4.5 п.5): the chain sewn on by the
// Clasp each stitch names, a retry taken at the start of the next_quire, a
// stitch closed and damaged read by `lead`, a branch of another history left
// out, a torn tail said, an Undone passed over, a Slip laid on the last block
// and a fresh block held to the digest its record carries (Gloss) on every
// road a read takes. Then every sector it laid is put back and the volume is
// mounted again as it was. No cycle runs while a test's stitches lie on the
// medium: the runner holds the cadence, and these tests ask for none.
// ============================================================================

#define STAND_RIBBON_MOUNT_MS  10000u
#define STAND_RIBBON_LAID      8u
#define STAND_RIBBON_RECORDS   8u

/* Where the ribbon goes on past the Tally the mount stood on, and what the
 * test laid there to put back. */
typedef struct {
    uint8_t  seat;
    uint64_t base;
    uint8_t  uuid[16];
    uint64_t quire;          /* the ribbon's Quire, its first block and blocks */
    uint64_t quire_blocks;
    uint64_t next;           /* the next_quire the slot names */
    uint64_t next_blocks;
    uint64_t at;             /* the block the ribbon goes on at */
    uint64_t seq;            /* the last stitch the Tally took in, and its Clasp */
    uint32_t clasp;
    uint64_t top;            /* the largest seq the mount saw: a stitch laid takes one past it */
    uint64_t fid;            /* the next file id the volume gives */
    uint64_t free;           /* two blocks in a row no owner names */
    uint64_t laid[STAND_RIBBON_LAID];
    uint32_t nlaid;
    bool     data;           /* the free blocks were written: their bytes are kept below */
} StandRibbon;

/* A stitch's records as the test lays them, and where each begins. */
typedef struct {
    uint8_t  bytes[2048];
    uint32_t len;
    uint32_t starts[STAND_RIBBON_RECORDS];
    uint32_t n;
    uint64_t fresh;
} StandRibbonRecords;

static uint8_t            s_ribbon_sectors[VOLUME_BLOCK_BYTES];
static uint8_t            s_ribbon_stream[TALLY_SHEET_PAYLOAD];
static uint8_t            s_ribbon_kept[2 * TAGFS_BLOCK_SIZE];
static uint8_t            s_ribbon_want[2 * TAGFS_BLOCK_SIZE];
static ReadJobStandAnswer s_ribbon_answer;
static StandRibbonRecords s_ribbon_records[3];
static uint8_t            s_ribbon_body[1100];

/* The test's `k`-th stitch's records, empty. */
static StandRibbonRecords* StandRibbonFresh(uint32_t k)
{
    memset(&s_ribbon_records[k], 0, sizeof(s_ribbon_records[k]));
    return &s_ribbon_records[k];
}

/* The volume's medium arriving again, attended until the mount road has
 * nothing left to do — mounted or refused — bounded. */
void StandRibbonArrive(void)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_RIBBON_MOUNT_MS);
    TagFSAttendArrival();
    while (!TagFSStandMountIdle() && rdtsc() < bound) {
        BoardroomAttendIfPending();
        TagFSServiceIfPending();
        BatonPump(amp_get_core_index());
        cpu_pause();
    }
}

/* The volume, let go of while the test laid its stitches, mounted again on
 * what the medium holds now and the runner counted back in: true when it
 * came up. */
bool StandRibbonUp(void)
{
    StandRibbonArrive();
    TagFSStandHoldAgain();
    return tagfs_get_state()->initialized;
}

/* Where the ribbon goes on past its last stitch — the writer's cursor, which
 * stands past every stitch the kernel itself sewed since the Tally — and two
 * free blocks of the data run. */
static bool StandRibbonWhere(StandRibbon* w)
{
    RibbonCursor c;
    memset(w, 0, sizeof(*w));
    RibbonCursorStand(&c);
    if (!g_book.stood || !c.up || c.off % TAGFS_BLOCK_SECTORS || (c.came.blocks && c.off == 0))
        return false;
    w->seat         = tagfs_get_seat();
    w->base         = tagfs_get_volume_base();
    memcpy(w->uuid, tagfs_get_state()->uuid, 16);
    w->quire        = c.quire.block;
    w->quire_blocks = c.quire.blocks;
    w->next         = c.next.block;
    w->next_blocks  = c.next.blocks;
    w->at           = c.quire.block + c.off / TAGFS_BLOCK_SECTORS;
    w->seq          = c.last;
    w->clasp        = c.clasp;
    w->top          = c.given;
    w->fid          = tagfs_get_state()->ledger.next_file_id;
    uint32_t windows = (uint32_t)((tagfs_get_state()->block_end + BLOCK_MAP_WINDOW_BLOCKS - 1u) /
                                  BLOCK_MAP_WINDOW_BLOCKS);
    for (uint32_t i = 0; i < windows; i++)
        if (BlockMapDerivedRunIn(i, 2, &w->free))
            return w->at >= w->quire && w->at + 4u <= w->quire + w->quire_blocks && w->next_blocks;
    return false;
}

/* `count` sectors from `vlba` laid on the medium, the place kept to be put
 * back. */
static bool StandRibbonLay(StandRibbon* w, uint64_t vlba, uint32_t count, const uint8_t* sectors)
{
    if (w->nlaid == STAND_RIBBON_LAID ||
        BoardroomWrite(w->seat, w->base + vlba, count, (void*)sectors, NULL) != 0)
        return false;
    if (!w->nlaid)
        kprintf("[TESTS] stand_ribbon: stitches laid past the Tally — the mounts on them are this test's, "
                "until what it laid is put back\n");
    w->laid[w->nlaid++] = vlba;
    return true;
}

/* One record more for a stitch. */
static void StandRibbonPut(StandRibbonRecords* r, uint64_t kind, const uint8_t* body, uint32_t len)
{
    r->starts[r->n++] = r->len;
    r->len += RibbonRecordPut(r->bytes + r->len, kind, 0, body, len);
}

/* A Born of `fid`, `size` bytes: its one run DATA `count` blocks from
 * `start` with their digests, or a hole of one block when count is 0; and a
 * Slip of `slip_len` bytes of `slip_byte` at `slip_off`. */
static void StandRibbonBorn(StandRibbonRecords* r, uint64_t fid, uint64_t size, uint64_t start, uint32_t count,
                            const uint64_t* digests, uint32_t slip_off, uint32_t slip_len, uint8_t slip_byte)
{
    uint8_t  body[256], runs[32], dig[16], tail[256];
    uint64_t cursor = 0;
    TallyRun run    = { .start = start, .count = count ? count : 1u, .hole = count ? 0u : 1u };
    uint32_t nruns  = size ? 1u : 0u;
    memset(tail, slip_byte, sizeof(tail));
    for (uint32_t i = 0; i < count; i++)
        TallyLePut(dig + 8u * i, digests[i], 8);
    RibbonBorn b = { .fid = fid, .flags = TALLY_FILE_ACTIVE, .size = size, .created_ns = 1, .modified_ns = 1,
                     .name = (const uint8_t*)"ribbon", .name_len = 6, .nruns = nruns, .runs = runs,
                     .runs_bytes = nruns ? RibbonRunPut(runs, &run, &cursor) : 0, .ndigests = count,
                     .digests = dig, .slip = { .off = slip_off, .len = slip_len, .bytes = tail } };
    StandRibbonPut(r, RIBBON_BORN, body, RibbonBornBodyPut(body, &b));
    r->fresh += count;
}

/*
 * A stitch of one block at block `block` of the volume: StitchHead `h`, the
 * records `r`, and a Clasp counting them and their fresh blocks, sealed at
 * `seq` and its first `write` sectors laid on the medium — fewer than eight
 * is a stitch torn while it was written. Its Clasp's sum, which the stitch
 * after it names; 0 when it would not lie down.
 */
static uint32_t StandRibbonSew(StandRibbon* w, uint64_t block, uint64_t seq, const StitchHead* h,
                               const StandRibbonRecords* r, uint32_t write)
{
    uint8_t  body[64];
    uint32_t starts[STAND_RIBBON_RECORDS + 2];
    uint32_t at = RibbonRecordPut(s_ribbon_stream, RIBBON_STITCH_HEAD, 0, body, StitchHeadBodyPut(body, h));
    starts[0] = 0;
    for (uint32_t i = 0; i < r->n; i++)
        starts[i + 1] = at + r->starts[i];
    memcpy(s_ribbon_stream + at, r->bytes, r->len);
    at += r->len;
    Clasp c = { .records = r->n, .fresh = r->fresh, .crc = KCrc32c(s_ribbon_stream, at) };
    starts[r->n + 1] = at;
    at += RibbonRecordPut(s_ribbon_stream + at, RIBBON_CLASP, 0, body, ClaspBodyPut(body, &c));
    TallyUnitSeal(s_ribbon_sectors, s_ribbon_stream, at, starts, r->n + 2, BOOK_UNIT_STITCH, seq,
                  block * TAGFS_BLOCK_SECTORS, VOLUME_BLOCK_SECTORS, w->uuid);
    return StandRibbonLay(w, block * TAGFS_BLOCK_SECTORS, write, s_ribbon_sectors) ? c.crc : 0;
}

/* The StitchHead of a stitch of `seq` after the one of `prev_seq` and Clasp
 * `prev_clasp`, DATA_FLUSHED, naming the slot's next_quire. */
static StitchHead StandRibbonHead(const StandRibbon* w, uint64_t seq, uint64_t prev_seq, uint32_t prev_clasp)
{
    StitchHead h = { .flags = STITCH_DATA_FLUSHED, .prev = prev_seq ? seq - prev_seq : 0,
                     .prev_clasp = prev_clasp, .tally = 1, .next_block = w->next, .next_blocks = w->next_blocks };
    return h;
}

/* The two free blocks written with a pattern of their own, their old bytes
 * kept, and their digests. */
static bool StandRibbonData(StandRibbon* w, uint64_t digests[2])
{
    BoxHashContext ctx;
    if (BoardroomRead(w->seat, w->base + w->free * TAGFS_BLOCK_SECTORS, 2u * TAGFS_BLOCK_SECTORS,
                      s_ribbon_kept) != 0)
        return false;
    for (uint32_t i = 0; i < 2u * TAGFS_BLOCK_SIZE; i++)
        s_ribbon_want[i] = (uint8_t)(i * 7u + 13u);
    if (BoardroomWrite(w->seat, w->base + w->free * TAGFS_BLOCK_SECTORS, 2u * TAGFS_BLOCK_SECTORS,
                       s_ribbon_want, NULL) != 0)
        return false;
    w->data = true;
    BoxHashInit(&ctx, w->uuid, 16);
    for (uint32_t i = 0; i < 2; i++)
        digests[i] = BoxHashIntegrity(s_ribbon_want + i * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, &ctx);
    return true;
}

/* Everything the test laid put back — the stitches' sectors to zeros, the
 * free blocks to their bytes — and the volume mounted again on it: false when
 * a sector would not take its bytes back or the volume did not come up. */
static bool StandRibbonPutBack(StandRibbon* w)
{
    bool ok = true;
    if (tagfs_get_state()->initialized)
        TagFSStandLetGo();
    memset(s_ribbon_sectors, 0, sizeof(s_ribbon_sectors));
    for (uint32_t i = 0; i < w->nlaid; i++)
        ok &= BoardroomWrite(w->seat, w->base + w->laid[i], VOLUME_BLOCK_SECTORS, s_ribbon_sectors, NULL) == 0;
    if (w->data)
        ok &= BoardroomWrite(w->seat, w->base + w->free * TAGFS_BLOCK_SECTORS, 2u * TAGFS_BLOCK_SECTORS,
                             s_ribbon_kept, NULL) == 0;
    tagfs_readahead_invalidate(w->free);
    tagfs_readahead_invalidate(w->free + 1u);
    if (w->nlaid)
        kprintf("[TESTS] stand_ribbon: what the test laid is put back\n");
    StandRibbonArrive();
    TagFSStandHoldAgain();
    return ok && tagfs_get_state()->initialized;
}

/* Why a test written for a book's ribbon stands aside here, or NULL. */
static const char* StandRibbonAside(StandRibbon* w)
{
    if (tagfs_get_state()->format != DEED_FORMAT_V2)
        return "the volume is not format v2: there is no ribbon to sew";
    const char* aside = StandStepsAsideFor();
    if (aside)
        return aside;
    if (!StandRibbonWhere(w))
        return "the ribbon has no Quire with room past its end on a block, a next_quire and two free blocks";
    return NULL;
}

/* Whether file `fid` is live, and its record when `out` is asked for. */
static bool StandRibbonLive(uint64_t fid, FileRecord** out)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    if (out)
        *out = rec;
    else if (rec)
        FileRecordRelease(rec);
    return rec != NULL;
}

// ----------------------------------------------------------------------------
// A Slip laid on the last block, and fresh blocks held to the digests their
// record carries, on every road a read takes
// ----------------------------------------------------------------------------

/* A read of `bytes` from `offset` of the file through a handle of its own,
 * into g_test_output. */
static error_t StandRibbonReadAt(uint64_t fid, uint64_t offset, uint32_t bytes)
{
    TagFSFileHandle* h = tagfs_open(fid, TAGFS_HANDLE_READ);
    uint64_t         got = 0;
    if (!h)
        return ERR_FILE_NOT_FOUND;
    h->offset  = offset;
    error_t rc = tagfs_read(h, g_test_output, bytes, &got);
    tagfs_close(h);
    return rc == OK && got != bytes ? ERR_IO : rc;
}

/* The port's read of the file's first `bytes`, answered. */
static error_t StandRibbonPortRead(uint64_t fid, uint32_t bytes)
{
    TagFSFileHandle* h = tagfs_open(fid, TAGFS_HANDLE_READ);
    ReadJobAsk       ask;
    if (!h)
        return ERR_FILE_NOT_FOUND;
    memset(&ask, 0, sizeof(ask));
    memset(&s_ribbon_answer, 0, sizeof(s_ribbon_answer));
    ask.file_id     = fid;
    ask.sink.kernel = g_test_output;
    ask.sink.bytes  = bytes;
    ask.stand       = &s_ribbon_answer;
    if (ReadJobSubmit(h, &ask, NULL, NULL) != ERR_WOULD_BLOCK) {
        tagfs_close(h);
        return ERR_BUSY;
    }
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_RIBBON_MOUNT_MS);
    while (!__atomic_load_n(&s_ribbon_answer.answered, __ATOMIC_ACQUIRE) && rdtsc() < bound) {
        StandPortTurn(tagfs_get_seat());
        cpu_pause();
    }
    return __atomic_load_n(&s_ribbon_answer.answered, __ATOMIC_ACQUIRE) ? s_ribbon_answer.rc : ERR_TIMEOUT;
}

#define STAND_RIBBON_SIZE  (2u * TAGFS_BLOCK_SIZE - 1000u)

/* The file's bytes as the book has them: the two blocks as written, and over
 * the last the Slips in the order of their stitches — 200 bytes of 's' at
 * 500, then 100 of 't' at 700. */
static void StandRibbonSlipped(void)
{
    memset(s_ribbon_want + TAGFS_BLOCK_SIZE + 500u, 's', 200);
    memset(s_ribbon_want + TAGFS_BLOCK_SIZE + 700u, 't', 100);
}

/* Each of the three roads gives the file's bytes, the Slips laid on the last
 * block; `rc` what each must answer — OK, or ERR_CHECKSUM for a block that
 * does not hold its digest. */
static TestResult StandRibbonThreeRoads(uint64_t fid, error_t rc)
{
    TestResult verdict = TEST_PASS;
    TEST_ASSERT_EQ(StandRibbonReadAt(fid, TAGFS_BLOCK_SIZE + 400u, 500), rc, "a read of the last block alone");
    if (rc == OK)
        TEST_ASSERT(!memcmp(g_test_output, s_ribbon_want + TAGFS_BLOCK_SIZE + 400u, 500),
                    "gives its bytes, the Slips over them");
    TEST_ASSERT_EQ(StandRibbonReadAt(fid, 0, STAND_RIBBON_SIZE), rc, "a read of the run through the window");
    if (rc == OK)
        TEST_ASSERT(!memcmp(g_test_output, s_ribbon_want, STAND_RIBBON_SIZE), "gives every byte of the file");
    if (!ReadJobSeatRunsAsync(tagfs_get_seat())) {
        if (rc == OK)
            kprintf("[TESTS] stand_ribbon_slip_three_roads: the port's road stands aside — the volume's seat "
                    "(%s) takes no read nobody stands over\n", BoardroomSeatName(tagfs_get_seat()));
        goto out;
    }
    TEST_ASSERT_EQ(StandRibbonPortRead(fid, STAND_RIBBON_SIZE), rc, "the port's read");
    if (rc == OK)
        TEST_ASSERT(!memcmp(g_test_output, s_ribbon_want, STAND_RIBBON_SIZE), "gives every byte of the file");
out:
    return verdict;
}

static TestResult TestStandRibbonSlipThreeRoads(void)
{
    TestResult         verdict = TEST_PASS;
    StandRibbon        w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid      said;
    FileRecord*        rec = NULL;
    uint64_t           digests[2], digest = 0;
    uint8_t            body[64], tail[100];
    const char*        aside = StandRibbonAside(&w);
    uint8_t*           was = g_test_buffer;

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    StandRibbonBorn(r1, w.fid, STAND_RIBBON_SIZE, w.free, 2, digests, 500, 200, 's');
    memset(tail, 't', sizeof(tail));
    RibbonShaped sh = { .fid = w.fid, .base = 1, .size = STAND_RIBBON_SIZE, .mtime_ns = 2,
                        .slip = { .off = 700, .len = 100, .bytes = tail } };
    StandRibbonPut(r2, RIBBON_SHAPED, body, RibbonShapedBodyPut(body, &sh));
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "a Born of two fresh blocks and a Slip, then a Shaped laying a second Slip, are sewn");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.sewn == 2 && said.applied == 2 && said.last == w.top + 2u, "both stitches sewn on and applied");
    TEST_ASSERT(StandRibbonLive(w.fid, &rec), "the file is born");
    bool slipped = rec->slip != NULL && rec->size == STAND_RIBBON_SIZE;
    FileRecordRelease(rec);
    TEST_ASSERT(slipped, "its last block carries the Slips");
    TEST_ASSERT(GlossFind(w.free, &digest) && digest == digests[0], "its fresh block is held to its record's digest");
    StandRibbonSlipped();
    verdict = StandRibbonThreeRoads(w.fid, OK);
    if (verdict != TEST_PASS)
        goto out;
    TEST_ASSERT(BoardroomRead(w.seat, w.base + (w.free + 1u) * TAGFS_BLOCK_SECTORS, 1, was) == 0,
                "the last block's first sector is read");
    was[17] ^= 0xA5;
    TEST_ASSERT(BoardroomWrite(w.seat, w.base + (w.free + 1u) * TAGFS_BLOCK_SECTORS, 1, was, NULL) == 0,
                "and a byte of it outside the Slips rots");
    tagfs_readahead_invalidate(w.free + 1u);
    verdict = StandRibbonThreeRoads(w.fid, ERR_CHECKSUM);
out:
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

// ----------------------------------------------------------------------------
// The rules a stitch is sewn on by (§4.5 п.5)
// ----------------------------------------------------------------------------

/* A Born of a file of no bytes: a stitch's record of the least weight. */
static void StandRibbonEmpty(StandRibbonRecords* r, uint64_t fid)
{
    StandRibbonBorn(r, fid, 0, 0, 0, NULL, 0, 0, 0);
}

/* Rule (а) and the torn tail (§3.9): a stitch whose Clasp the slot names is
 * sewn on; the one after it, torn while it was written, is not — the ribbon
 * ends on the first, says so, and the file the torn one bore is never given
 * its id again. The mount writes nothing on the way (C12). */
static TestResult TestStandRibbonTornTail(void)
{
    TestResult         verdict = TEST_PASS;
    StandRibbon        w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid      said;
    const char*        aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    StandRibbonEmpty(r1, w.fid);
    StandRibbonEmpty(r2, w.fid + 1u);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, 4),
                "a stitch, and the next torn to its first four sectors, are laid");
    BlockFailArm(w.seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);
    StandRibbonArrive();
    TagFSStandHoldAgain();
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "and the mount on them wrote nothing");
    BlockFailDisarm();
    TEST_ASSERT(tagfs_get_state()->initialized, "the volume mounts");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.sewn == 1 && said.applied == 1 && said.last == w.top + 1u, "the ribbon ends on the first");
    TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_RIBBON_TORN], 1, "the torn tail is said");
    TEST_ASSERT(StandRibbonLive(w.fid, NULL) && !StandRibbonLive(w.fid + 1u, NULL), "one file born, not two");
    TEST_ASSERT(tagfs_get_state()->ledger.next_file_id >= w.fid + 2u, "and the torn one's id is not given again");
out:
    BlockFailDisarm();
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

/* Rule (б): nothing after the first stitch where it ends, and at the start of
 * the slot's next_quire a stitch naming it by its Clasp and naming the Quire
 * it came from — the ribbon goes on there (a Quire's end, an early close, a
 * retry, M-6). */
static TestResult TestStandRibbonNextQuire(void)
{
    TestResult         verdict = TEST_PASS;
    StandRibbon        w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid      said;
    const char*        aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    StandRibbonEmpty(r1, w.fid);
    StandRibbonEmpty(r2, w.fid + 1u);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 3u, w.top + 1u, c1);
    h2.from_block  = w.quire;
    h2.from_blocks = w.quire_blocks;
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.next, w.top + 3u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "a stitch, and one at the start of the next_quire naming it, are laid");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.sewn == 2 && said.applied == 2 && said.last == w.top + 3u,
                "both are sewn on, the second in the next Quire");
    TEST_ASSERT(StandRibbonLive(w.fid, NULL) && StandRibbonLive(w.fid + 1u, NULL), "both files are born");
out:
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

/* Rule (в): a stitch one sector of which rotted after it settled, and a whole
 * stitch after it naming it — closed and damaged: what reads by `lead` is
 * replayed, the record the rotten sector cut is lost, said with its bytes. */
static TestResult TestStandRibbonClosedDamaged(void)
{
    TestResult         verdict = TEST_PASS;
    StandRibbon        w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1), *r3 = StandRibbonFresh(2);
    MountBookSaid      said;
    uint8_t*           text = s_ribbon_body;
    uint8_t*           rot  = g_test_buffer;
    const char*        aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    memset(text, 'x', 1000);
    StandRibbonEmpty(r1, w.fid);
    StandRibbonPut(r2, RIBBON_USE_CONTEXT, g_test_output, TallyUsePut(g_test_output, text, 1000));
    StandRibbonEmpty(r2, w.fid + 1u);
    StandRibbonEmpty(r3, w.fid + 2u);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    uint32_t   c2 = c1 ? StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS) : 0;
    StitchHead h3 = StandRibbonHead(&w, w.top + 3u, w.top + 2u, c2);
    TEST_ASSERT(c2 && StandRibbonSew(&w, w.at + 2u, w.top + 3u, &h3, r3, VOLUME_BLOCK_SECTORS),
                "three stitches are laid, the second's Use Context across its first sectors");
    uint64_t vlba = (w.at + 1u) * TAGFS_BLOCK_SECTORS + 1u;
    TEST_ASSERT(BoardroomRead(w.seat, w.base + vlba, 1, rot) == 0, "the second's sector 1 is read");
    rot[300] ^= 0x5A;
    TEST_ASSERT(BoardroomWrite(w.seat, w.base + vlba, 1, rot, NULL) == 0, "and rots");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.sewn == 3 && said.applied == 3 && said.last == w.top + 3u, "all three are sewn on");
    TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_STITCH_TORN], 1, "the second said closed and damaged");
    TEST_ASSERT(StandRibbonLive(w.fid + 1u, NULL), "the Born after the rotten sector, read by its lead, stands");
    TEST_ASSERT(StandRibbonLive(w.fid, NULL) && StandRibbonLive(w.fid + 2u, NULL), "and the files around it");
out:
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

/* A stitch past the end whose StitchHead names the last stitch by a Clasp not
 * its own: a dead branch of another history, not sewn on however its seq
 * reads (H-2), said, and no lie. */
static TestResult TestStandRibbonDeadBranch(void)
{
    TestResult         verdict = TEST_PASS;
    StandRibbon        w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid      said;
    const char*        aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    StandRibbonEmpty(r1, w.fid);
    StandRibbonEmpty(r2, w.fid + 1u);
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1 ^ 0xDEADBEEFu);
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "a stitch, and one naming it by a Clasp not its own, are laid");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.sewn == 1 && said.last == w.top + 1u, "the ribbon ends on the first");
    TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_CLASP_STRANGER], 1, "the second said a branch of another history");
    TEST_ASSERT(!said.lied, "and no lie said of the medium");
    TEST_ASSERT(!StandRibbonLive(w.fid + 1u, NULL), "its file is not born");
    TEST_ASSERT(tagfs_get_state()->ledger.next_file_id >= w.fid + 2u, "and its id is not given again");
out:
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

/* An Undone in the first stitch of a session passes over the records of the
 * fid it names in the stitch before — the file is not born, and the block its
 * record put in lies fallow (§2.8, M-1), neither the file's nor free. */
static TestResult TestStandRibbonUndone(void)
{
    TestResult         verdict = TEST_PASS;
    StandRibbon        w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1);
    MountBookSaid      said;
    uint64_t           digests[2];
    uint8_t            body[32];
    uint32_t           fid;
    const char*        aside = StandRibbonAside(&w);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    StandRibbonBorn(r1, w.fid, TAGFS_BLOCK_SIZE, w.free, 1, digests, 0, 0, 0);
    StandRibbonEmpty(r1, w.fid + 1u);
    fid = (uint32_t)w.fid;
    StandRibbonPut(r2, RIBBON_UNDONE, body, RibbonUndoneBodyPut(body, 1, UNDONE_NOT_LANDED, &fid, 1));
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of");
    StitchHead h1 = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    uint32_t   c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h1, r1, VOLUME_BLOCK_SECTORS);
    StitchHead h2 = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    TEST_ASSERT(c1 && StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h2, r2, VOLUME_BLOCK_SECTORS),
                "two Borns, and an Undone of the first, are laid");
    TEST_ASSERT(StandRibbonUp(), "the volume mounts on them");
    MountBookStandSaid(&said);
    TEST_ASSERT(said.sewn == 2 && said.applied == 2, "both stitches sewn on and applied");
    TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_UNDONE], 1, "the Undone said");
    TEST_ASSERT(!StandRibbonLive(w.fid, NULL) && StandRibbonLive(w.fid + 1u, NULL),
                "the file it names is not born, the other is");
    TEST_ASSERT_EQ(said.fallow, 1, "and the block the undone record put in lies fallow");
    TEST_ASSERT(!BlockMapDerivedIsFree(w.free), "neither a file's nor free");
out:
    if (!StandRibbonPutBack(&w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

static const TestCase g_stand_ribbon_tests[] = {
    { .name = "stand_ribbon_slip_three_roads", .func = TestStandRibbonSlipThreeRoads,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_ribbon_torn_tail", .func = TestStandRibbonTornTail,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_ribbon_next_quire", .func = TestStandRibbonNextQuire,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_ribbon_closed_damaged", .func = TestStandRibbonClosedDamaged,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_ribbon_dead_branch", .func = TestStandRibbonDeadBranch,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_ribbon_undone", .func = TestStandRibbonUndone,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
};

const TestSuite g_stand_ribbon_suite = {
    g_stand_ribbon_tests, sizeof(g_stand_ribbon_tests) / sizeof(g_stand_ribbon_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
