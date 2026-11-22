#include "tests.h"
#include "stand_tests.h"
#include "stand_stitch.h"
#include "stand_fresh.h"
#include "stand_binder.h"
#include "volume_ribbon.h"
#include "../tagfs.h"
#include "../ink/ink.h"
#include "../helm/helm.h"
#include "../run/run.h"
#include "../mount/ribbon.h"
#include "../settle/binder.h"
#include "../settle/settle.h"
#include "../settle/sewing.h"
#include "../file_map/file_map.h"
#include "block_fail.h"
#include "boardroom.h"
#include "write_job.h"
#include "amp.h"
#include "atomics.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_slip suite: a tail shorter than a block carried in the stitch
//
// A write that lies wholly in a file's last block and leaves it short of full
// lays no block: its bytes ride in the stitch as a Slip over the block's base
// — the medium's bytes of a DATA block, zeros of a hole — and the stitch lays
// only what changed since the last one settled. The Slips are let go when the
// last block is written whole (a write that fills it, crosses it, or breaks
// the one span they cover; a growth past it); cut to the size by a cut; folded
// into a block before a file comes to wear system, boot or god, and before a
// Tally takes its slice (Engross) — by the service of the Helm for a file
// nobody writes, a file kept in the trash among them, by the holder itself
// while a Tally is owed.
// ============================================================================

#define STAND_SLIP_LINES  100u
#define STAND_SLIP_LINE   40u
#define STAND_SLIP_ROOM   (3u * TAGFS_BLOCK_SIZE)
#define STAND_SLIP_TERM_S 3600ull     /* a term of the trash no test outlives */

/* The Spine and the ribbon as a Tally test found them: too large for a stack. */
static StandBinder s_slip_binder;

/* The file's last block as memory holds it: whether Slips lie over it, and the
 * kind of run it lies on. False for a file of none. */
static bool StandSlipTail(uint64_t fid, bool* slipped, uint32_t* kind)
{
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    FileSpot    spot;
    bool        found = rec && rec->size && FileRecordFind(rec, (rec->size - 1u) / TAGFS_BLOCK_SIZE, &spot);
    if (found) {
        *slipped = rec->slip != NULL;
        *kind    = spot.kind;
    }
    FileRecordRelease(rec);
    return found;
}

/* Whether the last block lies on `kind` with Slips over it or none. */
static bool StandSlipTailIs(uint64_t fid, bool slipped, uint32_t kind)
{
    bool     s = !slipped;
    uint32_t k = kind == STRIP_DATA ? STRIP_HOLE : STRIP_DATA;
    return StandSlipTail(fid, &s, &k) && s == slipped && k == kind;
}

/* The last stitch: its sectors in *of, and the Slip its Born or Shaped of
 * `fid` lays. False when it names the file in neither. */
static bool StandSlipStitched(uint64_t fid, uint32_t* of, uint64_t* off, uint64_t* len)
{
    RibbonUnit* u     = NULL;
    bool        found = false;
    if (!StandStitchRead(&u))
        return false;
    *of = u->of;
    for (uint32_t i = 0; i < u->nrecords; i++) {
        const RibbonRecord* r    = &u->records[i];
        uint32_t            used = 0;
        RibbonShaped        sh;
        RibbonBorn          b;
        if (r->kind == RIBBON_SHAPED && RibbonShapedGet(u->stream + r->body, r->len, u->seq, &sh, &used) &&
            sh.fid == fid) {
            *off  = sh.slip.off;
            *len  = sh.slip.len;
            found = true;
        } else if (r->kind == RIBBON_BORN && RibbonBornGet(u->stream + r->body, r->len, u->seq, &b, &used) &&
                   b.fid == fid) {
            *off  = b.slip.off;
            *len  = b.slip.len;
            found = true;
        }
    }
    RibbonUnitLetGo(u);
    return found;
}

/* The Helm's service run here — and on whichever core finds it listed first
 * — until the file's tail lies in its block, for STAND_FRESH_PORT_MS at the
 * most: whether it does. The cadence is the stand's, held: no cycle settles
 * meanwhile. */
static bool StandSlipEngrossed(uint64_t fid)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_FRESH_PORT_MS);
    do {
        HelmRunReady();
        if (StandSlipTailIs(fid, false, STRIP_DATA))
            return true;
        cpu_pause();
    } while (rdtsc() < bound);
    return false;
}

/* The bytes of data the writes of files have laid so far. */
static uint64_t StandSlipLaid(void)
{
    TagFSDataCost cost;
    TagFSDataCostTake(&cost);
    return cost.bytes;
}

/* A logger: a hundred lines of 40 bytes, each anchored by a cycle of its own.
 * No block of the file is written; each stitch is one atom and lays the line
 * alone, not the tail it has grown; mounted again, the file reads the same. */
static TestResult TestStandSlipLogger(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0, laid = 0, off = 0, len = 0, run = 0;
    uint32_t        of  = 0, seen = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-log.bin", NULL, 0, 0, &fid), 0, "a file is made on the book");
    TEST_ASSERT_OK(TagFSSettle(), "and its Born settled");
    laid = StandSlipLaid();
    run  = BoardroomSeatRun(s.seat);
    BlockFailArm(s.seat, BLOCK_FAIL_WRITE, 0, 0, BLOCK_FAIL_NEVER);     /* every write the seat takes, counted */
    for (uint32_t i = 0; i < STAND_SLIP_LINES; i++) {
        seen = BlockFailSeen();
        TEST_ASSERT_EQ(StandFreshWrite(&m, fid, m.size, STAND_SLIP_LINE, 0x5100u + i), STAND_SLIP_LINE,
                       "a line is written at the file's end");
        TEST_ASSERT_OK(TagFSSettle(), "and anchored by a cycle");
        TEST_ASSERT(StandSlipStitched(fid, &of, &off, &len), "the stitch names the file");
        TEST_ASSERT(of <= RibbonCursorNow()->atom, "in one atom (§10.4: a cycle costs the host one atom)");
        TEST_ASSERT(off == (uint64_t)i * STAND_SLIP_LINE && len == STAND_SLIP_LINE,
                    "and its Slip is the line alone — the change since the last stitch, not the tail (§16.5 п.13)");
        TEST_ASSERT(BlockFailSeen() - seen == (of + run - 1u) / run,
                    "and the medium took the stitch's own writes alone — no block of data on either road, counted "
                    "where the seat takes them (§10.4)");
    }
    TEST_ASSERT_EQ(StandSlipLaid(), laid, "not one block of data was written for the file");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "its last block is a hole with the Slips over it");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "mounted again, the file reads every line (C21)");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "its tail still out of the ribbon");
out:
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A file whose last block is DATA takes a Slip at its end; a write into the
 * middle of the block, apart from the span the Slip covers, lays the block
 * whole on fresh ground — read, the Slip laid over it, the write over that —
 * and the Slip is let go. */
static TestResult TestStandSlipMiddle(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0, laid = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-mid.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE + 100u, 0x5200u), TAGFS_BLOCK_SIZE + 100,
                   "a block and 100 bytes are written: both blocks, the write not lying in the last alone");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "its last block is DATA, no Slip over it");
    laid = StandSlipLaid();
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, m.size, 50, 0x5201u), 50, "50 bytes are written at its end");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_DATA) && StandSlipLaid() == laid,
                "they ride a Slip over the DATA block: no block written");
    TEST_ASSERT_OK(TagFSSettle(), "and settle");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, TAGFS_BLOCK_SIZE + 20u, 40, 0x5202u), 40,
                   "40 bytes are written into the middle of the last block, apart from the Slip's span");
    TEST_ASSERT_EQ(StandSlipLaid(), laid + TAGFS_BLOCK_SIZE, "the block is laid whole, once");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "and the Slip is let go: its bytes are in the block");
    TEST_ASSERT(StandFreshHolds(&m, fid), "the file reads the block, the Slip's bytes and the write");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "and mounted again, the same");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A new file of 100 bytes is born a hole and a Slip; grown past its block, the
 * block is laid with the Slip in it. */
static TestResult TestStandSlipNewFile(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0, laid = 0, off = 0, len = 0;
    uint32_t        of  = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    laid = StandSlipLaid();
    TEST_ASSERT_EQ(tagfs_create_file("slip-new.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 100, 0x5300u), 100, "and 100 bytes written to it");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE) && StandSlipLaid() == laid,
                "its one block is a hole with a Slip over it: no block written");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandSlipStitched(fid, &of, &off, &len) && off == 0 && len == 100,
                "its Born carries the 100 bytes as its Slip (§2.8)");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "mounted again, the file reads them");
    laid = StandSlipLaid();
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, m.size, TAGFS_BLOCK_SIZE, 0x5301u), TAGFS_BLOCK_SIZE,
                   "a block more is written at its end, past the first block");
    TEST_ASSERT_EQ(StandSlipLaid(), laid + 2u * TAGFS_BLOCK_SIZE, "both blocks are laid: the first with the Slip in it");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "and no Slip is left");
    TEST_ASSERT(StandFreshHolds(&m, fid), "the file reads every byte");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "and mounted again, the same");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A cut inside the block a Slip lies over, then a growth inside it: what grew
 * reads as zeros, in one cycle or two, over a hole (§16.5 п.13). */
static TestResult TestStandSlipCutOverHole(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-cut.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 100, 0x5400u), 100, "100 bytes are written");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "a hole with a Slip over it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 50), "the file is cut to 50 bytes");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 80), "and grown to 80");
    TEST_ASSERT(StandFreshHolds(&m, fid), "bytes 50..80 read as zeros, not the bytes the cut took");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 30), "cut to 30");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 90), "and grown to 90 in the same cycle");
    TEST_ASSERT(StandFreshHolds(&m, fid), "bytes 30..90 read as zeros");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "mounted again, the same: zeros past the cuts");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* The same over a DATA block: the Slip cut with the size, and a growth inside
 * the block lays it again with zeros past the cut. */
static TestResult TestStandSlipCutOverData(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-cut-data.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE + 100u, 0x5500u), TAGFS_BLOCK_SIZE + 100,
                   "a block and 100 bytes are written");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, m.size, 50, 0x5501u), 50, "50 bytes more at its end");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_DATA), "ride a Slip over the DATA block");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, TAGFS_BLOCK_SIZE + 120u), "the file is cut into the Slip's span");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_DATA), "the Slip stays over what is left of it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, TAGFS_BLOCK_SIZE + 200u), "and grown inside the block");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "the block is laid again, the Slip in it");
    TEST_ASSERT(StandFreshHolds(&m, fid), "bytes past the cut read as zeros");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "mounted again, the same");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A file that comes to wear system has its tail laid in a block first: the
 * loader reads such a file by its blocks, and would not see a Slip (§9.1). */
static TestResult TestStandSlipGuarded(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-sys.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 100, 0x5600u), 100, "100 bytes are written");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "a hole with a Slip over it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(StandKeepInRole(fid, TAGFS_FILE_ROLE_SYSTEM), "the file is kept in the system's role");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "its tail is laid in a block with it: no Slip is left");
    TEST_ASSERT(StandFreshHolds(&m, fid), "the file reads its 100 bytes");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "mounted again, the same");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "and the block holds them");
    TEST_ASSERT_OK(StandKeepInRole(fid, TAGFS_FILE_ROLE_ORDINARY), "the role let go again");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A Tally is not begun over a slice that carries a Slip: the service of the
 * Helm lays each tail in a block — through the Ink, as a defrag does — and
 * the Tally goes after the cycle that carries them (§16.5 п.14). */
static TestResult TestStandSlipTallyWaits(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b = &s_slip_binder;
    StandFreshModel m;
    uint64_t        fid = 0;
    const char*     aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-tally.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 100, 0x5700u), 100, "100 bytes are written");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "a hole with a Slip over it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    BinderDemanded();
    TEST_ASSERT_OK(TagFSSettle(), "a Tally is demanded, and a cycle weighs it");
    TEST_ASSERT(!BinderOwed(), "the Tally is not begun while the settled state carries the Slip");
    TEST_ASSERT(StandSlipEngrossed(fid), "the Helm's service lays the tail in a block (Engross)");
    TEST_ASSERT(StandFreshHolds(&m, fid), "the file reads its 100 bytes");
    TEST_ASSERT_OK(TagFSSettle(), "the cycle that carries it settles");
    TEST_ASSERT_OK(BinderRunWhole(), "and the Tally is written");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "mounted again, the same");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/* A Tally asked while a Slip lies: the tail is laid in a block first, and the
 * Tally carries none. */
static TestResult TestStandSlipTallyWhole(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b = &s_slip_binder;
    StandFreshModel m;
    uint64_t        fid = 0;
    const char*     aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-whole.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 100, 0x5800u), 100, "100 bytes are written");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "a hole with a Slip over it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally is asked for and written");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "the tail was laid in a block before it");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "mounted again on it, the file reads the same");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "out of its block");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/* A file kept in the trash whose tail lies in a Slip: the Tally lays it in its
 * block as any file's — no change of what the file holds — and the file stays
 * kept, its term the same, through a mount; brought back, it reads its bytes. */
static TestResult TestStandSlipTallyKept(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b = &s_slip_binder;
    StandFreshModel m;
    TagFSMetadata   md;
    uint64_t        fid = 0, until = 0;
    const char*     aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-kept.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 100, 0x5900u), 100, "100 bytes are written");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "a hole with a Slip over it");
    TEST_ASSERT_EQ(tagfs_keep_file(fid, STAND_SLIP_TERM_S, INK_WARRANTED), 0, "the file goes to the trash");
    TEST_ASSERT_EQ(tagfs_get_metadata(fid, &md), 0, "its record is there");
    until = md.keep_until;
    tagfs_metadata_free(&md);
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally is asked for and written");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "the kept file's tail was laid in a block before it");
    TEST_ASSERT(StandStitchRemount(), "mounted again on it");
    TEST_ASSERT_EQ(tagfs_get_metadata(fid, &md), 0, "the record is there");
    TEST_ASSERT((md.flags & TAGFS_FILE_DELETED) && md.keep_until == until, "still kept, its term the same");
    tagfs_metadata_free(&md);
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "its tail in its block");
    TEST_ASSERT_EQ(tagfs_restore_file(fid, INK_WARRANTED), 0, "the file is brought back");
    TEST_ASSERT(StandFreshHolds(&m, fid), "and reads its 100 bytes");
out:
    (void)tagfs_delete_file(fid, INK_WARRANTED);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/* A frequent writer holding its file's Helm, and a Tally owed: the service
 * cannot take the Helm, and the writer lays its own tail in a block with its
 * next write — no Slip is given while a Tally is owed — so the Tally goes. */
static TestResult TestStandSlipHolderEngrosses(void)
{
    TestResult       verdict = TEST_PASS;
    StandBinder*     b = &s_slip_binder;
    StandFreshModel  m;
    HelmWaiter       here;
    TagFSFileHandle* h = NULL;
    uint64_t         fid = 0, laid = 0, written = 0;
    bool             held = false;
    const char*      aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-holder.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, STAND_SLIP_LINE, 0x5900u), STAND_SLIP_LINE, "a line is written");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "a hole with a Slip over it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT((h = tagfs_open(fid, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND)) != NULL, "the writer opens it");
    TEST_ASSERT_OK(HelmTakeNow(h, &here), "and holds its Helm");
    held = true;
    BinderDemanded();
    TEST_ASSERT_OK(TagFSSettle(), "a Tally is demanded, and a cycle weighs it");
    HelmRunReady();
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "the service could not take the Helm: the Slip lies");
    TestFillAt(m.bytes + m.size, STAND_SLIP_LINE, 0x5901u, m.size);
    PlotSource src = { .kernel = m.bytes + m.size, .bytes = STAND_SLIP_LINE };
    laid = StandSlipLaid();
    TEST_ASSERT(HelmWrite(h, &src, &written) == OK && written == STAND_SLIP_LINE, "the writer writes its next line");
    m.size += STAND_SLIP_LINE;
    TEST_ASSERT_EQ(StandSlipLaid(), laid + TAGFS_BLOCK_SIZE, "as a block: no Slip while a Tally is owed");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA), "its own tail laid in it");
    HelmLetGo(h->ofe, &here);
    held = false;
    TEST_ASSERT_OK(TagFSSettle(), "the cycle that carries it settles");
    TEST_ASSERT_OK(BinderRunWhole(), "and the Tally is written");
    TEST_ASSERT(StandFreshHolds(&m, fid), "the file reads both lines");
out:
    if (held)
        HelmLetGo(h->ofe, &here);
    if (h)
        tagfs_close(h);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/* C22: the block an Engross laid lands, the stitch that would name it does
 * not: mounted again, the tail is the old Slips over the old base, and the
 * block is nobody's. */
static TestResult TestStandSlipEngrossCut(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b = &s_slip_binder;
    StandFreshModel m;
    FileSpot        spot;
    uint64_t        fid = 0, block = 0;
    const char*     aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-c22.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 100, 0x5A00u), 100, "100 bytes are written");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "a hole with a Slip over it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    BinderDemanded();
    TEST_ASSERT_OK(TagFSSettle(), "a Tally is demanded");
    TEST_ASSERT(StandSlipEngrossed(fid), "the Helm's service lays the tail in a block");
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    bool        on  = rec && FileRecordFind(rec, 0, &spot) && spot.kind == STRIP_DATA;
    block           = on ? spot.block : 0;
    FileRecordRelease(rec);
    TEST_ASSERT(on, "the service laid the tail in a block");
    BlockFailArmRefusedWrites(b->s.seat, 0, 0);
    TEST_ASSERT(TagFSSettle() != OK, "the medium refuses the stitch that would name it");
    BlockFailDisarm();
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again on what the medium holds");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE) && StandFreshHolds(&m, fid),
                "the tail is the old Slip over the hole, and reads the same");
    rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    on  = rec && FileRecordFind(rec, 0, &spot) && spot.kind == STRIP_DATA && spot.block == block;
    FileRecordRelease(rec);
    TEST_ASSERT(!on, "and the block the Engross laid is not the file's");
out:
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/* The port's road (WriteJob) rides a Slip as the synchronous one does: a
 * small write lays no block; a write across the block lays it with the Slip's
 * bytes in it; a write into the middle of a DATA block apart from its Slip's
 * span lays the block whole, the Slip's bytes read in over its base; and a
 * write the job cuts into a window and a tail of 100 bytes in a new last
 * block lays that block too — the write began blocks before it, so it does
 * not lie in the last block alone (plot_slip.c PlotSlipBarred). On a machine
 * whose seat has no port road — one core, a legacy channel — a program's
 * write is the synchronous one the other tests of the suite hold, and this one
 * stands aside, said, rather than passing on nothing — before it takes
 * anything, as the port's other tests (StandPortAside). */
static TestResult TestStandSlipPort(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0, laid = 0, off = 0, len = 0;
    uint32_t        of  = 0;
    const char*     aside = StandPortAside(tagfs_get_seat());

    if (aside || (aside = StandStitchAside(&s)) != NULL)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM + TagFSRunBlocks() * TAGFS_BLOCK_SIZE),
                "room for the file's bytes");
    TEST_ASSERT(WriteJobSeatRunsAsync(s.seat), "a program's write on this seat goes through the port");
    TEST_ASSERT_EQ(tagfs_create_file("slip-port.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_OK(TagFSSettle(), "and its Born settled");
    laid = StandSlipLaid();
    TEST_ASSERT_OK(StandFreshPortWrite(&m, fid, 0, 100), "100 bytes are written through the port");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE) && StandSlipLaid() == laid,
                "they ride a Slip over a hole: no block written");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandSlipStitched(fid, &of, &off, &len) && off == 0 && len == 100, "the stitch lays them as its Slip");
    TEST_ASSERT_OK(StandFreshPortWrite(&m, fid, m.size, TAGFS_BLOCK_SIZE), "a block's bytes are written past them");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA) && StandFreshHolds(&m, fid),
                "both blocks laid, the Slip's bytes in the first");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    laid = StandSlipLaid();
    TEST_ASSERT_OK(StandFreshPortWrite(&m, fid, m.size, 50), "50 bytes are written at its end through the port");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_DATA) && StandSlipLaid() == laid,
                "they ride a Slip over the DATA block: no block written");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(StandFreshPortWrite(&m, fid, TAGFS_BLOCK_SIZE + 20u, 40),
                   "40 bytes are written through the port into the last block, apart from the Slip's span");
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA) && StandFreshHolds(&m, fid),
                "the block is laid whole: its base, the Slip's bytes and the write");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    laid = StandSlipLaid();
    TEST_ASSERT_OK(StandFreshPortWrite(&m, fid, m.size, (uint32_t)HelmWindowBytes(m.size) + 100u),
                   "a window and 100 bytes are written through the port at its end, the job cutting them in two");
    TestEvidence("bytes of data the window and its tail laid", StandSlipLaid() - laid);
    TEST_ASSERT(StandSlipTailIs(fid, false, STRIP_DATA) &&
                StandSlipLaid() == laid + (uint64_t)(TagFSRunBlocks() + 1u) * TAGFS_BLOCK_SIZE,
                "the tail's block is laid with the window's: no Slip over it");
    TEST_ASSERT(StandFreshHolds(&m, fid), "the file reads every byte");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "and mounted again, the same");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A cut and a growth inside a hole's block in one cycle: the stitch lays the
 * hole's zeros over the Slips the cut took, and the Ink that bared them
 * claimed them — a cycle's claims hold what its stitch lays (P21). */
static TestResult TestStandSlipCutRegrowClaimed(void)
{
    TestResult      verdict = TEST_PASS;
    StandStitch     s;
    StandFreshModel m;
    uint64_t        fid = 0, off = 0, len = 0, claimed = 0, reserved = 0;
    uint32_t        of  = 0;
    const char*     aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-regrow.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, 4000u, 0x5b00u), 4000, "4000 bytes are written");
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "a hole with a Slip over it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 1), "the file is cut to one byte");
    TEST_ASSERT_OK(StandFreshSpan(&m, fid, 4000u), "and grown back inside its block in the same cycle");
    RibbonClaimsStand(&claimed, &reserved);
    TEST_ASSERT(claimed >= 3999u, "the Inks claimed the 3999 bytes of zeros the stitch lays over what the cut took");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandSlipStitched(fid, &of, &off, &len) && off == 1 && len == 3999,
                "the stitch lays them: the change since the last one");
    TEST_ASSERT(StandFreshHolds(&m, fid), "the file reads its first byte and zeros");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&m, fid), "and mounted again, the same");
out:
    StandFreshModelLetGo(&m);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A tail whose block cannot be read is its own file's refusal: the Helm's
 * service goes on past it and lays the tail after it; the Tally waits for that
 * one file, whose next write, with the block readable, lays its own tail. */
static TestResult TestStandSlipRefusedTail(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b = &s_slip_binder;
    StandFreshModel ma, mb;
    FileSpot        spot = { 0 };
    FileRecord*     rec = NULL;
    uint64_t        fa = 0, fb = 0;
    bool            found = false;
    const char*     aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    memset(&ma, 0, sizeof(ma));
    memset(&mb, 0, sizeof(mb));
    TEST_ASSERT(StandFreshModelMake(&ma, STAND_SLIP_ROOM) && StandFreshModelMake(&mb, STAND_SLIP_ROOM),
                "room for the files' bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-unread.bin", NULL, 0, 0, &fa), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&ma, fa, 0, TAGFS_BLOCK_SIZE + 100u, 0x5c00u), TAGFS_BLOCK_SIZE + 100,
                   "a block and 100 bytes are written to it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_EQ(StandFreshWrite(&ma, fa, ma.size, 50, 0x5c01u), 50, "50 bytes ride a Slip at its end");
    TEST_ASSERT_EQ(tagfs_create_file("slip-after.bin", NULL, 0, 0, &fb), 0, "a second file is made after it");
    TEST_ASSERT_EQ(StandFreshWrite(&mb, fb, 0, 100, 0x5c02u), 100, "100 bytes ride a Slip over a hole");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again: nothing of the files held in memory");
    rec   = FileMapRecordHold(tagfs_get_state()->files, fa, NULL);
    found = rec && FileRecordFind(rec, 1, &spot) && spot.kind == STRIP_DATA;
    FileRecordRelease(rec);
    TEST_ASSERT(found, "the first file's last block is DATA");
    BlockFailArmBadSectors(b->s.seat, tagfs_block_to_sector(spot.block), TAGFS_BLOCK_SECTORS);
    BinderDemanded();
    TEST_ASSERT_OK(TagFSSettle(), "its block cannot be read; a Tally is demanded, and a cycle weighs it");
    TEST_ASSERT(StandSlipEngrossed(fb), "the service goes on past the tail it cannot lay and lays the next one");
    TEST_ASSERT(StandSlipTailIs(fa, true, STRIP_DATA), "the first file keeps its Slip");
    TEST_ASSERT(BinderRunWhole() != OK, "a Tally asked for now is refused: it waits for that tail");
    BlockFailDisarm();
    StandBlockFailRaced("stand_slip");
    TEST_ASSERT_EQ(StandFreshWrite(&ma, fa, ma.size, 10, 0x5c03u), 10, "the block read again, the file is written");
    TEST_ASSERT(StandSlipTailIs(fa, false, STRIP_DATA), "and lays its own tail in a block, the Slip in it");
    TEST_ASSERT_OK(TagFSSettle(), "the cycle that carries it settles");
    TEST_ASSERT_OK(BinderRunWhole(), "and the Tally is written");
    TEST_ASSERT(StandFreshHolds(&ma, fa) && StandFreshHolds(&mb, fb), "both files read their bytes");
    TEST_ASSERT(StandStitchRemount() && StandFreshHolds(&ma, fa) && StandFreshHolds(&mb, fb),
                "and mounted again, the same");
out:
    BlockFailDisarm();
    StandFreshModelLetGo(&ma);
    StandFreshModelLetGo(&mb);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/* A Helm the service found held asks it again at the holder's let-go: the
 * tail is laid with no cycle between — the one look the service took at a
 * held Helm is never its last. */
static TestResult TestStandSlipHeldWakes(void)
{
    TestResult       verdict = TEST_PASS;
    StandBinder*     b = &s_slip_binder;
    StandFreshModel  m;
    HelmWaiter       here;
    TagFSFileHandle* h = NULL;
    uint64_t         fid = 0;
    bool             held = false;
    const char*      aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SLIP_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("slip-wakes.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, STAND_SLIP_LINE, 0x5d00u), STAND_SLIP_LINE, "a line is written");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT((h = tagfs_open(fid, TAGFS_HANDLE_WRITE)) != NULL, "a writer opens it");
    TEST_ASSERT_OK(HelmTakeNow(h, &here), "and holds its Helm");
    held = true;
    BinderDemanded();
    TEST_ASSERT_OK(TagFSSettle(), "a Tally is demanded, and a cycle weighs it");
    HelmRunReady();
    TEST_ASSERT(StandSlipTailIs(fid, true, STRIP_HOLE), "the service found the Helm held: the Slip lies");
    HelmLetGo(h->ofe, &here);
    held = false;
    TEST_ASSERT(StandSlipEngrossed(fid), "the writer lets go, and the service, asked again, lays the tail with no "
                                         "cycle between");
    TEST_ASSERT_OK(TagFSSettle(), "the cycle that carries it settles");
    TEST_ASSERT_OK(BinderRunWhole(), "and the Tally is written");
    TEST_ASSERT(StandFreshHolds(&m, fid), "the file reads its line");
out:
    if (held)
        HelmLetGo(h->ofe, &here);
    if (h)
        tagfs_close(h);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

static const TestCase g_stand_slip_tests[] = {
    { .name = "stand_slip_logger", .func = TestStandSlipLogger,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_middle", .func = TestStandSlipMiddle,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_new_file", .func = TestStandSlipNewFile,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_cut_over_hole", .func = TestStandSlipCutOverHole,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_cut_over_data", .func = TestStandSlipCutOverData,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_guarded", .func = TestStandSlipGuarded,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_tally_waits", .func = TestStandSlipTallyWaits,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_tally_whole", .func = TestStandSlipTallyWhole,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_tally_kept", .func = TestStandSlipTallyKept,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_holder_engrosses", .func = TestStandSlipHolderEngrosses,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_engross_cut", .func = TestStandSlipEngrossCut,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_port", .func = TestStandSlipPort,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_cut_regrow_claimed", .func = TestStandSlipCutRegrowClaimed,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_refused_tail", .func = TestStandSlipRefusedTail,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_held_wakes", .func = TestStandSlipHeldWakes,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_slip_suite = {
    g_stand_slip_tests, sizeof(g_stand_slip_tests) / sizeof(g_stand_slip_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
