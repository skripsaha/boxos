#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../braid/braid.h"
#include "../cow/cow.h"
#include "block_fail.h"
#include "boardroom.h"
#include "amp.h"
#include "pmm.h"
#include "vmm.h"

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
// on it say something false about TagFS.
//
// Every test that arms a fault takes it down on every path out; the runner
// takes down whatever is left standing and fails the test that left it
// (tests.c, TestLeftAFaultArmed).
// ============================================================================

const char* StandStepsAsideFor(void)
{
    if (BraidIsHealthy())
        return "Braid is healthy and takes every block of the volume, so a fault "
               "armed on the volume's own sectors would never meet its command";
    if (TagFS_CowIsActive(0))
        return "a snapshot of the whole volume is standing, so every write is "
               "copied for it and the block a fault names is not the block written";
    return NULL;
}

bool StandMakeFile(const char* name, const void* data, uint32_t bytes, uint32_t* out_file_id)
{
    uint32_t file_id = 0;

    *out_file_id = 0;
    if (tagfs_create_file(name, NULL, 0, &file_id) != 0)
        return false;
    if (bytes > 0 && TestFileWrite(file_id, data, bytes) != (int)bytes) {
        tagfs_delete_file(file_id);
        return false;
    }
    *out_file_id = file_id;
    return true;
}

bool StandBlockOf(uint32_t file_id, uint32_t k, uint32_t* out_block)
{
    TagFSMetadata meta;
    bool          found = false;

    memset(&meta, 0, sizeof(meta));
    if (tagfs_get_metadata(file_id, &meta) != 0)
        return false;
    for (uint16_t e = 0; e < meta.extent_count && meta.extents; e++) {
        if (k < meta.extents[e].block_count) {
            *out_block = meta.extents[e].start_block + k;
            found = true;
            break;
        }
        k -= meta.extents[e].block_count;
    }
    tagfs_metadata_free(&meta);
    return found;
}

int StandWriteAt(uint32_t file_id, uint64_t offset, const void* data, uint32_t bytes)
{
    TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    if (!handle)
        return -1;
    handle->offset = offset;
    int written = tagfs_write(handle, data, bytes);
    tagfs_close(handle);
    return written;
}

bool StandBlockInUse(uint32_t block)
{
    TagFSState* fs = tagfs_get_state();

    spin_lock(&fs->lock);
    bool used = block < fs->layout.data_blocks &&
                (fs->block_bitmap.bitmap[block / 8] & (uint8_t)(1u << (block % 8))) != 0;
    spin_unlock(&fs->lock);
    return used;
}

// ----------------------------------------------------------------------------
// BLOCKFAIL
// ----------------------------------------------------------------------------

/* Three one-block extents — the write path gives every 4 KiB its own, and an
 * extent of one block is never read ahead — and a plan on the middle block,
 * second read. The first read of the file meets the plan once and is served;
 * the second is refused at that block; an overwrite of the block is a write
 * and is not counted; the third read is served again, because the plan fires
 * once; and once the plan is down, nothing is counted at all. */
static TestResult TestStandBlockFailReadsTheNth(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint32_t    file_id = 0;
    uint32_t    middle  = 0;
    bool        armed   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, 3 * TAGFS_BLOCK_SIZE, 0x51, 0);
    TEST_ASSERT(StandMakeFile("stand_blockfail.tst", g_test_buffer, 3 * TAGFS_BLOCK_SIZE, &file_id),
                "a three-block file is made");
    TEST_ASSERT(StandBlockOf(file_id, 1, &middle), "its middle block is found");

    BlockFailArm(seat, BLOCK_FAIL_READ, tagfs_block_to_sector(middle), TAGFS_BLOCK_SECTORS, 2);
    armed = true;

    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 3 * TAGFS_BLOCK_SIZE), 3 * TAGFS_BLOCK_SIZE,
                   "the first read of the file is whole");
    TEST_ASSERT_EQ(BlockFailSeen(), 1, "and met the plan once, at the block it names");
    TEST_ASSERT(!BlockFailFired(), "without being refused — it was the first, not the second");

    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 3 * TAGFS_BLOCK_SIZE), -1,
                   "the second read is refused");
    TEST_ASSERT_EQ(BlockFailSeen(), 2, "at the second meeting");
    TEST_ASSERT(BlockFailFired(), "and the plan says it fired");

    TestFillAt(g_test_buffer + TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, 0x52, TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(StandWriteAt(file_id, TAGFS_BLOCK_SIZE, g_test_buffer + TAGFS_BLOCK_SIZE,
                                TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "the middle block is overwritten whole");
    TEST_ASSERT_EQ(BlockFailSeen(), 2, "a write is not a read, and is not counted");

    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 3 * TAGFS_BLOCK_SIZE), 3 * TAGFS_BLOCK_SIZE,
                   "the third read is whole — the plan refuses once");
    TEST_ASSERT_EQ(BlockFailSeen(), 3, "and it was still counted");

    TEST_ASSERT(BlockFailDisarm(), "the plan stood until it was taken down");
    armed = false;
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 3 * TAGFS_BLOCK_SIZE), 3 * TAGFS_BLOCK_SIZE,
                   "a read with the plan down is whole");
    TEST_ASSERT_EQ(BlockFailSeen(), 3, "and meets no plan");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, 3 * TAGFS_BLOCK_SIZE),
                   3 * TAGFS_BLOCK_SIZE, "and the file holds what was written");
out:
    if (armed)
        BlockFailDisarm();
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

/* A refused write is refused before the driver is called, so the block keeps
 * what it held: the file's write answers -1, and the block reads back as it
 * was. */
static TestResult TestStandBlockFailWriteNeverLands(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint32_t    file_id = 0;
    uint32_t    block   = 0;
    bool        armed   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x53, 0);
    TEST_ASSERT(StandMakeFile("stand_blockfail.tst", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &block), "its block is found");

    TestFillAt(g_test_output, TAGFS_BLOCK_SIZE, 0x54, 0);
    BlockFailArm(seat, BLOCK_FAIL_WRITE, tagfs_block_to_sector(block), TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    int wrote = TestFileWrite(file_id, g_test_output, TAGFS_BLOCK_SIZE);
    bool fired = BlockFailFired();
    uint32_t seen = BlockFailSeen();
    BlockFailDisarm();
    armed = false;

    TEST_ASSERT(fired, "the overwrite of the block was the write refused");
    TEST_ASSERT_EQ(seen, 1, "and the only one that met the plan");
    TEST_ASSERT_EQ(wrote, -1, "the file's write answers the refusal");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "the block reads back");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "holding what it held — nothing of the refused write landed");
out:
    if (armed)
        BlockFailDisarm();
    if (file_id)
        tagfs_delete_file(file_id);
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
    uint32_t    file_id = 0;
    uint32_t    block   = 0;
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
    TEST_ASSERT(StandMakeFile("stand_blockfail.tst", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
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
                                     StandAsyncAnswered, NULL);
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
        tagfs_delete_file(file_id);
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
// tag's row of the index had to grow for a new file id and the allocation for
// the wider row failed, tag_bitmap_set kept the old array and set the bit past
// its end. Asked on an index of its own, never the volume's: a row created for
// file 7 is one byte, and file 8 needs a second.
// ----------------------------------------------------------------------------

static TestResult TestStandTagRowRefusesWhatItCannotWiden(void)
{
    TestResult      verdict = TEST_PASS;
    TagBitmapIndex* idx     = tag_bitmap_create(4, 16);
    bool            armed   = false;

    TEST_ASSERT(idx != NULL, "an index of the test's own");
    TEST_ASSERT_EQ(tag_bitmap_set(idx, 1, 7), 0, "file 7 takes its bit in a fresh row of one byte");

    HeapFailArm(amp_get_core_index(), 1);
    armed = true;
    int set = tag_bitmap_set(idx, 1, 8);
    bool fired = HeapFailFired();
    HeapFailDisarm();
    armed = false;

    TEST_ASSERT(fired, "the row's widening was the allocation refused");
    TEST_ASSERT_EQ(set, -1, "a row that could not widen refuses the set instead of "
                            "writing past its end");
out:
    if (armed)
        HeapFailDisarm();
    if (idx)
        tag_bitmap_destroy(idx);
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

    { .name  = "stand_rmw_read_refused", .func = TestStandRmwReadRefused,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.3",
      .caveat_defect = "a write whose read-modify-write read is refused zeroes the rest "
                       "of the block, writes it back and answers success" },
    { .name  = "stand_create_all_or_nothing", .func = TestStandCreateAllOrNothing,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.4",
      .caveat_defect = "a create whose allocation is refused can answer OK and leave the "
                       "file out of its name's index, or leave a deleted file findable "
                       "by its name" },
    { .name  = "stand_sync_owns_a_refused_write", .func = TestStandSyncOwnsARefusedWrite,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.4",
      .caveat_defect = "a sync whose file-table write the medium refuses still writes "
                       "the ledger and answers OK" },
    { .name  = "stand_sync_owns_a_refused_flush", .func = TestStandSyncOwnsARefusedFlush,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.4",
      .caveat_defect = "a sync whose cache flush the medium refuses still answers OK" },
    { .name  = "stand_write_returns_its_ground", .func = TestStandWriteReturnsItsGround,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.3",
      .caveat_defect = "a write whose metadata commit cannot get memory answers -1 and "
                       "keeps the block it wrote, named by no file" },
    { .name  = "stand_write_all_or_refusal", .func = TestStandWriteAllOrRefusal,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.5",
      .caveat_defect = "a write whose extent list cannot grow answers 0 bytes as a "
                       "success instead of refusing" },
    { .name  = "stand_open_holds_what_it_names", .func = TestStandOpenHoldsWhatItNames,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.4",
      .caveat_defect = "an open whose extent copy cannot get memory hands back a handle "
                       "that counts extents and holds none" },
    { .name  = "stand_open_refuses_unread_metadata", .func = TestStandOpenRefusesUnreadMetadata,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.4",
      .caveat_defect = "an open whose metadata read cannot get memory hands back a "
                       "non-empty file as empty" },
    { .name  = "stand_open_joins_the_writers", .func = TestStandOpenJoinsTheWriters,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.5",
      .caveat_defect = "an open whose open-file entry cannot get memory succeeds without "
                       "one, so its writes are serialised with nobody's" },
    { .name  = "stand_frozen_block_freed_twice", .func = TestStandFrozenBlockFreedTwice,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.3",
      .caveat_defect = "deleting a file whose block a snapshot still keeps gives the "
                       "block back, and deleting the snapshot gives it back again, so "
                       "the allocator counts it free twice" },
};

const TestSuite g_stand_suite = {
    .name = "stand", .tests = g_stand_tests, .count = ARRAY_SIZE(g_stand_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
