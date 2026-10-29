#include "stand_binder.h"
#include "stand_fresh.h"
#include "../settle/binder.h"
#include "../settle/escheat.h"
#include "../settle/settle.h"
#include "../file_map/file_map.h"
#include "../helm/helm.h"
#include "block_fail.h"
#include "baton.h"
#include "amp.h"
#include "atomics.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_escheat suite: the act that lets go what a book can never give
// back (spec_v2 §16.5 п.20)
//
// Escheat is asked only with the warrant a cabin wearing system or god
// carries; with nothing to let go it says so. A Slip's tail whose block cannot
// be read holds every Tally back for that file alone, said, until the act:
// then the tail is lost — the file cut to the end of its last whole block —
// and the Tally goes on. The ranges of FILES the act lets go are the landings'
// (logcheck book_escheat B4, B6c). Every test puts the Spine and the ribbon
// back as it found them.
// ============================================================================

#define STAND_ESCHEAT_ROOM      (4u * TAGFS_BLOCK_SIZE)
#define STAND_ESCHEAT_BOUND_MS  10000u   /* a hang's guard: the act's walk is one turn of the service */

static StandBinder s_escheat_binder;

/* The size the file's record holds now. */
static uint64_t StandEscheatSize(uint64_t fid)
{
    FileRecord* rec  = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    uint64_t    size = rec ? rec->size : UINT64_MAX;
    FileRecordRelease(rec);
    return size;
}

static TestResult TestStandEscheatWarrant(void)
{
    TestResult  verdict = TEST_PASS;
    EscheatSaid said;

    TEST_ASSERT_EQ(TagFSEscheat(INK_UNWARRANTED, &said), ERR_PERMISSION_DENIED,
                   "a caller wearing neither system nor god is refused");
    TEST_ASSERT_EQ(TagFSEscheat(INK_WARRANTED, &said), ERR_OBJECT_NOT_FOUND,
                   "with no rotten range and no tail held back there is nothing to let go");
out:
    return verdict;
}

/* One round of the service points this core runs while it waits: the Helm's
 * service — listed by an ask, a let-go or a cycle's DONE — among them. */
static void StandEscheatServe(uint8_t me)
{
    HelmRunReady();
    BatonPump(me);
    cpu_pause();
}

/* The walk the ask began ended having tried every tail (EscheatTailsActing
 * 0): the service's or a holder's. False when none did within the guard. */
static bool StandEscheatWalked(void)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_ESCHEAT_BOUND_MS);
    uint8_t  me    = amp_get_core_index();
    while (EscheatTailsActing() != 0) {
        if (rdtsc() >= bound)
            return false;
        StandEscheatServe(me);
    }
    return true;
}

/* A Tally asked for walks every file, and its walk tries the tail and says it
 * refused (EngrossRefusedNow) — the answer Escheat is asked on. The service
 * the cycle listed may hold the file's Helm as the walk comes to it: that walk
 * passes the file over (ERR_BUSY), the service's turn is run, and the Tally
 * is asked for again. The refusal, or OK, or ERR_BUSY past the guard. */
static error_t StandEscheatTailRefused(void)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_ESCHEAT_BOUND_MS);
    uint8_t  me    = amp_get_core_index();
    error_t  rc;
    while ((rc = BinderRunWhole()) == ERR_BUSY && rdtsc() < bound)
        StandEscheatServe(me);
    return rc;
}

#define STAND_ESCHEAT_ASKED   0u
#define STAND_ESCHEAT_HANDED  1u
#define STAND_ESCHEAT_GONE    2u

static HelmWaiter        s_escheat_holder;
static volatile uint32_t s_escheat_hand;

static void StandEscheatHand(HelmWaiter* w)
{
    (void)w;
    __atomic_store_n(&s_escheat_hand, STAND_ESCHEAT_HANDED, __ATOMIC_RELEASE);
}

static void StandEscheatGone(HelmWaiter* w, uint64_t lost)
{
    (void)w;
    (void)lost;
    __atomic_store_n(&s_escheat_hand, STAND_ESCHEAT_GONE, __ATOMIC_RELEASE);
}

/* The file's Helm held by the stand, as a writer holds it: asked in its FIFO,
 * the service's hold let go first when it has it. STAND_ESCHEAT_HANDED, GONE
 * when the medium left, or ASKED — the waiter still in the FIFO, the handle
 * the caller's to leave open — past the guard. */
static uint32_t StandEscheatHold(TagFSFileHandle* h)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_ESCHEAT_BOUND_MS);
    uint8_t  me    = amp_get_core_index();
    s_escheat_holder = (HelmWaiter){ .hand = StandEscheatHand, .gone = StandEscheatGone };
    __atomic_store_n(&s_escheat_hand, STAND_ESCHEAT_ASKED, __ATOMIC_RELEASE);
    if (HelmAsk(h->ofe, &s_escheat_holder))
        return STAND_ESCHEAT_HANDED;
    while (__atomic_load_n(&s_escheat_hand, __ATOMIC_ACQUIRE) == STAND_ESCHEAT_ASKED && rdtsc() < bound)
        StandEscheatServe(me);
    return __atomic_load_n(&s_escheat_hand, __ATOMIC_ACQUIRE);
}

/* A file of a block and 150 bytes whose tail rides a Slip over a last block
 * the medium cannot read, mounted anew: the Helm's service says the tail it
 * cannot lay, and a Tally asked for waits for it. *fid the file. */
static TestResult StandEscheatTailWaits(StandBinder* b, StandFreshModel* m, uint64_t* fid)
{
    TestResult  verdict = TEST_PASS;
    FileSpot    spot = { 0 };
    FileRecord* rec = NULL;
    bool        found = false;
    error_t     rc = OK;

    TEST_ASSERT(StandFreshModelMake(m, STAND_ESCHEAT_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("escheat-tail.bin", NULL, 0, fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(m, *fid, 0, TAGFS_BLOCK_SIZE + 100u, 0x6e00u), TAGFS_BLOCK_SIZE + 100,
                   "a block and 100 bytes are written to it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_EQ(StandFreshWrite(m, *fid, m->size, 50, 0x6e01u), 50, "50 bytes ride a Slip at its end");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again: nothing of the file held in memory");
    rec   = FileMapRecordHold(tagfs_get_state()->files, *fid, NULL);
    found = rec && FileRecordFind(rec, 1, &spot) && spot.kind == STRIP_DATA;
    FileRecordRelease(rec);
    TEST_ASSERT(found, "the file's last block is DATA");
    BlockFailArmBadSectors(b->s.seat, tagfs_block_to_sector(spot.block), TAGFS_BLOCK_SECTORS);
    BinderDemanded();
    TEST_ASSERT_OK(TagFSSettle(), "its block cannot be read, and a Tally is demanded");
    rc = StandEscheatTailRefused();
    TEST_ASSERT(rc != OK && rc != ERR_BUSY, "a Tally asked for waits for that tail, refused for the file's own sake");
out:
    return verdict;
}

static TestResult TestStandEscheatTail(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b = &s_escheat_binder;
    StandFreshModel m;
    EscheatSaid     said;
    uint64_t        fid = 0;
    const char*     aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    memset(&m, 0, sizeof(m));
    verdict = StandEscheatTailWaits(b, &m, &fid);
    if (verdict != TEST_PASS)
        goto out;
    TEST_ASSERT_EQ(TagFSEscheat(INK_UNWARRANTED, &said), ERR_PERMISSION_DENIED,
                   "Escheat asked without the warrant is refused, and the tail is held still");
    TEST_ASSERT(StandEscheatSize(fid) == TAGFS_BLOCK_SIZE + 150u, "nothing of the file changed");
    TEST_ASSERT_OK(TagFSEscheat(INK_WARRANTED, &said), "Escheat asked with the warrant");
    TEST_ASSERT(said.tails >= 1, "it names the tail held back");
    /* The ask lists the service, whose walk may hold the file's Helm when a
     * Tally asked for now walks the files too: that walk passes the file over
     * (BinderRunWhole's ERR_BUSY), and the act stands until one tried it. */
    TEST_ASSERT(StandEscheatWalked(), "a walk begun after the ask tries the tail under the act");
    TEST_ASSERT_OK(BinderRunWhole(), "the Tally goes on: the tail is lost under the act");
    TEST_ASSERT_EQ(StandEscheatSize(fid), TAGFS_BLOCK_SIZE, "the file ends at its last whole block");
    BlockFailDisarm();
    StandBlockFailRaced("stand_escheat");
    m.size = TAGFS_BLOCK_SIZE;
    TEST_ASSERT(StandFreshHolds(&m, fid), "and reads the block before the tail");
    TEST_ASSERT(StandStitchRemount() && StandEscheatSize(fid) == TAGFS_BLOCK_SIZE && StandFreshHolds(&m, fid),
                "mounted again, the same");
out:
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/*
 * An ask of Escheat is spent only by a walk that tried every tail. The file's
 * Helm is held, as a writer holds it, when the act is asked: the walk of a
 * Tally asked for now passes the file over, and so does the service's — they
 * did not try its tail, and the act stands. Nor does a walk the heap refuses
 * at the file's open — a refusal that may pass, met before the Helm is asked
 * for (EngrossOpen), so no service on another core, which finds the Helm held,
 * comes between. Once the Helm is let go, the walk its let-go asks for cuts
 * the tail under the act, and the Tally goes on.
 */
static TestResult TestStandEscheatTailHelmHeld(void)
{
    TestResult       verdict = TEST_PASS;
    StandBinder*     b = &s_escheat_binder;
    StandFreshModel  m;
    EscheatSaid      said;
    TagFSFileHandle* holder = NULL;
    uint32_t         hand = STAND_ESCHEAT_GONE;
    bool             held = false;
    bool             fired = false;
    error_t          rc = OK;
    uint64_t         fid = 0;
    const char*      aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    memset(&m, 0, sizeof(m));
    verdict = StandEscheatTailWaits(b, &m, &fid);
    if (verdict != TEST_PASS)
        goto out;
    holder = tagfs_open(fid, TAGFS_HANDLE_WRITE);
    TEST_ASSERT(holder, "a writer opens the file");
    hand = StandEscheatHold(holder);
    held = hand == STAND_ESCHEAT_HANDED;
    TEST_ASSERT(held, "and holds its Helm, the service's turn let go of first");
    TEST_ASSERT_OK(TagFSEscheat(INK_WARRANTED, &said), "Escheat asked with the warrant while the Helm is held");
    TEST_ASSERT_EQ(BinderRunWhole(), ERR_BUSY, "a Tally asked for now passes the file over: its Helm is another's");
    TEST_ASSERT(EscheatTailsActing() != 0, "and the act stands: that walk did not try the tail");
    HeapFailArm(amp_get_core_index(), 1);
    rc    = BinderRunWhole();
    fired = HeapFailFired();
    HeapFailDisarm();
    TEST_ASSERT(fired, "the heap refuses the walk of a Tally asked for now");
    TEST_ASSERT_EQ(rc, ERR_NO_MEMORY, "at the file's open, before its Helm is asked for");
    TEST_ASSERT(EscheatTailsActing() != 0, "and the act stands: a walk the heap refused did not try the tail");
    TEST_ASSERT(StandEscheatSize(fid) == TAGFS_BLOCK_SIZE + 150u, "nothing of the file changed");
    HelmLetGo(holder->ofe, &s_escheat_holder);
    held = false;
    TEST_ASSERT(StandEscheatWalked(), "the walk the let-go asks for tries the tail under the act");
    TEST_ASSERT_OK(BinderRunWhole(), "the Tally goes on: the tail is lost under the act the walk passed over");
    TEST_ASSERT_EQ(StandEscheatSize(fid), TAGFS_BLOCK_SIZE, "the file ends at its last whole block");
    BlockFailDisarm();
    StandBlockFailRaced("stand_escheat");
out:
    HeapFailDisarm();
    if (held)
        HelmLetGo(holder->ofe, &s_escheat_holder);
    if (holder && hand != STAND_ESCHEAT_ASKED)
        tagfs_close(holder);   /* one still in the FIFO keeps its handle: it is handed the Helm on it */
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

// ----------------------------------------------------------------------------

static const TestCase g_stand_escheat_tests[] = {
    { .name = "stand_escheat_warrant", .func = TestStandEscheatWarrant,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_escheat_tail", .func = TestStandEscheatTail,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_escheat_tail_helm_held", .func = TestStandEscheatTailHelmHeld,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
};

const TestSuite g_stand_escheat_suite = {
    g_stand_escheat_tests, sizeof(g_stand_escheat_tests) / sizeof(g_stand_escheat_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
