#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../ink/ink.h"
#include "../helm/engross.h"
#include "../file_map/file_map.h"
#include "../block_map/block_map_derived.h"
#include "block_fail.h"
#include "boardroom.h"
#include "amp.h"
#include "pmm.h"
#include "vmm.h"
#include "scribe.h"
#include "scheduler.h"
#include "baton.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "op_registry.h"
#include "boxos_decks.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand — the faults a test arms, proved first, and the tests that stand
// on them
//
// BLOCKFAIL (core/boardroom/block_fail.h) refuses one command at the Boardroom
// the way the medium would; HEAPFAIL (klib.h) answers one kmalloc NULL the way
// an exhausted heap would. Before any test is allowed to lean on either, the
// mechanism itself is asked here whether it refuses exactly the command it was
// told to, once, on the seat and sectors and core it was told to, and nothing
// else. A mechanism that refused the wrong thing would make every test built
// on it say something false about TagFS. LockRank (tagfs/lock_rank), which the
// lock tests count with, is proved here the same way, and so are the flush
// tickets and the Scribe (tagfs/scribe), which the writers of the volume
// stand on.
//
// Every test that arms a fault takes it down on every path out; the runner
// takes down whatever is left standing and fails the test that left it
// (tests.c, TestLeftAFaultArmed).
// ============================================================================

const char* StandStepsAsideFor(void)
{
    if (tagfs_get_state()->braid)
        return "the volume is assembled in Braid, which takes every block of it, so a "
               "fault armed on the volume's own sectors would never meet its command";
    return NULL;
}

bool StandMakeFile(const char* name, const void* data, uint32_t bytes, uint64_t* out_file_id)
{
    return StandMakeTaggedFile(name, NULL, data, bytes, out_file_id);
}

bool StandMakeTaggedFile(const char* name, const char* tag, const void* data, uint32_t bytes,
                         uint64_t* out_file_id)
{
    uint64_t file_id = 0;
    uint32_t tag_id  = TAGFS_INVALID_TAG_ID;

    *out_file_id = 0;
    if (tag && tagfs_tag_intern(tag, &tag_id) != OK)
        return false;
    if (tagfs_create_file(name, tag ? &tag_id : NULL, tag ? 1 : 0, &file_id) != 0)
        return false;
    if (bytes > 0 && TestFileWrite(file_id, data, bytes) != (int)bytes) {
        tagfs_delete_file(file_id, INK_UNWARRANTED);
        return false;
    }
    *out_file_id = file_id;
    return true;
}

error_t StandKeepInRole(uint64_t file_id, uint32_t role)
{
    InkReceipt receipt;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = EngrossRoled(file_id, role, &receipt);
    tagfs_leave();
    return rc;
}

bool StandKeptFile(uint64_t* out_file_id)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    from = 0, ids[16];
    uint32_t    n;
    while ((n = FileMapFiles(fs->files, from, ids, 16)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec  = FileMapRecordHold(fs->files, ids[i], NULL);
            bool        kept = rec && rec->blocks && InkFileKept(rec->flags);
            FileRecordRelease(rec);
            if (kept) {
                *out_file_id = ids[i];
                return true;
            }
        }
        from = ids[n - 1] + 1;
    }
    return false;
}

bool StandBlockOf(uint64_t file_id, uint64_t k, uint64_t* out_block)
{
    FileSpot spot;
    if (!tagfs_enter())
        return false;
    FileRecord* rec   = FileMapRecordHold(tagfs_get_state()->files, file_id, NULL);
    bool        found = rec && FileRecordFind(rec, k, &spot) && StripOnGround(spot.kind);
    if (found)
        *out_block = spot.block;
    FileRecordRelease(rec);
    tagfs_leave();
    return found;
}

int StandWriteAt(uint64_t file_id, uint64_t offset, const void* data, uint32_t bytes)
{
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    if (!handle)
        return -1;
    handle->offset = offset;
    int written = TestHandleWrite(handle, data, bytes);
    tagfs_close(handle);
    return written;
}

void StandBlockFailRaced(const char* suite)
{
    kprintf("[TESTS] %s: BLOCKFAIL answered %u command(s) of the plan let go — as many as the Helm's service and "
            "the test made between them\n", suite, BlockFailSeen());
}

bool StandBlockInUse(uint64_t block)
{
    return !BlockMapDerivedIsFree(block);
}

/* Room for the widest params a program lays: a write's, a ferry's waybill after it. */
static uint8_t s_deck_op[sizeof(ManifestOp) + 28] __attribute__((aligned(8)));
static Crate   s_deck_crate;

int StandDeckAsk(uint16_t opcode, const void* params, uint16_t param_size, const void* in,
                 uint64_t in_size, const OpContext* ctx)
{
    const OpRegistration* reg = OpRegistryLookup(OP_KIND(DECK_STORAGE, opcode));
    ManifestOp*           op  = (ManifestOp*)s_deck_op;
    if (!reg || param_size > sizeof(s_deck_op) - sizeof(ManifestOp))
        return ERR_NOT_INITIALIZED;
    memset(s_deck_op, 0, sizeof(s_deck_op));
    op->op_kind    = OP_KIND(DECK_STORAGE, opcode);
    op->param_size = param_size;
    op->in_crate   = CRATE_INDEX_NONE;
    op->out_crate  = CRATE_INDEX_NONE;
    memcpy(op->params, params, param_size);
    if (in) {
        s_deck_crate = (Crate){ .magic = CRATE_MAGIC, .addr = (uint64_t)(uintptr_t)in,
                                .size = in_size, .capacity = in_size };
        op->in_crate = 0;
    }
    return reg->handler(op, &s_deck_crate, in ? 1 : 0, ctx);
}

// ----------------------------------------------------------------------------
// BLOCKFAIL
// ----------------------------------------------------------------------------

/* A plan on the middle block of a three-block file, second read. Each block
 * is read on its own, never through a read of the file, which would fetch the
 * middle one in the run of the block before it, one command for both. The
 * block before it meets no plan; the
 * first read of the middle one meets the plan once and is served; the second
 * is refused at that block; a write of the block is a write and is not
 * counted — the Boardroom's own, of the bytes the block holds, which meets the
 * plan's sectors, and the file's overwrite, which a book lays
 * on fresh ground; the third read is served again, because the plan fires
 * once; and once the plan is down, nothing is counted at all — not a read of
 * the block itself, nor of the file. */
static TestResult TestStandBlockFailReadsTheNth(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint64_t    file_id = 0;
    uint64_t    first   = 0;
    uint64_t    middle  = 0;
    bool        armed   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, 3 * TAGFS_BLOCK_SIZE, 0x51, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, 3 * TAGFS_BLOCK_SIZE, &file_id),
                "a three-block file is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &first), "its first block is found");
    TEST_ASSERT(StandBlockOf(file_id, 1, &middle), "and its middle block");

    BlockFailArm(seat, BLOCK_FAIL_READ, tagfs_block_to_sector(middle), TAGFS_BLOCK_SECTORS, 2);
    armed = true;

    TEST_ASSERT_OK(tagfs_read_block(first, g_test_output), "the block before it is read");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "and meets no plan — the plan names another block");
    TEST_ASSERT_OK(tagfs_read_block(middle, g_test_output), "the first read of the block is served");
    TEST_ASSERT_EQ(BlockFailSeen(), 1, "and met the plan once, at the block it names");
    TEST_ASSERT(!BlockFailFired(), "without being refused — it was the first, not the second");

    TEST_ASSERT(tagfs_read_block(middle, g_test_output) != OK, "the second read is refused");
    TEST_ASSERT_EQ(BlockFailSeen(), 2, "at the second meeting");
    TEST_ASSERT(BlockFailFired(), "and the plan says it fired");

    TEST_ASSERT_OK(BoardroomWrite(seat, tagfs_block_to_sector(middle), TAGFS_BLOCK_SECTORS,
                                  g_test_buffer + TAGFS_BLOCK_SIZE, NULL),
                   "the block is written again as it holds, by the Boardroom's door");
    TEST_ASSERT_EQ(BlockFailSeen(), 2, "a write is not a read, and is not counted");
    TestFillAt(g_test_buffer + TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 0x52, TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(StandWriteAt(file_id, TAGFS_BLOCK_SIZE, g_test_buffer + TAGFS_BLOCK_SIZE,
                                TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "the middle block is overwritten whole");
    TEST_ASSERT_EQ(BlockFailSeen(), 2, "and the file's overwrite is not counted either");

    TEST_ASSERT_OK(tagfs_read_block(middle, g_test_output),
                   "the third read is served — the plan refuses once");
    TEST_ASSERT_EQ(BlockFailSeen(), 3, "and it was still counted");

    TEST_ASSERT(BlockFailDisarm(), "the plan stood until it was taken down");
    armed = false;
    TEST_ASSERT_OK(tagfs_read_block(middle, g_test_output), "the block is read with the plan down");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 3 * TAGFS_BLOCK_SIZE), 3 * TAGFS_BLOCK_SIZE,
                   "and so is the file, whole");
    TEST_ASSERT_EQ(BlockFailSeen(), 3, "and meets no plan");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, 3 * TAGFS_BLOCK_SIZE),
                   3 * TAGFS_BLOCK_SIZE, "and the file holds what was written");
out:
    if (armed)
        BlockFailDisarm();
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

/* A refused write is refused before the driver is called, so the medium keeps
 * what it held. The write is the Boardroom's own — the door every write of
 * TagFS reaches the medium by — of a block of a file the
 * test made, in place: the plan refuses it, the medium is read straight back
 * past the digest a read through TagFS checks it against (a refused write
 * that landed anyway would be a digest mismatch there, refused before
 * anything was compared — and what this asks is what the medium holds), and
 * the file still reads under the digest of what it held. Where the file lies
 * is its volume's to choose; that the medium never sees a refused command is
 * BLOCKFAIL's. */
static TestResult TestStandBlockFailWriteNeverLands(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint64_t    file_id = 0;
    uint64_t    block   = 0;
    uint32_t    landed  = 0;
    bool        armed   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x53, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &block), "its block is found");

    TestFillAt(g_test_output, TAGFS_BLOCK_SIZE, 0x54, 0);
    BlockFailArm(seat, BLOCK_FAIL_WRITE, tagfs_block_to_sector(block), TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    int wrote = BoardroomWrite(seat, tagfs_block_to_sector(block), TAGFS_BLOCK_SECTORS, g_test_output,
                               &landed);
    bool fired = BlockFailFired();
    uint32_t seen = BlockFailSeen();
    BlockFailDisarm();
    armed = false;

    TEST_ASSERT(fired, "the write over the block was the one refused");
    TEST_ASSERT_EQ(seen, 1, "and the only one that met the plan");
    TEST_ASSERT(wrote != 0, "the Boardroom answers the refusal");
    TEST_ASSERT_EQ(landed, 0, "and counts no sector of it landed");
    TEST_ASSERT_OK(BoardroomRead(seat, tagfs_block_to_sector(block), TAGFS_BLOCK_SECTORS,
                                 g_test_output), "the block is read off the medium");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "holding what it held — nothing of the refused write landed");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "and the file reads back under the digest of what it held");
out:
    if (armed)
        BlockFailDisarm();
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

/* The flush: the second of three is refused, the other two are taken. */
static TestResult TestStandBlockFailFlushRefused(void)
{
    TestResult verdict = TEST_PASS;
    bool       armed   = false;

    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_FLUSH, 0, 0, 2);
    armed = true;
    TEST_ASSERT_OK(tagfs_flush_cache(), "the first flush is taken");
    TEST_ASSERT(tagfs_flush_cache() != OK, "the second is refused");
    TEST_ASSERT(BlockFailFired(), "by the plan");
    TEST_ASSERT_OK(tagfs_flush_cache(), "the third is taken again");
    TEST_ASSERT_EQ(BlockFailSeen(), 3, "and all three met the plan");
out:
    if (armed)
        BlockFailDisarm();
    return verdict;
}

/* What the seat's callback said, kept where a late answer cannot land on a
 * stack frame that has gone: a read that was really issued answers from the
 * completion path whenever the medium is done with it. */
static struct {
    volatile uint32_t calls;
    volatile error_t  status;
} s_async_answer;

/* Where the read waits if the seat is busy; the seat's until it answers. */
static RoadsteadBerth s_async_berth;

static void StandAsyncAnswered(uint8_t index, uint8_t slot, error_t status, void* ctx)
{
    (void)index;
    (void)slot;
    (void)ctx;
    s_async_answer.status = status;
    __atomic_add_fetch(&s_async_answer.calls, 1u, __ATOMIC_RELEASE);
}

/* An asynchronous read is refused the way the medium refuses one: taken, then
 * answered through the callback with ERR_IO, the page never touched. */
static TestResult TestStandBlockFailAsyncRead(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint64_t    file_id = 0;
    uint64_t    block   = 0;
    void*       dma     = NULL;
    bool        armed   = false;
    bool        issued  = false;
    const char* aside   = StandStepsAsideFor();

    if (!BoardroomSeatCanReadAsync(seat))
        return test_steps_aside("the volume's seat answers only a read somebody stands "
                                "over — there is no asynchronous read on it to refuse");
    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x55, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &block), "its block is found");

    dma = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    TEST_ASSERT(dma != NULL, "a page the medium could read into");
    uint8_t* page = (uint8_t*)vmm_phys_to_virt((uintptr_t)dma);
    memset(page, 0xA5, TAGFS_BLOCK_SIZE);
    memset(g_test_output, 0xA5, TAGFS_BLOCK_SIZE);

    s_async_answer.calls  = 0;
    s_async_answer.status = OK;
    uint64_t sector = tagfs_block_to_sector(block);
    BlockFailArm(seat, BLOCK_FAIL_READ, sector, TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    error_t sub = BoardroomReadAsync(seat, sector, TAGFS_BLOCK_SECTORS, dma,
                                     StandAsyncAnswered, NULL, &s_async_berth);
    bool fired = BlockFailFired();
    BlockFailDisarm();
    armed = false;
    issued = (sub == OK && !fired);

    TEST_ASSERT_OK(sub, "the seat takes the read, as it takes one the medium then refuses");
    TEST_ASSERT(fired, "and the read was the one the plan refused");
    TEST_ASSERT_EQ(__atomic_load_n(&s_async_answer.calls, __ATOMIC_ACQUIRE), 1,
                   "the answer came through the callback, once");
    TEST_ASSERT_EQ(s_async_answer.status, ERR_IO, "as the medium's error");
    TEST_ASSERT_EQ(TestFirstDifference(page, g_test_output, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "and nothing was read into the page");
out:
    if (armed)
        BlockFailDisarm();
    /* A read that went to the medium unrefused owns its page until it is
     * answered, and nothing here waits for that — so the page is left to it
     * rather than handed back under a transfer. The failed assertion above
     * has already said the plan did not refuse it. */
    if (dma && !issued)
        pmm_free(dma, 1);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// HEAPFAIL
// ----------------------------------------------------------------------------

/* The third allocation on this core is refused and no other; and the fourth
 * of a second plan, 3000 bytes — past the slab, on the pool's road — is
 * refused as well, because the plan is asked before either road is taken.
 * With the plan down the heap serves, and counts nothing. */
static TestResult TestStandHeapFailNthOnThisCore(void)
{
    static const size_t mixed[5] = { 16, 64, 32, 3000, 16 };
    static const size_t pool[4]  = { 16, 16, 16, 3000 };

    TestResult verdict = TEST_PASS;
    uint8_t    me      = amp_get_core_index();
    void*      got[5]  = { NULL, NULL, NULL, NULL, NULL };
    void*      after   = NULL;
    bool       armed   = false;

    HeapFailArm(me, 3);
    armed = true;
    for (uint32_t i = 0; i < ARRAY_SIZE(mixed); i++)
        got[i] = kmalloc(mixed[i]);
    TEST_ASSERT(got[0] && got[1] && got[3] && got[4], "every allocation but the third is served");
    TEST_ASSERT(got[2] == NULL, "the third is answered NULL");
    TEST_ASSERT_EQ(HeapFailSeen(), 5, "and all five were counted");
    TEST_ASSERT(HeapFailFired(), "and the plan says it fired");
    for (uint32_t i = 0; i < ARRAY_SIZE(got); i++) {
        kfree(got[i]);
        got[i] = NULL;
    }

    HeapFailArm(me, 4);
    for (uint32_t i = 0; i < ARRAY_SIZE(pool); i++)
        got[i] = kmalloc(pool[i]);
    TEST_ASSERT(got[0] && got[1] && got[2], "the three from the slab are served");
    TEST_ASSERT(got[3] == NULL, "and the one the pool would have served is refused");

    TEST_ASSERT(HeapFailDisarm(), "the plan stood until it was taken down");
    armed = false;
    after = kmalloc(16);
    TEST_ASSERT(after != NULL, "with the plan down the heap serves again");
    TEST_ASSERT_EQ(HeapFailSeen(), 4, "and counts nothing more");
out:
    if (armed)
        HeapFailDisarm();
    for (uint32_t i = 0; i < ARRAY_SIZE(got); i++)
        kfree(got[i]);
    kfree(after);
    return verdict;
}

/* A plan for a core no processor has is a plan that never fires: this core's
 * allocation is served, and not even counted. */
static TestResult TestStandHeapFailOtherCoreUntouched(void)
{
    TestResult verdict = TEST_PASS;
    void*      got     = NULL;
    bool       armed   = false;

    HeapFailArm(g_amp.total_cores, 1);
    armed = true;
    got = kmalloc(16);
    TEST_ASSERT(got != NULL, "an allocation on this core is served");
    TEST_ASSERT_EQ(HeapFailSeen(), 0, "and not counted — it is not the core the plan names");
    TEST_ASSERT(!HeapFailFired(), "the plan never fired");
out:
    if (armed)
        HeapFailDisarm();
    kfree(got);
    return verdict;
}

// ----------------------------------------------------------------------------
// The tag row that could not widen (PF-1)
//
// The precondition for sweeping a refused allocation through create: when a
// tag's members had to grow for a new file id and the allocation failed, the
// index wrote past what it held (PF-1: tag_bitmap_set kept the old row and
// set the bit past its end). Asked on an index of its own, never the
// volume's: a window's first array holds four members, and a fifth needs the
// array doubled.
// ----------------------------------------------------------------------------

static TestResult TestStandTagRowRefusesWhatItCannotWiden(void)
{
    TestResult      verdict = TEST_PASS;
    TagBitmapIndex* idx     = tag_bitmap_create();
    bool            armed   = false;

    TEST_ASSERT(idx != NULL, "an index of the test's own");
    for (uint64_t fid = 7; fid <= 10; fid++)
        TEST_ASSERT_EQ(tag_bitmap_set(idx, 1, fid), 0,
                       "files 7 to 10 fill the first array of the tag's window");

    HeapFailArm(amp_get_core_index(), 1);
    armed = true;
    int set = tag_bitmap_set(idx, 1, 11);
    bool fired = HeapFailFired();
    HeapFailDisarm();
    armed = false;

    TEST_ASSERT(fired, "the row's widening was the allocation refused");
    TEST_ASSERT_EQ(set, -1, "a row that could not widen refuses the set instead of "
                            "writing past its end");
    TEST_ASSERT(!TagBitmapHas(idx, 1, 11), "and the file it refused is not the tag's");
    for (uint64_t fid = 7; fid <= 10; fid++)
        TEST_ASSERT(TagBitmapHas(idx, 1, fid), "every member before it still is");
out:
    if (armed)
        HeapFailDisarm();
    if (idx)
        tag_bitmap_destroy(idx);
    return verdict;
}

// ----------------------------------------------------------------------------
// LockRank — the witness at the medium's doors and the order check, proved on
// two locks of the tests' own, so that no TagFS lock and no place of TagFS is
// touched and every number below is fixed by the test alone. They are proof
// locks: a STRICT kernel names their places as any other and does not stop.
// ----------------------------------------------------------------------------

static TagFSLock s_proof       = TAGFS_PROOF_LOCK_INIT("proof", LOCK_RANK_LEAF);
static TagFSLock s_proof_state = TAGFS_PROOF_LOCK_INIT("proofstate", LOCK_RANK_STATE);

static inline uint64_t StandInterruptsOff(void)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static inline void StandInterruptsBack(uint64_t flags)
{
    asm volatile("push %0; popfq" ::"r"(flags) : "memory", "cc");
}

/* Sector 0 of the volume read three times. Once holding nothing, with
 * interrupts off the way every system call on one core has them: the witness
 * keys on a TagFS lock held, never on IF, so it says nothing. Then twice under
 * the test's own lock: both reads are counted on the lock and on this core,
 * and the place is named once, not once per read. */
static TestResult TestStandLockRankNamesTheMedium(void)
{
    TestResult    verdict   = TEST_PASS;
    bool          held      = false;
    uint32_t      witnessed = s_proof.Witnessed;
    LockRankCount before, first, after;
    uint64_t      flags;
    int           rc;

    before = LockRankCountHere();
    flags = StandInterruptsOff();
    rc = tagfs_volume_read(0, 1, g_test_output);
    StandInterruptsBack(flags);
    after = LockRankCountHere();
    TEST_ASSERT_EQ(rc, 0, "sector 0 of the volume is read holding no TagFS lock");
    TEST_ASSERT_EQ(after.Witnessed - before.Witnessed, 0,
                   "a read that holds no TagFS lock is not witnessed, interrupts off or not");
    TEST_ASSERT(!LockRankAnyHeldHere(), "and this core holds no TagFS lock");

    before = LockRankCountHere();
    TagFSLockTake(&s_proof);
    held = true;
    TEST_ASSERT(LockRankHeldHere(&s_proof), "the test's lock is held here once taken");
    rc = tagfs_volume_read(0, 1, g_test_output);
    first = LockRankCountHere();
    if (rc == 0)
        rc = tagfs_volume_read(0, 1, g_test_output);
    TagFSLockDrop(&s_proof);
    held = false;
    after = LockRankCountHere();

    TEST_ASSERT_EQ(rc, 0, "and read twice under it");
    TEST_ASSERT_EQ(s_proof.Witnessed - witnessed, 2, "both reads are counted on the lock held across them");
    TEST_ASSERT_EQ(after.Witnessed - before.Witnessed, 2, "and on this core");
    TEST_ASSERT_EQ(first.Named - before.Named, 1, "the first read names the place");
    TEST_ASSERT_EQ(after.Named - first.Named, 0, "the second names it no more");
    TEST_ASSERT(!LockRankHeldHere(&s_proof), "and once let go the lock is held here no more");
out:
    if (held)
        TagFSLockDrop(&s_proof);
    return verdict;
}

/* The order is strictly downward. A state-rank lock with a leaf under it is
 * the order and says nothing; a state-rank lock taken under a leaf is out of
 * order — counted each time, on this core and on the lock taken, and named
 * once. */
static TestResult TestStandLockRankNamesTheOrder(void)
{
    TestResult    verdict = TEST_PASS;
    uint32_t      broke   = s_proof_state.OutOfOrder;
    LockRankCount before, mid, after;

    before = LockRankCountHere();
    TagFSLockTake(&s_proof_state);
    TagFSLockTake(&s_proof);
    TagFSLockDrop(&s_proof);
    TagFSLockDrop(&s_proof_state);
    mid = LockRankCountHere();
    TEST_ASSERT_EQ(mid.OutOfOrder - before.OutOfOrder, 0, "a leaf under a state-rank lock is the order");
    TEST_ASSERT_EQ(mid.Named - before.Named, 0, "and names nothing");

    for (uint32_t round = 0; round < 2; round++) {
        TagFSLockTake(&s_proof);
        TagFSLockTake(&s_proof_state);
        TagFSLockDrop(&s_proof_state);
        TagFSLockDrop(&s_proof);
    }
    after = LockRankCountHere();
    TEST_ASSERT_EQ(after.OutOfOrder - mid.OutOfOrder, 2,
                   "a state-rank lock taken under a leaf, twice, is out of order twice");
    TEST_ASSERT_EQ(s_proof_state.OutOfOrder - broke, 2, "counted on the lock taken out of order");
    TEST_ASSERT_EQ(after.Named - mid.Named, 1, "and named once");
    TEST_ASSERT(!LockRankAnyHeldHere(), "and nothing is left held here");
out:
    return verdict;
}

// ----------------------------------------------------------------------------
// Scribe — the flush tickets proved on the medium's own flushes, and the
// one-writer turn on Scribes of the tests' own, so that no structure of the
// volume is touched and every number below is fixed by the test alone
// ----------------------------------------------------------------------------

/* How long a wait below may take before the test gives up on it. The test's
 * own bound, not the kernel's: a wait that behaves is answered within a few
 * turns, and the bound only turns the defect it stands for into a failure the
 * runner can report instead of a machine that stops. */
#define STAND_SCRIBE_BOUND_MS 1000u

/* Three flushes, and what each proves. One asked for after a need was read is
 * kept for it. One whose ticket was taken before the need was read proves
 * nothing about that need even when the medium takes it — it is issued here
 * by hand, between its ticket and its answer, the way tagfs_flush_cache
 * issues one. One the medium refuses keeps nothing, and the need asked for
 * again through VolumeFlushFor is flushed; and a need already kept is answered
 * without a word to the medium. */
static TestResult TestStandFlushTicket(void)
{
    TestResult verdict = TEST_PASS;
    uint8_t    seat    = tagfs_get_seat();
    bool       armed   = false;
    uint64_t   need, ticket;
    int        said;

    need = VolumeFlushNeed();
    TEST_ASSERT_OK(tagfs_flush_cache(), "a flush asked for after the need was read is taken");
    TEST_ASSERT(VolumeFlushKept(need), "and is kept for that need");

    ticket = VolumeFlushBegin();
    need = VolumeFlushNeed();
    said = BoardroomFlush(seat);
    VolumeFlushEnd(ticket, said == 0);
    TEST_ASSERT_EQ(said, 0, "the flush begun before the need was read is taken by the medium");
    TEST_ASSERT(!VolumeFlushKept(need), "and proves nothing about that need");

    BlockFailArm(seat, BLOCK_FAIL_FLUSH, 0, 0, 1);
    armed = true;
    TEST_ASSERT(tagfs_flush_cache() != OK, "a flush the medium refuses");
    TEST_ASSERT(BlockFailFired(), "refused by the plan");
    TEST_ASSERT(!VolumeFlushKept(need), "keeps nothing");
    TEST_ASSERT(BlockFailDisarm(), "the plan stood until it was taken down");
    armed = false;
    TEST_ASSERT_OK(VolumeFlushFor(need), "asked for again, the need is flushed");
    TEST_ASSERT(VolumeFlushKept(need), "and kept");

    BlockFailArm(seat, BLOCK_FAIL_FLUSH, 0, 0, 1);
    armed = true;
    TEST_ASSERT_OK(VolumeFlushFor(need), "a need already kept is answered at once");
    TEST_ASSERT_EQ(BlockFailSeen(), 0, "without a word to the medium");
out:
    if (armed)
        BlockFailDisarm();
    return verdict;
}

static Scribe s_scribe_asked = SCRIBE_INIT(SCRIBE_RANK_SETTLE, "the stand's own Scribe");

/* An ask made while a pass runs is not what that pass covers: the first pass
 * answers the first ask WRITTEN, and the second ask takes a pass of its own,
 * whose refusal answers it and leaves the first one written. The core that
 * holds the Scribe is not switched away from until it gives it back. */
static TestResult TestStandScribeCoversOnlyWhatWasAsked(void)
{
    TestResult         verdict = TEST_PASS;
    Scribe            *s       = &s_scribe_asked;
    scheduler_state_t *here    = scheduler_get_state();
    uint32_t           held    = here ? here->held : 0;
    uint64_t           cover   = 0, other = 0, first, second;
    error_t            refusal = OK;
    bool               mine    = false;

    TEST_ASSERT(here != NULL, "this core has a scheduler to be held off");
    first = ScribeAsk(s);
    TEST_ASSERT_EQ(ScribeWait(s, first, &cover, &refusal), SCRIBE_MINE,
                   "the first ask finds the Scribe free and takes it");
    mine = true;
    TEST_ASSERT(cover >= first, "its pass covers its own ask");
    TEST_ASSERT(ScribeHeldHere(s) && ScribeAnyHeldHere(), "and this core holds it");
    TEST_ASSERT_EQ(here->held, held + 1, "and is not switched away from while it does");
    second = ScribeAsk(s);
    TEST_ASSERT(second > cover, "an ask made during the pass lies past what the pass covers");
    ScribeGive(s, cover, OK);
    mine = false;
    TEST_ASSERT_EQ(here->held, held, "given back, the core may be switched away from again");
    TEST_ASSERT(!ScribeAnyHeldHere(), "and holds no Scribe");

    TEST_ASSERT_EQ(ScribeTry(s, first, &other, &refusal), SCRIBE_WRITTEN,
                   "the first ask is written by the pass that covered it");
    TEST_ASSERT_EQ(ScribeTry(s, second, &cover, &refusal), SCRIBE_MINE,
                   "the second is not, and takes a pass of its own");
    mine = true;
    TEST_ASSERT(cover >= second, "which covers it");
    ScribeGive(s, cover, ERR_IO);
    mine = false;
    TEST_ASSERT_EQ(ScribeTry(s, second, &other, &refusal), SCRIBE_REFUSED,
                   "a pass that fails refuses what it covered");
    TEST_ASSERT_EQ(refusal, ERR_IO, "with the pass's own answer");
    TEST_ASSERT_EQ(ScribeTry(s, first, &other, &refusal), SCRIBE_WRITTEN,
                   "and takes nothing from what an earlier pass wrote");
out:
    if (mine)
        ScribeGive(s, cover, OK);
    return verdict;
}

/* True once the TSC passes the bound ctx points at: a wait nothing else
 * settles. */
static bool StandBoundPassed(const void *ctx)
{
    return rdtsc() >= *(const uint64_t *)ctx;
}

static Scribe s_scribe_self = SCRIBE_INIT(SCRIBE_RANK_SETTLE, "the stand's own Scribe, held beneath");

/* A Scribe this core holds beneath the caller is answered SELF — by a try, and
 * at once by both waits, which would otherwise spin for a holder that cannot
 * run until they return. And a Scribe held by a core that is not running is
 * not taken over: an orderly halt stops no core in the middle of a pass, so a
 * holder that is not running is one still finishing its pass, and it is
 * waited for. */
static TestResult TestStandScribeNeverWaitsOnItself(void)
{
    TestResult verdict = TEST_PASS;
    Scribe    *s       = &s_scribe_self;
    uint32_t   me      = (uint32_t)amp_get_core_index() + 1u;
    uint32_t   stopped = (uint32_t)g_amp.total_cores + 1u;   /* no processor has it */
    uint64_t   cover   = 0, ticket, bound;
    error_t    refusal = OK;

    ticket = ScribeAsk(s);
    __atomic_store_n(&s->holder, me, __ATOMIC_RELEASE);
    TEST_ASSERT_EQ(ScribeTry(s, ticket, &cover, &refusal), SCRIBE_SELF,
                   "a Scribe this core holds beneath the caller is answered SELF");
    TEST_ASSERT_EQ(ScribeWait(s, ticket, &cover, &refusal), SCRIBE_SELF,
                   "and a wait for it returns at once");
    bound = rdtsc() + cpu_ms_to_tsc(STAND_SCRIBE_BOUND_MS);
    TEST_ASSERT_EQ(ScribeWaitFor(&s->holder, StandBoundPassed, &bound), SCRIBE_SELF,
                   "and so does a wait for a turn this core keeps");
    TEST_ASSERT_EQ(__atomic_load_n(&s->holder, __ATOMIC_ACQUIRE), me,
                   "and the holder is left as it was");

    __atomic_store_n(&s->holder, stopped, __ATOMIC_RELEASE);
    TEST_ASSERT_EQ(ScribeTry(s, ticket, &cover, &refusal), SCRIBE_HELD,
                   "a Scribe held by a core that is not running is not taken over");
    TEST_ASSERT_EQ(__atomic_load_n(&s->holder, __ATOMIC_ACQUIRE), stopped,
                   "and its holder stays its holder");
out:
    __atomic_store_n(&s->holder, 0u, __ATOMIC_RELEASE);
    return verdict;
}

/* A pass held on another core, whose end only a baton brings: the test sets
 * the Scribe's holder to a running application core and hands the drain core
 * a probe that publishes what that core's ScribeGive would — written, then
 * the holder cleared. The drain core then waits for the turn. Kept in
 * statics, because a probe the wait did not run is still queued when the test
 * returns, and runs when the drain core next pumps. */
static Scribe            s_scribe_pumped = SCRIBE_INIT(SCRIBE_RANK_SETTLE,
                                                       "the stand's own Scribe, held on another core");
static Baton             s_scribe_probe;
static volatile uint64_t s_scribe_probe_cover;
static volatile uint32_t s_scribe_probe_ran;

static void StandProbeEndsThePass(void *ctx)
{
    (void)ctx;
    __atomic_store_n(&s_scribe_pumped.written, s_scribe_probe_cover, __ATOMIC_RELEASE);
    __atomic_store_n(&s_scribe_pumped.holder, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_scribe_probe_ran, 1u, __ATOMIC_RELEASE);
}

typedef struct {
    uint64_t ticket;
    uint64_t bound;
} StandPumpedWatch;

static bool StandPumpedSettled(const void *ctx)
{
    const StandPumpedWatch *w = ctx;
    return __atomic_load_n(&s_scribe_pumped.written, __ATOMIC_ACQUIRE) >= w->ticket ||
           rdtsc() >= w->bound;
}

static TestResult TestStandScribeWaiterKeepsPumping(void)
{
    TestResult       verdict = TEST_PASS;
    Scribe          *s       = &s_scribe_pumped;
    uint8_t          other   = g_amp.total_cores;
    uint64_t         cover   = 0;
    error_t          refusal = OK;
    bool             passed  = false;
    StandPumpedWatch watch;

    if (amp_get_core_index() != g_amp.bsp_index)
        return test_steps_aside("the wait is asked of the drain core, and the tests run on another");
    for (uint8_t c = 0; c < g_amp.total_cores; c++) {
        if (c != g_amp.bsp_index && amp_core_online(&g_amp.cores[c])) {
            other = c;
            break;
        }
    }
    if (other == g_amp.total_cores)
        return test_steps_aside("no other core is running to hold the Scribe");

    watch.ticket = ScribeAsk(s);
    s_scribe_probe_cover = watch.ticket;
    s_scribe_probe_ran   = 0;
    __atomic_store_n(&s->holder, (uint32_t)other + 1u, __ATOMIC_RELEASE);
    s_scribe_probe.run = StandProbeEndsThePass;
    s_scribe_probe.ctx = NULL;
    passed = BatonPass(&s_scribe_probe);
    TEST_ASSERT(passed, "the probe is handed to the drain core");

    watch.bound = rdtsc() + cpu_ms_to_tsc(STAND_SCRIBE_BOUND_MS);
    TEST_ASSERT_EQ(ScribeWaitFor(&s->holder, StandPumpedSettled, &watch), SCRIBE_WRITTEN,
                   "the drain core's wait for a turn another core keeps is settled");
    TEST_ASSERT(__atomic_load_n(&s_scribe_probe_ran, __ATOMIC_ACQUIRE),
                "by the probe, which the drain core ran while it waited");
    TEST_ASSERT_EQ(ScribeTry(s, watch.ticket, &cover, &refusal), SCRIBE_WRITTEN,
                   "and the ask is answered by the pass that ended");
out:
    if (!passed)
        __atomic_store_n(&s->holder, 0u, __ATOMIC_RELEASE);
    return verdict;
}

// ============================================================================
// The suite
// ============================================================================

static const TestCase g_stand_tests[] = {
    { .name = "stand_blockfail_reads_the_nth",    .func = TestStandBlockFailReadsTheNth,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_blockfail_write_never_lands", .func = TestStandBlockFailWriteNeverLands,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_blockfail_flush_refused",    .func = TestStandBlockFailFlushRefused,
      .reach = TEST_READS_ONLY },
    { .name = "stand_blockfail_async_read",       .func = TestStandBlockFailAsyncRead,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_heapfail_nth_on_this_core",  .func = TestStandHeapFailNthOnThisCore,
      .reach = TEST_READS_ONLY },
    { .name = "stand_heapfail_other_core_untouched", .func = TestStandHeapFailOtherCoreUntouched,
      .reach = TEST_READS_ONLY },
    { .name = "stand_tag_row_refuses_what_it_cannot_widen",
      .func = TestStandTagRowRefusesWhatItCannotWiden, .reach = TEST_READS_ONLY },
    { .name = "stand_lockrank_names_the_medium",  .func = TestStandLockRankNamesTheMedium,
      .reach = TEST_READS_ONLY },
    { .name = "stand_lockrank_names_the_order",   .func = TestStandLockRankNamesTheOrder,
      .reach = TEST_READS_ONLY },
    { .name = "stand_flush_ticket",               .func = TestStandFlushTicket,
      .reach = TEST_READS_ONLY },
    { .name = "stand_scribe_covers_only_what_was_asked",
      .func = TestStandScribeCoversOnlyWhatWasAsked, .reach = TEST_READS_ONLY },
    { .name = "stand_scribe_never_waits_on_itself", .func = TestStandScribeNeverWaitsOnItself,
      .reach = TEST_READS_ONLY },
    { .name = "stand_scribe_waiter_keeps_pumping", .func = TestStandScribeWaiterKeepsPumping,
      .reach = TEST_READS_ONLY },
    { .name = "stand_heap_grows_by_regions_and_gives_back",
      .func = TestStandHeapGrowsByRegionsAndGivesBack, .reach = TEST_READS_ONLY },
    { .name = "stand_heap_large_takes_pages_and_gives_back",
      .func = TestStandHeapLargeTakesPagesAndGivesBack, .reach = TEST_READS_ONLY },
    { .name = "stand_heap_class_refused_reserve_serves",
      .func = TestStandHeapClassRefusedReserveServes, .reach = TEST_READS_ONLY },
    { .name  = "stand_touch_queue_fire_one_pass", .func = TestStandTouchQueueFireOnePass,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a pass walks the touch queue from its head again for every node it "
                       "fires, with interrupts off for their number times the queue's depth" },
    { .name  = "stand_gone_through_the_teardown", .func = TestStandGoneThroughTheTeardown,
      .reach = TEST_READS_ONLY },

    { .name  = "stand_rmw_read_refused", .func = TestStandRmwReadRefused,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a write whose read-modify-write read is refused zeroes the rest "
                       "of the block, writes it back and answers success" },
    { .name  = "stand_create_all_or_nothing", .func = TestStandCreateAllOrNothing,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a create whose allocation is refused can answer OK and leave the "
                       "file out of its name's index, or leave a deleted file findable "
                       "by its name" },
    { .name  = "stand_write_returns_its_ground", .func = TestStandWriteReturnsItsGround,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a write whose metadata commit cannot get memory answers -1 and "
                       "keeps the block it wrote, named by no file" },
    { .name  = "stand_write_all_or_refusal", .func = TestStandWriteAllOrRefusal,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a write whose extent list cannot grow answers 0 bytes as a "
                       "success instead of refusing" },
    { .name  = "stand_open_holds_what_it_names", .func = TestStandOpenHoldsWhatItNames,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "an open whose extent copy cannot get memory hands back a handle "
                       "that counts extents and holds none" },
    { .name  = "stand_open_refuses_unread_metadata", .func = TestStandOpenRefusesUnreadMetadata,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "an open whose metadata read cannot get memory hands back a "
                       "non-empty file as empty" },
    { .name  = "stand_open_joins_the_writers", .func = TestStandOpenJoinsTheWriters,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "an open whose open-file entry cannot get memory succeeds without "
                       "one, so its writes are serialised with nobody's" },
};

const TestSuite g_stand_suite = {
    .tests = g_stand_tests, .count = ARRAY_SIZE(g_stand_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
