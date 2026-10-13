#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../settle/settle.h"
#include "../ledger/ledger.h"
#include "../mount/away.h"
#include "../metadata_pool/meta_pool.h"
#include "../file_map/file_map.h"
#include "pmm.h"          /* pages held back, and given back as a cabin's memory comes back */
#include "block_fail.h"   /* BLOCKFAIL — a read refused as a medium error */
#include "boardroom.h"
#include "amp.h"
#include "atomics.h"
#include "baton.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The road a mount takes when the medium does what a hand does — of the
// stand_medium suite, which registers these last (stand_medium_tests.c)
//
// The medium pulled out in the middle of a mount's reads, a read of the mount
// refused with the seat's bell rung inside the attempt — which asks nothing —
// and one refused with nothing to ring it, the seat reset once by its driver's
// own recovery either way; a medium error that stays, attempted twice and then
// left waiting for an event; a mount the machine had no memory for, owed until
// the memory comes back; a book given up and the ids its memory gave out kept;
// and a return one of whose Ledger reads was refused, judged only once the
// seat answers.
// Each takes the volume down and mounts it again, or takes its medium out of
// the room and seats it again, and leaves nothing at rest behind.
// ============================================================================

/* A mount of the stand's volume, start to end, on a slow medium — the test's
 * own bound, which turns the defect it stands for into a failure instead of a
 * machine that stops. */
#define STAND_MOUNT_BOUND_MS   10000u

/* The block read of a mount the seat empties at: past the first, so the mount
 * is part way through reading the volume when its medium goes. */
#define STAND_MOUNT_EMPTIES_AT 2u

/* Cycles enough for everything at rest in Fallow to come back: its freer's
 * cycle and the lag after it, for any freer the tests left, and the spare. */
#define STAND_BARE_CYCLES      8u

/* The room's and the volume's service passes run here until the mount road
 * has nothing left to do, bounded — whichever core passes its own may do the
 * work first. */
bool StandMountSettles(void)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_MOUNT_BOUND_MS);
    uint8_t  me    = amp_get_core_index();
    for (;;) {
        BoardroomAttendIfPending();
        TagFSServiceIfPending();
        if (TagFSStandMountIdle())
            return true;
        if (rdtsc() >= bound)
            return false;
        BatonPump(me);
        cpu_pause();
    }
}

/*
 * The volume as the medium holds it, and nothing resting: cycles until Fallow
 * has given back all it held, and one more, whose map says on the medium that
 * the ground came back. A take-down drops what rests without a word
 * (tagfs_clear_the_ground), and the next mount reads ground the medium's map
 * still holds as taken and named by nobody — the orphans a census gives back,
 * once it does (B1). A test that takes the volume down leaves none behind.
 */
bool StandSettledBare(void)
{
    TagFSState* fs = tagfs_get_state();
    for (uint32_t cycle = 0; cycle < STAND_BARE_CYCLES; cycle++) {
        if (TagFSSettle() != OK)
            return false;
        TagFSLockTake(&fs->lock);
        uint64_t resting = FallowBlocks();
        TagFSLockDrop(&fs->lock);
        if (resting == 0)
            return TagFSSettle() == OK;
    }
    return false;
}

/* Whatever a mount test left: its turn taken down, BLOCKFAIL down, the volume
 * up again if it is not — the medium seated, and an arrival attended — and
 * the runner counted back in. */
void StandMountClose(uint8_t seat)
{
    TagFSStandMountTurn(STAND_MOUNT_AS_IS, 0);
    BlockFailDisarm();
    if (!tagfs_get_state()->initialized) {
        if (!BoardroomSeatOccupied(seat))
            (void)BoardroomInit();
        TagFSAttendArrival();
        (void)StandMountSettles();
    }
    TagFSStandHoldAgain();
}

/*
 * R5 (Z11-2, Z15-1). The medium leaves in the middle of a mount's reads, and
 * comes back. The departure is the mount's refusal and nothing more: no book
 * is opened for a volume that was never up, and nothing waits on seat:ready —
 * the return is an arrival, and the arrival mounts. And a book a volume gone
 * left standing is not the next mount's. Red before: the mount's block reads
 * latched the departure as a mounted volume's, nothing took the latch down,
 * and every later mount was refused at its first read until the machine was
 * switched off.
 */
TestResult TestStandMediumMountLosesItsSeat(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    uint8_t     seat    = tagfs_get_seat();
    BoardKind   kind    = BoardroomSeatKind(seat);
    uint8_t     index   = BoardroomSeatIndex(seat);
    uint32_t    epoch   = tagfs_mount_epoch();
    bool        let_go  = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandSettledBare(), "every change is on the medium, and nothing rests, before it goes");
    let_go = true;
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, writing nothing");
    BoardroomNoteDeparture(kind, index);
    TEST_ASSERT(!BoardroomSeatOccupied(seat), "and its medium leaves the room");

    TagFSStandMountTurn(STAND_MOUNT_SEAT_EMPTIES, STAND_MOUNT_EMPTIES_AT);
    TEST_ASSERT_EQ(BoardroomInit(), 1, "the medium is seated again");
    TEST_ASSERT(StandMountSettles(), "and its arrival is attended");
    TEST_ASSERT(TagFSStandMountTurnFired(),
                "the seat emptied under the mount, in the middle of its reads");
    TEST_ASSERT(!fs->initialized, "the mount that lost its medium mounted nothing");
    TEST_ASSERT(!TagFSMediumLeft(), "and opened no book: no volume was up to hold one");
    TEST_ASSERT(!TagFSStandMountOwed(),
                "and nothing waits for seat:ready: the medium's return is the arrival that asks");
    TagFSStandMountTurn(STAND_MOUNT_AS_IS, 0);

    TagFSStandMediumLeaves();
    TEST_ASSERT(TagFSMediumLeft(), "a book is left standing while no volume is up");
    TEST_ASSERT_EQ(BoardroomInit(), 1, "the medium arrives again");
    TEST_ASSERT(StandMountSettles(), "and its arrival is attended");
    TEST_ASSERT(fs->initialized, "and the volume is MOUNTED");
    TEST_ASSERT_EQ(tagfs_mount_epoch(), epoch + 1u, "once");
    TEST_ASSERT(!TagFSMediumLeft(), "and holds no book a volume before it left standing");
out:
    if (let_go)
        StandMountClose(seat);
    return verdict;
}

/* The mount's attempts and the seats it reset since `attempts0` / `resets0`. */
static void StandMountCountedSince(uint32_t attempts0, uint32_t resets0, uint32_t* attempts,
                                   uint32_t* resets)
{
    TagFSStandMountCounts(attempts, resets);
    *attempts -= attempts0;
    *resets   -= resets0;
}

/*
 * R4 (Z11-5, Z15-4), and the bell's place (P14). A read of the mount is
 * refused, and the seat says seat:ready in the same breath — before the
 * attempt has ended, as a driver's recovery says it. That bell is the
 * attempt's own and asks nothing; the seat, rung by nothing after the attempt,
 * is reset once, and its bell asks the mount again: two attempts, one reset,
 * and the volume mounts. Red before Z11-5: the bell came while nothing was
 * owed yet, was dropped, and the mount waited for one that never came. Red
 * before P14: the bell asked the mount again at once — with a medium error
 * that stays, on every service pass for good (stand_medium_mount_bad_sector_
 * waits).
 */
TestResult TestStandMediumMountRingsInItsOwnBreath(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    uint8_t     seat    = tagfs_get_seat();
    BoardKind   kind    = BoardroomSeatKind(seat);
    uint8_t     index   = BoardroomSeatIndex(seat);
    uint32_t    epoch   = tagfs_mount_epoch();
    uint64_t    first   = tagfs_block_to_sector(0);
    uint64_t    span    = (uint64_t)fs->layout.data_blocks * TAGFS_BLOCK_SECTORS;
    uint32_t    attempts0 = 0, resets0 = 0, attempts = 0, resets = 0;
    bool        let_go  = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandSettledBare(), "every change is on the medium, and nothing rests, before it goes");
    let_go = true;
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, writing nothing");
    BoardroomNoteDeparture(kind, index);
    TEST_ASSERT(!BoardroomSeatOccupied(seat), "and its medium leaves the room");

    TagFSStandMountCounts(&attempts0, &resets0);
    BlockFailArm(seat, BLOCK_FAIL_READ, first, span, 1);
    TagFSStandMountTurn(STAND_MOUNT_SEAT_RINGS, 0);
    TEST_ASSERT_EQ(BoardroomInit(), 1, "the medium is seated again");
    TEST_ASSERT(StandMountSettles(), "and its arrival is attended, and what it owes");
    TEST_ASSERT(BlockFailFired(), "the mount's first read of the volume's data was refused");
    TEST_ASSERT(TagFSStandMountTurnFired(),
                "and the seat said seat:ready before the attempt had ended");
    StandMountCountedSince(attempts0, resets0, &attempts, &resets);
    TEST_ASSERT_EQ(resets, 1u, "that bell asked nothing: the seat was reset once");
    TEST_ASSERT_EQ(attempts, 2u, "and its bell asked the mount again, once");
    TEST_ASSERT(fs->initialized, "the volume is MOUNTED");
    TEST_ASSERT_EQ(tagfs_mount_epoch(), epoch + 1u, "once");
    TEST_ASSERT(!TagFSStandMountOwed(), "and nothing is owed any more");
out:
    if (let_go)
        StandMountClose(seat);
    return verdict;
}

/* Service passes the test runs to see that no attempt comes without an event:
 * each one a full turn of the room and the volume, many more than the one a
 * bell left owed would need. */
#define STAND_MOUNT_IDLE_TURNS 64u

/*
 * P14. The sectors of both of the Ledger's copies cannot be read, and stay so
 * — a medium error under the metadata the mount reads first — and the seat's
 * recovery says seat:ready each time it answers the refused command. The
 * attempts are counted: the arrival's, refused; the seat reset once, its bell
 * asking the second, refused; and none after that while nothing new happens,
 * however many service passes go by. A bell after the attempt — a command of
 * somebody else's answered — asks once more, and with the sectors whole again
 * the volume mounts. Red before: the bell rung inside each attempt asked the
 * next one, and the mount went again on every service pass for as long as the
 * error stood.
 */
TestResult TestStandMediumMountBadSectorWaits(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    uint8_t     seat    = tagfs_get_seat();
    BoardKind   kind    = BoardroomSeatKind(seat);
    uint8_t     index   = BoardroomSeatIndex(seat);
    uint32_t    epoch   = tagfs_mount_epoch();
    uint64_t    ledger  = tagfs_get_volume_base() +
                          (uint64_t)fs->layout.state_block * TAGFS_BLOCK_SECTORS;
    uint32_t    attempts0 = 0, resets0 = 0, attempts = 0, resets = 0;
    bool        let_go  = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandSettledBare(), "every change is on the medium, and nothing rests, before it goes");
    let_go = true;
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, writing nothing");
    BoardroomNoteDeparture(kind, index);
    TEST_ASSERT(!BoardroomSeatOccupied(seat), "and its medium leaves the room");

    TagFSStandMountCounts(&attempts0, &resets0);
    BlockFailArmBadSectors(seat, ledger, (uint64_t)VOLUME_LEDGER_COPIES * TAGFS_BLOCK_SECTORS);
    TagFSStandMountTurn(STAND_MOUNT_SEAT_RINGS, 0);
    TEST_ASSERT_EQ(BoardroomInit(), 1, "the medium is seated again");
    TEST_ASSERT(StandMountSettles(),
                "its arrival is attended, and the mount road comes to rest with the error standing");
    TEST_ASSERT(TagFSStandMountTurnFired(), "the seat said seat:ready inside the attempts");
    StandMountCountedSince(attempts0, resets0, &attempts, &resets);
    TEST_ASSERT_EQ(resets, 1u, "the seat was reset once");
    TEST_ASSERT_EQ(attempts, 2u, "two attempts: the arrival's, and the one the reset's bell asked");
    TEST_ASSERT(!fs->initialized, "nothing is mounted");
    TEST_ASSERT(TagFSStandMountOwed(), "and the mount is owed");

    for (uint32_t turn = 0; turn < STAND_MOUNT_IDLE_TURNS; turn++)
        TEST_ASSERT(StandMountSettles(), "every service pass after it comes to rest at once");
    StandMountCountedSince(attempts0, resets0, &attempts, &resets);
    TEST_ASSERT_EQ(attempts, 2u, "and no attempt comes without a new event");
    TEST_ASSERT_EQ(resets, 1u, "nor a second reset");

    BlockFailDisarm();
    TagFSStandMountTurn(STAND_MOUNT_AS_IS, 0);
    BoardroomNoteSeatReady(kind, index);
    TEST_ASSERT(StandMountSettles(), "a bell after the attempt is attended");
    StandMountCountedSince(attempts0, resets0, &attempts, &resets);
    TEST_ASSERT_EQ(attempts, 3u, "it asked the mount once more");
    TEST_ASSERT(fs->initialized, "and with the sectors whole, the volume is MOUNTED");
    TEST_ASSERT_EQ(tagfs_mount_epoch(), epoch + 1u, "once");
    TEST_ASSERT(!TagFSStandMountOwed(), "and nothing is owed any more");
out:
    if (let_go)
        StandMountClose(seat);
    return verdict;
}

/*
 * R5 (Z11-1). A read of the block that holds a file's only record is refused
 * while the mount walks the pool — a flash drive dropping one command. A
 * refusal is no verdict on any seal: the mount is refused as the read was,
 * asked again when the seat says it answers, and stands on the newest seal
 * with the file in it. Red before: the refused block was held against the
 * seals as one that is not the pool's — the mount went on to roll a block
 * back, fall back to the cycle before, or read the chain as a kernel before
 * the seal would, and was never refused at all.
 */
TestResult TestStandMediumMountRefusedReadIsNoVerdict(void)
{
    TestResult   verdict = TEST_PASS;
    TagFSState*  fs      = tagfs_get_state();
    uint8_t      seat    = tagfs_get_seat();
    BoardKind    kind    = BoardroomSeatKind(seat);
    uint8_t      index   = BoardroomSeatIndex(seat);
    uint32_t     epoch   = tagfs_mount_epoch();
    uint64_t     file_id = 0;
    uint32_t     block = 0;
    bool         let_go  = false;
    RecordWhere  where;
    FileRecord*  rec;
    const char*  aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0xB2u);
    TEST_ASSERT(StandMakeFile("refused_mid_mount", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    TEST_ASSERT(StandSettledBare(), "every change is on the medium, and nothing rests, before it goes");
    rec = FileMapRecordHold(fs->files, file_id, &where);
    TEST_ASSERT(rec, "the file's record is held");
    FileRecordRelease(rec);
    TEST_ASSERT(PoolLinkBlock(where.link, &block), "and lies in a block of the pool");
    let_go = true;
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, writing nothing");
    BoardroomNoteDeparture(kind, index);
    TEST_ASSERT(!BoardroomSeatOccupied(seat), "and its medium leaves the room");

    BlockFailArm(seat, BLOCK_FAIL_READ, tagfs_block_to_sector(block), TAGFS_BLOCK_SECTORS, 1);
    TagFSStandMountTurn(STAND_MOUNT_SEAT_RINGS, 0);
    TEST_ASSERT_EQ(BoardroomInit(), 1, "the medium is seated again");
    TEST_ASSERT(StandMountSettles(), "and its arrival is attended, and what it owes");
    TEST_ASSERT(BlockFailFired(), "the mount's read of the block holding the file's record was refused");
    TEST_ASSERT(TagFSStandMountTurnFired(),
                "the mount was refused as the read was, and the seat said seat:ready");
    TEST_ASSERT(fs->initialized, "asked again, the volume is MOUNTED");
    TEST_ASSERT_EQ(tagfs_mount_epoch(), epoch + 1u, "once");
    rec = FileMapRecordHold(fs->files, file_id, NULL);
    TEST_ASSERT(rec, "and the file is there: nothing was rolled back");
    FileRecordRelease(rec);
out:
    if (let_go)
        StandMountClose(seat);
    if (file_id && fs->initialized) {
        (void)tagfs_delete_file(file_id, INK_UNWARRANTED);
        (void)TagFSSettle();
    }
    return verdict;
}

/*
 * R9 (Z11-9). A book is given up while the volume's memory holds a create and
 * a tag no cycle settled — ids a program holds, which the medium never saw.
 * The volume is read again, and its next file id and next tag id are past
 * them: neither is given out again for the rest of the boot. Red before: the
 * mount read its numbers off the medium alone, and the next create was handed
 * the id the program still held.
 */
TestResult TestStandMediumGivenUpIdsNotGivenAgain(void)
{
    TestResult         verdict = TEST_PASS;
    TagFSState*        fs      = tagfs_get_state();
    uint8_t            seat    = tagfs_get_seat();
    BoardKind          kind    = BoardroomSeatKind(seat);
    uint8_t            index   = BoardroomSeatIndex(seat);
    uint64_t           file_id = 0;
    uint32_t           tag     = TAGFS_INVALID_TAG_ID;
    bool               let_go  = false;
    VolumeLedger       found;
    const TagSpelling  spelling = TagSpellingOfKey("stand_given_up");
    const char*        aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandSettledBare(), "every change is on the medium, and nothing rests, before it goes");
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0xB3u);
    TEST_ASSERT(StandMakeFile("given_up_ids", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    TEST_ASSERT_OK(tagfs_add_tag_string(file_id, &spelling, INK_UNWARRANTED),
                   "it wears a tag new to the volume");
    tag = TagRegistryLookup(fs->registry, &spelling);
    TEST_ASSERT(tag != TAGFS_INVALID_TAG_ID, "whose id the registry gave");
    TEST_ASSERT(InkedGen() > SettledGen(), "and no cycle settled either");

    TagFSStandMediumLeaves();
    TEST_ASSERT(LedgerNewestOn(seat, tagfs_get_volume_base(), &found),
                "the medium's newest Ledger is read");
    found.mark.closed_crc32 ^= 1u;
    TEST_ASSERT_EQ(TagFSStandMediumReturns(&found), AWAY_OTHER,
                   "the medium comes back another's, and the book is given up");
    let_go = true;
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, writing nothing");
    BoardroomNoteDeparture(kind, index);
    TEST_ASSERT(!BoardroomSeatOccupied(seat), "and its medium leaves the room");
    TEST_ASSERT_EQ(BoardroomInit(), 1, "the medium is seated again");
    TEST_ASSERT(StandMountSettles(), "and its arrival is attended");
    TEST_ASSERT(fs->initialized, "and the volume is MOUNTED");
    TEST_ASSERT(tagfs_file_ceiling() > file_id,
                "its next file id is past the one the given-up book's memory gave out");
    TagFSLockTake(&fs->registry->lock);
    uint32_t next_tag = fs->registry->next_id;
    TagFSLockDrop(&fs->registry->lock);
    TEST_ASSERT(next_tag > tag, "and its next tag id past the tag it gave out");
out:
    if (let_go)
        StandMountClose(seat);
    return verdict;
}

/*
 * R9 (Z11-8). The medium leaves and comes back as it was left, and the read of
 * its newest Ledger copy is refused once — a flash drive dropping one command.
 * A judge one of whose reads was refused is no judge: the book stays open; the
 * seat, refused with nothing left to bring it back, is reset once; and the
 * judge read again takes the book up. Red before: the older copy alone was
 * judged — a Ledger behind the one a cycle settled — and the book was given up
 * with the medium accused of lying.
 */
TestResult TestStandMediumReturnRefusedOnce(void)
{
    TestResult   verdict = TEST_PASS;
    TagFSState*  fs      = tagfs_get_state();
    uint8_t      seat    = tagfs_get_seat();
    BoardKind    kind    = BoardroomSeatKind(seat);
    uint8_t      index   = BoardroomSeatIndex(seat);
    uint32_t     epoch   = tagfs_mount_epoch();
    uint64_t     base    = tagfs_get_volume_base();
    VolumeLedger newest;
    VolumeLedger first;
    const char*  aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandSettledBare(), "every change is on the medium, and nothing rests, before it goes");
    TEST_ASSERT(LedgerNewestOn(seat, base, &newest), "the medium's newest Ledger is read");
    uint64_t copy0 = (uint64_t)fs->layout.state_block * TAGFS_BLOCK_SECTORS;
    TEST_ASSERT_EQ(tagfs_volume_read(copy0, 1, g_test_output), 0, "and its first copy");
    memcpy(&first, g_test_output, sizeof(first));
    uint64_t at  = base + copy0 + (first.seq == newest.seq ? 0u : TAGFS_BLOCK_SECTORS);
    uint32_t gen = BoardroomSeatReadyGen(seat);

    BoardroomNoteDeparture(kind, index);
    TEST_ASSERT(TagFSMediumLeft(), "the medium leaves, and the book opens");
    BlockFailArm(seat, BLOCK_FAIL_READ, at, 1, 1);
    TEST_ASSERT_EQ(BoardroomInit(), 1, "it is seated again");
    TEST_ASSERT(StandMountSettles(), "and its return is attended, and what it owes");
    TEST_ASSERT(BlockFailFired(), "the read of its newest Ledger copy was refused, once");
    TEST_ASSERT(BoardroomSeatReadyGen(seat) != gen, "the seat, reset, rang its bell");
    TEST_ASSERT(!TagFSMediumLeft(), "and judged again on it, the book is taken up");
    TEST_ASSERT(fs->initialized && tagfs_mount_epoch() == epoch,
                "the volume up all along, never read again");
    TEST_ASSERT(!AwayLiedLearned(fs->uuid), "and its medium is not taken for one that lied");
    TEST_ASSERT(!TagFSStandMountOwed(), "nothing is owed any more");
out:
    BlockFailDisarm();
    return verdict;
}

/*
 * (a). A read of the mount is refused as a medium error, and nothing rings the
 * seat's bell: no driver recovered from it, and while the mount is owed nobody
 * sends the seat a command. The mount resets the seat once, by its driver's
 * own recovery; the medium comes back from it, rings, and the mount goes again
 * and stands. Red before: the mount waited for a bell no command could ring,
 * until a hand replugged the medium.
 */
TestResult TestStandMediumMountResetsTheSeat(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    uint8_t     seat    = tagfs_get_seat();
    BoardKind   kind    = BoardroomSeatKind(seat);
    uint8_t     index   = BoardroomSeatIndex(seat);
    uint32_t    epoch   = tagfs_mount_epoch();
    uint64_t    first   = tagfs_block_to_sector(0);
    uint64_t    span    = (uint64_t)fs->layout.data_blocks * TAGFS_BLOCK_SECTORS;
    bool        let_go  = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandSettledBare(), "every change is on the medium, and nothing rests, before it goes");
    let_go = true;
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, writing nothing");
    BoardroomNoteDeparture(kind, index);
    TEST_ASSERT(!BoardroomSeatOccupied(seat), "and its medium leaves the room");

    uint32_t gen = BoardroomSeatReadyGen(seat);
    BlockFailArm(seat, BLOCK_FAIL_READ, first, span, 1);
    TEST_ASSERT_EQ(BoardroomInit(), 1, "the medium is seated again");
    TEST_ASSERT(StandMountSettles(), "and its arrival is attended, and what it owes");
    TEST_ASSERT(BlockFailFired(), "the mount's first read of the volume's data was refused");
    TEST_ASSERT(BoardroomSeatReadyGen(seat) != gen, "the seat, reset, rang its bell");
    TEST_ASSERT(fs->initialized, "the mount went again on it, and the volume is MOUNTED");
    TEST_ASSERT_EQ(tagfs_mount_epoch(), epoch + 1u, "once");
    TEST_ASSERT(!TagFSStandMountOwed(), "and nothing is owed any more");
out:
    if (let_go)
        StandMountClose(seat);
    return verdict;
}

/* Pages the memory test holds back and gives back: more than the stand's other
 * cores take and give meanwhile, so their coming back is the event itself. */
#define STAND_HELD_PAGES 64u

/*
 * (b) (Z11-17). The mount finds no memory for its registry — the first
 * allocation of the load answered NULL, as an exhausted heap answers it. That
 * is no verdict on the volume: the mount is owed until the machine's memory
 * comes back, and then goes again and stands. Red before: the load answered
 * -1 for both, the mount took it for a registry that would not read, and
 * nothing asked again until a medium arrived.
 */
TestResult TestStandMediumMountWaitsForMemory(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    uint8_t     seat    = tagfs_get_seat();
    BoardKind   kind    = BoardroomSeatKind(seat);
    uint8_t     index   = BoardroomSeatIndex(seat);
    uint32_t    epoch   = tagfs_mount_epoch();
    bool        let_go  = false;
    void*       held    = NULL;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandSettledBare(), "every change is on the medium, and nothing rests, before it goes");
    let_go = true;
    TEST_ASSERT(TagFSStandLetGo(), "the volume is let go of, writing nothing");
    BoardroomNoteDeparture(kind, index);
    TEST_ASSERT(!BoardroomSeatOccupied(seat), "and its medium leaves the room");

    held = PmmTakeExact(STAND_HELD_PAGES);
    TEST_ASSERT(held != NULL, "pages of the machine are held back");
    TagFSStandMountTurn(STAND_MOUNT_HEAP_REFUSES, 0);
    TEST_ASSERT_EQ(BoardroomInit(), 1, "the medium is seated again");
    TEST_ASSERT(StandMountSettles(), "and its arrival is attended");
    TEST_ASSERT(TagFSStandMountTurnFired() && HeapFailFired(),
                "the registry's load met a heap with nothing to give");
    HeapFailDisarm();
    TEST_ASSERT(fs->initialized || TagFSStandMountOwed(),
                "and the mount is owed for it, not refused for what the volume holds");

    PmmGiveExact(held, STAND_HELD_PAGES);
    held = NULL;
    TagFSMemoryCameBack();
    TEST_ASSERT(StandMountSettles(), "the memory comes back, and what the mount owes is attended");
    TEST_ASSERT(fs->initialized, "the volume is MOUNTED");
    TEST_ASSERT_EQ(tagfs_mount_epoch(), epoch + 1u, "once");
    TEST_ASSERT(!TagFSStandMountOwed(), "and nothing is owed any more");
out:
    HeapFailDisarm();
    if (held)
        PmmGiveExact(held, STAND_HELD_PAGES);
    if (let_go)
        StandMountClose(seat);
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
