#include "stand_binder.h"
#include "stand_fresh.h"
#include "stand_stitch.h"
#include "../settle/settle.h"
#include "../settle/sewing.h"
#include "../block_map/block_map.h"
#include "../block_map/block_map_derived.h"
#include "../file_map/file_map.h"
#include "../mount/book_mount.h"
#include "../helm/helm.h"
#include "../ink/fallow.h"
#include "../ink/ink.h"
#include "../plot/plot.h"
#include "../settle/binder.h"
#include "stand_tests.h"
#include "write_job.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_book_room suite: where a book's ground goes when it comes back
// (spec_v2 §3.6, §3.7's table)
//
// A next_quire is read past the Tally a mount stands on, and a reader takes it
// only clear of that Tally's files (§3.7, ribbon_collate.c RibbonNamed,
// stitch_ribbon.py Named): ground a file the newest Tally names gave up since
// may go to data, and goes to the ribbon only once a Tally past its giving up
// was written — for ground a delete gave back in this session and for the
// ground the mount found the ribbon let go alike (stand_book_room_quire_tests.c).
// A defrag gathers its file into one run it is handed whole, wherever the
// file's own window lies; asked as a program asks it, it gives its core back
// between its windows and is answered where the machine turns; and a file the
// system keeps it gathers not at all, warrant or none (B10). At a
// full volume's wall the reserve holds a write's ground from its take, each
// slot's own in whole Quires until the free has it, and a write short of room
// only by the measure of growth waits on the Tally that makes it exact
// (§16.6 п.8) — and a change short of room on the ribbon waits on the Tally
// that gives the ribbon its next_quire. Every test puts the volume back as it
// found it.
// ============================================================================

/*
 * ‼ A DEFRAG ON A BOOK IS HANDED ITS ONE RUN WHOLE. A file of many runs — every
 * other block of it written over, each version laid on fresh ground (§3.1)
 * from a window the file keeps in hand — is gathered one window at a time with
 * the Helm held as the defrag's job holds it. The run it gathers into is the
 * file's length: the window it keeps is shorter, and a take that went to the
 * window first would hand the gather a piece and refuse it no-space on a volume
 * with ground to spare. The file lies in one run and reads back whole.
 *
 * ‼ THE WINDOW IS ONE A TAKE WITH NO HINT MADE. What a write leaves its file
 * in hand is what its last take kept, as long as the ground let it be: the free
 * blocks past a run it went on at its hint (allotment.c, AllotmentBookAhead),
 * or the rest of a window sized to the file as it stood where its run broke
 * (AllotmentBookWindow). The versions come off that window's front, or lie at
 * their hint where it is free and keep what is free past it, so what they
 * leave in hand is the ground's doing too — and may be nothing, a window used
 * to its very end gone (AllotmentCut). So the window the versions leave is let
 * go, as the file's delete lets it go (ink_book.c, InkBookQuitclaimLocked), and
 * the file's first block is written over: a first block goes on from no run
 * (plot_window.c, PlotContinues), so its take has no hint and no window to
 * come off, and takes a new window sized to the file — itself in front, the
 * rest kept (Allotment 2) — on any seat and ground with a free run as long as
 * the file, which the gather's own take needs as well.
 */
#define STAND_BOOK_ROOM_GATHER  64u

static TestResult TestStandBookRoomGatherOneRun(void)
{
    TestResult       verdict = TEST_PASS;
    StandFreshModel  m;
    TagFSFileHandle* handle  = NULL;
    HelmWaiter       here;
    HelmDefragging   d;
    bool             holding = false, ended = false;
    uint64_t         fid = 0, runs = 0, start = 0;
    uint32_t         window = 0, turns = 0;
    error_t          rc = OK;
    const uint32_t   bytes = STAND_BOOK_ROOM_GATHER * TAGFS_BLOCK_SIZE;
    const char*      aside = StandStepsAsideFor();
    memset(&m, 0, sizeof(m));
    memset(&d, 0, sizeof(d));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, bytes), "room for the file");
    TEST_ASSERT_EQ(tagfs_create_file("book-gather.bin", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, bytes, 0x31u), (int)bytes, "written whole");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    for (uint32_t k = 1; k < STAND_BOOK_ROOM_GATHER; k += 2u)
        TEST_ASSERT_EQ(StandFreshWrite(&m, fid, (uint64_t)k * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 0x40u + k),
                       TAGFS_BLOCK_SIZE, "every other block written over");
    TagFSLockTake(&tagfs_get_state()->lock);
    AllotmentRelease(fid);
    TagFSLockDrop(&tagfs_get_state()->lock);
    TEST_ASSERT(!AllotmentHeld(fid, &start, &window), "the window the versions left in hand is let go");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 0x40u), TAGFS_BLOCK_SIZE,
                   "and the first block written over");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT(StandFreshPrint(fid, &runs) && runs > 1u, "the file lies in many runs");
    TestEvidence("runs the file lies in", runs);
    TEST_ASSERT(AllotmentHeld(fid, &start, &window) && window < STAND_BOOK_ROOM_GATHER,
                "and keeps a window in hand shorter than it is");
    TestEvidence("blocks of the window the file keeps", window);

    handle = tagfs_open(fid, TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle, "a handle opens it to write");
    TEST_ASSERT_OK(HelmTakeNow(handle, &here), "and holds its Helm, as the defrag's job does");
    holding = true;
    while (!ended && turns++ <= STAND_BOOK_ROOM_GATHER && (rc = HelmDefragWindow(handle, &d, &ended)) == OK)
        ;
    HelmDefragLetGo(&d);
    HelmLetGo(handle->ofe, &here);
    holding = false;
    TEST_ASSERT_OK(rc, "every window of the defrag is taken: the run it gathers into is handed whole");
    TEST_ASSERT(ended, "the defrag ends");
    TEST_ASSERT(StandFreshPrint(fid, &runs) && runs == 1u, "the file lies in one run");
    TEST_ASSERT(StandFreshHolds(&m, fid), "and reads back whole");
out:
    HelmDefragLetGo(&d);
    if (holding)
        HelmLetGo(handle->ofe, &here);
    tagfs_close(handle);
    if (fid)
        (void)tagfs_delete_file(fid, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    return verdict;
}

// ----------------------------------------------------------------------------
// A defrag asked as a program asks it, and a file the system keeps
// ----------------------------------------------------------------------------

#define STAND_BOOK_ROOM_JOB_MS 20000u      /* the test's own bound on a job it turns by hand */

static WriteJobStandAnswer s_room_answer;

/* A file of STAND_BOOK_ROOM_GATHER blocks of the model's stream lying in many
 * runs, laid as the gather above lays its own: written whole and settled,
 * every other block written over on fresh ground, the window the versions
 * left let go and the first block written over, settled. Its runs in *runs;
 * *fid is the caller's to give up whatever the answer. */
static bool StandBookRoomScattered(StandFreshModel* m, const char* name, uint64_t* fid, uint64_t* runs)
{
    const uint32_t B = TAGFS_BLOCK_SIZE;
    uint64_t       start = 0;
    uint32_t       window = 0;
    if (tagfs_create_file(name, NULL, 0, fid) != 0)
        return false;
    if (StandFreshWrite(m, *fid, 0, STAND_BOOK_ROOM_GATHER * B, 0x31u) != (int)(STAND_BOOK_ROOM_GATHER * B) ||
        TagFSSettle() != OK)
        return false;
    for (uint32_t k = 1; k < STAND_BOOK_ROOM_GATHER; k += 2u)
        if (StandFreshWrite(m, *fid, (uint64_t)k * B, B, 0x40u + k) != (int)B)
            return false;
    TagFSLockTake(&tagfs_get_state()->lock);
    AllotmentRelease(*fid);
    TagFSLockDrop(&tagfs_get_state()->lock);
    if (AllotmentHeld(*fid, &start, &window))
        return false;
    if (StandFreshWrite(m, *fid, 0, B, 0x40u) != (int)B || TagFSSettle() != OK)
        return false;
    return StandFreshPrint(*fid, runs) && *runs > 1u;
}

/* A defrag of the file asked as the deck asks it, with `warrant`, by a caller
 * in the kernel standing in its call: the job's answer. */
static int StandBookRoomDefrag(uint64_t fid, InkWarrant warrant)
{
    WriteJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    ask.kind    = WRITE_JOB_DEFRAG;
    ask.file_id = fid;
    ask.warrant = warrant;
    return WriteJobSubmit(&ask, NULL, NULL);
}

/* The machine turned by hand until the job answered the stand's record — its
 * windows run at the service point here or on another core — or the bound. */
static bool StandBookRoomAnswered(void)
{
    uint8_t  seat  = tagfs_get_seat();
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_BOOK_ROOM_JOB_MS);
    while (!__atomic_load_n(&s_room_answer.answered, __ATOMIC_ACQUIRE)) {
        if (rdtsc() >= bound)
            return false;
        StandPortTurn(seat);
        cpu_pause();
    }
    return true;
}

/*
 * R24 on a book: a defrag a program asks for goes a window at a time, its
 * holder giving the core back between two of them, the Helm kept — it is
 * answered where the machine turns, and the file lies in one run, whole.
 */
static TestResult TestStandBookRoomDefragGivesTheCoreBack(void)
{
    TestResult      verdict = TEST_PASS;
    StandFreshModel m;
    WriteJobAsk     ask;
    uint64_t        fid = 0, runs = 0;
    bool            pending = false;
    const char*     aside   = StandStepsAsideFor();
    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_BOOK_ROOM_GATHER * TAGFS_BLOCK_SIZE), "room for the file");
    TEST_ASSERT(StandBookRoomScattered(&m, "book-defrag-job.bin", &fid, &runs), "a file lies in many runs");
    TestEvidence("runs the file lies in", runs);

    memset(&ask, 0, sizeof(ask));
    memset(&s_room_answer, 0, sizeof(s_room_answer));
    ask.kind    = WRITE_JOB_DEFRAG;
    ask.file_id = fid;
    ask.stand   = &s_room_answer;
    int asked   = WriteJobSubmit(&ask, NULL, NULL);
    pending     = asked == ERR_WOULD_BLOCK;
    TEST_ASSERT_EQ(asked, ERR_WOULD_BLOCK, "its defrag gave its core back after its first window");
    TEST_ASSERT(StandBookRoomAnswered(), "and is answered where the machine turns");
    pending = false;
    TEST_ASSERT_OK(s_room_answer.rc, "OK");
    TEST_ASSERT(StandFreshPrint(fid, &runs) && runs == 1u, "the file lies in one run");
    TEST_ASSERT(StandFreshHolds(&m, fid), "and reads back whole");
out:
    if (pending && !StandBookRoomAnswered())
        fid = 0;          /* the job still holds the file's Helm: the file is left to it */
    if (fid)
        (void)tagfs_delete_file(fid, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    return verdict;
}

/*
 * B10 on a book: a file the system keeps is never moved to gather its runs.
 * A defrag with no warrant is refused at the Helm; one with the warrant
 * passes it and is refused by the gather (plot_gather.c, PlotGatherKept), the
 * runs as they were. With the role let go the same defrag gathers the file
 * into one run — so the refusal was the file's being the system's, and
 * nothing else.
 */
static TestResult TestStandBookRoomKeptFileNotGathered(void)
{
    TestResult      verdict = TEST_PASS;
    StandFreshModel m;
    uint64_t        fid = 0, runs = 0, now = 0;
    const char*     aside = StandStepsAsideFor();
    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_BOOK_ROOM_GATHER * TAGFS_BLOCK_SIZE), "room for the file");
    TEST_ASSERT(StandBookRoomScattered(&m, "book-kept.bin", &fid, &runs), "a file lies in many runs");
    TEST_ASSERT_OK(StandKeepInRole(fid, TAGFS_FILE_ROLE_SYSTEM), "it is kept in the system's role");

    TEST_ASSERT_EQ(StandBookRoomDefrag(fid, INK_UNWARRANTED), ERR_PERMISSION_DENIED,
                   "a defrag with no warrant is refused");
    TEST_ASSERT_EQ(StandBookRoomDefrag(fid, INK_WARRANTED), ERR_PERMISSION_DENIED,
                   "and one with the warrant: its blocks are not moved");
    TEST_ASSERT(StandFreshPrint(fid, &now) && now == runs, "the file lies in the runs it lay in");

    TEST_ASSERT_OK(StandKeepInRole(fid, TAGFS_FILE_ROLE_ORDINARY), "the role is let go");
    TEST_ASSERT_OK(StandBookRoomDefrag(fid, INK_UNWARRANTED), "and the same defrag is taken");
    TEST_ASSERT(StandFreshPrint(fid, &now) && now == 1u, "the file lies in one run");
    TEST_ASSERT(StandFreshHolds(&m, fid), "and reads back whole");
out:
    if (fid)
        (void)tagfs_delete_file(fid, INK_WARRANTED);
    StandFreshModelLetGo(&m);
    return verdict;
}

// ----------------------------------------------------------------------------
// The reserve at a full volume's wall (§16.4 п.7, §16.6 п.8)
// ----------------------------------------------------------------------------

/*
 * ‼ THE MEASURE HOLDS A WRITE'S GROUND FROM ITS TAKE. The reserve a write of
 * data leaves is measured before its Ink names the ground it took, so the
 * measure taken while that ground is staked must hold what the Ink adds: else
 * a write taken to a full volume's reserve raises it, once inked, past the
 * ground left, and the Quire the reserve kept the ribbon is the write's.
 */
#define STAND_BOOK_ROOM_STAKED  4u   /* the blocks the write takes: the test buffer's */

static TestResult TestStandBookRoomStakesCounted(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    static HelmWaiter  helm;
    static Plot        plot;
    TagFSFileHandle*   handle = NULL;
    PlotSource         src    = { .kernel = g_test_buffer, .bytes = STAND_BOOK_ROOM_STAKED * TAGFS_BLOCK_SIZE };
    InkReceipt         inked;
    uint64_t           fid = 0, at = 0, done = 0, staked = 0, after = 0;
    bool               holding = false, loaded = false;
    const char*        aside   = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    memset(g_test_buffer, 0x5A, src.bytes);
    TEST_ASSERT(StandMakeFile("book-room-staked.bin", g_test_buffer, TAGFS_BLOCK_SIZE, &fid),
                "a file of one block is made");
    TEST_ASSERT_OK(StandBinderTallies(1), "and a Tally takes it in, the lag's stitches settled past it");
    handle = tagfs_open(fid, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    TEST_ASSERT(handle, "the writer's handle opens");
    memset(&helm, 0, sizeof(helm));
    TEST_ASSERT(HelmAsk(handle->ofe, &helm), "and it holds the file's Helm");
    holding = true;
    TEST_ASSERT_OK(PlotLoad(&plot, fid, INK_UNWARRANTED), "the write loads the record");
    loaded    = true;
    plot.helm = &handle->ofe->helm;
    TEST_ASSERT_OK(PlotBeginWrite(&plot, plot.size, false, src.bytes, &at), "and takes the ground it grows by");
    staked = TallyCostBlocks();
    TEST_ASSERT_OK(PlotWriteBlocks(&plot, at, &src, &done), "its blocks land");
    TEST_ASSERT_OK(PlotEndWrite(&plot, at, done), "the write is made ready to be the file's");
    TEST_ASSERT_OK(TagFSInkPlot(&plot, &inked), "and inked");
    after = TallyCostBlocks();
    TestEvidence("blocks of the next Tally measured while the write's ground was staked", staked);
    TestEvidence("... and once it was inked", after);
    TEST_ASSERT(staked >= after, "the measure taken while the write's ground was staked holds what its Ink added");
out:
    if (loaded)
        PlotRelease(&plot);
    if (holding)
        HelmLetGo(handle->ofe, &helm);
    tagfs_close(handle);
    if (fid)
        (void)tagfs_delete_file(fid, INK_UNWARRANTED);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/* The slot the next Tally is written over: the one of the least seq — every
 * one holds, the ring filled (binder_slots.c, BinderSlotNext). */
static uint32_t StandBookRoomLeast(void)
{
    uint32_t least = 0;
    for (uint32_t i = 1; i < g_binder.ring; i++)
        if (g_binder.slot[i].seq < g_binder.slot[least].seq)
            least = i;
    return least;
}

/*
 * ‼ A SLOT'S OWN IS WHAT A TALLY OVER IT GIVES BACK, IN WHOLE QUIRES, AND STAYS
 * ITS OWN UNTIL THE FREE HAS IT. The reserve keeps what each slot of the ring
 * lacks of a whole Tally by the ground a Tally written over it gives back:
 * whole Tally-Quires — each the last slot's to go of those that name it —
 * which its nodes' blocks rounded down to a Quire fell short of by up to a
 * Quire a copy, every slot; and that ground rests in Fallow for the lag's
 * stitches after the DONE — counted the slot's no more at the DONE, the
 * reserve rose before the free did.
 */
static TestResult TestStandBookRoomOwnWhole(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    uint64_t           q = 0, was = 0, fallow = 0, rests = 0, held = 0;
    uint32_t           r = 0;
    const char*        aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    q = RibbonCursorNow()->quire_blocks;
    TEST_ASSERT(q > 0, "the ribbon stands in a Quire");
    TEST_ASSERT_OK(StandBinderTallies(g_binder.ring + 1u), "the ring is filled, a Tally over every slot");
    for (uint32_t i = 0; i < g_binder.ring; i++)
        TEST_ASSERT_EQ(__atomic_load_n(&g_binder.own[i], __ATOMIC_ACQUIRE) % q, 0,
                       "each slot's own ground is counted in whole Quires, as a Tally over it gives it back");
    r      = StandBookRoomLeast();
    was    = __atomic_load_n(&g_binder.own[r], __ATOMIC_ACQUIRE);
    fallow = FallowBlocks();
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally is written over the slot of the least seq");
    rests = FallowBlocks() > fallow ? FallowBlocks() - fallow : 0;
    held  = __atomic_load_n(&g_binder.own[r], __ATOMIC_ACQUIRE);
    TEST_ASSERT(FallowBlocks() >= fallow, "the DONE gave nothing back before its lag");
    TestEvidence("blocks the slot counted its own", was);
    TestEvidence("blocks its Tally let go to Fallow", rests);
    TEST_ASSERT(rests >= was, "what the slot counted its own went back whole");
    TEST_ASSERT(held >= was, "and while it rests in Fallow the slot counts it its own still: the reserve rises no "
                             "sooner than the free");
    for (uint64_t l = 0; l <= FallowLag(); l++)
        TEST_ASSERT_OK(TagFSSettle(), "the lag's stitches settle");
    TEST_ASSERT(FallowBlocks() <= fallow, "and Fallow gives it back");
    TEST_ASSERT(rests == 0 || __atomic_load_n(&g_binder.own[r], __ATOMIC_ACQUIRE) < held,
                "and the slot's count goes with it");
out:
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/*
 * ‼ A TAKE SHORT OF ROOM ONLY BY THE MEASURE OF GROWTH WAITS ON THE TALLY THAT
 * MAKES IT EXACT, FOR A TURN OF THE RING AT THE MOST. The book's reserve keeps
 * the next Tally by the last one's exact size and, from above, the growth
 * since (§16.4 п.7), each Quire of it once for the Tally and once for each
 * slot holding less: a file written past the newest Tally is measured wider
 * than the Tally that takes it in lays it. A write that finds no room past
 * that measure waits for the Tally — asked, event by event, no timer — and is
 * refused no-space only past it (§16.6 п.8). With every free block past the
 * reserve promised away, a take for the file's data is told to wait, not
 * refused; the wait for room writes the Tally, and the next is measured less
 * than the file past the last one was; asked again round after round, the
 * promises standing so nothing comes back to data, the take is refused
 * no-space once the ring has turned — it never waits for ever.
 */
#define STAND_BOOK_ROOM_ROUNDS  24u

/* The volume mounted afresh — no Tally under way, every turn of the ring a
 * wait of the mounting before spent let go — and the lag's stitches settled
 * past it, what the mount found the ribbon let go given back. */
static bool StandBookRoomQuiet(void)
{
    if (!StandStitchRemount())
        return false;
    for (uint64_t l = 0; l <= FallowLag() + 1u; l++)
        if (TagFSSettle() != OK)
            return false;
    return true;
}

/* Every free block past the book's fresh reserve promised: how many more. */
static uint64_t StandBookRoomPromiseAll(uint32_t epoch)
{
    uint64_t n = 0, keep = EndpaperDataKeeps(true);
    while (AllotmentPromise(keep, epoch))
        n++;
    return n;
}

static TestResult TestStandBookRoomTallyGivesRoom(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    uint64_t           fid = 0, promised = 0, start = 0, before = 0, q = 0;
    uint32_t           got = 0, epoch = 0, rounds = 0, bytes = 0;
    error_t            rc    = OK;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBookRoomQuiet(), "the volume mounted afresh, the lag's stitches settled past it");
    q     = RibbonCursorNow()->quire_blocks;
    epoch = tagfs_mount_epoch();
    bytes = (uint32_t)(2u * q * TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandFreshModelMake(&m, bytes), "room for the file");
    TEST_ASSERT_EQ(tagfs_create_file("book-room-t.bin", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, bytes, 0xA5u), (int)bytes, "two Quires of it written");
    TEST_ASSERT_OK(TagFSSettle(), "and settled past the newest Tally");
    before   = TallyCostBlocks();
    promised = StandBookRoomPromiseAll(epoch);
    rc       = AllotmentTakeBook(fid, 1u, 1u, 0, false, EndpaperDataKeeps(false), false, &start, &got);
    TEST_ASSERT_EQ(rc, ERR_WOULD_BLOCK, "a block of the file's data, every other free one promised: the take waits on "
                                        "the Tally that measures the file to the block");
    /* The ribbon on its Quire's last sector waits a stitch for the Tally to be
     * begun, and a cycle that closes a Quire early may rest a Quire in
     * Fallow: as many waits as those take, each its own event. */
    for (rounds = 0; rc == ERR_WOULD_BLOCK && rounds < STAND_BOOK_ROOM_ROUNDS; rounds++) {
        TEST_ASSERT_OK(SettleForRoom(), "its wait for room ends");
        if (TallyCostBlocks() < before)
            break;
        promised += StandBookRoomPromiseAll(epoch);
        rc = AllotmentTakeBook(fid, 1u, 1u, 0, false, EndpaperDataKeeps(false), false, &start, &got);
    }
    TEST_ASSERT(TallyCostBlocks() < before, "its wait for room writes that Tally, and the next Tally is measured "
                                            "less than the file past the last was");
    for (rounds = 0; rounds < STAND_BOOK_ROOM_ROUNDS; rounds++) {
        promised += StandBookRoomPromiseAll(epoch);
        rc = AllotmentTakeBook(fid, 1u, 1u, 0, false, EndpaperDataKeeps(false), false, &start, &got);
        if (rc != ERR_WOULD_BLOCK && rc != OK)
            break;
        if (rc == OK)
            (void)BlockMapDerivedGive(fid, start, got);
        else
            TEST_ASSERT_OK(SettleForRoom(), "each wait for room ends");
    }
    TestEvidence("rounds a take short of room waited", rounds);
    TEST_ASSERT_EQ(rc, ERR_TAGFS_NO_SPACE, "nothing let come back to data, the take is refused no-space once the "
                                           "ring has turned — never waits for ever");
out:
    for (; promised > 0; promised--)
        AllotmentPromiseLetGo(epoch);
    if (fid)
        (void)tagfs_delete_file(fid, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/* A Tally begun by a step of the test's own, and under way: the ribbon on
 * its Quire's last sector lets none begin until a stitch more. */
static bool StandBookRoomBegun(void)
{
    for (uint32_t i = 0; i < 3u; i++) {
        if (BinderTurn() && BinderHoldsGround())
            return true;
        if (TagFSSettle() != OK)
            return false;
    }
    return false;
}

/*
 * ‼ A SLOT'S REST GOES WITH ITS GROUND, NOT WITH ITS CYCLE. What a Tally's DONE
 * let go rests in Fallow, the slot counting it its own, until Fallow gives it
 * back — and Fallow gives nothing while a Tally is under way. Let go at the
 * TAKE of the cycle the ground was due back at, the count went while the
 * ground stayed, a Tally under way: the reserve rose by a slot's ground while
 * the free did not, at a full volume's wall where Tallies follow one another.
 * Every cycle of the lag settled with the next Tally under way, the slot
 * counts its ground its own while Fallow rests it; once that Tally ends and
 * the lag's cycles give the ground back, the count goes with it.
 *
 * The Tally weighed is the first over a slot that holds one whose DONE lets
 * ground go. A Tally that changes nothing lays no node anew, and one that
 * changes little lays it in the pair the slot's Tally lay in: what the slot
 * holds alone are the Quires of the ribbon its `at` lies before every newer
 * slot's (binder_slots.c, SlotGoes) — none while the ring's Tallies stand in
 * one Quire, as empty stitches leave them. So before each Tally the ribbon is
 * run on into a Quire it had not entered: within a turn of the ring every slot
 * stands in a Quire of its own, and the one written over lets its Quire go.
 */
static bool StandBookRoomQuireOn(uint64_t k)
{
    RibbonCursor c;
    bool         kept = false;
    RibbonCursorStand(&c);
    uint64_t was = c.quire.block, half = c.quire_blocks * TAGFS_BLOCK_SECTORS / 2u;
    for (uint32_t i = 0; i < 3u && c.quire.block == was; i++) {
        if (StandStitchSayFilling(half, (char)('a' + (k + i) % 26u), k + i, &kept) != OK)
            return false;
        RibbonCursorStand(&c);
    }
    return c.quire.block != was;
}

static TestResult TestStandBookRoomRestUnderTally(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    uint64_t           rested = 0, fallow = 0, held = 0;
    uint32_t           r = 0;
    bool               under = false;
    error_t            rc = OK;
    const char*        aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_OK(TagFSSettle(), "a stitch of this mount settles: a Tally may begin");
    for (uint32_t t = 0; t <= g_binder.ring && rested <= fallow; t++) {
        bool holds;
        TEST_ASSERT(StandBookRoomQuireOn(400u + 4u * t), "the ribbon is run on into a Quire it had not entered");
        r      = BinderSlotNext();
        holds  = g_binder.slot[r].holds;
        fallow = FallowBlocks();
        if ((rc = BinderRunWhole()) == ERR_BUSY && TagFSSettle() == OK) {
            fallow = FallowBlocks();    /* the cycle a Tally waited on may have given ground back */
            rc     = BinderRunWhole();
        }
        TEST_ASSERT_OK(rc, "a Tally is written over the slot the ring writes next");
        rested = holds ? FallowBlocks() : fallow;
    }
    TEST_ASSERT(rested > fallow, "within a turn of the ring a Tally's DONE lets the Quire of the ribbon only the "
                                 "slot it wrote over named go to Fallow");
    held = __atomic_load_n(&g_binder.own[r], __ATOMIC_ACQUIRE);
    TestEvidence("blocks its Tally let go to Fallow", rested - fallow);
    TEST_ASSERT(StandBookRoomBegun(), "the next Tally is begun at once, and is under way");
    under = true;
    for (uint64_t l = 0; l <= FallowLag() + 1u; l++) {
        TEST_ASSERT_OK(TagFSSettle(), "a stitch of the lag settles while it is under way");
        TEST_ASSERT(FallowBlocks() >= rested, "and Fallow gives nothing back while a Tally is under way");
        TEST_ASSERT(__atomic_load_n(&g_binder.own[r], __ATOMIC_ACQUIRE) >= held,
                    "so the slot counts what it let go its own still: the reserve rises no sooner than the free");
    }
    TEST_ASSERT_OK(BinderRunWhole(), "the Tally under way ends");
    under = false;
    for (uint64_t l = 0; l <= FallowLag() + 1u; l++)
        TEST_ASSERT_OK(TagFSSettle(), "the lag's stitches settle past it");
    TEST_ASSERT(FallowBlocks() < rested, "and Fallow gives the ground back");
    TEST_ASSERT(__atomic_load_n(&g_binder.own[r], __ATOMIC_ACQUIRE) < held, "and the slot's count goes with it");
out:
    if (under)
        (void)BinderRunWhole();
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/*
 * ‼ A WRITE WAITS A TURN OF THE RING AT THE MOST, A TALLY UNDER WAY AT EVERY
 * ASK. A take short of room only by the measure of growth waits while a Tally
 * is under way — its end gives its slot's ground back — and counts that
 * Tally's end a turn of the ring's. A Tally begun between every two asks (the
 * ribbon asks one at each DONE while it has no next_quire, the useful bytes
 * one when they reach TALLY_SLACK) held the write for as long as Tallies were
 * begun: the turn was not counted on the ask that found one under way. With
 * every free block past the reserve promised, one begun before each ask, the
 * take waits and is then refused no-space once the ring has turned. An ask
 * that finds ground resting in Fallow — the begun Tally's cycles may let a
 * Quire of the ribbon go — waits on the cycle that gives it back, not on the
 * Tally (allotment.c, AllotmentBookShort), and spends no turn: counted apart.
 */
static TestResult TestStandBookRoomWaitBounded(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    uint64_t           fid = 0, promised = 0, start = 0, q = 0;
    uint32_t           got = 0, epoch = 0, rounds = 0, waited = 0, rested = 0, bytes = 0;
    error_t            rc     = OK;
    bool               under  = false, fallow = false;
    const char*        aside  = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBookRoomQuiet(), "the volume mounted afresh, the lag's stitches settled past it");
    q     = RibbonCursorNow()->quire_blocks;
    epoch = tagfs_mount_epoch();
    bytes = (uint32_t)(q * TAGFS_BLOCK_SIZE);
    TEST_ASSERT(StandFreshModelMake(&m, bytes), "room for the file");
    TEST_ASSERT_EQ(tagfs_create_file("book-room-w.bin", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, bytes, 0x5Au), (int)bytes, "a Quire of it written");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    for (rounds = 0; rounds < STAND_BOOK_ROOM_ROUNDS; rounds++) {
        for (uint64_t l = 0; l <= FallowLag() + 1u; l++)
            TEST_ASSERT_OK(TagFSSettle(), "the lag's stitches settle, what the last Tally let go given back");
        promised += StandBookRoomPromiseAll(epoch);
        TEST_ASSERT(StandBookRoomBegun(), "a Tally is begun before the ask, and is under way");
        under  = true;
        fallow = FallowBlocks() > 0;
        rc     = AllotmentTakeBook(fid, 1u, 1u, 0, false, EndpaperDataKeeps(false), false, &start, &got);
        if (rc == OK)
            (void)BlockMapDerivedGive(fid, start, got);
        if (rc != ERR_WOULD_BLOCK && rc != OK)
            break;
        waited += rc == ERR_WOULD_BLOCK && !fallow ? 1u : 0u;
        rested += rc == ERR_WOULD_BLOCK && fallow ? 1u : 0u;
        TEST_ASSERT_OK(BinderRunWhole(), "the Tally under way ends");
        under = false;
    }
    TestEvidence("asks a take short of room waited on a Tally under way", waited);
    TestEvidence("asks it waited on ground resting in Fallow", rested);
    TEST_ASSERT(waited > 0, "a block of data, every other free one promised: the take waits on the Tally under way");
    TEST_ASSERT_EQ(rc, ERR_TAGFS_NO_SPACE, "and is refused no-space once the ring has turned, a Tally under way at "
                                           "every ask — never held for as long as Tallies are begun");
    TEST_ASSERT(waited <= g_binder.ring, "a turn of the ring at the most");
out:
    if (under)
        (void)BinderRunWhole();
    for (; promised > 0; promised--)
        AllotmentPromiseLetGo(epoch);
    if (fid)
        (void)tagfs_delete_file(fid, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/* A file of a block made, settled and given up past the newest Tally: its
 * ground rests until the ribbon leaves the Quire its Quitclaim lies in. */
static TestResult StandBookRoomGivenUp(StandFreshModel* m, const char* name)
{
    TestResult verdict = TEST_PASS;
    uint64_t   fid     = 0;
    TEST_ASSERT(StandFreshModelMake(m, TAGFS_BLOCK_SIZE), "room for the file");
    TEST_ASSERT_EQ(tagfs_create_file(name, NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(m, fid, 0, TAGFS_BLOCK_SIZE, 0x3Cu), (int)TAGFS_BLOCK_SIZE,
                   "a block of it written");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT_EQ(tagfs_delete_file(fid, INK_UNWARRANTED), 0, "it is given up");
    fid = 0;
    TEST_ASSERT_OK(TagFSSettle(), "its Quitclaim settles past the newest Tally");
out:
    if (fid)
        (void)tagfs_delete_file(fid, INK_UNWARRANTED);
    return verdict;
}

/*
 * ‼ A CHANGE THAT WAITS FOR A BARRED NEXT_QUIRE WAITS ON THE TALLY THAT LIFTS
 * THE BAR. A file is given up past the newest Tally and room asked for closes
 * the Quire its stitch lies in early: the ribbon stands in its next_quire with
 * none past it, and every free Quire lies on ground kept from the ribbon as the
 * delete's is (fallow.c, NodeBarred — FallowStandBar). A file asked for waits
 * for room on the ribbon, and its own wait for room writes the Tally past the
 * delete — the bar lifted, a next_quire given — within a turn of the ring, each
 * wait laying no more stitches than the lag's. Waited out a cycle at a time,
 * the Tally a step between every two, the cycles' stitches spent the rest of
 * the Quire the ribbon stood in, and the GiB written the moment a GiB was
 * deleted was refused half-way (fullvolume, roomback). The file asked for
 * again is made. The runner holds the cadence: no Tally is written but by the
 * wait.
 */
static TestResult TestStandBookRoomBarWaitsTally(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    RibbonCursor       c;
    uint64_t           made = 0, tallies = 0, last = 0;
    uint32_t           waits = 0;
    bool               armed = false;
    int                rc    = 0;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBookRoomQuiet(), "the volume mounted afresh, the lag's stitches settled past it");
    TEST_ASSERT(StandBookRoomGivenUp(&m, "book-room-x.bin") == TEST_PASS, "a file is given up");
    FallowStandBar(RibbonCursorNow()->last);
    armed = true;
    TEST_ASSERT_OK(SettleForRoom(), "room is asked for, closing the Quire early");
    RibbonCursorStand(&c);
    TEST_ASSERT(!c.next.blocks && c.taken == ERR_WOULD_BLOCK,
                "the ribbon stands in its next_quire with none past it: every free Quire lies on ground kept from it");
    tallies = g_binder.tallies;
    last    = c.last;
    rc      = tagfs_create_file("book-room-y.bin", NULL, 0, &made);
    TEST_ASSERT_EQ(rc, -(int)ERR_WOULD_BLOCK, "a file asked for waits for room on the ribbon");
    for (waits = 0; rc == -(int)ERR_WOULD_BLOCK && waits < g_binder.ring; waits++) {
        TEST_ASSERT_OK(SettleForRoom(), "its wait for room ends");
        rc = tagfs_create_file("book-room-y.bin", NULL, 0, &made);
    }
    TestEvidence("waits for room the file asked for took", waits);
    TEST_ASSERT(g_binder.tallies > tallies, "its wait for room writes the Tally past the delete");
    TEST_ASSERT_EQ(rc, 0, "and the file asked for again is made: the bar lifted, the ribbon names a next_quire");
    RibbonCursorStand(&c);
    TestEvidence("stitches the waits laid", c.last - last);
    TEST_ASSERT(c.last - last <= (uint64_t)waits * (FallowLag() + 2u),
                "each wait laid no more stitches than the lag's — never the Quire's rest a cycle at a time");
out:
    if (armed)
        FallowStandBar(0);
    if (made && rc == 0)
        (void)tagfs_delete_file(made, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/* Every free block past the Tallies' reserve promised, and words run the
 * ribbon on into its next_quire: it stands there with none past it, its ask
 * refused past the reserve — nothing resting in a Quire it left, no claim or
 * hold, no stitch in flight, so no cycle makes room on it (ribbon.c,
 * RibbonRoomFor) and a change short of room is refused at once but for a
 * Tally. */
static TestResult StandBookRoomRibbonShut(uint32_t epoch, uint64_t* promised)
{
    TestResult   verdict = TEST_PASS;
    RibbonCursor c;
    uint64_t     q = RibbonCursorNow()->quire_blocks, left = 0, claimed = 0, reserved = 0;
    bool         kept = false;
    while (AllotmentPromise(EndpaperKeeps(q), epoch))
        (*promised)++;
    RibbonCursorStand(&c);
    left = c.quire.blocks * TAGFS_BLOCK_SECTORS - c.off;
    TEST_ASSERT(left > 2u * c.atom && c.next.blocks, "the ribbon has more than two atoms of its Quire, and a next");
    TEST_ASSERT_OK(StandStitchSayFilling(left - 2u * c.atom, 'u', 80, &kept), "a word leaves two atoms of it");
    TEST_ASSERT_OK(StandStitchSayFilling(4u * c.atom, 'v', 81, &kept),
                   "a word four atoms long runs on into the next_quire and settles");
    RibbonCursorStand(&c);
    RibbonClaimsStand(&claimed, &reserved);
    TEST_ASSERT(!c.next.blocks && c.taken == ERR_TAGFS_NO_SPACE,
                "every free block past the Tallies' reserve promised: no next_quire past it, refused past the reserve");
    TEST_ASSERT(FallowLeftBlocks() == 0 && claimed == 0 && reserved == 0,
                "nothing rests in a Quire the ribbon left and no room is claimed or held: no cycle makes room");
out:
    return verdict;
}

/*
 * ‼ A CHANGE REFUSED ROOM ON THE RIBBON WAITS ON THE TALLY UNDER WAY. A Tally
 * is begun and the ribbon shut past the Tallies' reserve (StandBookRoomRibbonShut)
 * while the Tally under way — whose end asks the next_quire again by the
 * measure it makes (ribbon.c, RibbonNextAgain) — rests nothing in Fallow for a
 * cycle to give back. A file asked for waits on it rather than be refused at
 * once — at a full volume's wall a measure from above refused a next_quire the
 * exact one gives (§16.6 п.8) — and its wait for room ends that Tally; with
 * none under way the file is answered, made with what the Tally gave back or
 * refused no-space, never waiting for ever.
 */
static TestResult TestStandBookRoomRibbonWaitsUnderWay(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    uint64_t           made = 0, promised = 0;
    uint32_t           epoch = 0;
    bool               under = false;
    int                rc    = 0;
    const char*        aside = StandBinderAside(&b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBookRoomQuiet(), "the volume mounted afresh, the lag's stitches settled past it");
    TEST_ASSERT(StandBookRoomBegun(), "a Tally is begun, and is under way");
    under = true;
    epoch = tagfs_mount_epoch();
    TEST_ASSERT(StandBookRoomRibbonShut(epoch, &promised) == TEST_PASS, "the ribbon is shut past the reserve");
    rc = tagfs_create_file("book-room-v.bin", NULL, 0, &made);
    TEST_ASSERT_EQ(rc, -(int)ERR_WOULD_BLOCK, "a file asked for waits on the Tally under way, not refused at once");
    TEST_ASSERT_OK(SettleForRoom(), "its wait for room ends");
    under = BinderHoldsGround();
    TEST_ASSERT(!under, "and ends the Tally under way");
    for (uint32_t waits = 0; rc == -(int)ERR_WOULD_BLOCK && waits <= g_binder.ring; waits++) {
        rc = tagfs_create_file("book-room-v.bin", NULL, 0, &made);
        if (rc == -(int)ERR_WOULD_BLOCK)
            TEST_ASSERT_OK(SettleForRoom(), "each wait for room ends");
    }
    TEST_ASSERT(rc == 0 || rc == -(int)ERR_TAGFS_NO_SPACE,
                "with no Tally under way the file is answered: made with what the Tally gave back, or refused "
                "no-space — never waiting for ever");
out:
    if (under)
        (void)BinderRunWhole();
    for (; promised > 0; promised--)
        AllotmentPromiseLetGo(epoch);
    if (made && rc == 0)
        (void)tagfs_delete_file(made, INK_UNWARRANTED);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/* Whether the reserve a refusal met was measured to the block or a turn of the
 * ring was waited for (binder.h, BinderRoomMeasured): not from above with no
 * Tally waited for. */
static bool StandBookRoomMeasuredBy(void)
{
    const char* why      = NULL;
    const char* measured = BinderRoomMeasured(&why);
    return strcmp(measured, "to the block by its last Tally") == 0 ||
           strcmp(measured, "from above past its last Tally, a turn of the ring waited for") == 0;
}

/*
 * ‼ A CHANGE REFUSED ROOM ON THE RIBBON BY A MEASURE FROM ABOVE WAITS ON THE
 * TALLY THAT MAKES IT EXACT. A file is written past the newest Tally — the
 * Tallies' reserve measured from above (§16.4 п.7) — no Tally is under way,
 * and the ribbon is shut past that reserve (StandBookRoomRibbonShut). A file
 * asked for waits rather than be refused at once: at a full volume's wall a
 * measure from above refused the next_quire with 7168 free under a reserve of
 * 7424, and the GiB written the moment a GiB was deleted came 3840 blocks
 * short with no Tally waited for (§16.6 п.8). Its own wait for room writes
 * that Tally; asked again round after round, it is answered within a turn of
 * the ring — made with the next_quire the Tally gives, or refused no-space by
 * a reserve measured to the block or a turn of the ring waited for, no Tally
 * left asked for. The runner holds the cadence: no Tally is written but by the
 * wait.
 */
static TestResult TestStandBookRoomRibbonWaitsGrown(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    StandFreshModel    m;
    uint64_t           fid = 0, made = 0, promised = 0, tallies = 0;
    uint32_t           epoch = 0, waits = 0;
    int                rc    = 0;
    const char*        aside = StandBinderAside(&b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandBookRoomQuiet(), "the volume mounted afresh, the lag's stitches settled past it");
    TEST_ASSERT(StandFreshModelMake(&m, TAGFS_BLOCK_SIZE), "room for the file");
    TEST_ASSERT_EQ(tagfs_create_file("book-room-g.bin", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE, 0x6Bu), (int)TAGFS_BLOCK_SIZE,
                   "a block of it written");
    TEST_ASSERT_OK(TagFSSettle(), "and settled past the newest Tally");
    epoch = tagfs_mount_epoch();
    TEST_ASSERT(StandBookRoomRibbonShut(epoch, &promised) == TEST_PASS, "the ribbon is shut past the reserve");
    TEST_ASSERT(EndpaperGrown() && !BinderHoldsGround(),
                "the reserve is measured from above past the newest Tally, and no Tally is under way");
    tallies = g_binder.tallies;
    rc      = tagfs_create_file("book-room-h.bin", NULL, 0, &made);
    TEST_ASSERT_EQ(rc, -(int)ERR_WOULD_BLOCK,
                   "a file asked for waits on the Tally that makes the measure exact, not refused at once");
    TEST_ASSERT(BinderRoomAwaited(), "that Tally asked of its own wait for room");
    TEST_ASSERT_OK(SettleForRoom(), "its wait for room ends");
    TEST_ASSERT(g_binder.tallies > tallies, "and writes the Tally");
    for (waits = 0; rc == -(int)ERR_WOULD_BLOCK && waits <= g_binder.ring; waits++) {
        rc = tagfs_create_file("book-room-h.bin", NULL, 0, &made);
        if (rc == -(int)ERR_WOULD_BLOCK)
            TEST_ASSERT_OK(SettleForRoom(), "each wait for room ends");
    }
    TestEvidence("asks of the file past its first wait", waits);
    TEST_ASSERT(rc == 0 || (rc == -(int)ERR_TAGFS_NO_SPACE && StandBookRoomMeasuredBy()),
                "answered within a turn of the ring: made, or refused no-space by a reserve measured to the block "
                "or a turn of the ring waited for — never from above with no Tally waited for");
    TEST_ASSERT(!BinderRoomAwaited(), "and no Tally is left asked for");
out:
    for (; promised > 0; promised--)
        AllotmentPromiseLetGo(epoch);
    if (made && rc == 0)
        (void)tagfs_delete_file(made, INK_UNWARRANTED);
    if (fid)
        (void)tagfs_delete_file(fid, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/*
 * ‼ A SLOT HELD UNSURE OVER NOTHING SPENDS NO STEP OF THE BAR'S LAG. A Tally
 * whose slot write was refused over a slot that held nothing may stand there
 * or not at all, and a mount whose write there tore stands on the newest slot
 * before it: ground freed past that slot's slice and given to the ribbon would
 * be the ground of a file the Tally the mount stands on names, and a reader
 * refuses the ribbon's step onto it (RIBBON_TORN). So the bar is the least
 * slice of the newest slot before it too — the lag's steps spent by slots a
 * mount stands on whole. Memory alone (BinderBarSlice): slots laid by hand.
 */
static TestResult TestStandBookRoomBarPastUnsure(void)
{
    TestResult verdict = TEST_PASS;
    SlotGround s[3];

    memset(s, 0, sizeof(s));
    s[0] = (SlotGround){ .holds = true, .seq = 10, .tally = 10, .slice = 100 };
    s[1] = (SlotGround){ .holds = true, .unsure = true, .seq = 13, .tally = 13, .tally2 = 13, .slice = 130 };
    TEST_ASSERT_EQ(BinderBarSlice(s, 3, 1, UINT64_MAX), 100,
                   "unsure over a slot that held nothing: the bar stands at the slice of the newest slot before it");
    s[2] = (SlotGround){ .holds = true, .seq = 7, .tally = 7, .slice = 70 };
    TEST_ASSERT_EQ(BinderBarSlice(s, 3, 1, UINT64_MAX), 100, "one step of the lag, the honest medium's: no further");
    TEST_ASSERT_EQ(BinderBarSlice(s, 3, 2, UINT64_MAX), 70, "two, the medium lied: the slot before that too");
    s[1] = (SlotGround){ .holds = true, .unsure = true, .seq = 4, .tally = 4, .tally2 = 13, .slice = 40 };
    TEST_ASSERT_EQ(BinderBarSlice(s, 3, 1, UINT64_MAX), 100,
                   "unsure over a slot that held a Tally: by the older one, the newest sure slot the bar's");
    TEST_ASSERT_EQ(BinderBarSlice(s, 3, 1, 50), 50, "and the slot a mount stood on below them all");
out:
    return verdict;
}

// ----------------------------------------------------------------------------

static const TestCase g_stand_book_room_tests[] = {
    { .name = "stand_book_room_next_quire_clear", .func = TestStandBookRoomNextQuireClear,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_gather_one_run", .func = TestStandBookRoomGatherOneRun,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_defrag_gives_the_core_back", .func = TestStandBookRoomDefragGivesTheCoreBack,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_kept_file_not_gathered", .func = TestStandBookRoomKeptFileNotGathered,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_stakes_counted", .func = TestStandBookRoomStakesCounted,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_own_whole", .func = TestStandBookRoomOwnWhole,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_tally_gives_room", .func = TestStandBookRoomTallyGivesRoom,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_rest_under_tally", .func = TestStandBookRoomRestUnderTally,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_wait_bounded", .func = TestStandBookRoomWaitBounded,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_bar_waits_tally", .func = TestStandBookRoomBarWaitsTally,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_ribbon_waits_under_way", .func = TestStandBookRoomRibbonWaitsUnderWay,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_ribbon_waits_grown", .func = TestStandBookRoomRibbonWaitsGrown,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_book_room_bar_past_unsure", .func = TestStandBookRoomBarPastUnsure,
      .reach = TEST_READS_ONLY },
};

const TestSuite g_stand_book_room_suite = {
    g_stand_book_room_tests, sizeof(g_stand_book_room_tests) / sizeof(g_stand_book_room_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
