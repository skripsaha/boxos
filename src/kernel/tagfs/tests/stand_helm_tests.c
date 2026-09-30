#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../helm/helm.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../plot/plot.h"
#include "../settle/settle.h"
#include "write_job.h"   /* WriteJobSubmit — the one automaton, asked by a caller in the kernel */
#include "block_fail.h"  /* BLOCKFAIL — a read the medium refuses in the middle of a write */
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
// open handle views outlives the delete until the handle lets go. And the
// write's one contract: inked a window at a time, the core given back between
// two; refused part way with the bytes the file holds of it and its size; the
// record's room claimed before a byte moves, so a tag is refused and not the
// data; every waiter answered when the medium leaves; and a file the loader
// reads by its blocks refused by the volume's one guard.
//
// A waiter here is the test's own: handed the Helm, it is listed for a
// service point as a program's synchronous job is (HelmReady), and the test
// runs the service itself — any core passing its own service point may run it
// first, so the test waits, bounded, for it to be done. The waiters are static:
// a waiter still in a FIFO when a test fails must not be on a stack that is
// gone by the time it is handed the Helm.
// ============================================================================

#define STAND_HELM_BOUND_MS 1000u
#define STAND_HELM_WINDOWS_BOUND_MS 10000u   /* a write of four windows, one at a time */

typedef enum {
    STAND_TURN_WRITE = 0,
    STAND_TURN_DEFRAG,
    STAND_TURN_WINDOWS,     /* a write a window a turn, listed again between two */
} StandTurnWork;

typedef struct {
    HelmWaiter        waiter;
    StandTurnWork     work;
    TagFSFileHandle  *handle;
    const uint8_t    *data;
    uint32_t          bytes;
    volatile uint32_t handed;
    volatile uint32_t done;
    volatile uint32_t gone;     /* answered that the medium left */
    uint64_t          lost;
    uint32_t          turns;    /* times its work was run at a service point */
    error_t           rc;
    uint64_t          wrote;
} StandTurn;

static StandTurn  s_turn;
static StandTurn  s_later;    /* a second waiter, behind s_turn */
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
    PlotSource src = { .kernel = t->data, .bytes = t->bytes };
    t->turns++;
    if (t->work == STAND_TURN_WRITE) {
        t->rc = HelmWrite(t->handle, &src, &t->wrote);
    } else if (t->work == STAND_TURN_WINDOWS) {
        t->rc = HelmWriteWindow(t->handle, &src, &t->wrote);
        if (t->rc == OK && t->wrote < t->bytes) {
            HelmReady(w);   /* the core given back: the next window at the next service point */
            return;
        }
    } else {
        t->rc = HelmDefrag(t->handle, 0);
    }
    HelmLetGo(t->handle->ofe, w);
    __atomic_store_n(&t->done, 1u, __ATOMIC_RELEASE);
}

static void StandTurnGone(HelmWaiter *w, uint64_t lost)
{
    StandTurn *t = container_of(w, StandTurn, waiter);
    t->lost = lost;
    __atomic_add_fetch(&t->gone, 1u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&t->done, 1u, __ATOMIC_RELEASE);
}

static void StandTurnSetOn(StandTurn *t, StandTurnWork work, TagFSFileHandle *handle,
                           const uint8_t *data, uint32_t bytes)
{
    memset(t, 0, sizeof(*t));
    t->waiter.hand = StandTurnHanded;
    t->waiter.run  = StandTurnRun;
    t->waiter.gone = StandTurnGone;
    t->work        = work;
    t->handle      = handle;
    t->data        = data;
    t->bytes       = bytes;
}

static void StandTurnSet(StandTurnWork work, TagFSFileHandle *handle, const uint8_t *data,
                         uint32_t bytes)
{
    StandTurnSetOn(&s_turn, work, handle, data, bytes);
}

/* The service point run here, then a bounded wait for the turn to be done
 * wherever it ran. */
static bool StandTurnDone(const StandTurn *t, uint32_t bound_ms)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(bound_ms);
    uint8_t  me    = amp_get_core_index();
    for (;;) {
        HelmRunReady();
        if (__atomic_load_n(&t->done, __ATOMIC_ACQUIRE))
            return true;
        if (rdtsc() >= bound)
            return false;
        BatonPump(me);
        cpu_pause();
    }
}

static bool StandTurnFinished(void)
{
    return StandTurnDone(&s_turn, STAND_HELM_BOUND_MS);
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

// ----------------------------------------------------------------------------
// A write is inked a window at a time, and gives its core back between two
//
// RT-16. A write held its core for its whole length and inked once at its
// end, so a large one stood over the K-Core for as long as the medium took,
// and nothing it wrote was the file's until all of it was. Now each window —
// the run the medium takes in one command — is loaded, reserved, written and
// inked on its own, and the holder lists itself for the next service point
// between two while it keeps the Helm: a write of four windows is four turns,
// four Inks, and one file.
// ----------------------------------------------------------------------------

static TestResult TestStandHelmWriteYieldsBetweenWindows(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0;
    TagFSFileHandle *writer  = NULL;
    uint8_t         *data    = NULL;
    uint8_t         *back    = NULL;
    bool             asked   = false;
    const uint32_t   B       = TAGFS_BLOCK_SIZE;
    const uint64_t   W       = HelmWindowBytes(0);
    const uint32_t   bytes   = (uint32_t)(3 * W + B / 2);
    const char      *aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    data = kmalloc(B + bytes);
    back = kmalloc(B + bytes);
    TEST_ASSERT(data && back, "room for the file's bytes and for reading them back");
    TestFillAt(data, B + bytes, 0x4E1D0300u, 0);
    TEST_ASSERT(StandMakeFile("test_file", data, B, &file_id), "a one-block file is made");
    writer = tagfs_open(file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    TEST_ASSERT(writer, "a writer opens it to append");

    uint64_t inked = InkedGen();
    StandTurnSetOn(&s_turn, STAND_TURN_WINDOWS, writer, data + B, bytes);
    asked = true;
    TEST_ASSERT(HelmAsk(writer->ofe, &s_turn.waiter), "the writer holds the file's Helm");
    HelmReady(&s_turn.waiter);
    TEST_ASSERT(StandTurnDone(&s_turn, STAND_HELM_WINDOWS_BOUND_MS),
                "its write runs to its end at service points");
    asked = false;
    TEST_ASSERT_OK(s_turn.rc, "and is answered OK");
    TEST_ASSERT_EQ(s_turn.wrote, bytes, "with every byte the file's");

    TestEvidence("turns a write of three windows and a half took at the service points",
                 s_turn.turns);
    TEST_ASSERT_EQ(s_turn.turns, 4, "one turn a window: the core was given back between two");
    TEST_ASSERT(InkedGen() - inked >= 4, "and one Ink a window, each its own record");
    TEST_ASSERT_EQ(TestFileRead(file_id, back, B + bytes), (int)(B + bytes),
                   "the file reads back whole");
    TEST_ASSERT_EQ(TestFirstDifference(back, data, B + bytes), B + bytes,
                   "byte for byte, the windows where they belong");
out:
    if (asked && !StandTurnDone(&s_turn, STAND_HELM_WINDOWS_BOUND_MS))
        writer = NULL;
    tagfs_close(writer);
    if (file_id)
        tagfs_delete_file(file_id);
    kfree(data);
    kfree(back);
    return verdict;
}

// ----------------------------------------------------------------------------
// A write refused part way answers what the file holds of it, and its size
//
// W-ERR. A write the medium stopped part way answered a refusal and nothing
// else, or a short count and no refusal, depending on the road — and a
// program told "refused" writes again what already landed. Now every answer
// carries the one contract: the reason, the bytes of the write the file's
// inked record holds, and the size the file is. Here the read of a block the
// second window overlays is refused: the first window and the whole block
// before the refused one are the file's, the rest of it is as it was.
// ----------------------------------------------------------------------------

static TestResult TestStandHelmWriteRefusedPartWay(void)
{
    TestResult     verdict = TEST_PASS;
    uint32_t       file_id = 0;
    uint32_t       block   = 0;
    uint8_t       *old     = NULL;
    uint8_t       *data    = NULL;
    uint8_t       *back    = NULL;
    bool           armed   = false;
    const uint32_t B       = TAGFS_BLOCK_SIZE;
    const uint64_t W       = HelmWindowBytes(0);
    const uint32_t made    = (uint32_t)(W + B + 100);   /* its last block holds 100 bytes */
    const uint32_t bytes   = (uint32_t)(W + B + 200);   /* ... and the write lays 200 over it */
    uint64_t       stats[2] = { UINT64_MAX, UINT64_MAX };
    const char    *aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    old  = kmalloc(made);
    data = kmalloc(bytes);
    back = kmalloc(made);
    TEST_ASSERT(old && data && back, "room for the file, the write and the read back");
    TestFillAt(old, made, 0x4E1D0400u, 0);
    TestFillAt(data, bytes, 0x4E1D0401u, 0);
    TEST_ASSERT(StandMakeFile("test_file", old, made, &file_id),
                "a file of a window, a block and 100 bytes is made");
    TEST_ASSERT(StandBlockOf(file_id, (uint32_t)(W / B) + 1, &block),
                "the block the second window lays 200 bytes over is found");

    Crate out = { .addr = (uint64_t)(uintptr_t)stats, .size = 0, .capacity = sizeof(stats) };
    WriteJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    ask.kind          = WRITE_JOB_WRITE;
    ask.file_id       = file_id;
    ask.source.kernel = data;
    ask.source.bytes  = bytes;
    ask.offset        = 0;
    ask.out_crate     = &out;

    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_READ, tagfs_block_to_sector(block),
                 TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    int answer = WriteJobSubmit(&ask, NULL, NULL);
    bool fired = BlockFailFired();
    BlockFailDisarm();
    armed = false;

    TEST_ASSERT(fired, "the read of that block was the one refused");
    TEST_ASSERT(answer != OK, "the write answers the refusal");
    TEST_ASSERT_EQ(out.size, sizeof(stats), "and fills its out crate on the refusal too");
    TestEvidence("bytes the refused write answered it left in the file", stats[0]);
    TEST_ASSERT_EQ(stats[0], W + B, "the first window and the block before the refused one");
    TEST_ASSERT_EQ(stats[1], made, "and the size the file is, which the write did not reach");

    TEST_ASSERT_EQ(TestFileRead(file_id, back, made), (int)made, "the file reads back");
    TEST_ASSERT_EQ(TestFirstDifference(back, data, (uint32_t)(W + B)), (uint32_t)(W + B),
                   "holding the bytes answered as written");
    TEST_ASSERT_EQ(TestFirstDifference(back + W + B, old + W + B, made - (uint32_t)(W + B)),
                   made - (uint32_t)(W + B), "and what it held past them, untouched");
out:
    if (armed)
        BlockFailDisarm();
    if (file_id)
        tagfs_delete_file(file_id);
    kfree(old);
    kfree(data);
    kfree(back);
    return verdict;
}

// ----------------------------------------------------------------------------
// The record's room is claimed before a byte moves: a tag is refused, not the
// data
//
// RT-19. A write checked that its record would fit, wrote its blocks, and only
// then inked — while a rename or a tag inked meanwhile could make the record
// wider, so the write's Ink was refused ERR_TAGFS_RECORD_FULL with its data
// already on the medium, and lost. Now the holder of the Helm claims the room
// its extents need when it reserves them, and a name or a tag that would take
// that room is refused itself. Here a file grows by a block laid apart from
// it — its record then names two extents — and a name that leaves room for one
// is refused while the write is in flight; the write's Ink then succeeds.
// ----------------------------------------------------------------------------

static TestResult TestStandHelmRecordRoomClaimed(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0;
    uint32_t         filler  = 0;
    TagFSFileHandle *handle  = NULL;
    bool             holding = false;
    bool             loaded  = false;
    char            *name    = NULL;
    Plot             plot;
    const uint32_t   B       = TAGFS_BLOCK_SIZE;
    const char      *aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, 3 * B, 0x4E1D0500u, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, B, &file_id), "a one-block file is made");
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer + B, B, &filler), "and a filler after it");

    handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    TEST_ASSERT(handle, "the writer's handle opens");
    memset(&s_first, 0, sizeof(s_first));
    TEST_ASSERT(HelmAsk(handle->ofe, &s_first), "and it holds the file's Helm");
    holding = true;
    TEST_ASSERT_OK(PlotLoad(&plot, file_id), "the write loads the record under the Helm");
    loaded    = true;
    plot.helm = &handle->ofe->helm;

    uint64_t at = 0;
    TEST_ASSERT_OK(PlotBeginWrite(&plot, plot.size, false, B, &at),
                   "and reserves a block to grow by, claiming the record's room");
    if (plot.run_count <= plot.runs_before) {
        verdict = test_steps_aside("the volume laid the block next to the file's, so its record "
                                   "names no more extents and nothing is claimed");
        goto out;
    }
    TestEvidence("extents the write in flight claimed room for", plot.claimed);
    TEST_ASSERT_EQ(HelmClaimedRuns(file_id), plot.run_count, "the claim is its Helm's");

    /* A name with no stem wears no tag, so the record is its name and its
     * extents: this one leaves room for exactly the extents it names now. */
    uint32_t name_bytes = TAGFS_MPOOL_DATA_SIZE - RECORD_HEADER_SIZE -
                          plot.runs_before * (uint32_t)sizeof(FileExtent);
    name = kmalloc(name_bytes + 1);
    TEST_ASSERT(name, "room for the long name");
    memset(name, 'q', name_bytes);
    name[0]          = '.';
    name[name_bytes] = '\0';
    TagFSMetadata now = { .filename = name, .extent_count = plot.runs_before };
    TEST_ASSERT(PlotFits(&now, NULL), "the name fits beside the extents the record names now");

    TEST_ASSERT_EQ(tagfs_rename_file(file_id, name), -(int)ERR_TAGFS_RECORD_FULL,
                   "and is refused: it would take the room the write in flight claimed");

    PlotSource src  = { .kernel = g_test_buffer + 2 * B, .bytes = B };
    uint64_t   done = 0;
    TEST_ASSERT_OK(PlotWriteBlocks(&plot, at, &src, &done), "the write's block lands");
    TEST_ASSERT_OK(PlotEndWrite(&plot, at, done), "and is made ready to be the file's");
    InkReceipt inked;
    TEST_ASSERT_OK(TagFSInkPlot(&plot, INK_MAKES_SPARES, &inked),
                   "the write's Ink is not refused: the room was its own");
    TEST_ASSERT_EQ(HelmClaimedRuns(file_id), 0, "and once inked nothing is claimed");
    PlotRelease(&plot);
    loaded = false;
    HelmLetGo(handle->ofe, &s_first);
    holding = false;

    memcpy(g_test_buffer + B, g_test_buffer + 2 * B, B);
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 2 * B), (int)(2 * B),
                   "the file reads back both blocks");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, 2 * B), 2 * B,
                   "the grown one among them");
out:
    if (loaded)
        PlotRelease(&plot);
    if (holding)
        HelmLetGo(handle->ofe, &s_first);
    tagfs_close(handle);
    kfree(name);
    if (file_id)
        tagfs_delete_file(file_id);
    if (filler)
        tagfs_delete_file(filler);
    return verdict;
}

// ----------------------------------------------------------------------------
// The medium leaves: every waiter in a Helm's FIFO is answered
//
// RT-13. A job waiting for a file's Helm held the volume's door and waited for
// a holder whose medium had gone: nothing answered it, and the volume could
// not be let go of while it stood inside. Now the departure answers every
// waiter at the next service point, with the count of changes inked since the
// last settled cycle — and none of them is ever handed the Helm. The test
// says the departure to the Helm alone (HelmMediumLeft): the medium stays, and
// nothing else of the volume is told.
// ----------------------------------------------------------------------------

static TestResult TestStandHelmMediumLeftAnswersWaiters(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0;
    TagFSFileHandle *holder  = NULL;
    TagFSFileHandle *one     = NULL;
    TagFSFileHandle *two     = NULL;
    bool             holding = false;
    bool             asked   = false;
    const uint32_t   B       = TAGFS_BLOCK_SIZE;
    const char      *aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, 2 * B, 0x4E1D0600u, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, B, &file_id), "a one-block file is made");
    holder = tagfs_open(file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    one    = tagfs_open(file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    two    = tagfs_open(file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    TEST_ASSERT(holder && one && two, "a holder and two waiters open it");

    uint64_t owed = HelmOwed();
    memset(&s_first, 0, sizeof(s_first));
    TEST_ASSERT(HelmAsk(holder->ofe, &s_first), "the holder holds the file's Helm");
    holding = true;
    StandTurnSetOn(&s_turn, STAND_TURN_WRITE, one, g_test_buffer + B, B);
    StandTurnSetOn(&s_later, STAND_TURN_WRITE, two, g_test_buffer + B, B);
    asked = true;
    TEST_ASSERT(!HelmAsk(one->ofe, &s_turn.waiter), "the first waiter is queued");
    TEST_ASSERT(!HelmAsk(two->ofe, &s_later.waiter), "and the second behind it");

    PlotSource grow  = { .kernel = g_test_buffer + B, .bytes = B };
    uint64_t   wrote = 0;
    TEST_ASSERT_OK(HelmWrite(holder, &grow, &wrote), "the holder appends — a change no cycle settled");
    uint64_t unsettled = InkedGen() - SettledGen();

    HelmMediumLeft();
    TEST_ASSERT(StandTurnDone(&s_turn, STAND_HELM_BOUND_MS), "the first waiter is answered");
    TEST_ASSERT(StandTurnDone(&s_later, STAND_HELM_BOUND_MS), "and so is the second");
    asked = false;
    TEST_ASSERT_EQ(s_turn.gone + s_later.gone, 2, "each once, that the medium left");
    TEST_ASSERT(!s_turn.handed && !s_later.handed, "and neither was handed the Helm");
    TestEvidence("changes the answer said left with the medium", s_turn.lost);
    TEST_ASSERT(s_turn.lost >= unsettled && unsettled > 0,
                "with the changes no cycle had settled, the holder's among them");

    HelmLetGo(holder->ofe, &s_first);
    holding = false;
    TEST_ASSERT(!s_turn.handed && !s_later.handed, "the holder lets go and hands nobody the Helm");
    TEST_ASSERT_EQ(HelmOwed(), owed, "and nothing is owed on the file any more");
out:
    if (holding)
        HelmLetGo(holder->ofe, &s_first);
    if (asked && (!StandTurnDone(&s_turn, STAND_HELM_BOUND_MS) ||
                  !StandTurnDone(&s_later, STAND_HELM_BOUND_MS))) {
        one = NULL;
        two = NULL;
    }
    tagfs_close(holder);
    tagfs_close(one);
    tagfs_close(two);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A file the loader reads by its blocks is refused by the volume's one guard
//
// The storage deck used to ask, at its own entry, whether a file wore system
// or boot — and so did the plot's gather, and the Ink, each its own way. The
// guard is the volume's now, in one place (InkMayChange): a program's write
// into such a file is refused when its job loads the file, before a byte
// moves, and so is a span. A file of the volume that wears system is written
// with the bytes it already holds, so nothing could change even if the guard
// let it through — but nothing reaches the medium at all.
// ----------------------------------------------------------------------------

static TestResult TestStandHelmGuardedFileRefused(void)
{
    TestResult  verdict  = TEST_PASS;
    uint32_t    file_id  = 0;
    uint32_t    block    = 0;
    bool        armed    = false;
    TagSpelling system   = TagSpellingOfKey("system");
    uint64_t    stats[2] = { UINT64_MAX, UINT64_MAX };
    TagFSPage   page;
    const char *aside    = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_OK(TagFSQueryPage(&system, 1, 1, &file_id, 1, false, &page),
                   "the volume is asked for a file that wears system");
    if (page.delivered == 0)
        return test_steps_aside("no file of this volume wears system");

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    TEST_ASSERT_EQ(tagfs_get_metadata(file_id, &meta), 0, "its record is read");
    uint64_t size = meta.size;
    tagfs_metadata_free(&meta);
    TEST_ASSERT(size > 0 && StandBlockOf(file_id, 0, &block), "and its first block is found");
    uint32_t bytes = size < TAGFS_BLOCK_SIZE ? (uint32_t)size : TAGFS_BLOCK_SIZE;
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, bytes), (int)bytes,
                   "the bytes it begins with are read");

    Crate out = { .addr = (uint64_t)(uintptr_t)stats, .size = 0, .capacity = sizeof(stats) };
    WriteJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    ask.kind          = WRITE_JOB_WRITE;
    ask.file_id       = file_id;
    ask.source.kernel = g_test_output;
    ask.source.bytes  = bytes;
    ask.offset        = 0;
    ask.out_crate     = &out;

    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_WRITE, tagfs_block_to_sector(block),
                 TAGFS_BLOCK_SECTORS, BLOCK_FAIL_NEVER);
    armed = true;
    int answer = WriteJobSubmit(&ask, NULL, NULL);
    uint32_t seen = BlockFailSeen();
    BlockFailDisarm();
    armed = false;

    TEST_ASSERT_EQ(answer, ERR_PERMISSION_DENIED, "a write into it is refused");
    TEST_ASSERT_EQ(stats[0], 0, "answering that none of it is the file's");
    TEST_ASSERT_EQ(stats[1], size, "and the size the file still is");
    TestEvidence("medium writes the refused write issued to the file's first block", seen);
    TEST_ASSERT_EQ(seen, 0, "refused before a byte moved: no write reached the file's block");
    TEST_ASSERT_EQ(tagfs_span_file(file_id, size), ERR_PERMISSION_DENIED,
                   "and a span of it is refused by the same guard");
out:
    if (armed)
        BlockFailDisarm();
    return verdict;
}

static const TestCase g_stand_helm_tests[] = {
    { .name  = "stand_helm_writers_take_turns", .func = TestStandHelmWritersTakeTurns,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_helm_delete_keeps_the_readers_blocks",
      .func  = TestStandHelmDeleteKeepsReadersBlocks, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_helm_defrag_beside_delete", .func = TestStandHelmDefragBesideDelete,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_helm_write_yields_between_windows",
      .func  = TestStandHelmWriteYieldsBetweenWindows, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_helm_write_refused_part_way", .func = TestStandHelmWriteRefusedPartWay,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_helm_record_room_claimed", .func = TestStandHelmRecordRoomClaimed,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_helm_medium_left_answers_waiters",
      .func  = TestStandHelmMediumLeftAnswersWaiters, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_helm_guarded_file_refused", .func = TestStandHelmGuardedFileRefused,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_helm_suite = {
    .tests = g_stand_helm_tests, .count = ARRAY_SIZE(g_stand_helm_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
