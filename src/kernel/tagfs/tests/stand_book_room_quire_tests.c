#include "stand_binder.h"
#include "stand_fresh.h"
#include "stand_stitch.h"
#include "../settle/settle.h"
#include "../settle/sewing.h"
#include "../block_map/block_map.h"
#include "../block_map/block_map_derived.h"
#include "../file_map/file_map.h"
#include "../mount/book_mount.h"
#include "../ink/fallow.h"
#include "../ink/ink.h"
#include "../settle/binder.h"
#include "stand_tests.h"
#include "tagfs_volume.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// A next_quire given clear of the files the newest Tally names
// — a case of the stand_book_room suite (stand_book_room_tests.c): ground a
// file the newest Tally names gave up since is kept from the ribbon, and is
// the ribbon's too once every slot a mount may stand on took the delete in.
// ============================================================================

#define STAND_BOOK_ROOM_RUNS  16u

typedef struct {
    Strip    runs[STAND_BOOK_ROOM_RUNS];
    uint32_t n;
} StandBookRoomGround;

/* The ground file `fid` names, the first STAND_BOOK_ROOM_RUNS runs of it. */
static bool StandBookRoomGroundOf(TagFSVolume* vol, uint64_t fid, StandBookRoomGround* g)
{
    FileRecord* rec = FileMapRecordHold(vol->state.files, fid, NULL);
    StripWalk   w;
    Strip       s;
    g->n = 0;
    if (!rec)
        return false;
    StripWalkBegin(rec, &w);
    while (g->n < STAND_BOOK_ROOM_RUNS && StripWalkNextGround(&w, &s))
        g->runs[g->n++] = s;
    FileRecordRelease(rec);
    return g->n > 0;
}

static bool StandBookRoomMeets(const StandBookRoomGround* g, uint64_t start, uint64_t count)
{
    for (uint32_t i = 0; i < g->n; i++)
        if (g->runs[i].start < start + count && start < g->runs[i].start + g->runs[i].count)
            return true;
    return false;
}

/* A file of two Quires' blocks written and settled, a Tally of it written:
 * its id, and the ground it names, the newest slot's Tally naming it. */
static TestResult StandBookRoomLaid(TagFSVolume* vol, StandFreshModel* m, const char* name, uint64_t q, uint64_t* fid,
                                    StandBookRoomGround* g)
{
    TestResult verdict = TEST_PASS;
    uint32_t   bytes   = (uint32_t)(2u * q * TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandFreshModelMake(m, bytes), "room for the file");
    TEST_ASSERT_EQ(tagfs_create_file(vol, name, NULL, 0, 0, fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(vol, m, *fid, 0, bytes, 0x5Au), (int)bytes, "two Quires of it written");
    TEST_ASSERT_OK(TagFSSettle(vol), "and settled");
    TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "a Tally of it is written: the newest slot names its ground");
    TEST_ASSERT(StandBookRoomGroundOf(vol, *fid, g), "the ground it names is read");
out:
    return verdict;
}

/* A run of the ribbon of `blocks` asked for as RibbonNextGiven asks a Quire,
 * looked for from `from` on: whether it meets `g` — and it is given back. */
static TestResult StandBookRoomAsk(TagFSVolume* vol, uint64_t blocks, uint64_t from, const StandBookRoomGround* g, bool* meets)
{
    TestResult verdict = TEST_PASS;
    uint64_t   start   = 0;
    TEST_ASSERT_OK(BlockMapTakeQuire(vol, GROUND_RIBBON, blocks, 1u, 0, vol->book.frame.data_block, from, &start),
                   "a run of the ribbon is given");
    TestEvidence("the run given begins at block", start);
    *meets = StandBookRoomMeets(g, start, blocks);
    TEST_ASSERT_OK(BlockMapDerivedGiveAs(vol, GROUND_RIBBON, 0, start, blocks), "and given back");
out:
    return verdict;
}

/* Whether any run of `g` is kept from the ribbon (fallow.c, NodeBarred): ground
 * a freer gave back that a Tally a mount may stand on still names as a file's. */
static bool StandBookRoomKept(TagFSVolume* vol, const StandBookRoomGround* g)
{
    uint64_t past = 0;
    bool     kept = false;
    TagFSLockTake(&vol->state.lock);
    for (uint32_t i = 0; i < g->n && !kept; i++)
        kept = FallowBarsRun(vol, g->runs[i].start, g->runs[i].count, &past);
    TagFSLockDrop(&vol->state.lock);
    return kept;
}

/* The longest stretch of `g` that lies free from the first block of one of its
 * runs, in no file's window and inside that run, `q` blocks at the most — a
 * run of the ribbon asked from there is given there unless it is kept — its
 * first block into *from: how many blocks, 0 when none is free. A file laid in
 * one run has a Quire of it; on a book that lived its runs may be shorter,
 * and the ribbon is asked the longest of them. */
static uint64_t StandBookRoomQuireIn(TagFSVolume* vol, const StandBookRoomGround* g, uint64_t q, uint64_t* from)
{
    uint64_t most = 0;
    TagFSLockTake(&vol->state.lock);
    for (uint32_t i = 0; i < g->n; i++) {
        uint64_t free = BlockMapDerivedFreeFrom(vol, g->runs[i].start, g->runs[i].count < q ? g->runs[i].count : q, 0);
        if (free > most) {
            most  = free;
            *from = g->runs[i].start;
        }
    }
    TagFSLockDrop(&vol->state.lock);
    return most;
}

/* [from, from + n) held for the stand (GROUND_HELD) while the Tallies past the
 * delete are written: a Tally's pair of Tally-Quires is taken first-fit from
 * the data run's start (binder.c, TallyRunsTake) and may lie on the very
 * ground the ribbon is asked on again — given back before it is asked. True
 * when it is held there. */
static bool StandBookRoomHold(TagFSVolume* vol, uint64_t from, uint64_t n)
{
    uint64_t got = 0;
    if (BlockMapTakeQuire(vol, GROUND_HELD, n, 1u, 0, vol->book.frame.data_block, from, &got) != OK)
        return false;
    if (got == from)
        return true;
    (void)BlockMapDerivedGiveAs(vol, GROUND_HELD, 0, got, n);
    return false;
}

/* The stitch the oldest of the slots a mount may stand on took in — FallowLag()
 * of the newest (binder_slots.c, BinderBarPublish): ground a freer past it gave
 * back is kept from the ribbon. */
static uint64_t StandBookRoomBar(TagFSVolume* vol)
{
    uint32_t n = vol->binder.nslots < vol->binder.ring ? vol->binder.nslots : vol->binder.ring;
    return BinderBarSlice(vol->binder.slot, n, FallowLag(vol), UINT64_MAX);
}

/*
 * ‼ A NEXT_QUIRE IS GIVEN CLEAR OF THE FILES THE NEWEST TALLY NAMES. A file the
 * newest Tally holds is given up and its ground comes back: kept from the
 * ribbon, and asked where a stretch of it lies free, the ribbon is given
 * elsewhere — named past that Tally, a reader would refuse the ribbon's step
 * there (RIBBON_TORN) and every stitch past it would be lost to the next
 * mount. Once every slot a mount may stand on took the delete in — the newest,
 * the two newest on a medium that lied — the ground is the ribbon's too: none
 * of it kept, and asked there again the ribbon is given there — the stretch
 * held while those Tallies are written, so their own pairs do not take it.
 * The same holds of the ground the mount found the ribbon let go: a file
 * deleted past the Tally, the volume mounted again on it and the ground back —
 * the ribbon asked for there is given elsewhere.
 */
TestResult TestStandBookRoomNextQuireClear(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult          verdict = TEST_PASS;
    static StandBinder  b;
    StandFreshModel     m1, m2;
    StandBookRoomGround g1, g2;
    uint64_t            fid1 = 0, fid2 = 0, q = 0, gone = 0, from = 0, n = 0, held = 0;
    bool                meets = false;
    const char*         aside = StandBinderAside(vol, &b);
    memset(&m1, 0, sizeof(m1));
    memset(&m2, 0, sizeof(m2));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandStitchRemount(vol) && TagFSSettle(vol) == OK, "the volume mounted afresh, a stitch of it settled");
    q = RibbonCursorNow(vol)->quire_blocks;
    TEST_ASSERT(StandBookRoomLaid(vol, &m1, "book-room-a.bin", q, &fid1, &g1) == TEST_PASS, "the file is laid");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, fid1, INK_UNWARRANTED), 0, "it is given up");
    fid1 = 0;
    TEST_ASSERT(TagFSSettle(vol) == OK && SettleForRoom(vol) == OK, "settled, and its ground asked back");
    gone = RibbonCursorNow(vol)->last;
    TEST_ASSERT_EQ(FallowBlocks(vol), 0, "nothing rests: its ground is free again");
    TEST_ASSERT(StandBookRoomKept(vol, &g1), "and kept from the ribbon while the newest Tally names the file");
    n = StandBookRoomQuireIn(vol, &g1, q, &from);
    TestEvidence("blocks of the file's ground free in a row the ribbon is asked", n);
    TEST_ASSERT(n > 0, "a stretch of the file's ground lies free, from the first block of a run of it");
    TEST_ASSERT(StandBookRoomAsk(vol, n, from, &g1, &meets) == TEST_PASS, "the ribbon is asked there");
    TEST_ASSERT(!meets, "and given clear of the ground the newest Tally names a file on");
    TEST_ASSERT(StandBookRoomHold(vol, from, n), "the stretch held while the Tallies past the delete are written");
    held = n;
    for (uint32_t t = 0; t < vol->binder.ring && StandBookRoomBar(vol) < gone; t++)
        TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "a Tally past the delete is written");
    TEST_ASSERT(StandBookRoomBar(vol) >= gone, "within a turn of the ring every slot a mount may stand on took the "
                                            "delete in");
    TEST_ASSERT_OK(BlockMapDerivedGiveAs(vol, GROUND_HELD, 0, from, n), "the stretch given back");
    held = 0;
    TEST_ASSERT(!StandBookRoomKept(vol, &g1), "and once no such slot's Tally names the file, none of its ground is kept "
                                         "from the ribbon");
    TEST_ASSERT(StandBookRoomAsk(vol, n, from, &g1, &meets) == TEST_PASS, "the ribbon is asked there again");
    TEST_ASSERT(meets, "and is given on the file's ground: it is the ribbon's too");

    TEST_ASSERT(StandBookRoomLaid(vol, &m2, "book-room-b.bin", q, &fid2, &g2) == TEST_PASS, "a second file is laid");
    TEST_ASSERT_EQ(tagfs_delete_file(vol, fid2, INK_UNWARRANTED), 0, "it is given up");
    fid2 = 0;
    TEST_ASSERT(TagFSSettle(vol) == OK, "the delete settled past the Tally");
    TEST_ASSERT(StandStitchRemount(vol), "the volume mounted again on that Tally");
    TEST_ASSERT_OK(SettleForRoom(vol), "the ground the ribbon let go asked back");
    TEST_ASSERT_EQ(FallowBlocks(vol), 0, "nothing rests: the file's ground is free again");
    n = StandBookRoomQuireIn(vol, &g2, q, &from);
    TestEvidence("blocks of the second file's ground free in a row the ribbon is asked", n);
    TEST_ASSERT(n > 0, "a stretch of its ground lies free, from the first block of a run of it");
    TEST_ASSERT(StandBookRoomAsk(vol, n, from, &g2, &meets) == TEST_PASS, "the ribbon is asked there");
    TEST_ASSERT(!meets, "and given clear of the ground the Tally the mount stands on names a file on");
out:
    if (held)
        (void)BlockMapDerivedGiveAs(vol, GROUND_HELD, 0, from, held);
    if (fid1)
        (void)tagfs_delete_file(vol, fid1, INK_UNWARRANTED);
    if (fid2)
        (void)tagfs_delete_file(vol, fid2, INK_UNWARRANTED);
    StandFreshModelLetGo(&m1);
    StandFreshModelLetGo(&m2);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
