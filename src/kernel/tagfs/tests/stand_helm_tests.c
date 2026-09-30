#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../helm/helm.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../plot/plot.h"
#include "../settle/settle.h"
#include "baton.h"
#include "amp.h"
#include "atomics.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The Helm (step 1.5)
//
// One holder of a file's extents at a time, and a FIFO instead of a spin: a
// second writer of a file waits for the first to let go and then loads the
// file as the first left it; a delete takes no Helm, but its file's holder
// answers ERR_FILE_NOT_FOUND and nothing brings the file back; and what an
// open handle views outlives the delete until the handle lets go.
//
// A waiter here is the test's own: handed the Helm, it is listed for a
// service point as a program's synchronous job is (HelmReady), and the test
// runs the service itself — any core passing its own service point may run it
// first, so the test waits, bounded, for it to be done. The waiters are static:
// a waiter still in a FIFO when a test fails must not be on a stack that is
// gone by the time it is handed the Helm.
// ============================================================================

#define STAND_HELM_BOUND_MS 1000u

typedef enum {
    STAND_TURN_WRITE = 0,
    STAND_TURN_DEFRAG,
} StandTurnWork;

typedef struct {
    HelmWaiter        waiter;
    StandTurnWork     work;
    TagFSFileHandle  *handle;
    const uint8_t    *data;
    uint32_t          bytes;
    volatile uint32_t handed;
    volatile uint32_t done;
    error_t           rc;
    uint64_t          wrote;
} StandTurn;

static StandTurn  s_turn;
static HelmWaiter s_first;

static void StandTurnHanded(HelmWaiter *w)
{
    StandTurn *t = container_of(w, StandTurn, waiter);
    __atomic_store_n(&t->handed, 1u, __ATOMIC_RELEASE);
    HelmReady(w);
}

static void StandTurnRun(HelmWaiter *w)
{
    StandTurn *t = container_of(w, StandTurn, waiter);
    if (t->work == STAND_TURN_WRITE) {
        PlotSource src = { .kernel = t->data, .bytes = t->bytes };
        t->rc = HelmWrite(t->handle, &src, &t->wrote);
    } else {
        t->rc = HelmDefrag(t->handle, 0);
    }
    HelmLetGo(t->handle->ofe, w);
    __atomic_store_n(&t->done, 1u, __ATOMIC_RELEASE);
}

static void StandTurnSet(StandTurnWork work, TagFSFileHandle *handle, const uint8_t *data,
                         uint32_t bytes)
{
    memset(&s_turn, 0, sizeof(s_turn));
    s_turn.waiter.hand = StandTurnHanded;
    s_turn.waiter.run  = StandTurnRun;
    s_turn.work        = work;
    s_turn.handle      = handle;
    s_turn.data        = data;
    s_turn.bytes       = bytes;
}

/* The service point run here, then a bounded wait for the turn to be done
 * wherever it ran. */
static bool StandTurnFinished(void)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_HELM_BOUND_MS);
    uint8_t  me    = amp_get_core_index();
    for (;;) {
        HelmRunReady();
        if (__atomic_load_n(&s_turn.done, __ATOMIC_ACQUIRE))
            return true;
        if (rdtsc() >= bound)
            return false;
        BatonPump(me);
        cpu_pause();
    }
}

/* A test's way out with a turn in the FIFO: the first holder lets go, which
 * hands the turn the Helm, and the turn is run. False when it never finished —
 * its handle is then left open rather than freed under it. */
static bool StandTurnDrained(bool holding_first, TagFSFileHandle *first_handle, bool asked)
{
    if (holding_first)
        HelmLetGo(first_handle->ofe, &s_first);
    return !asked || StandTurnFinished();
}

// ----------------------------------------------------------------------------
// Two writers of one file take turns
//
// W-STALE. Two appends of one file used to load the file each on its own —
// the synchronous one under a write lock and the asynchronous one under a
// token, neither excluding the other — so the second could lay its block over
// the size the first had not yet inked, and one append was lost. Here the
// first holds the Helm and appends; the second asks for it, is queued — the
// ask returns at once, nobody spins — and runs only once the first lets go,
// loading the file the first left: its block lands after the first's.
// ----------------------------------------------------------------------------

static TestResult TestStandHelmWritersTakeTurns(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0;
    TagFSFileHandle *first   = NULL;
    TagFSFileHandle *second  = NULL;
    bool             holding = false;
    bool             asked   = false;
    const uint32_t   B       = TAGFS_BLOCK_SIZE;
    uint8_t         *made    = g_test_buffer;          /* block 0 */
    uint8_t         *by_one  = g_test_buffer + B;      /* block 1, the first writer's */
    uint8_t         *by_two  = g_test_buffer + 2 * B;  /* block 2, the second's */
    const char      *aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, 3 * B, 0x4E1D0000u, 0);
    TEST_ASSERT(StandMakeFile("test_file", made, B, &file_id), "a one-block file is made");
    first  = tagfs_open(file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    second = tagfs_open(file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    TEST_ASSERT(first && second, "two writers open it, each to append");

    memset(&s_first, 0, sizeof(s_first));
    TEST_ASSERT(HelmAsk(first->ofe, &s_first), "the first is handed the file's Helm at once");
    holding = true;

    StandTurnSet(STAND_TURN_WRITE, second, by_two, B);
    asked = true;
    TEST_ASSERT(!HelmAsk(second->ofe, &s_turn.waiter),
                "the second is queued behind the first, and its ask returns at once");
    TEST_ASSERT(!__atomic_load_n(&s_turn.handed, __ATOMIC_ACQUIRE),
                "and nothing hands it the Helm while the first holds it");

    PlotSource one   = { .kernel = by_one, .bytes = B };
    uint64_t   wrote = 0;
    TEST_ASSERT_OK(HelmWrite(first, &one, &wrote), "the first appends while it holds the Helm");
    TEST_ASSERT_EQ(wrote, B, "a whole block");
    TEST_ASSERT(!__atomic_load_n(&s_turn.handed, __ATOMIC_ACQUIRE),
                "the second still waits: a write does not hand the Helm on");

    HelmLetGo(first->ofe, &s_first);
    holding = false;
    TEST_ASSERT(__atomic_load_n(&s_turn.handed, __ATOMIC_ACQUIRE),
                "letting go hands the Helm to the one that waited");
    TEST_ASSERT(StandTurnFinished(), "and its write runs at a service point");
    TEST_ASSERT_OK(s_turn.rc, "the second's append is answered OK");
    TEST_ASSERT_EQ(s_turn.wrote, B, "a whole block");

    uint64_t began = second->offset - s_turn.wrote;
    TestEvidence("offset the waiting writer's append began at, of the 8192 the first left", began);
    TEST_ASSERT_EQ(began, 2 * B, "it loaded the file as the first left it: no append is lost");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 3 * B), 3 * B, "the file reads back whole");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, 3 * B), 3 * B,
                   "its blocks in the order their writers held the Helm");
out:
    if (!StandTurnDrained(holding, first, asked))
        second = NULL;
    tagfs_close(first);
    tagfs_close(second);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A delete under a reader keeps the reader's blocks until it lets go
//
// W-DEL / RP-N3. A delete gave a file's blocks back while a handle still had
// the file open, and a read through the handle then read whatever the block's
// next owner put there. A delete is a Quitclaim now, and the ground it frees
// rests in Fallow — and past its cycles, too, while a handle opened before it
// is open: the reader reads the bytes it opened, and the block goes back once
// the reader lets go.
// ----------------------------------------------------------------------------

static TestResult TestStandHelmDeleteKeepsReadersBlocks(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0;
    uint32_t         block   = 0;
    TagFSFileHandle *reader  = NULL;
    const char      *aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x4E1D0100u, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &block), "its block is found");
    reader = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(reader, "a reader opens it");

    TEST_ASSERT_EQ(tagfs_delete_file(file_id), 0, "the file is deleted under the reader");
    file_id = 0;
    TEST_ASSERT(HelmVacated(reader->ofe), "and its Helm says the file was given up");

    uint64_t rested = 0;
    for (uint64_t c = 0; c <= FallowLag() + 1; c++, rested++)
        TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle after the delete");
    TEST_ASSERT(StandBlockInUse(block), "the reader's block still rests past the delete's cycles");
    TestEvidence("cycles the deleted file's block rested while a reader had it open", rested);

    TEST_ASSERT_EQ(TestHandleRead(reader, g_test_output, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "the reader reads the file it opened");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "byte for byte");

    tagfs_close(reader);
    reader = NULL;
    for (uint64_t c = 0; c <= FallowLag(); c++)
        TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle after the reader let go");
    TEST_ASSERT(!StandBlockInUse(block), "and the block goes back once the reader let go");
out:
    tagfs_close(reader);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A defrag beside a delete never brings the file back
//
// CR-9. The defrag let go of the volume's state lock around its allocation and
// then wrote the record it had built without looking the file up again, so a
// delete that landed in between was undone: the file came back, over blocks
// its Quitclaim had already given away. Now a delete takes no Helm and the
// defrag holds one; the delete lands both where the defrag works and where it
// waits for the Helm, and each time the defrag answers ERR_FILE_NOT_FOUND and
// the file stays given up.
//
// Where it works: the test holds the Helm as the defrag does and walks its
// steps — the record loaded, every extent gathered into one run when there is
// more than one, the delete, then the Ink of the gathered record. Where it
// waits: a defrag queued behind a holder of the Helm, the file deleted, and
// the Helm handed on.
// ----------------------------------------------------------------------------

static bool StandFileIsGone(uint32_t file_id)
{
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (tagfs_get_metadata(file_id, &meta) == 0) {
        tagfs_metadata_free(&meta);
        return false;
    }
    TagFSFileHandle *again = tagfs_open(file_id, TAGFS_HANDLE_READ);
    tagfs_close(again);
    return again == NULL;
}

static TestResult TestStandHelmDefragBesideDelete(void)
{
    TestResult       verdict  = TEST_PASS;
    uint32_t         file_id  = 0;
    uint32_t         filler   = 0;
    uint32_t         queued   = 0;
    TagFSFileHandle *handle   = NULL;
    TagFSFileHandle *waiting  = NULL;
    bool             holding  = false;
    bool             asked    = false;
    bool             loaded   = false;
    Plot             plot;
    const uint32_t   B        = TAGFS_BLOCK_SIZE;
    const char      *aside    = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    /* Two blocks of the file with another file's between them, so a gather
     * has something to move where the allocator lays them that way. */
    TestFillAt(g_test_buffer, 3 * B, 0x4E1D0200u, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, B, &file_id), "a one-block file is made");
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer + B, B, &filler), "and a filler after it");
    TEST_ASSERT_EQ(StandWriteAt(file_id, B, g_test_buffer + 2 * B, B), (int)B,
                   "the first grows by a block");

    handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle, "the defrag's handle opens");
    memset(&s_first, 0, sizeof(s_first));
    TEST_ASSERT(HelmAsk(handle->ofe, &s_first), "and it holds the file's Helm");
    holding = true;

    TEST_ASSERT_OK(PlotLoad(&plot, file_id), "the defrag loads the record under the Helm");
    loaded = true;
    uint64_t gathered = 0;
    if (plot.run_count > 1) {
        TEST_ASSERT_OK(PlotGather(&plot, 0, 0), "and gathers every extent into one run");
        gathered = 1;
    }
    TestEvidence("gathers the defrag had made when the delete landed", gathered);

    TEST_ASSERT_EQ(tagfs_delete_file(file_id), 0,
                   "the delete lands while the defrag holds the Helm, and waits for nothing");
    InkReceipt inked;
    TEST_ASSERT_EQ(TagFSInkPlot(&plot, INK_MAKES_SPARES, &inked), ERR_FILE_NOT_FOUND,
                   "the defrag's Ink finds no file to lay its record over");
    PlotRelease(&plot);
    loaded = false;
    HelmLetGo(handle->ofe, &s_first);
    holding = false;
    tagfs_close(handle);
    handle = NULL;
    TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle after both");
    TEST_ASSERT(StandFileIsGone(file_id), "and the file stays given up");
    file_id = 0;

    /* And the delete that lands while the defrag waits for the Helm. */
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, B, &queued), "another file is made");
    handle  = tagfs_open(queued, TAGFS_HANDLE_WRITE);
    waiting = tagfs_open(queued, TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle && waiting, "a holder and a defrag open it");
    memset(&s_first, 0, sizeof(s_first));
    TEST_ASSERT(HelmAsk(handle->ofe, &s_first), "the holder holds its Helm");
    holding = true;
    StandTurnSet(STAND_TURN_DEFRAG, waiting, NULL, 0);
    asked = true;
    TEST_ASSERT(!HelmAsk(waiting->ofe, &s_turn.waiter), "the defrag waits for the Helm");
    TEST_ASSERT_EQ(tagfs_delete_file(queued), 0, "the file is deleted while the defrag waits");

    HelmLetGo(handle->ofe, &s_first);
    holding = false;
    TEST_ASSERT(StandTurnFinished(), "the defrag is handed the Helm and runs");
    TEST_ASSERT_EQ(s_turn.rc, ERR_FILE_NOT_FOUND, "and answers that the file is gone");
    TEST_ASSERT(StandFileIsGone(queued), "which it stays");
    queued = 0;
out:
    if (loaded)
        PlotRelease(&plot);
    if (!StandTurnDrained(holding, handle, asked))
        waiting = NULL;
    tagfs_close(handle);
    tagfs_close(waiting);
    if (file_id)
        tagfs_delete_file(file_id);
    if (queued)
        tagfs_delete_file(queued);
    if (filler)
        tagfs_delete_file(filler);
    return verdict;
}

static const TestCase g_stand_helm_tests[] = {
    { .name  = "stand_helm_writers_take_turns", .func = TestStandHelmWritersTakeTurns,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_helm_delete_keeps_the_readers_blocks",
      .func  = TestStandHelmDeleteKeepsReadersBlocks, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_helm_defrag_beside_delete", .func = TestStandHelmDefragBesideDelete,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_helm_suite = {
    .tests = g_stand_helm_tests, .count = ARRAY_SIZE(g_stand_helm_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
