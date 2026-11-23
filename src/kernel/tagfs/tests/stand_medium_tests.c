#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../helm/helm.h"
#include "../ink/ink.h"
#include "../settle/settle.h"
#include "../scribe/scribe.h"
#include "../mount/away.h"
#include "../tag_registry/tag_registry.h"
#include "block_fail.h"   /* BLOCKFAIL — a flush answered with the sense a USB disk gives */
#include "boardroom.h"
#include "xhci_msd.h"     /* the USB disk's memory of its SYNCHRONIZE CACHE answer */
#include "touch.h"        /* an ear on seat:ready */
#include "logbook.h"
#include "amp.h"
#include "atomics.h"
#include "baton.h"
#include "cpu_calibrate.h"
#include "tagfs_volume.h"

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
static const char* StandFlushAside(TagFSVolume* vol, uint8_t seat)
{
    if (BoardroomSeatKind(seat) != BOARD_USB)
        return "the volume's medium is not a USB disk, and SYNCHRONIZE CACHE is a USB disk's "
               "command";
    return StandStepsAsideFor(vol);
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
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat(vol);
    const char* aside   = StandFlushAside(vol, seat);

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_OK(tagfs_flush_cache(vol), "a flush the device carries out is kept");
    uint64_t need = VolumeFlushNeed(vol);
    BlockFailArmFlushSense(seat, STAND_SENSE_NOT_READY, STAND_ASC_NO_MEDIUM, 0, 1);
    error_t rc    = tagfs_flush_cache(vol);
    bool    fired = BlockFailFired();
    BlockFailDisarm();
    TEST_ASSERT(fired, "the flush was answered MEDIUM NOT PRESENT");
    TestEvidence("the flush's answer to MEDIUM NOT PRESENT:", (uint64_t)rc);
    TEST_ASSERT(rc != OK, "and is refused, never success");
    TEST_ASSERT(!VolumeFlushKept(vol, need), "no write before it is called kept by it");
    TEST_ASSERT_OK(tagfs_flush_cache(vol), "the next flush, answered by the device, is kept");
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
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat(vol);
    uint8_t     unit    = BoardroomSeatIndex(seat);
    bool        known   = false;
    const char* aside   = StandFlushAside(vol, seat);

    if (aside)
        return test_steps_aside(aside);

    BlockFailArmFlushSense(seat, STAND_SENSE_ILLEGAL_REQUEST, STAND_ASC_INVALID_OPCODE, 0, 1);
    known = true;
    TEST_ASSERT_OK(tagfs_flush_cache(vol), "a device with no SYNCHRONIZE CACHE is taken at its word");
    TEST_ASSERT(BlockFailFired(), "its answer was ILLEGAL REQUEST");

    BlockFailArmFlushSense(seat, STAND_SENSE_NOT_READY, STAND_ASC_NO_MEDIUM, 0, 1);
    TEST_ASSERT_OK(tagfs_flush_cache(vol),
                   "the next flush is not put to the device: its answer stands for the seating");
    TEST_ASSERT(BlockFailFired(), "though a NO MEDIUM was armed for it, and struck");
    BlockFailDisarm();

    xhci_msd_forget_flush_answer(unit);
    known = false;
    BlockFailArmFlushSense(seat, STAND_SENSE_NOT_READY, STAND_ASC_NO_MEDIUM, 0, 1);
    TEST_ASSERT(tagfs_flush_cache(vol) != OK,
                "forgotten, the device is asked again, and its NO MEDIUM refuses");
    BlockFailDisarm();
    TEST_ASSERT_OK(tagfs_flush_cache(vol), "and a flush it carries out is kept");
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
static bool StandBookTurnSettled(TagFSVolume* vol, const StandBookTurn* t)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_BOOK_BOUND_MS);
    uint8_t  me    = amp_get_core_index();
    for (;;) {
        HelmRunReady(vol);
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
    uint64_t         file_id;
    uint32_t         tag;
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

static TestResult StandBookOpen(TagFSVolume* vol, StandBook* b, const char* name)
{
    TestResult verdict = TEST_PASS;
    memset(b, 0, sizeof(*b));
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0xB0u);
    TEST_ASSERT(StandMakeFile(vol, name, g_test_buffer, TAGFS_BLOCK_SIZE, &b->file_id),
                "a file is made and written");
    TEST_ASSERT_OK(tagfs_add_tag_string(vol, TagOwnerPerson(), b->file_id, StandBookTag()),
                   "it wears stand_book");
    b->tag = TagRegistryLookup(vol->state.registry, TagOwnerPerson(), StandBookTag());
    TEST_ASSERT_OK(TagFSSettle(vol), "and a cycle settles it, every write it saw kept");

    b->holder  = tagfs_open(vol, b->file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    b->waiting = tagfs_open(vol, b->file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    TEST_ASSERT(b->holder && b->waiting, "a holder and a waiter open it");
    memset(&s_holder, 0, sizeof(s_holder));
    TEST_ASSERT(HelmAsk(vol, b->holder->ofe, &s_holder), "the holder holds its Helm");
    b->holding = true;
    StandBookTurnSet(&s_waiting);
    bool mine = HelmAsk(vol, b->waiting->ofe, &s_waiting.waiter);
    b->queued = !mine;
    if (mine)
        HelmLetGo(vol, b->waiting->ofe, &s_waiting.waiter);
    TEST_ASSERT(!mine, "and the waiter is queued");

    TEST_ASSERT_EQ(tagfs_remove_tag(vol, b->file_id, b->tag), 0,
                   "stand_book comes off — a change that writes nothing");
    b->gen       = InkedGen(vol);
    b->unsettled = InkedGen(vol) - SettledGen(vol);
    TEST_ASSERT(b->unsettled > 0, "and no cycle settled it");
    TEST_ASSERT_EQ(SettleWaitNow(vol, b->gen), ERR_WOULD_BLOCK, "an anchor for it waits");
    memset(&s_anchor, 0, sizeof(s_anchor));
    SettleWaitLink(vol, &s_anchor.w, b->gen, StandBookAnswered);
    b->anchored = true;
out:
    return verdict;
}

/* The departure: the Helm's holder lets go with the medium away, and nothing
 * is handed on, answered, or settled while the book is held. */
static TestResult StandBookLeaves(TagFSVolume* vol, StandBook* b)
{
    TestResult verdict = TEST_PASS;
    InkReceipt receipt;

    TagFSStandMediumLeaves(vol);
    TEST_ASSERT(TagFSMediumLeft(vol), "the medium is said to have left");
    TEST_ASSERT_EQ(InkFields(vol, b->file_id, NULL, b->tag, TAGFS_INVALID_TAG_ID, &receipt),
                   ERR_DEVICE_NOT_READY, "a new Ink is refused ERR_DEVICE_NOT_READY");
    HelmLetGo(vol, b->holder->ofe, &s_holder);
    b->holding = false;
    HelmRunReady(vol);
    SettleServiceIfPending(vol);
    TEST_ASSERT(!s_waiting.handed && !s_waiting.gone,
                "the holder let go and the waiter is neither handed the Helm nor answered");
    TEST_ASSERT(!s_anchor.answered, "and the anchor is not answered: both wait with the book");

    StandBookTurnSet(&s_late);
    TEST_ASSERT(!HelmAsk(vol, b->waiting->ofe, &s_late.waiter),
                "one who asks for the parked Helm now is not given it");
    TEST_ASSERT_EQ(s_late.gone, 1, "and is answered at once that the medium is away");
    TEST_ASSERT_EQ(s_late.lost, 0, "nothing of its own lost");
out:
    return verdict;
}

/* Whatever a test left: the latch let go of, the Helm and the anchor given
 * back, the file gone and settled. */
static void StandBookClose(TagFSVolume* vol, StandBook* b)
{
    if (TagFSMediumLeft(vol))
        (void)TagFSStandMediumReturns(vol);
    if (b->holding)
        HelmLetGo(vol, b->holder->ofe, &s_holder);
    if (b->queued && StandBookTurnSettled(vol, &s_waiting)) {
        if (s_waiting.handed)
            HelmLetGo(vol, b->waiting->ofe, &s_waiting.waiter);
    } else if (b->queued) {
        b->waiting = NULL;           /* still queued: its handle must outlive it */
    }
    if (b->anchored && !s_anchor.answered)
        (void)SettleWaitUnlink(vol, &s_anchor.w);
    tagfs_close(vol, b->holder);
    tagfs_close(vol, b->waiting);
    if (b->file_id)
        (void)tagfs_delete_file(vol, b->file_id, INK_UNWARRANTED);
    (void)TagFSSettle(vol);
}

/*
 * RT-13. The medium leaves and comes back as it was left — its Ledger's seq
 * and seal the ones a cycle left, every write kept — and the book is taken
 * up: the waiter is handed the Helm, the next cycle settles the change the
 * book held, and the anchor hears OK. Nothing was lost.
 */
static TestResult TestStandMediumBookTakenUp(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    StandBook   b;
    const char* aside   = StandStepsAsideFor(vol);

    if (aside)
        return test_steps_aside(aside);

    verdict = StandBookOpen(vol, &b, "book_taken_up");
    if (verdict == TEST_PASS)
        verdict = StandBookLeaves(vol, &b);
    if (verdict != TEST_PASS)
        goto out;

    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT,
                   "the medium came back as this kernel left it");
    TEST_ASSERT(!TagFSMediumLeft(vol), "and the book is taken up");
    TEST_ASSERT(StandBookTurnSettled(vol, &s_waiting) && s_waiting.handed,
                "the waiter is handed the parked Helm");
    TEST_ASSERT_EQ(s_waiting.gone, 0, "and was never answered that the medium left");
    HelmLetGo(vol, b.waiting->ofe, &s_waiting.waiter);
    b.queued = false;

    TEST_ASSERT_OK(TagFSSettle(vol), "the next cycle, built from S0, settles");
    TEST_ASSERT(s_anchor.answered, "the anchor is answered");
    TEST_ASSERT_OK(s_anchor.rc, "with OK");
    TestEvidence("changes the book held across the departure", b.unsettled);
    TEST_ASSERT(SettledGen(vol) >= b.gen, "the change the book held is settled — none lost");
    TEST_ASSERT(!tagfs_has_tag(vol, b.file_id, b.tag), "and the file is as the change left it");
out:
    StandBookClose(vol, &b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The bell — and the road a mount takes when the medium does what a hand does,
// which stand_mount_tests.c holds and this suite runs last
// ----------------------------------------------------------------------------

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
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat(vol);
    TouchWatch* ear     = NULL;
    bool        armed   = false;
    TouchTag    full    = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    const char* aside   = StandStepsAsideFor(vol);

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

/*
 * R9 (П0-K1, X3-5). The book is open. A writer asking for a file's Helm, free
 * as it is, is answered at once that the medium is away, and a read of the
 * volume is refused as the departure it is, before any of the work is begun.
 * And the room is searched for the volume's return while the one medium
 * carrying its identity was seated before the book opened — the volume's own
 * here, standing for a copy of it in another seat that never left. That is no
 * return, and it is not judged: the book stays open, and nothing is owed.
 * Red before: a free Helm was handed out and the work went through the
 * allocator and a cycle the medium refused, answered "no room" or "write
 * failed"; and the first seat carrying the identity was judged, whatever sat
 * in it all along — a replugged stick's book judged against the internal disk
 * it was copied from, its Ledger found older, and the medium accused of lying.
 */
static TestResult TestStandMediumNeverLeftIsNoReturn(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult       verdict = TEST_PASS;
    TagFSState*      fs      = &vol->state;
    uint32_t         epoch   = tagfs_mount_epoch(vol);
    uint64_t         file_id = 0;
    TagFSFileHandle* handle  = NULL;
    bool             mine    = false;
    const char*      aside   = StandStepsAsideFor(vol);

    if (aside)
        return test_steps_aside(aside);

    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0xB4u);
    TEST_ASSERT(StandMakeFile(vol, "never_left", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    TEST_ASSERT_OK(TagFSSettle(vol), "every change is on the medium");
    handle = tagfs_open(vol, file_id, TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle != NULL, "and the file is opened to be written");

    TagFSStandMediumLeaves(vol);
    TEST_ASSERT(TagFSMediumLeft(vol), "the book opens, the medium carrying the volume still seated");
    TEST_ASSERT_EQ(tagfs_volume_read(vol, 0, 1, g_test_output), ERR_DEVICE_NOT_READY,
                   "a read of the volume is refused as the medium's departure, no medium's refusal");
    StandBookTurnSet(&s_late);
    mine = HelmAsk(vol, handle->ofe, &s_late.waiter);
    TEST_ASSERT(!mine, "the file's Helm, free, is not handed to a writer while the medium is away");
    TEST_ASSERT_EQ(s_late.gone, 1, "the writer is answered at once that the medium is away");

    TagFSAttendArrival(vol);
    TEST_ASSERT(StandMountSettles(vol), "the room is searched for its return");
    TEST_ASSERT(TagFSMediumLeft(vol),
                "and no medium seated before the book opened is judged as it: the book stays open");
    TEST_ASSERT(!TagFSStandMountOwed(vol), "nothing is owed: no seat refused");
    TEST_ASSERT(fs->initialized && tagfs_mount_epoch(vol) == epoch,
                "and the volume is the one up all along");
out:
    if (TagFSMediumLeft(vol))
        (void)TagFSStandMediumReturns(vol);
    if (mine)
        HelmLetGo(vol, handle->ofe, &s_late.waiter);
    tagfs_close(vol, handle);
    if (file_id)
        (void)tagfs_delete_file(vol, file_id, INK_UNWARRANTED);
    (void)TagFSSettle(vol);
    return verdict;
}

/*
 * R9 (X3-3). The medium refused a write as protected — the volume read-only —
 * then leaves and comes back as it was left, its switch turned off meanwhile.
 * The book is taken up and the volume takes changes again: whether this
 * seating refuses writes is its next refused write's to say. Red before: the
 * latch was let go of only by a mount, so the book taken up held its changes,
 * took none, and no cycle ever settled them.
 */
static TestResult TestStandMediumTakenUpTakesWrites(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict    = TEST_PASS;
    uint8_t     seat       = tagfs_get_seat(vol);
    bool        protected_ = false;
    const char* aside      = StandStepsAsideFor(vol);

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_OK(TagFSSettle(vol), "every change is on the medium");
    BlockFailProtect(seat, true);
    protected_ = true;
    TagFSWriteRefused(vol);
    TEST_ASSERT(TagFSReadOnly(vol), "the medium refuses writes as protected, and the volume takes none");
    BlockFailProtect(seat, false);
    protected_ = false;

    TagFSStandMediumLeaves(vol);
    TEST_ASSERT_EQ(TagFSStandMediumReturns(vol), AWAY_AS_LEFT,
                   "it leaves, and comes back as this kernel left it");
    TEST_ASSERT(!TagFSReadOnly(vol), "and its book taken up, the volume takes changes again");
    TEST_ASSERT_OK(TagFSSettle(vol), "and a cycle settles on it");
out:
    if (protected_)
        BlockFailProtect(seat, false);
    if (TagFSMediumLeft(vol))
        (void)TagFSStandMediumReturns(vol);
    InkReadOnlyLift(vol);
    return verdict;
}

/*
 * R32 (Z08-1). A mount numbers the registry past every tag id the pool's
 * records wear — ids a rolled-back registry no longer names — and nothing
 * under the numbers is sized by them: an id no tag has spells nothing at any
 * height, and the next tag takes the number past them. Red before: the
 * numbers moved past the end of a table indexed by id, every reader bounded
 * by them read past it, and the take-down freed what nobody allocated.
 */
static TestResult TestStandMediumNumbersPastTheTable(void)
{
    TestResult   verdict = TEST_PASS;
    TagRegistry* scratch = NULL;
    bool         built   = false;
    TagSpelling  spelled;

    scratch = kmalloc(sizeof(TagRegistry));
    TEST_ASSERT(scratch != NULL, "memory for a registry of the test's own");
    TEST_ASSERT_EQ(tag_registry_init(scratch), 0, "a registry of the test's own is built");
    built = true;

    uint32_t next = 100000u;
    TagRegistryNumbersFrom(scratch, next);
    TEST_ASSERT_EQ(scratch->next_id, next, "its numbers move to the first id past the ones the records wear");
    TEST_ASSERT(!TagRegistrySpelling(scratch, next - 1u, &spelled, NULL, NULL),
                "an id under the numbers that no tag has spells nothing");
    TEST_ASSERT(!TagRegistrySpelling(scratch, TAGFS_MAX_TAG_ID, &spelled, NULL, NULL),
                "nor does the highest id there is");

    TagSpelling fresh = TagSpellingOfKey("stand.numbers.past");
    uint32_t    id    = TAGFS_INVALID_TAG_ID;
    TEST_ASSERT_OK(TagRegistryIntern(scratch, TagOwnerPerson(), &fresh, &id), "a new tag is interned past them");
    TestEvidence("the id it is given", id);
    TEST_ASSERT_EQ(id, next, "with the first number past them — wider than 16 bits");
    TEST_ASSERT(TagRegistrySpelling(scratch, id, &spelled, NULL, NULL) &&
                TagSpanIs("stand.numbers.past", spelled.key, spelled.key_len),
                "and spells itself by that number");
out:
    if (built)
        tag_registry_destroy(scratch);
    kfree(scratch);
    return verdict;
}

static StandBookWait s_let_go_wait;

/*
 * R9 (F02-3). A waiter for a cycle is linked to a volume that is then let go
 * of — as one linked after its book was given up is — and is answered when
 * the volume goes, ERR_DEVICE_NOT_READY, not when a mount after it stands.
 * Red before: waiters were answered by the book's give-up, which one linked
 * after it never heard, and by the next mount that succeeded — a mount that
 * failed left the program parked on its Chit for good.
 */
static TestResult TestStandMediumLetGoAnswersItsWaiters(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat(vol);
    bool        linked  = false;
    bool        let_go  = false;
    const char* aside   = StandStepsAsideFor(vol);

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandSettledBare(vol), "every change is on the medium, and nothing rests, before it goes");
    memset(&s_let_go_wait, 0, sizeof(s_let_go_wait));
    SettleWaitLink(vol, &s_let_go_wait.w, InkedGen(vol) + 1u, StandBookAnswered);
    linked = true;
    TEST_ASSERT(!s_let_go_wait.answered, "a waiter for a cycle of the volume waits");
    let_go = true;
    TEST_ASSERT(TagFSStandLetGo(vol), "the volume is let go of, writing nothing");
    TEST_ASSERT(s_let_go_wait.answered, "and the waiter is answered as it goes, before any mount");
    TEST_ASSERT_EQ(s_let_go_wait.rc, ERR_DEVICE_NOT_READY, "with ERR_DEVICE_NOT_READY");
out:
    if (linked && !s_let_go_wait.answered)
        (void)SettleWaitUnlink(vol, &s_let_go_wait.w);
    if (let_go)
        StandMountClose(vol, seat);
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
    { .name  = "stand_medium_bell_rings_once",
      .func  = TestStandMediumBellRingsOnce, .reach = TEST_READS_ONLY },
    { .name  = "stand_medium_never_left_is_no_return",
      .func  = TestStandMediumNeverLeftIsNoReturn, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_numbers_past_the_table",
      .func  = TestStandMediumNumbersPastTheTable, .reach = TEST_READS_ONLY },
    { .name  = "stand_medium_taken_up_takes_writes",
      .func  = TestStandMediumTakenUpTakesWrites, .reach = TEST_WRITES_TO_VOLUME },
    /* Last, every one: each takes the volume down and mounts it again, or
     * takes its medium out of the room and seats it again. */
    { .name  = "stand_medium_mount_loses_its_seat",
      .func  = TestStandMediumMountLosesItsSeat, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_mount_rings_in_its_own_breath",
      .func  = TestStandMediumMountRingsInItsOwnBreath, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_mount_bad_sector_waits",
      .func  = TestStandMediumMountBadSectorWaits, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_mount_resets_the_seat",
      .func  = TestStandMediumMountResetsTheSeat, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_mount_waits_for_memory",
      .func  = TestStandMediumMountWaitsForMemory, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_medium_let_go_answers_its_waiters",
      .func  = TestStandMediumLetGoAnswersItsWaiters, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_medium_suite = {
    .tests = g_stand_medium_tests, .count = ARRAY_SIZE(g_stand_medium_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
