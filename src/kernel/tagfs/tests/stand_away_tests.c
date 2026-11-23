#include "tests.h"
#include "stand_tests.h"
#include "stand_ribbon.h"
#include "stand_stitch.h"
#include "volume_ribbon.h"
#include "volume_book.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../mount/away.h"
#include "../mount/perambulation.h"
#include "../mount/book_mount.h"
#include "../mount/errata.h"
#include "../settle/sewing.h"
#include "../settle/settle.h"
#include "../settle/binder.h"
#include "../helm/helm.h"
#include "../plot/plot.h"
#include "stand_binder.h"
#include "block_fail.h"
#include "boardroom.h"
#include "tagfs_volume.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_away suite: a book's medium that leaves and comes back (C14, C20)
//
// The volume is let go of nowhere here: the stand's departure latches it
// (TagFSStandMediumLeaves), what a hand or another machine would do to the
// medium meanwhile is done past TagFS by the Boardroom's doors, and the return
// is judged by the road a medium coming back takes (TagFSStandMediumReturns).
// A book is taken up when the medium holds it as this kernel left it — its
// last stitch where it was sewn, nothing sewn past it but this kernel's own
// attempt cut short, byte for byte, its newest slot the one it knows — a slot
// held unsure only byte for byte as sealed — and on a medium that lied its
// whole ribbon from the slot stood on — and every block of data no stitch
// settled reads back as written; its next stitch then goes on past its own
// attempt, as a retry does (M-6). Anything another machine sewed or laid gives
// the book up, and every waiter hears its change lost — a seq this kernel gave
// is no proof of its own (R11); what the mount found past the last stitch is
// no other writer's. A stitch a kept flush covered gone from where it was
// sewn, nothing another writer's in its place, is a medium that lied: the
// volume is remembered so, and its next mount holds it so. A block that did
// not land gives it up with ERR_TAGFS_NOT_LANDED, and a write that laid its
// block as the medium left is refused so at its Ink. A mark memory would not
// keep gives a return up only until it is made good. The verdict the mount
// held is held while the book is taken up, and built again by a mount of the
// medium another machine changed — no Undone laid twice. Everything laid is
// put back.
// ============================================================================

#define STAND_AWAY_FAST  (1ull << 50)   /* a read pace that makes a stitch's check cheaper than any cycle */

static uint8_t s_away_bytes[2u * TAGFS_BLOCK_SIZE];
static uint8_t s_away_slot[2u * VOLUME_BLOCK_SECTORS * BOOK_SECTOR_BYTES];

/* The zeros of a torn tail (TestStandAwayTornTail) are half a stitch laid by
 * hand, which is an atom of the ribbon long and STAND_STITCH_LAID_MOST at the
 * most: they never read past these bytes. */
_Static_assert(sizeof(s_away_bytes) >= STAND_STITCH_LAID_MOST / 2u * BOOK_SECTOR_BYTES,
               "half the longest stitch laid by hand fits the away tests' bytes");

/* `blocks` blocks of a pattern of `seed`, made a file `name` no stitch carries
 * yet, its waiter linked: *fid. */
static bool StandAwayFile(TagFSVolume* vol, const char* name, uint32_t blocks, uint8_t seed, StandStitchWaiter* waiter,
                          uint64_t* fid)
{
    for (uint32_t i = 0; i < blocks * TAGFS_BLOCK_SIZE; i++)
        s_away_bytes[i] = (uint8_t)(i * 13u + seed);
    memset(waiter, 0, sizeof(*waiter));
    if (!StandMakeFile(vol, name, s_away_bytes, blocks * TAGFS_BLOCK_SIZE, fid))
        return false;
    SettleWaitLink(vol, &waiter->w, InkedGen(vol), StandStitchAnswered);
    return true;
}

/* Whether file `fid` is in the map. */
static bool StandAwayLive(TagFSVolume* vol, uint64_t fid)
{
    FileRecord* rec = FileMapRecordHold(vol->state.files, fid, NULL);
    FileRecordRelease(rec);
    return rec != NULL;
}

/* Block `block` of the volume overwritten past TagFS with bytes no write of
 * it laid — what a medium whose cache went with its power holds there. */
static bool StandAwaySpoil(const StandStitch* s, uint64_t block)
{
    memset(s_away_bytes, 0xA5, TAGFS_BLOCK_SIZE);
    return BoardroomWrite(s->seat, s->base + block * TAGFS_BLOCK_SECTORS, TAGFS_BLOCK_SECTORS, s_away_bytes,
                          NULL) == 0;
}

/* The waiter of a change the departure held, unlinked if never answered. */
static void StandAwayUnlink(TagFSVolume* vol, StandStitchWaiter* waiter)
{
    if (!waiter->answered)
        (void)SettleWaitUnlink(vol, &waiter->w);
}

/* The seq of the sector at the volume's sector `vlba`, read past TagFS, when
 * it holds as a sector of the book there: 0 for one that does not. */
static uint64_t StandAwaySeqAt(const StandStitch* s, uint64_t vlba)
{
    BookSectorHead h;
    if (BoardroomRead(s->seat, s->base + vlba, 1, s_away_bytes) != 0 ||
        !BookSectorCheck(s_away_bytes, s->uuid, vlba) || !BookSectorHeadHolds(s_away_bytes, &h))
        return 0;
    return h.seq;
}

// ----------------------------------------------------------------------------
// C20: another machine sews two stitches past the last this kernel left
// ----------------------------------------------------------------------------

static TestResult TestStandAwayOtherStitches(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult               verdict = TEST_PASS;
    StandStitch              s, b;
    static StandStitchWaiter waiter;
    RibbonCursor             now;
    MountBookSaid            said;
    uint64_t                 fid  = 0;
    uint32_t                 c1   = 0;
    bool                     away = false;
    const char*              aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    if (s.at.atom > STAND_STITCH_LAID_MOST)
        return test_steps_aside(STAND_STITCH_ATOM_LONG);
    if (s.at.off + 2u * s.at.atom > s.at.quire.blocks * TAGFS_BLOCK_SECTORS)
        return test_steps_aside("the ribbon's Quire has no room left for two stitches laid by hand");
    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT(StandAwayFile(vol, "away-other.bin", 1, 5, &waiter, &fid), "a file is made, no stitch carrying it");
    TagFSStandMediumLeaves(vol);
    away = true;
    b  = s;
    /* Each an atom of the ribbon long: a flash page on a removable medium,
     * where this kernel's next stitch begins (§16.6 п.2). */
    c1 = StandStitchLayAt(&b, STITCH_DATA_FLUSHED, b.at.atom, &b.at.next);
    TEST_ASSERT(c1 != 0, "another machine sews a stitch where this kernel's next would go");
    b.at.last  = b.at.given + 1u;
    b.at.given = b.at.last;
    b.at.clasp = c1;
    b.at.off  += b.at.atom;
    b.at.came  = (RibbonQuire){ 0, 0 };
    TEST_ASSERT(StandStitchLayAt(&b, STITCH_DATA_FLUSHED, b.at.atom, &b.at.next) != 0, "and a second one after it");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_OTHER, "the medium comes back holding stitches this kernel "
                   "did not sew: its book is given up");
    away = false;
    TEST_ASSERT(waiter.answered && waiter.rc == ERR_DEVICE_NOT_READY, "the file's waiter is told its change is lost");
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted again on what its medium holds");
    MountBookStandSaid(vol, &said);
    TEST_ASSERT_EQ(said.last, b.at.given + 1u, "its ribbon ends on the other machine's second stitch");
    TEST_ASSERT(!StandAwayLive(vol, fid), "the file of the book given up is not on it");
    TEST_ASSERT_OK(TagFSSettle(vol), "a cycle settles on it");
    RibbonCursorStand(vol, &now);
    TEST_ASSERT(now.last > b.at.given + 1u, "its stitch goes on past the other machine's: no seq is sewn twice");
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    StandAwayUnlink(vol, &waiter);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// X3-1: a block written since the last kept flush that did not land
// ----------------------------------------------------------------------------

static TestResult TestStandAwayNotLanded(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    static StandStitchWaiter waiter;
    uint64_t                 fid  = 0, block = 0;
    bool                     away = false;
    const char*              aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT(StandAwayFile(vol, "away-lost.bin", 2, 9, &waiter, &fid) && StandBlockOf(vol, fid, 1, &block),
                "a file of two blocks is made, no stitch carrying it");
    TagFSStandMediumLeaves(vol);
    away = true;
    TEST_ASSERT(StandAwaySpoil(&s, block), "its second block holds, on the medium, what no write of it laid");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_NOT_LANDED, "the medium comes back as its book was left, "
                   "but a block written since the last kept flush reads otherwise: the book is given up");
    away = false;
    TEST_ASSERT(waiter.answered && waiter.rc == ERR_TAGFS_NOT_LANDED,
                "and the file's waiter hears its data did not land");
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted again on what its medium holds");
    TEST_ASSERT(!StandAwayLive(vol, fid), "without the file no stitch carried");
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    StandAwayUnlink(vol, &waiter);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A write whose block landed as the medium left, inked after it came back
// ----------------------------------------------------------------------------

/* Whether file `fid` is `bytes` long in the map. */
static bool StandAwaySized(TagFSVolume* vol, uint64_t fid, uint64_t bytes)
{
    FileRecord* rec  = FileMapRecordHold(vol->state.files, fid, NULL);
    bool        same = rec && rec->size == bytes;
    FileRecordRelease(rec);
    return same;
}

/* The medium leaves as the write's block is answered, before the writer hears
 * it landed — the count of departures its Plot is held to is the one before
 * the write went out — and the block is spoiled while the medium is out. The
 * book is taken up: the write is in no Docket. Its Ink reads the block back
 * and refuses the write; nothing of it is inked. */
static TestResult TestStandAwayPlotInFlight(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult        verdict = TEST_PASS;
    StandStitch       s;
    TagFSFileHandle*  handle = NULL;
    static HelmWaiter helm;
    static Plot       plot;
    PlotSource        src  = { .kernel = s_away_bytes + TAGFS_BLOCK_SIZE, .bytes = TAGFS_BLOCK_SIZE };
    InkReceipt        inked;
    uint64_t          fid = 0, at = 0, done = 0;
    bool              holding = false, loaded = false, away = false;
    const char*       aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    memset(s_away_bytes, 0x3C, 2u * TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandMakeFile(vol, "away-plot.bin", s_away_bytes, TAGFS_BLOCK_SIZE, &fid) && TagFSSettle(vol) == OK,
                "a file of one block is made and settled");
    handle = tagfs_open(vol, fid, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    TEST_ASSERT(handle, "the writer's handle opens");
    memset(&helm, 0, sizeof(helm));
    TEST_ASSERT(HelmAsk(vol, handle->ofe, &helm), "and it holds the file's Helm");
    holding = true;
    TEST_ASSERT_OK(PlotLoad(vol, &plot, fid, INK_UNWARRANTED), "the write loads the record");
    loaded    = true;
    plot.helm = &handle->ofe->helm;
    TEST_ASSERT_OK(PlotBeginWrite(vol, &plot, plot.size, false, TAGFS_BLOCK_SIZE, &at), "and takes a block to grow by");
    TagFSStandLeavesAfterRun();
    TEST_ASSERT_OK(PlotWriteBlocks(vol, &plot, at, &src, &done), "the write's block lands");
    away = TagFSMediumLeft(vol);
    TEST_ASSERT(away && plot.digest_count == 1, "and the medium leaves as it answers, before the writer hears it");
    TEST_ASSERT(StandAwaySpoil(&s, plot.digests[0].block), "the block holds, on the medium, what no write of it laid");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT, "the medium comes back as its book was left: the "
                   "write is in no Docket yet");
    away = false;
    TEST_ASSERT_OK(PlotEndWrite(vol, &plot, at, done), "the write is made ready to be the file's");
    TEST_ASSERT_EQ(TagFSInkPlot(vol, &plot, &inked), ERR_TAGFS_NOT_LANDED, "and its Ink is refused: "
                   "the block it laid before the medium left does not read back as written");
    TEST_ASSERT(StandAwaySized(vol, fid, TAGFS_BLOCK_SIZE), "the file is as the last stitch left it, nothing of the "
                "write inked");
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    if (loaded)
        PlotRelease(vol, &plot);
    if (holding)
        HelmLetGo(vol, handle->ofe, &helm);
    tagfs_close(vol, handle);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// C14 and its own attempt: the medium leaves before a stitch's last flush
// ----------------------------------------------------------------------------

/* The next cycle sewn and its medium taken out before its last flush: the
 * cycle refused, the attempt whole on the medium past the last stitch. */
static bool StandAwayCutShort(TagFSVolume* vol)
{
    StitchStandLeaves(UINT32_MAX);
    error_t rc = TagFSSettle(vol);
    return rc != OK && TagFSMediumLeft(vol);
}

/* A cycle settled of a change of its own: what makes the last stitch name a
 * next_quire (ribbon.c, RibbonNextGiven at its DONE). */
static bool StandAwaySettled(TagFSVolume* vol, const char* name)
{
    uint64_t fid = 0;
    return StandMakeFile(vol, name, "x", 1, &fid) && TagFSSettle(vol) == OK;
}

static TestResult TestStandAwayOwnAttempt(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    static StandStitchWaiter waiter;
    RibbonCursor             cut, now;
    MountBookSaid            said;
    uint64_t                 fid  = 0;
    bool                     away = false;
    const char*              aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT(StandAwaySettled(vol, "away-own-1") && StandAwaySettled(vol, "away-own-2"), "two cycles settle");
    RibbonCursorStand(vol, &cut);
    if (!cut.named.blocks || !RibbonQuireSame(&cut.quire, &cut.ended)) {
        verdict = test_steps_aside("the last stitch names no next_quire past the Quire the ribbon stands in");
        goto out;
    }
    TEST_ASSERT(StandAwayFile(vol, "away-own.bin", 2, 17, &waiter, &fid), "a file of two blocks is made");
    TEST_ASSERT(StandAwayCutShort(vol), "its stitch is sewn whole and the medium leaves before its last flush: the "
                "cycle is refused");
    away = true;
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT, "the medium comes back holding past the last stitch "
                   "only this kernel's own attempt, byte for byte as sealed: its book is taken up");
    away = false;
    RibbonCursorStand(vol, &now);
    TEST_ASSERT(RibbonQuireSame(&now.quire, &cut.named) && now.off == 0,
                "the next stitch goes to the start of the next_quire the last stitch names, never over the attempt");
    TEST_ASSERT(!waiter.answered, "the file's waiter is told nothing yet");
    TEST_ASSERT(StandAwayCutShort(vol), "that stitch too is cut short before its last flush");
    away = true;
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT, "and its book taken up again, both attempts its own");
    away = false;
    RibbonCursorStand(vol, &now);
    TEST_ASSERT(RibbonQuireSame(&now.quire, &cut.named) && now.off > 0,
                "the next goes on in the same next_quire past the attempt that was a retry already (§16.4 п.4 (б′))");
    TEST_ASSERT_OK(TagFSSettle(vol), "the cycle settles");
    TEST_ASSERT(waiter.answered && waiter.rc == OK, "and the file's waiter is answered OK by it");
    RibbonCursorStand(vol, &now);
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted again on what its medium holds");
    MountBookStandSaid(vol, &said);
    TEST_ASSERT_EQ(said.last, now.last, "its ribbon ends on the retry that settled — no attempt taken in its place");
    TEST_ASSERT(StandAwayLive(vol, fid), "and holds the file");
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    StandAwayUnlink(vol, &waiter);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A stitch of two atoms cut once its first is written: the part of its own
 * attempt that landed taken for its own, byte for byte, and the next stitch
 * sewn in the next_quire, which the mount ends on. */
static TestResult TestStandAwayCutMidway(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    RibbonCursor  cut, now;
    MountBookSaid said;
    uint64_t      first = 0, tried = 0;
    bool          kept = false, away = false;
    const char*   aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandAwaySettled(vol, "away-mid-1") && StandAwaySettled(vol, "away-mid-2"), "two cycles settle");
    RibbonCursorStand(vol, &cut);
    if (!cut.named.blocks || !RibbonQuireSame(&cut.quire, &cut.ended) ||
        cut.off + 4u * cut.atom > cut.quire.blocks * TAGFS_BLOCK_SECTORS) {
        verdict = test_steps_aside("the last stitch names no next_quire past the Quire the ribbon stands in, or "
                                   "that Quire has no room for a stitch of two atoms");
        goto out;
    }
    first = cut.quire.block * TAGFS_BLOCK_SECTORS + cut.off;
    StitchStandLeaves(cut.atom);
    TEST_ASSERT(StandStitchSayFilling(vol, 2u * cut.atom, 'm', 90, &kept) != OK && TagFSMediumLeft(vol),
                "a word whose stitch takes two atoms is said, and the medium leaves once the first is written");
    away  = true;
    tried = StandAwaySeqAt(&s, first);
    TEST_ASSERT(tried > cut.last && StandAwaySeqAt(&s, first + cut.atom) != tried,
                "the first atom of its attempt lies past the last stitch, the second does not");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT, "the medium comes back holding past the last stitch "
                   "only the part of its own attempt that landed, byte for byte as sealed: its book is taken up");
    away = false;
    RibbonCursorStand(vol, &now);
    TEST_ASSERT(RibbonQuireSame(&now.quire, &cut.named) && now.off == 0,
                "the next stitch goes to the start of the next_quire the last stitch names, never over the attempt");
    TEST_ASSERT_OK(TagFSSettle(vol), "the cycle settles");
    RibbonCursorStand(vol, &now);
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted again on what its medium holds");
    MountBookStandSaid(vol, &said);
    TEST_ASSERT_EQ(said.last, now.last, "its ribbon ends on the retry that settled — the attempt cut part way is none");
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* An attempt cut short, and another writer's stitch of the very seq laid in
 * its place: a seq this kernel gave is no proof of its own (R11) — the book
 * is given up. */
static TestResult TestStandAwayOwnForged(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult               verdict = TEST_PASS;
    StandStitch              s, b;
    static StandStitchWaiter waiter;
    uint64_t                 fid = 0, tried = 0;
    bool                     away = false;
    const char*              aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    if (s.at.atom > STAND_STITCH_LAID_MOST)
        return test_steps_aside(STAND_STITCH_ATOM_LONG);
    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT(StandAwayFile(vol, "away-forged.bin", 1, 29, &waiter, &fid), "a file is made, no stitch carrying it");
    TEST_ASSERT(StandAwayCutShort(vol), "its stitch is sewn whole and the medium leaves before its last flush");
    away  = true;
    tried = StandAwaySeqAt(&s, s.at.quire.block * TAGFS_BLOCK_SECTORS + s.at.off);
    TEST_ASSERT(tried > s.at.last, "its attempt lies where the ribbon stood, past the last stitch");
    b          = s;
    b.at.given = tried - 1u;
    /* An atom of the ribbon long, as the attempt it lies over: a flash page
     * on a removable medium (§16.6 п.2). */
    TEST_ASSERT(StandStitchLayAt(&b, STITCH_DATA_FLUSHED, b.at.atom, &b.at.next) != 0,
                "another machine sews a stitch of that seq in the very place of the attempt");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_OTHER, "the medium comes back holding a unit of a seq this "
                   "kernel gave that is not the one it sealed: its book is given up");
    away = false;
    TEST_ASSERT(waiter.answered && waiter.rc == ERR_DEVICE_NOT_READY, "the file's waiter is told its change is lost");
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    StandAwayUnlink(vol, &waiter);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A Born in a stitch cut short whose data did not land (§16.5 п.7)
// ----------------------------------------------------------------------------

static TestResult TestStandAwayBornNotLanded(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult               verdict = TEST_PASS;
    StandStitch              s;
    static StandStitchWaiter waiter;
    MountBookSaid            said;
    uint64_t                 fid  = 0, block = 0, after = 0;
    bool                     away = false;
    const char*              aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    memset(&waiter, 0, sizeof(waiter));
    TEST_ASSERT(StandAwaySettled(vol, "away-born-0"), "a cycle settles: the last cycle's D is measured");
    BlockFailReadPace(true, STAND_AWAY_FAST, 0);
    TEST_ASSERT(StandAwayFile(vol, "away-born.bin", 1, 23, &waiter, &fid) && StandBlockOf(vol, fid, 0, &block),
                "a file of one block is born");
    TEST_ASSERT(StandAwayCutShort(vol), "its stitch, sparing the flush of its data, is cut short before its last flush");
    away = true;
    TEST_ASSERT(StandAwaySpoil(&s, block), "and its block holds what no write of it laid");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_NOT_LANDED, "the medium comes back: its book given up for "
                   "the block that did not land");
    away = false;
    TEST_ASSERT(waiter.answered && waiter.rc == ERR_TAGFS_NOT_LANDED, "the Born's waiter hears ERR_TAGFS_NOT_LANDED");
    TEST_ASSERT(StandStitchRemount(vol), "the volume is mounted again on what its medium holds");
    MountBookStandSaid(vol, &said);
    TEST_ASSERT_EQ(said.ribbon[RIBBON_CODE_NOT_LANDED], 1, "the attempt, the newest stitch, is checked and undone");
    TEST_ASSERT(!StandAwayLive(vol, fid), "the file born in it does not exist");
    TEST_ASSERT(StandMakeFile(vol, "away-born-after", "y", 1, &after) && after > fid,
                "and a file made after it takes an id past the one burned");
out:
    BlockFailReadPace(false, 0, 0);
    if (away)
        (void)TagFSStandMediumReturns(vol);
    StandAwayUnlink(vol, &waiter);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A newer slot: another machine wrote a Tally meanwhile
// ----------------------------------------------------------------------------

/* The first sector of Spine slot `i`, its volume sector. */
static uint64_t StandAwaySlotVlba(TagFSVolume* vol, uint32_t i)
{
    return (vol->book.frame.spine_block + (uint64_t)i * vol->book.frame.spine_stride) * VOLUME_BLOCK_SECTORS;
}

/* The newest slot that holds (*newest) and one other (*other), read off the
 * medium past TagFS: false when the ring has no second slot. */
static bool StandAwaySlots(TagFSVolume* vol, const StandStitch* s, uint32_t* newest, uint32_t* other, uint64_t* seq)
{
    uint8_t* sector = s_away_slot;
    *seq = 0;
    for (uint32_t i = 0; i < vol->book.frame.spine_slots; i++) {
        BookSectorHead h;
        if (BoardroomRead(s->seat, s->base + StandAwaySlotVlba(vol, i), 1, sector) != 0)
            return false;
        if (BookSectorCheck(sector, s->uuid, StandAwaySlotVlba(vol, i)) && BookSectorHeadHolds(sector, &h) &&
            h.kind == BOOK_UNIT_SLOT && h.seq > *seq) {
            *seq    = h.seq;
            *newest = i;
        }
    }
    *other = *newest ? 0 : 1;
    return *seq && vol->book.frame.spine_slots > 1;
}

/* Slot `to` laid as a copy of slot `from` sealed at seq `seq` — its own eight
 * sectors kept in the second half of s_away_slot for the put-back. */
static bool StandAwaySlotForged(TagFSVolume* vol, const StandStitch* s, uint32_t from, uint32_t to, uint64_t seq)
{
    uint8_t* copy = s_away_slot;
    uint8_t* kept = s_away_slot + VOLUME_BLOCK_SECTORS * BOOK_SECTOR_BYTES;
    if (BoardroomRead(s->seat, s->base + StandAwaySlotVlba(vol, from), VOLUME_BLOCK_SECTORS, copy) != 0 ||
        BoardroomRead(s->seat, s->base + StandAwaySlotVlba(vol, to), VOLUME_BLOCK_SECTORS, kept) != 0)
        return false;
    for (uint32_t k = 0; k < VOLUME_BLOCK_SECTORS; k++) {
        BookSectorHead h;
        memcpy(&h, copy + (uint64_t)k * BOOK_SECTOR_BYTES, sizeof(h));
        h.seq = seq;
        BookSectorSeal(copy + (uint64_t)k * BOOK_SECTOR_BYTES, s->uuid, StandAwaySlotVlba(vol, to) + k, &h);
    }
    return BoardroomWrite(s->seat, s->base + StandAwaySlotVlba(vol, to), VOLUME_BLOCK_SECTORS, copy, NULL) == 0;
}

static TestResult TestStandAwayNewerSlot(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    uint32_t    newest = 0, other = 0;
    uint64_t    seq = 0, slot_vlba = 0;
    bool        away = false, forged = false;
    const char* aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    if (!StandAwaySlots(vol, &s, &newest, &other, &seq))
        return test_steps_aside("the Spine's ring has no second slot to lay a newer one in");
    /* Where the slot lies, kept before the book is given up: the frame it is
     * reckoned from goes with the book, and a slot put back by a frame let go
     * of lands on the volume's head and its Deed. */
    slot_vlba = StandAwaySlotVlba(vol, other);
    TagFSStandMediumLeaves(vol);
    away = true;
    forged = StandAwaySlotForged(vol, &s, newest, other, s.at.given + 1000u);
    TEST_ASSERT(forged, "another machine lays a slot newer than any this kernel knows");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_OTHER, "the medium comes back with a newer slot: its book is "
                   "given up");
    away = false;
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    if (forged) {
        TagFSStandLetGo(vol);
        if (BoardroomWrite(s.seat, s.base + slot_vlba, VOLUME_BLOCK_SECTORS,
                           s_away_slot + VOLUME_BLOCK_SECTORS * BOOK_SECTOR_BYTES, NULL) != 0)
            verdict = TestFailedAt(__FILE__, __LINE__, "the slot laid would not be put back (slot, 0)", other, 0);
    }
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A slot of the ring other than `not` holding a SLOT unit, read past TagFS. */
static bool StandAwaySlotOther(TagFSVolume* vol, const StandStitch* s, uint32_t not, uint32_t* i)
{
    for (uint32_t k = 0; k < vol->book.frame.spine_slots; k++) {
        BookSectorHead h;
        if (k != not && BoardroomRead(s->seat, s->base + StandAwaySlotVlba(vol, k), 1, s_away_slot) == 0 &&
            BookSectorCheck(s_away_slot, s->uuid, StandAwaySlotVlba(vol, k)) && BookSectorHeadHolds(s_away_slot, &h) &&
            h.kind == BOOK_UNIT_SLOT) {
            *i = k;
            return true;
        }
    }
    return false;
}

/* A Tally whose slot is written whole and its write refused: the slot held as
 * naming either Tally. Its unit, this kernel's own byte for byte, is taken up;
 * another machine's slot of the very seq in its place is not (R11). */
static TestResult TestStandAwaySlotUnsure(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    BinderStandSaid    bs;
    uint32_t           newest = 0, other = 0, from = 0;
    uint64_t           seq = 0;
    error_t            rc  = OK;
    bool               away = false;
    const char*        aside = StandBinderAside(vol, &b);

    if (aside)
        return test_steps_aside(aside);
    if (vol->book.frame.spine_slots < 2u) {
        verdict = test_steps_aside("the Spine's ring has no second slot to stand on while one is unsure");
        goto out;
    }
    TEST_ASSERT(StandAwaySettled(vol, "away-slot"), "a cycle settles: a Tally has a stitch to take in");
    BinderStand(vol, &bs);
    BinderStandTearSlot(VOLUME_BLOCK_SECTORS);
    rc = BinderRunWhole(vol);
    BinderStandTearSlot(0);
    TEST_ASSERT(rc != OK && rc != ERR_BUSY, "a Tally's slot is written whole and its write refused");
    TEST_ASSERT(StandAwaySlots(vol, &b.s, &newest, &other, &seq) && newest == bs.next_slot,
                "the medium holds the new Tally's slot whole, the newest of the ring");
    TagFSStandMediumLeaves(vol);
    away = true;
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT, "the medium comes back as it was left: the slot "
                   "this kernel holds unsure holds its own unit, byte for byte as sealed — its book is taken up");
    away = false;
    TagFSStandMediumLeaves(vol);
    away = true;
    TEST_ASSERT(StandAwaySlotOther(vol, &b.s, newest, &from) && StandAwaySlotForged(vol, &b.s, from, newest, seq),
                "another machine lays a slot of its own of that very seq in that very slot");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_OTHER, "the medium comes back: a seq this kernel gave is no "
                   "proof of its own, and its book is given up");
    away = false;
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The last stitch where it was sewn, and the ground past it (§3.8)
// ----------------------------------------------------------------------------

/* On a medium not shown to lie the last stitch is read back by its seq and
 * its Clasp: another machine's stitch of the same seq in its place, its body
 * and Clasp another, gives the book up. */
static TestResult TestStandAwayLastClasp(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult   verdict = TEST_PASS;
    StandStitch  s, b;
    RibbonCursor now;
    bool         away = false;
    const char*  aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    if (s.at.atom > STAND_STITCH_LAID_MOST)
        return test_steps_aside(STAND_STITCH_ATOM_LONG);
    TEST_ASSERT(StandAwaySettled(vol, "away-clasp"), "a cycle settles, its stitch the last this kernel left");
    RibbonCursorStand(vol, &now);
    TagFSStandMediumLeaves(vol);
    away       = true;
    b          = s;
    b.at.quire = now.sewn.quire;
    b.at.off   = now.sewn.off;
    b.at.given = now.last - 1u;
    TEST_ASSERT(StandStitchLayAt(&b, STITCH_DATA_FLUSHED, b.at.atom, &b.at.next) != 0,
                "another machine lays a stitch of the same seq in its place, its body and its Clasp another");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_OTHER, "the medium comes back: its last stitch does not stand "
                   "where this kernel sewed it by its Clasp, and its book is given up");
    away = false;
    TEST_ASSERT(!AwayLiedLearned(vol, s.uuid), "and the volume is not remembered as one whose medium lied: another "
                "writer's stitch of the same seq proves nothing of the medium");
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* A torn tail past the last stitch, as a power cut leaves one, is what the
 * mount found there: a return before any stitch of the session settled takes
 * the book up — no other writer gives a seq the mount saw. */
static TestResult TestStandAwayTornTail(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    MountBookSaid said;
    uint64_t      half = 0;
    bool          away = false;
    const char*   aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    if (s.at.atom > STAND_STITCH_LAID_MOST)
        return test_steps_aside(STAND_STITCH_ATOM_LONG);
    half = s.at.atom / 2u;
    TEST_ASSERT(TagFSStandLetGo(vol), "the volume is let go of");
    /* The stitch an atom of the ribbon long: a flash page on a removable
     * medium (§16.6 п.2). */
    memset(s_away_bytes, 0, sizeof(s_away_bytes));
    TEST_ASSERT(StandStitchLayAt(&s, STITCH_DATA_FLUSHED, s.at.atom, &s.at.next) != 0 &&
                BoardroomWrite(s.seat, s.base + s.at.quire.block * TAGFS_BLOCK_SECTORS + s.at.off + half, half,
                               s_away_bytes, NULL) == 0,
                "a stitch is laid at the ribbon's end and the second half of it lost: a torn tail");
    TEST_ASSERT(StandRibbonUp(vol), "the volume mounts on it");
    MountBookStandSaid(vol, &said);
    TEST_ASSERT(said.last <= s.at.given, "its ribbon ends before the torn tail");
    TagFSStandMediumLeaves(vol);
    away = true;
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT, "the medium comes back before a stitch of the session "
                   "settled: the torn tail its mount found is no other writer's, and its book is taken up");
    away = false;
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A medium that lied: the whole ribbon from the slot stood on (§16.5 п.11)
// ----------------------------------------------------------------------------

static TestResult TestStandAwayLiedWhole(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    RibbonCursor  first;
    MountBookSaid said;
    bool          away = false;
    const char*   aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    if (s.at.atom > STAND_STITCH_LAID_MOST)
        return test_steps_aside(STAND_STITCH_ATOM_LONG);
    TEST_ASSERT(TagFSStandLetGo(vol), "the volume is let go of");
    TEST_ASSERT(StandStitchLayAt(&s, STITCH_DATA_FLUSHED | STITCH_MEDIUM_LIED, s.at.atom, &s.at.next) != 0,
                "a stitch saying the medium lied, an atom of the ribbon long, is laid at the ribbon's end");
    TEST_ASSERT(StandRibbonUp(vol), "the volume mounts on it");
    MountBookStandSaid(vol, &said);
    TEST_ASSERT(said.lied, "and holds its medium to have lied");
    TEST_ASSERT(StandAwaySettled(vol, "away-lied-1"), "a cycle settles");
    RibbonCursorStand(vol, &first);
    TEST_ASSERT(StandAwaySettled(vol, "away-lied-2"), "and a second after it");
    TagFSStandMediumLeaves(vol);
    away = true;
    memset(s_away_bytes, 0, BOOK_SECTOR_BYTES);
    TEST_ASSERT(BoardroomWrite(s.seat, s.base + RibbonPlaceVlba(&first.sewn), 1, s_away_bytes, NULL) == 0,
                "the first of the two stitches loses its first sector while the medium is out");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_OTHER, "its last stitch stands where it was sewn, and still "
                   "the book is given up: on a medium that lied the whole ribbon from the slot is walked");
    away = false;
    TEST_ASSERT(AwayLiedLearned(vol, s.uuid), "a stitch a kept flush covered lost a sector, the rest of it its own: the "
                "volume is remembered as one whose medium lied");
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    (void)TagFSStandLiedForget(vol);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A medium that lost a stitch a kept flush covered: remembered as one that lied
// ----------------------------------------------------------------------------

/* `n` sectors from the volume's sector `vlba` to zeros, past TagFS. */
static bool StandAwayZeros(const StandStitch* s, uint64_t vlba, uint64_t n)
{
    uint32_t most = sizeof(s_away_bytes) / BOOK_SECTOR_BYTES;
    memset(s_away_bytes, 0, sizeof(s_away_bytes));
    for (uint64_t k = 0; k < n;) {
        uint32_t run = n - k < most ? (uint32_t)(n - k) : most;
        if (BoardroomWrite(s->seat, s->base + vlba + k, run, s_away_bytes, NULL) != 0)
            return false;
        k += run;
    }
    return true;
}

/* A cycle settles — its stitch the last this kernel left, covered by the
 * flush the medium said it kept — and the medium comes back with zeros where
 * that stitch was sewn, as a stick that answered a flush it never kept comes
 * back from its power: the book is given up, the volume remembered as one
 * whose medium lied, and its next mount holds the medium so. The lie is
 * forgotten before the ribbon is put back: the stand's volume is every
 * test's. */
static TestResult TestStandAwayLostSettled(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult    verdict = TEST_PASS;
    StandStitch   s;
    RibbonCursor  now;
    MountBookSaid said;
    uint64_t      left = 0;
    bool          away = false;
    const char*   aside = StandStitchAside(vol, &s);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(!AwayLiedLearned(vol, s.uuid), "the volume is not remembered as one whose medium lied");
    TEST_ASSERT(StandAwaySettled(vol, "away-lost"), "a cycle settles, its stitch the last this kernel left");
    RibbonCursorStand(vol, &now);
    left = RibbonPlaceLeft(&now.sewn);
    TagFSStandMediumLeaves(vol);
    away = true;
    TEST_ASSERT(StandAwayZeros(&s, RibbonPlaceVlba(&now.sewn), now.sewn_of < left ? now.sewn_of : left) &&
                (now.sewn_of <= left ||
                 StandAwayZeros(&s, now.sewn_hop.block * TAGFS_BLOCK_SECTORS, now.sewn_of - left)),
                "the medium loses that stitch while it is out: zeros where it was sewn");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_OTHER, "the medium comes back: its last stitch does not stand "
                   "where this kernel sewed it, and its book is given up");
    away = false;
    TEST_ASSERT(PerambulationLast(vol)->lied && AwayLiedLearned(vol, s.uuid), "the stitch is gone, nothing another writer's "
                "in its place, though a kept flush covered it: the volume is remembered as one whose medium lied");
    TEST_ASSERT(StandStitchRemount(vol), "the volume mounts again on what its medium holds");
    MountBookStandSaid(vol, &said);
    TEST_ASSERT(said.lied && RibbonCursorLied(vol), "and holds its medium to have lied: every stitch it sews says "
                "MEDIUM_LIED");
    TEST_ASSERT_EQ(FallowLag(vol), FALLOW_LIED_LAG, "and Fallow waits its longer lag on it");
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    (void)TagFSStandLiedForget(vol);
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

// ----------------------------------------------------------------------------
// A mark memory would not keep, made good (§16.5 п.11)
// ----------------------------------------------------------------------------

/* Memory refuses the mark of the mount's last stitch: on a medium not shown to
 * lie the next stitch settling makes it good, and a return takes the book up.
 * On one that lied, the mark refused is of a stitch past the slot, made good
 * once a Tally takes that stitch in. */
static TestResult TestStandAwayMarksRefused(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandStitch        t;
    MountBookSaid      said;
    bool               away = false;
    const char*        aside = StandBinderAside(vol, &b);

    if (aside)
        return test_steps_aside(aside);
    if (b.s.at.atom > STAND_STITCH_LAID_MOST) {
        verdict = test_steps_aside(STAND_STITCH_ATOM_LONG);
        goto out;
    }
    TEST_ASSERT(StandAwaySettled(vol, "away-marks-0") && TagFSStandLetGo(vol), "a cycle settles, a stitch past the slot, "
                "and the volume is let go of");
    RibbonStandMarksRefused();
    TEST_ASSERT(StandRibbonUp(vol), "it mounts, memory refusing to keep the place of its last stitch");
    TEST_ASSERT(StandAwaySettled(vol, "away-marks-1"), "a cycle settles, the place of its stitch kept");
    TagFSStandMediumLeaves(vol);
    away = true;
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT, "the medium comes back as it was left: held to the "
                   "last stitch alone, its book is taken up");
    away = false;
    t = b.s;
    RibbonCursorStand(vol, &t.at);
    TEST_ASSERT(TagFSStandLetGo(vol) &&
                StandStitchLayAt(&t, STITCH_DATA_FLUSHED | STITCH_MEDIUM_LIED, t.at.atom, &t.at.next) != 0,
                "the volume is let go of, and a stitch saying the medium lied, an atom of the ribbon long, laid at "
                "the ribbon's end");
    RibbonStandMarksRefused();
    TEST_ASSERT(StandRibbonUp(vol), "it mounts, memory refusing to keep the place of that stitch");
    MountBookStandSaid(vol, &said);
    TEST_ASSERT(said.lied, "and holds its medium to have lied");
    TEST_ASSERT(StandAwaySettled(vol, "away-marks-2"), "a cycle settles");
    TEST_ASSERT_OK(BinderRunWhole(vol), "and a Tally takes every stitch in");
    TagFSStandMediumLeaves(vol);
    away = true;
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT, "the medium comes back as it was left: the stitch "
                   "whose mark memory refused lies behind the slot, and its book is taken up");
    away = false;
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The verdict a mount holds, at a return (§16.5 п.12)
// ----------------------------------------------------------------------------

/* A Shaped of `fid` over the record the stitch before it laid, its one block
 * put in at `block` with `digest` (stand_errata_tests.c lays the same). */
static __attribute__((noinline)) void StandAwayShaped(StandRibbonRecords* r, uint64_t fid, uint64_t block, uint64_t digest)
{
    uint8_t  body[128], splices[48], dig[8];
    uint32_t n      = 0;
    uint64_t cursor = 0;
    TallyRun run    = { .start = block, .count = 1 };
    n += BookVarintPut(splices + n, 0);
    n += BookVarintPut(splices + n, 1);
    n += BookVarintPut(splices + n, 1);
    n += RibbonRunPut(splices + n, &run, &cursor);
    TallyLePut(dig, digest, 8);
    RibbonShaped sh = { .fid = fid, .base = 1, .size = TAGFS_BLOCK_SIZE, .mtime_ns = 2, .nsplice = 1,
                        .splices = splices, .splices_bytes = n, .ndigests = 1, .digests = dig };
    StandRibbonPut(r, RIBBON_SHAPED, body, RibbonShapedBodyPut(body, &sh));
    r->fresh += 1;
}

/* Stitch top+1 bears file F, stitch top+2 — sewn without its first flush —
 * puts a second block in for it that never landed: the mount holds the
 * Undone of top+2 as its verdict. Taken up as left, the verdict is held. Then
 * another machine, mounting the same, lays that Undone in a stitch of its
 * own past top+2: the medium comes back changed, and the mount of it finds
 * the verdict laid already — the session's first stitch carries none. */
static TestResult TestStandAwayVerdict(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult          verdict = TEST_PASS;
    StandRibbon         w;
    StandRibbonRecords *r1 = StandRibbonFresh(0), *r2 = StandRibbonFresh(1), *r3 = StandRibbonFresh(2);
    MountBookSaid       said;
    RibbonCursor        c;
    RibbonUnit*         u = NULL;
    StitchHead          h;
    uint32_t            c1 = 0, c2 = 0;
    uint64_t            digests[2], fid = 0;
    static uint8_t      body[64];
    bool                away = false;
    const char*         aside = StandRibbonAside(vol, &w);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandRibbonData(&w, digests), "two free blocks are written with a pattern");
    StandRibbonBorn(r1, w.fid, TAGFS_BLOCK_SIZE, w.free, 1, digests, 0, 0, 0);
    StandAwayShaped(r2, w.fid, w.free + 1u, digests[1] ^ 1u);
    TEST_ASSERT(TagFSStandLetGo(vol), "the volume is let go of");
    h  = StandRibbonHead(&w, w.top + 1u, w.seq, w.clasp);
    c1 = StandRibbonSew(&w, w.at, w.top + 1u, &h, r1, VOLUME_BLOCK_SECTORS);
    h  = StandRibbonHead(&w, w.top + 2u, w.top + 1u, c1);
    h.flags = 0;
    c2 = c1 ? StandRibbonSew(&w, w.at + 1u, w.top + 2u, &h, r2, VOLUME_BLOCK_SECTORS) : 0;
    TEST_ASSERT(c2 != 0, "a Born, and a Shaped whose data never landed, are laid");
    TEST_ASSERT(StandRibbonUp(vol) && ErrataHeld(vol), "the volume mounts on them, holding a verdict");
    TagFSStandMediumLeaves(vol);
    away = true;
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT, "the medium leaves and comes back as it was left");
    away = false;
    TEST_ASSERT(ErrataHeld(vol), "and the verdict is held: no stitch of the session has settled it yet");
    TagFSStandMediumLeaves(vol);
    away = true;
    StandRibbonPut(r3, RIBBON_UNDONE, body, RibbonUndoneBodyPut(body, 1, UNDONE_NOT_LANDED, &w.fid, 1));
    h = StandRibbonHead(&w, w.top + 3u, w.top + 2u, c2);
    TEST_ASSERT(StandRibbonSew(&w, w.at + 2u, w.top + 3u, &h, r3, VOLUME_BLOCK_SECTORS) != 0,
                "out of it, another machine lays the same verdict as an Undone of its own");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_OTHER, "the medium comes back changed: its book given up");
    away = false;
    TEST_ASSERT(TagFSStandLetGo(vol) && StandRibbonUp(vol), "the volume mounts on what its medium holds");
    MountBookStandSaid(vol, &said);
    TEST_ASSERT(said.ribbon[RIBBON_CODE_UNDONE] == 1 && said.ribbon[RIBBON_CODE_NOT_LANDED] == 0 && !ErrataHeld(vol),
                "the verdict is the other machine's Undone, and none is held to be laid again");
    RibbonCursorStand(vol, &c);
    TEST_ASSERT(w.nlaid < STAND_RIBBON_LAID, "room to put back the stitch the next cycle lays");
    w.laid[w.nlaid++] = (c.quire.block + c.off / TAGFS_BLOCK_SECTORS) * TAGFS_BLOCK_SECTORS;
    TEST_ASSERT_EQ(tagfs_create_file(vol, "away-verdict", NULL, 0, 0, &fid), 0, "a file is made on what it holds");
    TEST_ASSERT_OK(TagFSSettle(vol), "the session's first cycle settles, the file born in it");
    TEST_ASSERT(StandStitchRead(vol, &u), "and its stitch reads back");
    TEST_ASSERT(u->nrecords > 0 && u->records[0].kind != RIBBON_UNDONE, "and its stitch carries no Undone: none twice");
out:
    if (away)
        (void)TagFSStandMediumReturns(vol);
    RibbonUnitLetGo(u);
    if (!StandRibbonPutBack(vol, &w))
        verdict = TestFailedAt(__FILE__, __LINE__, "what the test laid would not be put back (laid, 0)",
                               w.nlaid, 0);
    return verdict;
}

static const TestCase g_stand_away_tests[] = {
    { .name = "stand_away_other_stitches", .func = TestStandAwayOtherStitches,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_not_landed", .func = TestStandAwayNotLanded,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_own_attempt", .func = TestStandAwayOwnAttempt,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_born_not_landed", .func = TestStandAwayBornNotLanded,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_newer_slot", .func = TestStandAwayNewerSlot,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_lied_whole", .func = TestStandAwayLiedWhole,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_lost_settled", .func = TestStandAwayLostSettled,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_verdict", .func = TestStandAwayVerdict,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_plot_in_flight", .func = TestStandAwayPlotInFlight,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_cut_midway", .func = TestStandAwayCutMidway,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_own_forged", .func = TestStandAwayOwnForged,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_slot_unsure", .func = TestStandAwaySlotUnsure,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_last_clasp", .func = TestStandAwayLastClasp,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_torn_tail", .func = TestStandAwayTornTail,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_away_marks_refused", .func = TestStandAwayMarksRefused,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_away_suite = {
    g_stand_away_tests, sizeof(g_stand_away_tests) / sizeof(g_stand_away_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
