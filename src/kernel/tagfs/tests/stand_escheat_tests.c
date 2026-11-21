#include "stand_binder.h"
#include "stand_fresh.h"
#include "../settle/binder.h"
#include "../settle/dog_ear.h"
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
// The stand_escheat suite: the act that lets go what a book can never
// give back
//
// Escheat is asked only with the warrant a cabin wearing system or god
// carries; with nothing to let go it says so. A Slip's tail whose block cannot
// be read holds every Tally back for that file alone, said, until the act:
// then the tail is lost — the file cut to the end of its last whole block —
// and the Tally goes on. The ranges of FILES the act lets go are the landings'
// (logcheck book_escheat B4, B6c), and a range rotten by hand here: the act
// asked with nobody writing is laid by the Tally the cadence writes — mounted
// again with nothing written, a stitch after the ask ending on its Quire's
// last sector, a Tally refused before it — and the keys of DIGESTS it changes,
// every window of the ground it lets go, are DogEar's. Every test puts the
// Spine and the ribbon back as it found them.
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
    TEST_ASSERT_EQ(tagfs_create_file("escheat-tail.bin", NULL, 0, 0, fid), 0, "a file is made");
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
// The act asked has no waiter to ask again: its Tally begun by the ask and by
// the Settle's holder, nobody writing
// ----------------------------------------------------------------------------

/* The act laid by a Tally the cadence writes by itself, nobody writing: the
 * cadence the runner holds (tests.c) let go of, a turn of it run by hand on
 * this core as well, until no ask is owed and neither a Tally nor a cycle is
 * under way — false past the guard. */
static bool StandEscheatLaidByCadence(void)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_ESCHEAT_BOUND_MS);
    SettleCadenceHold(false);
    while ((EscheatOwed() || BinderHoldsGround() || __atomic_load_n(&g_settle_scribe.holder, __ATOMIC_ACQUIRE)) &&
           rdtsc() < bound)
        StandSettleTurn();
    SettleCadenceHold(true);
    return !EscheatOwed();
}

/*
 * ‼ AN ACT ASKED HAS NO WAITER TO ASK AGAIN. A write short of room waits on
 * the Tally it asked and asks once more each round; Escheat is answered by a
 * Tally nobody waits on, so the ask and the Settle's holder after it see that
 * Tally begun. Mounted again, nothing written since, no Tally may be begun
 * before a stitch of this mount settles (§4.2) — held so by this mount's own
 * count, never the count of the one before it, which the book's weigh at the
 * mount still read (binder.c BinderOwnMayNot): a Tally said owed, begun by
 * the committer's turn, held back there — and the ask asks the cycle whose
 * stitch lets it be begun. The cadence then writes the Tally that lays the act.
 */
static TestResult TestStandEscheatRemounted(void)
{
    TestResult   verdict = TEST_PASS;
    StandBinder* b = &s_escheat_binder;
    EscheatSaid  said;
    uint64_t     files = 0;
    const char*  aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    verdict = StandBinderRot(b, &files);
    if (verdict != TEST_PASS)
        goto out;
    TEST_ASSERT_OK(TagFSEscheat(INK_WARRANTED, &said), "Escheat asked with the warrant, nothing written since the mount");
    TEST_ASSERT(said.ranges >= 1, "it names the rotten range");
    TEST_ASSERT(!BinderOwed(), "no stitch of this mount has settled: the Tally it asks is not owed before one does");
    TEST_ASSERT(TagFSStandSettleDemands() & SETTLE_DEMAND_ROOM, "and the ask asks the cycle whose stitch lets it begin");
    TEST_ASSERT(StandEscheatLaidByCadence(), "the cadence settles that stitch and writes the Tally that lays the act, "
                                             "nobody writing");
out:
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/*
 * Nor is the act left to a write by the stitch after the ask: one that ends
 * on its Quire's last sector leaves the ribbon where no slot can put it on,
 * no Tally begun until a stitch more (binder.c s_on_edge), and the Settle's
 * holder, weighing that stitch, asks the cycle of the next itself
 * (BinderWeigh) — the Tally then begun lays the act.
 */
static TestResult TestStandEscheatOnEdge(void)
{
    TestResult   verdict = TEST_PASS;
    StandBinder* b = &s_escheat_binder;
    EscheatSaid  said;
    RibbonCursor c;
    uint64_t     files = 0, left = 0;
    bool         kept  = false;
    const char*  aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    verdict = StandBinderRot(b, &files);
    if (verdict != TEST_PASS)
        goto out;
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    RibbonCursorStand(&c);
    left = c.quire.blocks * TAGFS_BLOCK_SECTORS - c.off;
    if (left <= 2u * c.atom) {
        /* Too little of the Quire for a word of its own to fill it. */
        TEST_ASSERT_OK(StandStitchSayFilling(4u * c.atom, 'w', 90, &kept), "a word runs on into the next Quire");
        RibbonCursorStand(&c);
        left = c.quire.blocks * TAGFS_BLOCK_SECTORS - c.off;
    }
    TEST_ASSERT_OK(TagFSEscheat(INK_WARRANTED, &said), "Escheat asked with the warrant");
    TEST_ASSERT(BinderOwed(), "the Tally it asks is owed: one may be begun as the ribbon stands");
    TEST_ASSERT_OK(StandStitchSayFilling(left, 'x', 91, &kept), "a word whose stitch fills the rest of its Quire is "
                                                                "laid down, the cadence held");
    RibbonCursorStand(&c);
    TEST_ASSERT(kept && c.off == 0 && !BinderOwed(), "the stitch ended on its Quire's last sector: no Tally may be "
                                                     "begun until a stitch more");
    TEST_ASSERT(TagFSStandSettleDemands() & SETTLE_DEMAND_ROOM, "and the holder, weighing that stitch, asks the cycle "
                                                                "of the next");
    TEST_ASSERT(StandEscheatLaidByCadence(), "the cadence settles it and writes the Tally that lays the act, nobody "
                                             "writing");
out:
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/* The slots of the ring held as naming either of two Tallies: a Tally refused
 * once its slot's write began leaves its slot so (binder_done.c
 * BinderSlotUnsure), one refused at a node before it leaves the ring as it
 * was (C7). */
static uint32_t StandEscheatUnsure(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; g_binder.slot && i < g_binder.nslots; i++)
        n += g_binder.slot[i].unsure;
    return n;
}

/*
 * Nor by a Tally refused before the ask: one refused is tried again once a
 * cycle more settles (binder.c retry_after), and the ask asks that cycle, as
 * no write will — the Tally then begun lays the act. The cycle settled before
 * it carries a word of the Use Context, so the Tally has a node to lay and the
 * medium refuses that node (C7): with nothing changed since the mount, the
 * rotten node kept as it lies, its first write would be its slot (C9).
 */
static TestResult TestStandEscheatRetried(void)
{
    TestResult   verdict = TEST_PASS;
    StandBinder* b = &s_escheat_binder;
    EscheatSaid  said;
    uint64_t     files = 0;
    uint32_t     unsure = 0;
    error_t      rc = OK;
    bool         kept = false;
    const char*  aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    verdict = StandBinderRot(b, &files);
    if (verdict != TEST_PASS)
        goto out;
    TEST_ASSERT_OK(StandStitchSay("escheat retried", 15, 1, &kept), "a word of the Use Context settles in a stitch "
                                                                     "of this mount: the Tally has a node to lay");
    TEST_ASSERT(kept, "and the volume holds it");
    unsure = StandEscheatUnsure();
    BlockFailArm(b->s.seat, BLOCK_FAIL_WRITE, 0, 0, 1);
    rc = BinderRunWhole();
    BlockFailDisarm();
    TEST_ASSERT(rc != OK && BlockFailFired() && !BinderNoWrite(), "the Tally whose first node the medium refuses is "
                                                                  "refused, and the volume takes writes still");
    TEST_ASSERT_EQ(StandEscheatUnsure(), unsure, "refused at its node, before its slot was written: no slot of the "
                                                 "ring comes to be held as naming either Tally (C7)");
    TEST_ASSERT_OK(TagFSEscheat(INK_WARRANTED, &said), "Escheat asked with the warrant");
    TEST_ASSERT(!BinderOwed(), "the Tally it asks is not owed before a cycle more settles");
    TEST_ASSERT(TagFSStandSettleDemands() & SETTLE_DEMAND_ROOM, "and the ask asks that cycle");
    TEST_ASSERT(StandEscheatLaidByCadence(), "the cadence settles it and writes the Tally that lays the act, nobody "
                                             "writing");
out:
    BlockFailDisarm();
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

// ----------------------------------------------------------------------------
// The keys of DIGESTS the act changes: every window of the ground it lets go
// ----------------------------------------------------------------------------

/* Whether DogEar's span `s` holds every key of DIGESTS over blocks [lo, hi):
 * (lo, 0) not below it, (hi - 1, 0) below its end. */
static bool StandEscheatSpanHolds(const DogEarSpan* s, uint64_t lo, uint64_t hi)
{
    bool from = s->lo.k1 < lo || (s->lo.k1 == lo && s->lo.k2 == 0);
    bool to   = s->hi.k1 > hi - 1u || (s->hi.k1 == hi - 1u && s->hi.k2 > 0);
    return from && to;
}

/* How many runs of the ground the mount holds for its rotten ranges lie in a
 * window of DIGEST_ITEM_MOST blocks — a DIGESTS element's — that DogEar does
 * not hold whole among the spans put in since the last slice; the first such
 * said. None when DogEar was lost whole: every key is then changed (dog_ear.h).
 * UINT64_MAX when memory refuses the room to read the spans. */
static uint64_t StandEscheatWindowsOpen(void)
{
    DogEarSpan* spans = NULL;
    uint32_t    room  = 0, n = 0;
    uint64_t    open  = 0;
    /* DogEarStand answers how many spans it holds, not how many it copied: a
     * span put in from another core since the room was weighed (a read of a
     * digest mends a leaf, DogEarMended) is read again into room for all. */
    while ((n = DogEarStand(TALLY_DIGESTS, false, spans, room)) > room) {
        kfree(spans);
        room  = n;
        spans = kmalloc(sizeof(DogEarSpan) * room);
        if (!spans)
            return UINT64_MAX;
    }
    /* Asked after the read: lost before it or during it, what was read is
     * not DogEar's word, and every key is held changed anyway. */
    if (DogEarStandLost(false)) {
        kprintf("[TESTS] stand_escheat: DogEar was lost whole — every key of DIGESTS is held changed\n");
        kfree(spans);
        return 0;
    }
    for (uint64_t i = 0; i < g_binder.nheld; i++) {
        const BinderGround* g  = &g_binder.held[i];
        uint64_t            lo = g->start / DIGEST_ITEM_MOST * DIGEST_ITEM_MOST;
        uint64_t            hi = (g->start + g->count - 1u) / DIGEST_ITEM_MOST * DIGEST_ITEM_MOST + DIGEST_ITEM_MOST;
        bool                held = false;
        for (uint32_t s = 0; !held && s < n; s++)
            held = StandEscheatSpanHolds(&spans[s], lo, hi);
        if (!held && open++ == 0)
            kprintf("[TESTS] stand_escheat: the ground [%llu, %llu) the act lets go lies in the windows of DIGESTS "
                    "[%llu, %llu), and DogEar does not hold them whole\n", (unsigned long long)g->start,
                    (unsigned long long)(g->start + g->count), (unsigned long long)lo, (unsigned long long)hi);
    }
    kfree(spans);
    return open;
}

/* The act's Tally refused at its first node, as the medium refuses it: its
 * answer. One the ribbon lets no Tally begin yet is asked again past a stitch
 * more, the medium's refusal armed only for the Tally. */
static error_t StandEscheatRefusedAtNode(const StandBinder* b)
{
    error_t rc = ERR_BUSY;
    for (uint32_t t = 0; rc == ERR_BUSY && t < 2u; t++) {
        if (t && TagFSSettle() != OK)
            break;
        BlockFailArm(b->s.seat, BLOCK_FAIL_WRITE, 0, 0, 1);
        rc = BinderRunWhole();
        BlockFailDisarm();
    }
    return rc;
}

/*
 * ‼ THE ACT CHANGES WHOLE WINDOWS OF DIGESTS. A DIGESTS element is the
 * digests of blocks in a row inside one window of DIGEST_ITEM_MOST blocks
 * (volume_tally.h), so ground let go out of the middle of one leaves the
 * blocks past it in the window an element of their own, keyed by the first of
 * them — a key the Tally before never had. DogEar takes every window the
 * ground let go lies in, as it takes a file's (dog_ear.c WindowsNoted), or
 * the self-check — built in every stand kernel (binder_internal.h,
 * BINDER_CHECKS) — meets that key outside it where a live block lies past
 * the ground in its window: a STRICT kernel stops there, the stand's
 * other kernels refuse every Tally that owes the act. Here, on any book: a
 * Tally that took the act, refused at its first node, gives DogEar back what
 * it took, read whole windows — a run of ground that ends inside its window
 * shows the rule, a live block past it or not; and the act's Tally after it is
 * laid, held to the one before — which the self-check bites on only where a
 * live block lies past the ground in its window (the stand's padded book has
 * one).
 */
static TestResult TestStandEscheatWindows(void)
{
    TestResult   verdict = TEST_PASS;
    StandBinder* b = &s_escheat_binder;
    EscheatSaid  said;
    uint64_t     files = 0, held = 0, open = 0, inside = 0;
    error_t      rc = OK;
    const char*  aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    verdict = StandBinderRot(b, &files);
    if (verdict != TEST_PASS)
        goto out;
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles");
    TEST_ASSERT_OK(TagFSEscheat(INK_WARRANTED, &said), "Escheat asked with the warrant");
    for (uint64_t i = 0; i < g_binder.nheld; i++) {
        held += g_binder.held[i].count;
        inside += (g_binder.held[i].start + g_binder.held[i].count) % DIGEST_ITEM_MOST != 0;
    }
    TestEvidence("blocks the mount holds for the rotten range", held);
    /* The rule shows on a run that ends inside its window: one ended on a
     * window's last block leaves no element past it to be keyed anew. */
    TestEvidence("runs of that ground that end inside their window of DIGESTS", inside);
    TEST_ASSERT(said.blocks > 0 && said.blocks == held, "the act lets go ground, every block the mount holds for "
                                                        "the range: no file locked for another reason keeps any");
    rc = StandEscheatRefusedAtNode(b);
    TEST_ASSERT(rc != OK && BlockFailFired() && !BinderNoWrite(), "the Tally that took the act is refused at its "
                                                                  "first node, and the volume takes writes still");
    TEST_ASSERT(EscheatOwed(), "the act is owed again, what it took back where it was");
    open = StandEscheatWindowsOpen();
    TestEvidence("runs of the ground let go whose windows of DIGESTS DogEar does not hold whole", open);
    TEST_ASSERT_EQ(open, 0, "DogEar holds every window of DIGESTS the ground the act lets go lies in");
    TEST_ASSERT_OK(StandBinderTallies(1u), "the act's Tally is laid past a cycle more, its self-check held to the "
                                           "Tally before");
    TEST_ASSERT(!EscheatOwed(), "and the act with it");
out:
    BlockFailDisarm();
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

// ----------------------------------------------------------------------------

static const TestCase g_stand_escheat_tests[] = {
    { .name = "stand_escheat_warrant", .func = TestStandEscheatWarrant,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_escheat_tail", .func = TestStandEscheatTail,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_escheat_tail_helm_held", .func = TestStandEscheatTailHelmHeld,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_escheat_remounted", .func = TestStandEscheatRemounted,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_escheat_on_edge", .func = TestStandEscheatOnEdge,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_escheat_retried", .func = TestStandEscheatRetried,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_escheat_windows", .func = TestStandEscheatWindows,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_escheat_suite = {
    g_stand_escheat_tests, sizeof(g_stand_escheat_tests) / sizeof(g_stand_escheat_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
