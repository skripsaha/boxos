#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../helm/helm.h"
#include "../ink/ink.h"
#include "../settle/settle.h"
#include "../scribe/scribe.h"
#include "../ledger/ledger.h"
#include "../mount/away.h"
#include "../metadata_pool/meta_pool.h"
#include "block_fail.h"   /* BLOCKFAIL — a flush answered with the sense a USB disk gives */
#include "boardroom.h"
#include "xhci_msd.h"     /* the USB disk's memory of its SYNCHRONIZE CACHE answer */
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

    TEST_ASSERT_EQ(tagfs_remove_tag(b->file_id, b->tag), 0,
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
    TEST_ASSERT_EQ(InkFields(b->file_id, NULL, b->tag, TAGFS_INVALID_TAG_ID, &receipt),
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
        (void)tagfs_delete_file(b->file_id);
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
        (void)tagfs_delete_file(file_id);
    (void)TagFSSettle();
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
};

const TestSuite g_stand_medium_suite = {
    .tests = g_stand_medium_tests, .count = ARRAY_SIZE(g_stand_medium_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
