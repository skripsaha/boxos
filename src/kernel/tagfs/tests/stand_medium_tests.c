#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../helm/helm.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../settle/settle.h"
#include "../scribe/scribe.h"
#include "../ledger/ledger.h"
#include "../mount/away.h"
#include "../metadata_pool/meta_pool.h"
#include "block_fail.h"   /* BLOCKFAIL — a flush answered with the sense a USB disk gives */
#include "boardroom.h"
#include "xhci_msd.h"     /* the USB disk's memory of its SYNCHRONIZE CACHE answer */
#include "touch.h"        /* an ear on seat:ready */
#include "logbook.h"
#include "amp.h"
#include "atomics.h"
#include "baton.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The real medium (step 1.12)
//
// A USB disk's answer to SYNCHRONIZE CACHE, heard for what it says: NO MEDIUM
// is a flush that kept nothing, and ILLEGAL REQUEST is a device that has no
// such command — remembered for its seating, and not asked again. The answers
// are the driver's own judgment of a sense BLOCKFAIL hands it in place of the
// device's (block_fail.h), so they are asked only where the volume sits on a
// USB disk.
//
// And the open book (mount/away.h): a medium that leaves takes nothing down,
// its waiters wait with it, and its return as it was left loses nothing —
// while a return this kernel did not leave answers every one of them with the
// changes that went. The departure and the return are the stand's own
// (TagFSStandMediumLeaves / Returns): the medium stays where it is, and the
// return road's judge reads it as a medium that came back.
// ============================================================================

#define STAND_SENSE_NOT_READY       0x02u
#define STAND_SENSE_ILLEGAL_REQUEST 0x05u
#define STAND_ASC_INVALID_OPCODE    0x20u
#define STAND_ASC_NO_MEDIUM         0x3Au

#define STAND_BOOK_BOUND_MS 1000u

/* Why the flush tests cannot be asked here, or NULL. */
static const char* StandFlushAside(uint8_t seat)
{
    if (BoardroomSeatKind(seat) != BOARD_USB)
        return "the volume's medium is not a USB disk, and SYNCHRONIZE CACHE is a USB disk's "
               "command";
    return StandStepsAsideFor();
}

/*
 * NO MEDIUM on a flush is a refusal. The driver kept it beside ILLEGAL REQUEST
 * as "no error here" — Linux's list — so a stick whose medium was gone while
 * its cache was being committed answered the durability barrier with success.
 * Red before step 1.12: the flush answered OK, and every write before it was
 * called kept.
 */
static TestResult TestStandMediumNoMediumFlushRefused(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    const char* aside   = StandFlushAside(seat);

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_OK(tagfs_flush_cache(), "a flush the device carries out is kept");
    uint64_t need = VolumeFlushNeed();
    BlockFailArmFlushSense(seat, STAND_SENSE_NOT_READY, STAND_ASC_NO_MEDIUM, 0, 1);
    error_t rc    = tagfs_flush_cache();
    bool    fired = BlockFailFired();
    BlockFailDisarm();
    TEST_ASSERT(fired, "the flush was answered MEDIUM NOT PRESENT");
    TestEvidence("the flush's answer to MEDIUM NOT PRESENT:", (uint64_t)rc);
    TEST_ASSERT(rc != OK, "and is refused, never success");
    TEST_ASSERT(!VolumeFlushKept(need), "no write before it is called kept by it");
    TEST_ASSERT_OK(tagfs_flush_cache(), "the next flush, answered by the device, is kept");
out:
    BlockFailDisarm();
    return verdict;
}

/*
 * ILLEGAL REQUEST to SYNCHRONIZE CACHE is remembered for the seating: the
 * device said it has no such command, and later flushes are not put to it —
 * a NO MEDIUM armed for the next one strikes and never reaches the judge. The
 * memory is said once, when it is made (the line is counted on the host).
 * Forgotten, the device is asked again, and the NO MEDIUM refuses.
 */
static TestResult TestStandMediumSyncCacheRemembered(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint8_t     unit    = BoardroomSeatIndex(seat);
    bool        known   = false;
    const char* aside   = StandFlushAside(seat);

    if (aside)
        return test_steps_aside(aside);

    BlockFailArmFlushSense(seat, STAND_SENSE_ILLEGAL_REQUEST, STAND_ASC_INVALID_OPCODE, 0, 1);
    known = true;
    TEST_ASSERT_OK(tagfs_flush_cache(), "a device with no SYNCHRONIZE CACHE is taken at its word");
    TEST_ASSERT(BlockFailFired(), "its answer was ILLEGAL REQUEST");

    BlockFailArmFlushSense(seat, STAND_SENSE_NOT_READY, STAND_ASC_NO_MEDIUM, 0, 1);
    TEST_ASSERT_OK(tagfs_flush_cache(),
                   "the next flush is not put to the device: its answer stands for the seating");
    TEST_ASSERT(BlockFailFired(), "though a NO MEDIUM was armed for it, and struck");
    BlockFailDisarm();

    xhci_msd_forget_flush_answer(unit);
    known = false;
    BlockFailArmFlushSense(seat, STAND_SENSE_NOT_READY, STAND_ASC_NO_MEDIUM, 0, 1);
    TEST_ASSERT(tagfs_flush_cache() != OK,
                "forgotten, the device is asked again, and its NO MEDIUM refuses");
    BlockFailDisarm();
    TEST_ASSERT_OK(tagfs_flush_cache(), "and a flush it carries out is kept");
out:
    BlockFailDisarm();
    if (known)
        xhci_msd_forget_flush_answer(unit);
    return verdict;
}

// ----------------------------------------------------------------------------
// The open book
// ----------------------------------------------------------------------------

/* A place in a Helm's FIFO: whether the Helm was handed to it, or it was
 * answered that the medium left. Static: a waiter still queued when a test
 * fails must not be on a stack that is gone. */
typedef struct {
    HelmWaiter        waiter;
    volatile uint32_t handed;
    volatile uint32_t gone;
    uint64_t          lost;
} StandBookTurn;

/* A waiter for a cycle, as an anchor's is. */
typedef struct {
    SettleWaiter w;
    bool         answered;
    error_t      rc;
} StandBookWait;

static StandBookTurn s_waiting;
static StandBookTurn s_late;
static HelmWaiter    s_holder;
static StandBookWait s_anchor;

static void StandBookHanded(HelmWaiter* w)
{
    StandBookTurn* t = container_of(w, StandBookTurn, waiter);
    __atomic_store_n(&t->handed, 1u, __ATOMIC_RELEASE);
}

static void StandBookGone(HelmWaiter* w, uint64_t lost)
{
    StandBookTurn* t = container_of(w, StandBookTurn, waiter);
    t->lost = lost;
    __atomic_add_fetch(&t->gone, 1u, __ATOMIC_ACQ_REL);
}

static void StandBookTurnSet(StandBookTurn* t)
{
    memset(t, 0, sizeof(*t));
    t->waiter.hand = StandBookHanded;
    t->waiter.gone = StandBookGone;
}

static void StandBookAnswered(SettleWaiter* w, error_t rc)
{
    StandBookWait* s = container_of(w, StandBookWait, w);
    s->rc       = rc;
    s->answered = true;
}

/* The service point run here until `t` was handed the Helm or answered —
 * whichever core passes its own may run it first — bounded. */
static bool StandBookTurnSettled(const StandBookTurn* t)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_BOOK_BOUND_MS);
    uint8_t  me    = amp_get_core_index();
    for (;;) {
        HelmRunReady();
        if (__atomic_load_n(&t->handed, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&t->gone, __ATOMIC_ACQUIRE))
            return true;
        if (rdtsc() >= bound)
            return false;
        BatonPump(me);
        cpu_pause();
    }
}

/* What a book is held with: a file whose tag is on the medium, settled; its
 * Helm held by one handle and waited for by another; and a change of it no
 * cycle settled — the tag dropped, which writes nothing — with an anchor's
 * waiter for it. */
typedef struct {
    uint32_t         file_id;
    uint16_t         tag;
    TagFSFileHandle* holder;
    TagFSFileHandle* waiting;
    bool             holding;
    bool             queued;
    bool             anchored;
    uint64_t         gen;
    uint64_t         unsettled;
} StandBook;

static const TagSpelling* StandBookTag(void)
{
    static TagSpelling tag;
    tag = TagSpellingOfKey("stand_book");
    return &tag;
}

static TestResult StandBookOpen(StandBook* b, const char* name)
{
    TestResult verdict = TEST_PASS;
    memset(b, 0, sizeof(*b));
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0xB0u);
    TEST_ASSERT(StandMakeFile(name, g_test_buffer, TAGFS_BLOCK_SIZE, &b->file_id),
                "a file is made and written");
    TEST_ASSERT_OK(tagfs_add_tag_string(b->file_id, StandBookTag()), "it wears stand_book");
    b->tag = TagRegistryLookup(tagfs_get_state()->registry, StandBookTag());
    TEST_ASSERT_OK(TagFSSettle(), "and a cycle settles it, every write it saw kept");

    b->holder  = tagfs_open(b->file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    b->waiting = tagfs_open(b->file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    TEST_ASSERT(b->holder && b->waiting, "a holder and a waiter open it");
    memset(&s_holder, 0, sizeof(s_holder));
    TEST_ASSERT(HelmAsk(b->holder->ofe, &s_holder), "the holder holds its Helm");
    b->holding = true;
    StandBookTurnSet(&s_waiting);
    bool mine = HelmAsk(b->waiting->ofe, &s_waiting.waiter);
    b->queued = !mine;
    if (mine)
        HelmLetGo(b->waiting->ofe, &s_waiting.waiter);
    TEST_ASSERT(!mine, "and the waiter is queued");

    TEST_ASSERT_EQ(tagfs_remove_tag(b->file_id, b->tag, INK_UNWARRANTED), 0,
                   "stand_book comes off — a change that writes nothing");
    b->gen       = InkedGen();
    b->unsettled = InkedGen() - SettledGen();
    TEST_ASSERT(b->unsettled > 0, "and no cycle settled it");
    TEST_ASSERT_EQ(SettleWaitNow(b->gen), ERR_WOULD_BLOCK, "an anchor for it waits");
    memset(&s_anchor, 0, sizeof(s_anchor));
    SettleWaitLink(&s_anchor.w, b->gen, StandBookAnswered);
    b->anchored = true;
out:
    return verdict;
}

/* The departure: the Helm's holder lets go with the medium away, and nothing
 * is handed on, answered, or settled while the book is held. */
static TestResult StandBookLeaves(StandBook* b)
{
    TestResult verdict = TEST_PASS;
    InkReceipt receipt;

    TagFSStandMediumLeaves();
    TEST_ASSERT(TagFSMediumLeft(), "the medium is said to have left");
    TEST_ASSERT_EQ(InkFields(b->file_id, NULL, b->tag, TAGFS_INVALID_TAG_ID, INK_UNWARRANTED, &receipt),
                   ERR_DEVICE_NOT_READY, "a new Ink is refused ERR_DEVICE_NOT_READY");
    HelmLetGo(b->holder->ofe, &s_holder);
    b->holding = false;
    HelmRunReady();
    SettleServiceIfPending(false);
    TEST_ASSERT(!s_waiting.handed && !s_waiting.gone,
                "the holder let go and the waiter is neither handed the Helm nor answered");
    TEST_ASSERT(!s_anchor.answered, "and the anchor is not answered: both wait with the book");

    StandBookTurnSet(&s_late);
    TEST_ASSERT(!HelmAsk(b->waiting->ofe, &s_late.waiter),
                "one who asks for the parked Helm now is not given it");
    TEST_ASSERT_EQ(s_late.gone, 1, "and is answered at once that the medium is away");
    TEST_ASSERT_EQ(s_late.lost, 0, "nothing of its own lost");
out:
    return verdict;
}

/* Whatever a test left: the latch let go of, the Helm and the anchor given
 * back, the file gone and settled. */
static void StandBookClose(StandBook* b)
{
    if (TagFSMediumLeft())
        (void)TagFSStandMediumReturns(NULL);
    if (b->holding)
        HelmLetGo(b->holder->ofe, &s_holder);
    if (b->queued && StandBookTurnSettled(&s_waiting)) {
        if (s_waiting.handed)
            HelmLetGo(b->waiting->ofe, &s_waiting.waiter);
    } else if (b->queued) {
        b->waiting = NULL;           /* still queued: its handle must outlive it */
    }
    if (b->anchored && !s_anchor.answered)
        (void)SettleWaitUnlink(&s_anchor.w);
    tagfs_close(b->holder);
    tagfs_close(b->waiting);
    if (b->file_id)
        (void)tagfs_delete_file(b->file_id, INK_UNWARRANTED);
    (void)TagFSSettle();
}

/*
 * RT-13. The medium leaves and comes back as it was left — its Ledger's seq
 * and seal the ones a cycle left, every write kept — and the book is taken
 * up: the waiter is handed the Helm, the next cycle settles the change the
 * book held, and the anchor hears OK. Nothing was lost.
 */
static TestResult TestStandMediumBookTakenUp(void)
{
    TestResult  verdict = TEST_PASS;
    StandBook   b;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    verdict = StandBookOpen(&b, "book_taken_up");
    if (verdict == TEST_PASS)
        verdict = StandBookLeaves(&b);
    if (verdict != TEST_PASS)
        goto out;

    TEST_ASSERT_EQ(TagFSStandMediumReturns(NULL), AWAY_AS_LEFT,
                   "the medium came back as this kernel left it");
    TEST_ASSERT(!TagFSMediumLeft(), "and the book is taken up");
    TEST_ASSERT(StandBookTurnSettled(&s_waiting) && s_waiting.handed,
                "the waiter is handed the parked Helm");
    TEST_ASSERT_EQ(s_waiting.gone, 0, "and was never answered that the medium left");
    HelmLetGo(b.waiting->ofe, &s_waiting.waiter);
    b.queued = false;

    TEST_ASSERT_OK(TagFSSettle(), "the next cycle, built from S0, settles");
    TEST_ASSERT(s_anchor.answered, "the anchor is answered");
    TEST_ASSERT_OK(s_anchor.rc, "with OK");
    TestEvidence("changes the book held across the departure", b.unsettled);
    TEST_ASSERT(SettledGen() >= b.gen, "the change the book held is settled — none lost");
    TEST_ASSERT(!tagfs_has_tag(b.file_id, b.tag), "and the file is as the change left it");
out:
    StandBookClose(&b);
    return verdict;
}

/*
 * The return is judged by what this kernel left: an intact Ledger as read is
 * as left, one a cycle behind the settled one is a medium that lied, and one
 * whose seal is not the one left — a medium another kernel settled on — is
 * another's. A return that is not as left gives the book up: every waiter,
 * the Helm's FIFO among them, is answered with the changes no cycle settled,
 * and the anchor hears ERR_DEVICE_NOT_READY. The stand's volume stays mounted
 * (away.h, TagFSStandMediumReturns).
 */
static TestResult TestStandMediumBookGivenUp(void)
{
    TestResult   verdict = TEST_PASS;
    StandBook    b;
    VolumeLedger found;
    VolumeLedger other;
    const char*  aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    verdict = StandBookOpen(&b, "book_given_up");
    if (verdict == TEST_PASS)
        verdict = StandBookLeaves(&b);
    if (verdict != TEST_PASS)
        goto out;

    TEST_ASSERT(LedgerNewestOn(tagfs_get_seat(), tagfs_get_volume_base(), &found),
                "the medium's newest Ledger is read");
    TEST_ASSERT(found.mark.settle == found.seq, "and carries a seal of its own");
    TEST_ASSERT_EQ(AwayJudge(&found), AWAY_AS_LEFT, "as read, it is as this kernel left it");
    other = found;
    other.seq--;
    TEST_ASSERT_EQ(AwayJudge(&other), AWAY_OLDER, "a cycle behind it, the medium lied");
    other = found;
    other.mark.closed_crc32 ^= 1u;
    TEST_ASSERT_EQ(AwayJudge(&other), AWAY_OTHER, "and with another seal, it is another's");

    uint64_t owed = HelmOwed();
    TEST_ASSERT_EQ(TagFSStandMediumReturns(&other), AWAY_OTHER, "that return gives the book up");
    TEST_ASSERT(StandBookTurnSettled(&s_waiting), "the waiter is answered");
    b.queued = false;
    TEST_ASSERT_EQ(s_waiting.gone, 1, "once, that the medium left");
    TEST_ASSERT(!s_waiting.handed, "and was never handed the Helm");
    TestEvidence("changes the answer said left with the medium", s_waiting.lost);
    TEST_ASSERT_EQ(s_waiting.lost, b.unsettled, "with the changes no cycle settled");
    TEST_ASSERT(s_anchor.answered, "the anchor is answered too");
    TEST_ASSERT_EQ(s_anchor.rc, ERR_DEVICE_NOT_READY, "with ERR_DEVICE_NOT_READY");
    TEST_ASSERT_EQ(HelmOwed(), owed - 1u, "and nothing is owed on the file any more");
out:
    StandBookClose(&b);
    return verdict;
}

/*
 * K3. A cycle with a waiter closes the tail onto a Spare and makes the reserve
 * whole again, and the new Spare's stamp is a write: the cycle's last flush
 * keeps it. So a volume nobody writes to after it — saved, taken out, put back
 * — comes back as this kernel left it, and its book is taken up. Red before:
 * the reserve was made again after the last flush, every such book was in
 * doubt, and the return gave it up.
 */
static TestResult TestStandMediumIdleTakenUpAfterSpare(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    uint32_t    file_id = 0;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0xB1u);
    TEST_ASSERT(StandMakeFile("idle_after_spare", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    uint64_t gen = InkedGen();
    TEST_ASSERT_OK(TagFSSettle(), "a cycle with a waiter settles it");
    TEST_ASSERT(PoolClosedSettledGen() >= gen,
                "in a closed block: the cycle closed the tail onto a Spare");
    TagFSLockTake(&fs->lock);
    uint32_t held = PoolSparesHeld(NULL, 0);
    TagFSLockDrop(&fs->lock);
    TEST_ASSERT_EQ(held, SPARE_RESERVE, "and made the reserve whole again");
    TEST_ASSERT(VolumeWritesKept(),
                "every write the cycle made, the new Spare's stamp among them, is kept");

    TagFSStandMediumLeaves();
    TEST_ASSERT(TagFSMediumLeft(), "the medium leaves with nothing owed");
    TEST_ASSERT_EQ(TagFSStandMediumReturns(NULL), AWAY_AS_LEFT,
                   "and comes back as this kernel left it");
    TEST_ASSERT(!TagFSMediumLeft(), "its book is taken up");
out:
    if (TagFSMediumLeft())
        (void)TagFSStandMediumReturns(NULL);
    if (file_id)
        (void)tagfs_delete_file(file_id, INK_UNWARRANTED);
    (void)TagFSSettle();
    return verdict;
}

// ----------------------------------------------------------------------------
// The bell, and the road a mount takes when the medium does what a hand does
// ----------------------------------------------------------------------------

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

/* seat:ready of one seat, as an ear on the tag hears it. */
typedef struct {
    uint8_t           seat;
    volatile uint32_t heard;
} StandBell;

static StandBell s_bell;

static void StandBellHeard(TouchTag tag, const void* payload, uint32_t plen,
                           uint32_t source_pid, void* ctx)
{
    (void)tag; (void)source_pid;
    StandBell*                b  = (StandBell*)ctx;
    const BoardroomSeatEvent* ev = (const BoardroomSeatEvent*)payload;
    if (plen >= sizeof(*ev) && ev->first == b->seat)
        __atomic_add_fetch(&b->heard, 1u, __ATOMIC_ACQ_REL);
}

/* The room's pass run here until the ear has heard `want`, bounded: a bell is
 * said by whichever core passes first. */
static bool StandBellHeardAt(uint32_t want)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_BOOK_BOUND_MS);
    while (__atomic_load_n(&s_bell.heard, __ATOMIC_ACQUIRE) < want) {
        if (rdtsc() >= bound)
            return false;
        BoardroomAttendIfPending();
        cpu_pause();
    }
    return true;
}

/*
 * The bell (B2). A seat that refused a command says seat:ready on the first
 * command it answers after — once, whatever kind of medium sits in it, the
 * legacy channel's among them. Red before: only an AHCI port's recovery and a
 * USB disk's settled cache flush ever said it, and a mount or a cycle refused
 * anywhere else waited for an event that could not come.
 */
static TestResult TestStandMediumBellRingsOnce(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    TouchWatch* ear     = NULL;
    bool        armed   = false;
    TouchTag    full    = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(BoardroomSeatAnnouncesReady(seat), "the volume's seat says seat:ready, whatever it is");
    memset(&s_bell, 0, sizeof(s_bell));
    s_bell.seat = seat;
    TouchLogbookResolve("seat:ready", &full, &bare);
    ear = TouchWatchSet(full, StandBellHeard, &s_bell);
    TEST_ASSERT(ear != NULL, "an ear on seat:ready");

    uint32_t gen = BoardroomSeatReadyGen(seat);
    BlockFailArm(seat, BLOCK_FAIL_READ, 0, TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    TEST_ASSERT(BoardroomRead(seat, 0, TAGFS_BLOCK_SECTORS, g_test_output) != 0,
                "a read the medium refuses");
    TEST_ASSERT(BlockFailFired(), "refused by BLOCKFAIL in the medium's place");
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT_EQ(BoardroomSeatReadyGen(seat), gen, "a refusal rings nothing");

    TEST_ASSERT_EQ(BoardroomRead(seat, 0, TAGFS_BLOCK_SECTORS, g_test_output), 0,
                   "the next read is answered");
    TEST_ASSERT_EQ(BoardroomSeatReadyGen(seat), gen + 1u,
                   "and the seat counts seat:ready for the refusal before it");
    TEST_ASSERT(StandBellHeardAt(1), "and the room says seat:ready of it");

    TEST_ASSERT_EQ(BoardroomRead(seat, 0, TAGFS_BLOCK_SECTORS, g_test_output), 0,
                   "a read after that is answered too");
    BoardroomAttendIfPending();
    TEST_ASSERT_EQ(BoardroomSeatReadyGen(seat), gen + 1u,
                   "and rings nothing: the refusal was answered once");
    TestEvidence("seat:ready said of the seat, of 1", s_bell.heard);
    TEST_ASSERT_EQ(s_bell.heard, 1, "seat:ready was said exactly once");
out:
    if (armed)
        BlockFailDisarm();
    if (ear)
        TouchWatchClear(ear);
    return verdict;
}

/* The room's and the volume's service passes run here until the mount road
 * has nothing left to do, bounded — whichever core passes its own may do the
 * work first. */
static bool StandMountSettles(void)
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
static bool StandSettledBare(void)
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
static void StandMountClose(uint8_t seat)
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
static TestResult TestStandMediumMountLosesItsSeat(void)
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

/*
 * R4 (Z11-5, Z15-4). A read of the mount is refused, and the seat says
 * seat:ready in the same breath — before the mount has heard it was refused,
 * as a driver's recovery says it. That seat:ready is the one the mount is owed:
 * it is asked again at once, and the volume mounts with no other event. Red
 * before: the event came while nothing was owed yet, was dropped, and the
 * mount waited for a second one that never came.
 */
static TestResult TestStandMediumMountHearsTheReadyItWasOwed(void)
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

    BlockFailArm(seat, BLOCK_FAIL_READ, first, span, 1);
    TagFSStandMountTurn(STAND_MOUNT_SEAT_RINGS, 0);
    TEST_ASSERT_EQ(BoardroomInit(), 1, "the medium is seated again");
    TEST_ASSERT(StandMountSettles(), "and its arrival is attended, and what it owes");
    TEST_ASSERT(BlockFailFired(), "the mount's first read of the volume's data was refused");
    TEST_ASSERT(TagFSStandMountTurnFired(),
                "and the seat said seat:ready before the mount had heard of it");
    TEST_ASSERT(fs->initialized, "the mount was asked again on it, and the volume is MOUNTED");
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
static TestResult TestStandMediumMountRefusedReadIsNoVerdict(void)
{
    TestResult   verdict = TEST_PASS;
    TagFSState*  fs      = tagfs_get_state();
    uint8_t      seat    = tagfs_get_seat();
    BoardKind    kind    = BoardroomSeatKind(seat);
    uint8_t      index   = BoardroomSeatIndex(seat);
    uint32_t     epoch   = tagfs_mount_epoch();
    uint32_t     file_id = 0, block = 0;
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
static TestResult TestStandMediumGivenUpIdsNotGivenAgain(void)
{
    TestResult         verdict = TEST_PASS;
    TagFSState*        fs      = tagfs_get_state();
    uint8_t            seat    = tagfs_get_seat();
    BoardKind          kind    = BoardroomSeatKind(seat);
    uint8_t            index   = BoardroomSeatIndex(seat);
    uint32_t           file_id = 0;
    uint16_t           tag     = TAGFS_INVALID_TAG_ID;
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
    TEST_ASSERT_OK(tagfs_add_tag_string(file_id, &spelling), "it wears a tag new to the volume");
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
    uint16_t next_tag = fs->registry->next_id;
    TagFSLockDrop(&fs->registry->lock);
    TEST_ASSERT(next_tag > tag, "and its next tag id past the tag it gave out");
out:
    if (let_go)
        StandMountClose(seat);
    return verdict;
}

static const TestCase g_stand_medium_tests[] = {
    { .name  = "stand_medium_no_medium_flush_refused",
      .func  = TestStandMediumNoMediumFlushRefused, .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a USB disk with no medium answered its cache flush with success" },
    { .name  = "stand_medium_sync_cache_remembered",
      .func  = TestStandMediumSyncCacheRemembered, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_book_taken_up", .func = TestStandMediumBookTakenUp,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_book_given_up", .func = TestStandMediumBookGivenUp,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_idle_taken_up_after_spare",
      .func  = TestStandMediumIdleTakenUpAfterSpare, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_bell_rings_once",
      .func  = TestStandMediumBellRingsOnce, .reach = TEST_READS_ONLY },
    /* Last, every one: each takes the volume down and mounts it again. */
    { .name  = "stand_medium_mount_loses_its_seat",
      .func  = TestStandMediumMountLosesItsSeat, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_mount_hears_the_ready_it_was_owed",
      .func  = TestStandMediumMountHearsTheReadyItWasOwed, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_mount_refused_read_is_no_verdict",
      .func  = TestStandMediumMountRefusedReadIsNoVerdict, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_given_up_ids_not_given_again",
      .func  = TestStandMediumGivenUpIdsNotGivenAgain, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_medium_suite = {
    .tests = g_stand_medium_tests, .count = ARRAY_SIZE(g_stand_medium_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
