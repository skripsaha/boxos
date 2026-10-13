#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../ink/fallow.h"
#include "../metadata_pool/meta_pool.h"
#include "../tag_registry/tag_registry.h"
#include "../settle/settle.h"
#include "write_job.h"      /* WriteJobSubmit, and the stand's own answer record */
#include "settle_ops.h"     /* the deck's wait for room, and the stand's answer record for it */
#include "chit.h"           /* ChitAnswer — the border every answer passes */
#include "op_registry.h"    /* the storage deck's create, asked the way a program asks it */
#include "boxos_decks.h"
#include "amp.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// Room a cycle gives back is waited for, never answered (R6)
//
// A volume short of ground while Fallow rests blocks answers the allocator
// ERR_WOULD_BLOCK: the room is on its way, and a Settle cycle brings it. That
// word is a handler's to the guide, and a program's boxlib drops a Result that
// carries it — so an operation that met it must wait for the cycle's end and
// be done again, and never hand the word on as its answer. These tests take
// every free block of the volume, give a file up so its ground rests in
// Fallow, and ask the roads that met the word as an answer: a write through
// the port that needs a Spare, a create that grows the tag registry, and a
// write on the core whose own cycle stands between two of its steps. Each is
// answered, and answered OK, once the ground came back.
//
// The runner holds the cadence, so the test stands in for it: whenever a
// waiter of the Settle is owed, it runs the cycle itself, and it turns the
// machine a job continues in (StandPortTurn) between two.
// ============================================================================

/* Files with nothing in them, more than a tail holds Quitclaims of: given up
 * one by one, they leave the pool's tail no room for one more record. */
#define STAND_ROOM_EMPTIES  (POOL_QUITCLAIMS_PER_BLOCK + 3u)

/* The test's own bound on an answer: a job that never answers fails here
 * rather than stopping the machine. */
#define STAND_ROOM_BOUND_MS 20000u

static uint64_t            s_room_empties[STAND_ROOM_EMPTIES];
static WriteJobStandAnswer s_room_answer;
static uint8_t             s_room_op[sizeof(ManifestOp) + 40] __attribute__((aligned(8)));
static Crate               s_room_crate;
static uint64_t            s_room_fid;
static char                s_room_text[TAG_TEXT_WIDTH + 1];

static uint32_t StandRoomTail(void)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    uint32_t room = PoolTailRoom();
    TagFSLockDrop(&fs->lock);
    return room;
}

static uint64_t StandRoomResting(void)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    uint64_t resting = FallowBlocks();
    TagFSLockDrop(&fs->lock);
    return resting;
}

/* One block of the file's at `offset`, written through a fresh handle as a
 * caller in the kernel writes it: tagfs_write's answer itself. */
static error_t StandRoomWrite(uint64_t file_id, uint64_t offset)
{
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    if (!handle)
        return ERR_FILE_NOT_FOUND;
    PlotSource src     = { .kernel = g_test_buffer, .bytes = TAGFS_BLOCK_SIZE };
    uint64_t   written = 0;
    handle->offset     = offset;
    error_t rc         = tagfs_write(handle, &src, &written);
    tagfs_close(handle);
    return rc;
}

/* A write of one block of the file's, as a program's is made — a WriteJob,
 * the road its seat takes — answered to the stand's record. */
static bool StandRoomSubmit(uint64_t file_id, uint64_t offset, bool append)
{
    WriteJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    memset(&s_room_answer, 0, sizeof(s_room_answer));
    ask.kind          = WRITE_JOB_WRITE;
    ask.file_id       = file_id;
    ask.source.kernel = g_test_buffer;
    ask.source.bytes  = TAGFS_BLOCK_SIZE;
    ask.offset        = offset;
    ask.append        = append;
    ask.stand         = &s_room_answer;
    int submit = WriteJobSubmit(&ask, NULL, NULL);
    return submit == ERR_WOULD_BLOCK || __atomic_load_n(&s_room_answer.answered, __ATOMIC_ACQUIRE);
}

/* The machine turned — the seat's answers, the batons, the service point —
 * until what was asked waits for a cycle's end as a waiter of the Settle, or
 * answers: true when it waits, unanswered. No cycle is run here. */
static bool StandRoomWaits(uint8_t seat, const volatile uint32_t* answered)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_ROOM_BOUND_MS);
    while (!__atomic_load_n(answered, __ATOMIC_ACQUIRE) && !SettleWaitersOwed()) {
        if (rdtsc() >= bound)
            return false;
        StandPortTurn(seat);
        cpu_pause();
    }
    return !__atomic_load_n(answered, __ATOMIC_ACQUIRE);
}

/* The machine turned, and the cycle every waiter of the Settle is owed run,
 * until what was asked answers: false when the bound ran out first. */
static bool StandRoomAnswered(uint8_t seat, const volatile uint32_t* answered)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_ROOM_BOUND_MS);
    while (!__atomic_load_n(answered, __ATOMIC_ACQUIRE)) {
        if (rdtsc() >= bound)
            return false;
        StandPortTurn(seat);
        if (SettleWaitersOwed())
            (void)TagFSSettle();
        cpu_pause();
    }
    return true;
}

static bool StandRoomMakeEmpties(void)
{
    for (uint32_t i = 0; i < STAND_ROOM_EMPTIES; i++) {
        s_room_empties[i] = 0;
        if (tagfs_create_file("room_empty", NULL, 0, &s_room_empties[i]) != 0)
            return false;
    }
    return true;
}

static void StandRoomLetGo(uint64_t* file_id)
{
    if (*file_id)
        (void)tagfs_delete_file(*file_id, INK_UNWARRANTED);
    *file_id = 0;
}

static void StandRoomLetGoEmpties(void)
{
    for (uint32_t i = 0; i < STAND_ROOM_EMPTIES; i++)
        StandRoomLetGo(&s_room_empties[i]);
}

/* Every free block of the volume taken, and then `ground`'s file given up:
 * its block rests in Fallow, and the volume is short with room on its way.
 * The take ends on the allocator's refusal — ERR_WOULD_BLOCK as well, when the
 * pool's chain keeps slack Glean may take off. */
static error_t StandRoomShort(StandInkGround* g, uint64_t* ground)
{
    error_t rc = StandInkTakeAll(g);
    if (rc != ERR_TAGFS_NO_SPACE && rc != ERR_WOULD_BLOCK)
        return rc == OK ? ERR_INVALID_STATE : rc;
    rc = tagfs_delete_file(*ground, INK_UNWARRANTED) == 0 ? OK : ERR_INVALID_STATE;
    *ground = 0;
    return rc == OK && StandRoomResting() == 0 ? ERR_INVALID_STATE : rc;
}

// ----------------------------------------------------------------------------
// A write through the port that needs a Spare
//
// Z07-1. The pool's tail is left with no room for one more record, and the
// reserve is the deletes' alone: an asynchronous write's Ink asks for a Spare
// and the job stamps one through the port (W_SPARE) — but the volume is short
// while Fallow rests a block, and the allocator answers ERR_WOULD_BLOCK. The
// write overwrites a block the file has, so its own ground was never the
// question. Red before: the job ended on the allocator's answer, and the
// program was told ERR_WOULD_BLOCK, which its boxlib drops.
// ----------------------------------------------------------------------------

static TestResult TestStandRoomSpareThroughThePort(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        seat    = tagfs_get_seat();
    StandInkGround ground  = { NULL, NULL, 0, 0 };
    uint64_t       file_id = 0;
    uint64_t       rests = 0;
    bool           asked   = false;
    const char*    aside   = StandPortAside(seat);

    if (aside)
        return test_steps_aside(aside);

    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x71u);
    TEST_ASSERT(StandMakeFile("room_spare", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "the file the write overwrites is made");
    TEST_ASSERT(StandMakeFile("room_ground", g_test_buffer, TAGFS_BLOCK_SIZE, &rests),
                "a file whose ground the volume will wait for is made");
    TEST_ASSERT(StandRoomMakeEmpties(), "files with nothing in them, more than a tail holds, are made");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle with a waiter settles them, closing the tail their "
                                  "records lie in");
    TEST_ASSERT_OK(StandRoomShort(&ground, &rests),
                   "every free block is taken, and the file with ground is given up: it rests");

    for (uint32_t i = 0; i < STAND_ROOM_EMPTIES && StandRoomTail() >= RECORD_HEADER_SIZE; i++)
        StandRoomLetGo(&s_room_empties[i]);
    TEST_ASSERT(StandRoomTail() < RECORD_HEADER_SIZE,
                "the empty files given up leave the tail no room for one more record");

    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x72u);
    uint64_t cycles = SettledCycles();
    asked = StandRoomSubmit(file_id, 0, false);
    TEST_ASSERT(asked, "the overwrite is taken, its answer to come");
    TEST_ASSERT(StandRoomWaits(seat, &s_room_answer.answered),
                "it waits for the next cycle's end as a waiter of the Settle, unanswered");
    TEST_ASSERT_EQ(SettledCycles(), cycles, "and no cycle ran inside anybody's call for it");
    TEST_ASSERT(StandRoomAnswered(seat, &s_room_answer.answered), "the cycle's end answers it");
    asked = false;
    TestEvidence("the overwrite's answer, of OK", s_room_answer.rc);
    TEST_ASSERT_OK(s_room_answer.rc, "OK, once a cycle brought the room its record needed");
    TEST_ASSERT_EQ(s_room_answer.written, TAGFS_BLOCK_SIZE, "with every byte of it the file's");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, TAGFS_BLOCK_SIZE), (int)TAGFS_BLOCK_SIZE,
                   "the file reads back");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "holding what the overwrite wrote");
out:
    if (asked && !StandRoomAnswered(seat, &s_room_answer.answered))
        file_id = 0;      /* the job still holds the file's Helm, and takes its bytes from here */
    StandInkGiveBack(&ground);
    StandRoomLetGo(&file_id);
    StandRoomLetGo(&rests);
    StandRoomLetGoEmpties();
    (void)TagFSSettle();
    return verdict;
}

// ----------------------------------------------------------------------------
// A create that grows the tag registry
//
// F01-1. The registry's last block on the medium is filled to its brim, so the
// tag of a new name lengthens its chain by a block — which the allocator,
// short while Fallow rests one, answers ERR_WOULD_BLOCK. The storage deck's
// create is asked from the kernel, where nobody is parked: it waits for the
// cycles that give the ground back in its own call and is answered OK. Red
// before: the create answered ERR_WOULD_BLOCK.
// ----------------------------------------------------------------------------

/* The registry's last block on the medium filled to its brim with one tag of
 * the stand's — none at all when what is left could not take the record of a
 * new name `stem` either. True when the brim is reached. */
static bool StandRoomRegistryBrim(const char* stem)
{
    TagFSState*             fs    = tagfs_get_state();
    const TagRegistryBlock* tail  = (const TagRegistryBlock*)g_test_output;
    StandChain              chain = { 0 };

    if (tag_registry_flush(fs->registry) != OK ||
        !StandChainOnMedium(fs->layout.tag_registry_block, TAGFS_REGISTRY_MAGIC, &chain) ||
        chain.broken || TagFSMetaBlockRead(chain.last, g_test_output) != OK ||
        tail->used_bytes > TAGFS_REGISTRY_DATA_SIZE)
        return false;

    /* "stand_room:<k>-rrr…", k the next file id — no run before this one
     * spelled it — padded so its record takes exactly what is left. */
    uint32_t room  = TAGFS_REGISTRY_DATA_SIZE - tail->used_bytes;
    uint32_t head  = (uint32_t)ksnprintf(s_room_text, sizeof(s_room_text), "stand_room:%u-",
                                         (unsigned)fs->ledger.next_file_id);
    uint32_t key   = (uint32_t)strlen("stand_room");
    uint32_t least = TAGFS_REGISTRY_RECORD_HEAD + key + (head - key - 1u);
    if (room >= least) {
        memset(s_room_text + head, 'r', room - least);
        s_room_text[head + room - least] = '\0';
        uint32_t id;
        if (tagfs_tag_intern(s_room_text, &id) != OK || tag_registry_flush(fs->registry) != OK ||
            TagFSMetaBlockRead(chain.last, g_test_output) != OK)
            return false;
        room = TAGFS_REGISTRY_DATA_SIZE - tail->used_bytes;
    }
    return room < TAGFS_REGISTRY_RECORD_HEAD + strlen(stem);
}

/* The storage deck's create of `name`, everywhere, asked from the kernel with
 * no program: the handler's answer, and the new file's id in s_room_fid. */
static int StandRoomCreate(const char* name)
{
    const OpRegistration* reg = OpRegistryLookup(OP_KIND(DECK_STORAGE, STORAGE_OBJ_CREATE));
    ManifestOp*           op  = (ManifestOp*)s_room_op;
    size_t                len = strlen(name);

    if (!reg || len > 32)
        return ERR_NOT_INITIALIZED;
    memset(s_room_op, 0, sizeof(s_room_op));
    op->op_kind    = OP_KIND(DECK_STORAGE, STORAGE_OBJ_CREATE);
    op->param_size = 33;
    op->in_crate   = CRATE_INDEX_NONE;
    op->out_crate  = 0;
    memcpy(op->params, name, len);
    op->params[32] = STORAGE_SCOPE_EVERYWHERE;
    s_room_fid     = 0;
    s_room_crate   = (Crate){ .magic = CRATE_MAGIC, .addr = (uint64_t)(uintptr_t)&s_room_fid,
                              .capacity = sizeof(s_room_fid) };
    return reg->handler(op, &s_room_crate, 1, NULL);
}

static TestResult TestStandRoomCreateGrowsTheRegistry(void)
{
    TestResult     verdict = TEST_PASS;
    StandInkGround ground  = { NULL, NULL, 0, 0 };
    uint64_t       rests   = 0;
    char           name[32];
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    s_room_fid = 0;
    ksnprintf(name, sizeof(name), "room_grows_%u", (unsigned)tagfs_get_state()->ledger.next_file_id);
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x73u);
    TEST_ASSERT(StandMakeFile("room_ground", g_test_buffer, TAGFS_BLOCK_SIZE, &rests),
                "a file whose ground the volume will wait for is made");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle with a waiter settles it");
    TEST_ASSERT(StandRoomRegistryBrim(name),
                "the registry's last block on the medium has no room for a new name's tag");
    TEST_ASSERT_OK(StandRoomShort(&ground, &rests),
                   "every free block is taken, and the file with ground is given up: it rests");

    int made = StandRoomCreate(name);
    TestEvidence("the create's answer, of OK", (uint64_t)made);
    TEST_ASSERT_OK(made, "a create whose name grows the registry is answered OK, once the "
                         "cycles it waited for in its call gave the ground back");
    TEST_ASSERT(s_room_fid != 0, "and says the new file's id");
    TEST_ASSERT(StandRegistryHoldsOnMedium(name, NULL),
                "its name's tag is on the medium, in the block the registry grew by");
out:
    StandInkGiveBack(&ground);
    StandRoomLetGo(&s_room_fid);
    StandRoomLetGo(&rests);
    (void)TagFSSettle();
    return verdict;
}

/*
 * F01-1, RT-17. The same create, asked as a program asks it: the stand brings
 * the place its answer is told to (SettleOpStandAnswer) in place of the
 * program the deck would park. It meets room on its way, is parked, and waits
 * for the next cycle's end as a waiter of the Settle — no cycle runs inside
 * its call; a service point after the cycle asks the very op again, and its
 * answer is OK with the new file's id. Red before: the create answered
 * ERR_WOULD_BLOCK at once, and nothing ever answered again.
 */
static TestResult TestStandRoomCreateParked(void)
{
    TestResult                 verdict = TEST_PASS;
    uint8_t                    seat    = tagfs_get_seat();
    StandInkGround             ground  = { NULL, NULL, 0, 0 };
    uint64_t                   rests   = 0;
    bool                       parked  = false;
    char                       name[32];
    static SettleOpStandAnswer answer;
    const char*                aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    memset(&answer, 0, sizeof(answer));
    s_room_fid = 0;
    ksnprintf(name, sizeof(name), "room_parks_%u", (unsigned)tagfs_get_state()->ledger.next_file_id);
    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x76u);
    TEST_ASSERT(StandMakeFile("room_ground", g_test_buffer, TAGFS_BLOCK_SIZE, &rests),
                "a file whose ground the volume will wait for is made");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle with a waiter settles it");
    TEST_ASSERT(StandRoomRegistryBrim(name),
                "the registry's last block on the medium has no room for a new name's tag");
    TEST_ASSERT_OK(StandRoomShort(&ground, &rests),
                   "every free block is taken, and the file with ground is given up: it rests");

    uint64_t cycles = SettledCycles();
    SettleOpStandAnswerTo(&answer);
    int made = StandRoomCreate(name);
    parked   = made == ERR_WOULD_BLOCK;
    TEST_ASSERT_EQ(made, ERR_WOULD_BLOCK, "the create meets room on its way and is parked, as a "
                                          "program's is");
    TEST_ASSERT(StandRoomWaits(seat, &answer.answered),
                "it waits for the next cycle's end as a waiter of the Settle, unanswered");
    TEST_ASSERT_EQ(SettledCycles(), cycles, "and no cycle ran inside its call");
    TEST_ASSERT(StandRoomAnswered(seat, &answer.answered),
                "a service point after the cycle asks it again, and it answers");
    parked = false;
    TestEvidence("the create's answer, of OK", answer.rc);
    TEST_ASSERT_OK(answer.rc, "OK, once the ground came back");
    TEST_ASSERT(s_room_fid != 0, "and says the new file's id");
    TEST_ASSERT(StandRegistryHoldsOnMedium(name, NULL),
                "its name's tag is on the medium, in the block the registry grew by");
out:
    SettleOpStandAnswerTo(NULL);
    if (parked && !StandRoomAnswered(seat, &answer.answered))
        s_room_fid = 0;   /* the op is still the service point's, and writes its id here */
    StandInkGiveBack(&ground);
    StandRoomLetGo(&s_room_fid);
    StandRoomLetGo(&rests);
    (void)TagFSSettle();
    return verdict;
}

// ----------------------------------------------------------------------------
// A write on the core whose cycle stands between two of its steps
//
// Z01-3, F01-2. The committer's core holds the Settle from a cycle's S0 to its
// S6 and serves pockets between two of its steps. A write served there finds
// the volume short while Fallow rests a block: a program's job waits for the
// cycle's end — this core's own cycle among the ones it waits for — and is
// answered OK; a write in the kernel on that core runs its core's cycle to the
// end itself, then the cycles that give the ground back, and is taken. Red
// before: either was answered ERR_WOULD_BLOCK at once, as a caller that may
// not wait — the core holds a Scribe — and the program never heard more.
// ----------------------------------------------------------------------------

/* The volume's ground given back, every change settled with the reserve made
 * whole, and taken again: the deletes have their Spares, and the file `rests`
 * given up rests in Fallow. */
static error_t StandRoomShortAgain(StandInkGround* g, uint64_t* rests)
{
    StandInkGiveBack(g);
    error_t settled = TagFSSettle();
    return settled == OK ? StandRoomShort(g, rests) : settled;
}

static TestResult TestStandRoomJobBetweenTheSteps(void)
{
    TestResult     verdict = TEST_PASS;
    uint8_t        seat    = tagfs_get_seat();
    StandInkGround ground  = { NULL, NULL, 0, 0 };
    uint64_t       file_id = 0;
    uint64_t       rests = 0;
    bool           began   = false;
    bool           asked   = false;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x74u);
    TEST_ASSERT(StandMakeFile("room_between", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "the file the write appends to is made");
    TEST_ASSERT(StandMakeFile("room_ground", g_test_buffer, TAGFS_BLOCK_SIZE, &rests),
                "a file whose ground the volume will wait for is made");
    TEST_ASSERT_OK(StandRoomShortAgain(&ground, &rests),
                   "every free block is taken, and the file with ground is given up: it rests");

    uint64_t cycles = SettledCycles();
    began = TagFSStandSettleBegin();
    TEST_ASSERT(began, "a cycle is begun as the cadence begins one, and stands between two of "
                       "its steps on this core");
    asked = StandRoomSubmit(file_id, 0, true);
    TEST_ASSERT(asked, "a program's append on that core is taken, its answer to come");
    TEST_ASSERT(StandRoomWaits(seat, &s_room_answer.answered),
                "it waits for the cycle's end as a waiter of the Settle, unanswered");
    TEST_ASSERT_EQ(SettledCycles(), cycles, "and no cycle ran inside anybody's call for it");
    TEST_ASSERT(StandRoomAnswered(seat, &s_room_answer.answered), "the cycles' end answers it");
    asked = false;
    began = false;   /* the cycles the waiter was owed ran this core's cycle to its end first */
    TestEvidence("the append's answer, of OK", s_room_answer.rc);
    TEST_ASSERT_OK(s_room_answer.rc, "OK, once the cycles it waited for gave the ground back");
    TEST_ASSERT_EQ(s_room_answer.written, TAGFS_BLOCK_SIZE, "with every byte of it the file's");
    TEST_ASSERT_EQ(s_room_answer.size, 2u * TAGFS_BLOCK_SIZE, "and the size it grew to");
out:
    if (asked && !StandRoomAnswered(seat, &s_room_answer.answered))
        file_id = 0;
    if (began)
        (void)TagFSSettle();
    StandInkGiveBack(&ground);
    StandRoomLetGo(&file_id);
    StandRoomLetGo(&rests);
    (void)TagFSSettle();
    return verdict;
}

static TestResult TestStandRoomKernelWriteBetweenTheSteps(void)
{
    TestResult     verdict = TEST_PASS;
    StandInkGround ground  = { NULL, NULL, 0, 0 };
    uint64_t       file_id = 0;
    uint64_t       rests = 0;
    bool           began   = false;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x75u);
    TEST_ASSERT(StandMakeFile("room_between", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "the file the write appends to is made");
    TEST_ASSERT(StandMakeFile("room_ground", g_test_buffer, TAGFS_BLOCK_SIZE, &rests),
                "a file whose ground the volume will wait for is made");
    TEST_ASSERT_OK(StandRoomShortAgain(&ground, &rests),
                   "every free block is taken, and the file with ground is given up: it rests");

    uint64_t cycles = SettledCycles();
    began = TagFSStandSettleBegin();
    TEST_ASSERT(began, "a cycle is begun as the cadence begins one, and stands between two of "
                       "its steps on this core");
    error_t wrote = StandRoomWrite(file_id, TAGFS_BLOCK_SIZE);
    began = SettledCycles() == cycles;
    TestEvidence("the write's answer, of OK", wrote);
    TEST_ASSERT_OK(wrote, "a write in the kernel on that core runs its core's cycle to the end, "
                          "and the cycles that give the ground back, and is taken");
    TEST_ASSERT(!began, "the cycle that stood between its steps is settled");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 2u * TAGFS_BLOCK_SIZE),
                   (int)(2u * TAGFS_BLOCK_SIZE), "the file reads back two blocks long");
out:
    if (began)
        (void)TagFSSettle();
    StandInkGiveBack(&ground);
    StandRoomLetGo(&file_id);
    StandRoomLetGo(&rests);
    (void)TagFSSettle();
    return verdict;
}

// ----------------------------------------------------------------------------
// Deletes on a full volume: files that hold no ground, then one that holds some
//
// E1. Every free block taken, and a run of empty files given up — more than
// the tail and the whole reserve hold Quitclaims of — through the storage
// deck, asked from the kernel, where nobody is parked. Their records lie
// behind blocks of files that stay, so the room their deletes free comes back
// only once Glean has moved those off the chain's front. A delete the
// deletes' share does not hold frees no ground a cycle could give back: it
// waits in its call for the cycles of Glean's sweep (glean.h), and is
// answered OK. Then the file that holds ground is given up, and answered OK.
// Red before: every delete past the 98th was refused ERR_TAGFS_NO_SPACE, the
// one whose ground would have been the room among them, and the volume could
// free nothing again; and red with Glean's share open to the deletes, which
// spend it and leave Glean no step to move the files that stay with.
// ----------------------------------------------------------------------------

#define STAND_ROOM_STAY  (2u * POOL_QUITCLAIMS_PER_BLOCK)
#define STAND_ROOM_BARE_ROUNDS 16u   /* cycles the ground at rest gets to come back in */
#define STAND_ROOM_MANY  ((2u + SPARE_RESERVE) * POOL_QUITCLAIMS_PER_BLOCK)

static uint64_t s_room_stay[STAND_ROOM_STAY];
static uint64_t s_room_many[STAND_ROOM_MANY];

/* The storage deck's delete of `file_id`, asked from the kernel with no
 * program: the handler's answer. */
static int StandRoomDelete(uint64_t file_id)
{
    const OpRegistration* reg = OpRegistryLookup(OP_KIND(DECK_STORAGE, STORAGE_OBJ_DELETE));
    ManifestOp*           op  = (ManifestOp*)s_room_op;

    if (!reg)
        return ERR_NOT_INITIALIZED;
    memset(s_room_op, 0, sizeof(s_room_op));
    op->op_kind    = OP_KIND(DECK_STORAGE, STORAGE_OBJ_DELETE);
    op->param_size = sizeof(file_id);
    op->in_crate   = CRATE_INDEX_NONE;
    op->out_crate  = CRATE_INDEX_NONE;
    memcpy(op->params, &file_id, sizeof(file_id));
    return reg->handler(op, NULL, 0, NULL);
}

/* `count` empty files called `name`, their ids into `ids`: false at the first
 * that is not made. */
static bool StandRoomMakeMany(const char* name, uint64_t* ids, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        if (tagfs_create_file(name, NULL, 0, &ids[i]) != 0)
            return false;
    return true;
}

/* Cycles until nothing rests in Fallow — the ground the tests before freed
 * back with the allocator before every block of the volume is taken — and one
 * more, which makes the reserve whole from it: true once at rest. */
static bool StandRoomBare(void)
{
    for (uint32_t round = 0; round < STAND_ROOM_BARE_ROUNDS; round++) {
        bool bare = StandRoomResting() == 0;
        if (TagFSSettle() != OK)
            return false;
        if (bare)
            return true;
    }
    return false;
}

static void StandRoomLetGoMany(uint64_t* ids, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        StandRoomLetGo(&ids[i]);
}

static TestResult TestStandRoomEmptyDeletesOnAFullVolume(void)
{
    TestResult     verdict = TEST_PASS;
    StandInkGround ground  = { NULL, NULL, 0, 0 };
    uint32_t       refused = 0, first_error = 0;
    uint64_t       big     = 0;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    memset(s_room_stay, 0, sizeof(s_room_stay));
    memset(s_room_many, 0, sizeof(s_room_many));
    fill_pattern(g_test_buffer, sizeof(g_test_buffer), 0x77u);
    TEST_ASSERT(StandRoomMakeMany("room_stay", s_room_stay, STAND_ROOM_STAY),
                "files that stay are made, blocks of records before the ones given up");
    TEST_ASSERT(StandRoomMakeMany("room_many", s_room_many, STAND_ROOM_MANY),
                "files with nothing in them are made, more than the tail and the whole "
                "reserve hold Quitclaims of");
    TEST_ASSERT(StandMakeFile("room_big", g_test_buffer, sizeof(g_test_buffer), &big),
                "a file that holds ground is made");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle with a waiter settles them");
    TEST_ASSERT(StandRoomBare(), "cycles give back all that earlier changes left resting");
    error_t taken = StandInkTakeAll(&ground);
    TEST_ASSERT(taken == ERR_TAGFS_NO_SPACE || taken == ERR_WOULD_BLOCK,
                "every free block of the volume is taken, and nothing rests to come back: "
                "what room the deletes meet is Glean's to make");

    uint64_t cycles = SettledCycles();
    for (uint32_t i = 0; i < STAND_ROOM_MANY; i++) {
        int gone = StandRoomDelete(s_room_many[i]);
        if (gone == OK) {
            s_room_many[i] = 0;
        } else {
            if (!refused)
                first_error = (uint32_t)gone;
            refused++;
        }
    }
    int dropped = StandRoomDelete(big);
    if (dropped == OK)
        big = 0;
    uint64_t waited = SettledCycles() - cycles;

    TestEvidence("deletes refused on a volume with no free block, the one of ground among them",
                 (uint64_t)refused + (dropped != OK));
    TestEvidence("cycles the deletes waited for in their calls", waited);
    if (refused || dropped != OK) {
        kprintf("[TESTS] %u of %u empty file(s) not given up on a full volume, the first "
                "answering error %u; the file that holds ground answered %d\n", refused,
                (unsigned)STAND_ROOM_MANY, first_error, dropped);
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT(waited > 0, "past the deletes' share they waited for cycles: room on its way");
out:
    StandInkGiveBack(&ground);
    StandRoomLetGoMany(s_room_many, STAND_ROOM_MANY);
    StandRoomLetGoMany(s_room_stay, STAND_ROOM_STAY);
    StandRoomLetGo(&big);
    (void)TagFSSettle();
    return verdict;
}

// ----------------------------------------------------------------------------
// The border every answer passes
//
// R6. Whatever answers a program's submit passes one border (ChitAnswer): an
// answer of ERR_WOULD_BLOCK there is the kernel's defect — a STRICT kernel
// stops on it, and stands this test down; any other says so and answers
// ERR_TAGFS_NO_SPACE, the refusal the word stood for. Every other answer
// passes as it is.
// ----------------------------------------------------------------------------

static TestResult TestStandRoomNeverAnAnswer(void)
{
#if CONFIG_STRICT
    return test_steps_aside("a STRICT kernel stops at the border every answer passes: an answer "
                            "of ERR_WOULD_BLOCK is a defect it panics on, never one it hands on");
#else
    TestResult verdict = TEST_PASS;

    TEST_ASSERT_OK(ChitAnswer(OK, 0, 0, "the stand"), "OK passes the border as it is");
    TEST_ASSERT_EQ(ChitAnswer(ERR_TAGFS_NO_SPACE, 0, 0, "the stand"), ERR_TAGFS_NO_SPACE,
                   "and so does a refusal");
    TEST_ASSERT_EQ(ChitAnswer(ERR_DEVICE_NOT_READY, 0, 0, "the stand"), ERR_DEVICE_NOT_READY,
                   "any refusal");
    error_t said = ChitAnswer(ERR_WOULD_BLOCK, 0, 0, "the stand");
    TestEvidence("answers of ERR_WOULD_BLOCK the border let through to a program",
                 said == ERR_WOULD_BLOCK);
    if (said == ERR_WOULD_BLOCK) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(said, ERR_TAGFS_NO_SPACE,
                   "ERR_WOULD_BLOCK leaves it as ERR_TAGFS_NO_SPACE, the refusal it stood for");
out:
    return verdict;
#endif
}

static const TestCase g_stand_room_tests[] = {
    { .name  = "stand_room_spare_through_the_port", .func = TestStandRoomSpareThroughThePort,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_room_create_grows_the_registry",
      .func  = TestStandRoomCreateGrowsTheRegistry, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_room_create_parked", .func = TestStandRoomCreateParked,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_room_job_between_the_steps", .func = TestStandRoomJobBetweenTheSteps,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_room_kernel_write_between_the_steps",
      .func  = TestStandRoomKernelWriteBetweenTheSteps, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_room_empty_deletes_on_a_full_volume",
      .func  = TestStandRoomEmptyDeletesOnAFullVolume, .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "on a volume with no free block, every delete past the 98th of files that "
                       "hold no ground is refused ERR_TAGFS_NO_SPACE, the delete of a file that "
                       "holds ground among them, and nothing on it can be deleted again" },
    { .name  = "stand_room_never_an_answer", .func = TestStandRoomNeverAnAnswer,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "an answer of ERR_WOULD_BLOCK reaches a program, whose boxlib drops it, "
                       "and the program waits for ever" },
};

const TestSuite g_stand_room_suite = {
    .tests = g_stand_room_tests, .count = ARRAY_SIZE(g_stand_room_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
