#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../cow/cow.h"
#include "../disk_book/disk_book.h"
#include "../integrity/integrity.h"
#include "block_fail.h"
#include "boardroom.h"
#include "ata_async.h"
#include "amp.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// What crosses the medium, or turns interrupts on, under a lock that must not
// allow it
//
// A TagFS lock is an interrupts-off spinlock. A command to the medium made
// while one is held keeps every core that wants the lock spinning with its
// interrupts off for as long as the medium takes — and a core that waits for
// a completion only its own interrupt brings waits for ever. LockRank counts
// every command that passes the medium's doors while a TagFS lock is held, on
// the lock (TagFSLock.Witnessed) and on the core (LockRankCountHere); BLOCKFAIL
// armed with BLOCK_FAIL_NEVER counts the commands of one kind that meet a run
// of sectors and refuses none.
//
// Each test asks its question of one operation it makes itself, and its
// evidence is whether that operation did the thing — 0 or 1, or a count the
// test fixed in advance — never how many commands it took, which would depend
// on caches and on what the boot before this one left on the volume.
// ============================================================================

#define STAND_MAP_PAGE_BLOCKS 512u    /* blocks one page of the integrity map covers */
#define STAND_MAP_PAGES       17u     /* one page more than the map keeps in memory */
#define STAND_REDIRECTS       64u     /* the redirect log pushed the cache on every 64th */
#define STAND_DMA_SECTORS     8u      /* one legacy-IDE DMA transfer (ATA_ASYNC_MAX_SECTORS) */
#define STAND_RFLAGS_IF       (1u << 9)

// ----------------------------------------------------------------------------
// The registry's flush
// ----------------------------------------------------------------------------

static char s_flush_tag[32];
static char s_flush_value[12];

/* The first name of the "stand_flush:<k>" series the volume has never had. A
 * name is interned for good, so every boot of a stand volume takes the next
 * one. */
static const char* StandFreshFlushTag(void)
{
    uint32_t k = 0;
    do {
        ksnprintf(s_flush_value, sizeof(s_flush_value), "%u", k++);
        ksnprintf(s_flush_tag, sizeof(s_flush_tag), "stand_flush:%s", s_flush_value);
    } while (tagfs_tag_lookup(s_flush_tag) != TAGFS_INVALID_TAG_ID);
    return s_flush_tag;
}

/* One name the volume has never had, interned — which does not flush — and
 * then the registry flushed once. Whatever that flush writes crosses the
 * medium; the question is whether it does so with the registry's own lock
 * held, and the lock's own count answers it. The name read back off the
 * medium proves the flush was not an empty one. */
static TestResult TestStandRegistryFlushHoldsNoLock(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();

    TEST_ASSERT(tagfs_tag_intern(StandFreshFlushTag()) != TAGFS_INVALID_TAG_ID,
                "a name the volume never had is interned, and not yet flushed");
    uint32_t witnessed = fs->registry->lock.Witnessed;
    int      flushed   = tag_registry_flush(fs->registry);
    uint32_t crossed   = fs->registry->lock.Witnessed - witnessed;
    TEST_ASSERT_EQ(flushed, 0, "the registry's flush of it is taken");
    TEST_ASSERT(StandRegistryHoldsOnMedium("stand_flush", s_flush_value),
                "and the name is on the medium");

    TestEvidence("registry flushes of one new name that crossed the medium holding the registry's lock",
                 crossed ? 1 : 0);
    if (crossed)
        verdict = TEST_DEFECT;
out:
    return verdict;
}

// ----------------------------------------------------------------------------
// The Ledger and the integrity map
// ----------------------------------------------------------------------------

/* One Ledger write, asked for the way the sync asks for it, and the medium's
 * newest copy read back to prove it was made. */
static TestResult TestStandLedgerWritesOutsideItsLock(void)
{
    TestResult verdict   = TEST_PASS;
    uint64_t   seq       = StandLedgerSeq();
    uint32_t   witnessed = g_ledger_lock.Witnessed;
    error_t    wrote     = tagfs_write_ledger();
    uint32_t   crossed   = g_ledger_lock.Witnessed - witnessed;

    TEST_ASSERT(seq > 0, "a Ledger copy reads before the write");
    TEST_ASSERT_OK(wrote, "the Ledger is written");
    TEST_ASSERT_EQ(StandLedgerSeq(), seq + 1, "and the medium's newest copy is the one it wrote");

    TestEvidence("Ledger writes of one that crossed the medium holding the Ledger's lock",
                 crossed ? 1 : 0);
    if (crossed)
        verdict = TEST_DEFECT;
out:
    return verdict;
}

/* One block written under each of seventeen pages of the integrity map — one
 * page more than the map keeps in memory, so at least one of them is paged
 * in off the medium, and the pages the writes dirty are paged back out as
 * others push them aside. The sync first leaves no page owed from before.
 *
 * The test holds no TagFS lock and a raw block write takes none but the
 * map's own on its way to the medium, so any door this core passes under a
 * lock between the first write and the last is the map paging under its
 * lock: this core's count answers the question, whatever lock the map's
 * paging is given later. */
static TestResult TestStandIntegrityPagesOutsideItsLock(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    span    = (STAND_MAP_PAGES - 1) * STAND_MAP_PAGE_BLOCKS + 1;
    uint32_t    first   = 0;
    bool        taken   = false;
    error_t     wrote   = OK;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(IntegrityIsInitialized(), "the volume's integrity map is up");
    TEST_ASSERT_OK(tagfs_sync(), "a sync first, so no page of the map is owed to the medium");
    TEST_ASSERT_EQ(tagfs_alloc_blocks(span, &first), 0,
                   "a run of blocks under seventeen pages of the map is taken");
    taken = true;

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x61, 0);
    LockRankCount before = LockRankCountHere();
    for (uint32_t k = 0; k < STAND_MAP_PAGES && wrote == OK; k++)
        wrote = tagfs_write_block(first + k * STAND_MAP_PAGE_BLOCKS, g_test_buffer);
    LockRankCount after = LockRankCountHere();
    TEST_ASSERT_OK(wrote, "one block is written under every page");
    for (uint32_t k = 0; k < STAND_MAP_PAGES; k++)
        TEST_ASSERT(IntegrityVerify(first + k * STAND_MAP_PAGE_BLOCKS, g_test_buffer),
                    "and the map holds each one's digest");

    uint32_t crossed = after.Witnessed - before.Witnessed;
    TestEvidence("17-page runs of block writes whose map paging crossed the medium under a lock",
                 crossed ? 1 : 0);
    if (crossed)
        verdict = TEST_DEFECT;
out:
    if (taken)
        tagfs_free_blocks(first, span);
    return verdict;
}

// ----------------------------------------------------------------------------
// Copy-on-write with no snapshot standing
// ----------------------------------------------------------------------------

/* Whether a file is copied on write, asked of a file no snapshot names. The
 * answer is in memory — the snapshots — and the question must not go to the
 * medium for it. It went there by way of the file's metadata, and the medium
 * is asked only when the record is not at hand: not in the pool's mirror (a
 * file made after the mount is never in it), not in the block the pool is
 * appending to (a second file is written until the pool moves on), and not in
 * the read-ahead cache (dropped). Then every read of the record's block is
 * counted, and none refused.
 *
 * With no snapshot standing at all the question takes no lock either. Asked
 * once more under the volume's state lock — a rank the copy-on-write lock
 * must never be taken under — it breaks no order only because it takes
 * nothing. */
static TestResult TestStandCowQuestionAsksNoMedium(void)
{
    TestResult    verdict = TEST_PASS;
    uint32_t      first   = 0, second = 0;
    uint32_t      block   = 0, offset = 0;
    uint32_t      now     = 0, now_offset = 0;
    uint32_t      listed  = 0, standing = 0;
    bool          armed   = false;
    TagFSMetadata cached;
    TagFSState*   fs      = tagfs_get_state();
    const char*   aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x62, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &first),
                "a one-block file is made");
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &second),
                "and a second under the same name");
    TEST_ASSERT_EQ(file_table_lookup(first, &block, &offset), 0, "the first file's record is found");
    TEST_ASSERT_EQ(file_table_lookup(second, &now, &now_offset), 0, "and the second's");
    for (uint32_t i = 0; now == block && i < TAGFS_MPOOL_DATA_SIZE; i++) {
        TEST_ASSERT_EQ(StandWriteAt(second, 0, g_test_buffer, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                       "the second file is written again");
        TEST_ASSERT_EQ(file_table_lookup(second, &now, &now_offset), 0, "and its new record found");
    }
    TEST_ASSERT(now != block, "the pool moved on from the block that holds the first file's record");

    memset(&cached, 0, sizeof(cached));
    bool mirrored = meta_pool_read_cached(first, &cached) == 0;
    if (mirrored)
        tagfs_metadata_free(&cached);
    TEST_ASSERT(!mirrored, "the pool's mirror does not hold the first file, so its record is only "
                           "on the medium");
    tagfs_readahead_invalidate(block);

    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_READ, tagfs_block_to_sector(block),
                 TAGFS_BLOCK_SECTORS, BLOCK_FAIL_NEVER);
    armed = true;
    bool     copied = TagFS_CowIsActive(first);
    uint32_t reads  = BlockFailSeen();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT(!copied, "no snapshot of the file stands, and the question says so");

    TestEvidence("reads of a file's metadata block the copy-on-write question asked, no snapshot standing",
                 reads);
    if (reads) {
        verdict = TEST_DEFECT;
        goto out;
    }

    TEST_ASSERT_OK(TagFS_SnapshotList(&listed, 1, &standing), "the snapshots standing are listed");
    TEST_ASSERT_EQ(standing, 0, "and not one stands, of any file");
    LockRankCount before = LockRankCountHere();
    TagFSLockTake(&fs->lock);
    copied = TagFS_CowIsActive(first);
    TagFSLockDrop(&fs->lock);
    LockRankCount after = LockRankCountHere();
    TEST_ASSERT(!copied, "asked again under the state lock, the question still says no");
    TEST_ASSERT_EQ(after.OutOfOrder - before.OutOfOrder, 0,
                   "and takes no lock: under the state lock it breaks no order");
out:
    if (armed)
        BlockFailDisarm();
    if (second)
        tagfs_delete_file(second);
    if (first)
        tagfs_delete_file(first);
    return verdict;
}

/* Sixty-four redirects logged for a snapshot of the whole volume, the way the
 * write path logs one after it has copied a block for it, with every cache
 * flush the medium is asked for counted. The log used to push the cache
 * itself on every 64th redirect, from inside the copy-on-write lock; 64 in a
 * row cross that mark exactly once wherever its count stood, so a push that
 * comes back is seen. Deleting the snapshot gives back the 64 blocks it kept
 * and empties the log of them; the 64 that stood in for them are the test's
 * to give back. */
static TestResult TestStandRedirectLogLeavesTheFlush(void)
{
    TestResult    verdict  = TEST_PASS;
    uint32_t      snap     = 0;
    uint32_t      kept     = 0, stood = 0;
    bool          have_kept = false, have_stood = false;
    uint32_t      appended = 0;
    bool          armed    = false;
    DiskBookStats stats;
    CowSnapshot   info;
    const char*   aside    = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);
    if (!DiskBookIsInitialized())
        return test_steps_aside("the volume's redirect log is not up, so no redirect is logged");
    memset(&stats, 0, sizeof(stats));
    TEST_ASSERT_OK(DiskBookGetStats(&stats), "the redirect log's counts are read");
    if (stats.free_entries < STAND_REDIRECTS)
        return test_steps_aside("the redirect log has fewer than 64 free slots, and 64 are needed "
                                "to cross the mark its push stood at");
    uint32_t logged = stats.redirects_logged;

    error_t made = TagFS_SnapshotCreate("stand-cr6", 0, &snap);
    if (made != OK)
        snap = 0;           /* ERR_ALREADY_EXISTS names somebody else's snapshot */
    TEST_ASSERT_OK(made, "a snapshot of the whole volume is taken");
    TEST_ASSERT_EQ(tagfs_alloc_blocks(STAND_REDIRECTS, &kept), 0, "64 blocks for the snapshot to keep");
    have_kept = true;
    TEST_ASSERT_EQ(tagfs_alloc_blocks(STAND_REDIRECTS, &stood), 0, "and 64 to stand in for them");
    have_stood = true;

    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_FLUSH, 0, 0, BLOCK_FAIL_NEVER);
    armed = true;
    error_t logged_one = OK;
    while (appended < STAND_REDIRECTS) {
        logged_one = TagFS_CowAfterWrite(0, kept + appended, stood + appended);
        if (logged_one != OK)
            break;
        appended++;
    }
    uint32_t flushes = BlockFailSeen();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT_OK(logged_one, "every redirect is taken");

    TEST_ASSERT_OK(DiskBookGetStats(&stats), "the redirect log's counts are read again");
    TEST_ASSERT_EQ(stats.redirects_logged - logged, STAND_REDIRECTS, "the log took all 64");
    memset(&info, 0, sizeof(info));
    TEST_ASSERT_OK(TagFS_SnapshotInfo(snap, &info), "the snapshot is found");
    TEST_ASSERT_EQ(info.redirect_count, STAND_REDIRECTS, "and it keeps all 64");

    TestEvidence("cache flushes asked inside 64 redirects, under the copy-on-write lock", flushes);
    if (flushes)
        verdict = TEST_DEFECT;
out:
    if (armed)
        BlockFailDisarm();
    if (snap)
        TagFS_SnapshotDelete(snap);     /* gives back the blocks it kept */
    if (have_kept && appended < STAND_REDIRECTS)
        tagfs_free_blocks(kept + appended, STAND_REDIRECTS - appended);
    if (have_stood)
        tagfs_free_blocks(stood, STAND_REDIRECTS);
    return verdict;
}

// ----------------------------------------------------------------------------
// A DMA wait under a caller with interrupts off
// ----------------------------------------------------------------------------

static inline uint64_t StandFlagsOff(void)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static inline uint64_t StandFlagsNow(void)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags)::"memory");
    return flags;
}

static inline void StandFlagsBack(uint64_t flags)
{
    asm volatile("push %0; popfq" ::"r"(flags) : "memory", "cc");
}

/* Eight sectors of the volume read by a caller that turned its interrupts
 * off — which is what every caller holding a TagFS lock has done. On the
 * legacy IDE channels the read is a bus-master DMA whose interrupt is routed
 * to the boot core; a wait that turned interrupts on so that interrupt could
 * arrive would do it under the caller's lock, on the caller's core, and that
 * is the defect this test stands for. What the caller's interrupts are when
 * the read comes back is the answer. The buffer is taken before interrupts go
 * off. */
static TestResult TestStandDmaWaitKeepsTheCallersInterrupts(void)
{
    TestResult verdict = TEST_PASS;
    uint8_t    seat    = tagfs_get_seat();
    uint8_t*   buffer  = NULL;

    if (BoardroomSeatKind(seat) != BOARD_ATA)
        return test_steps_aside("the volume's seat is not on the legacy IDE channels, the one road "
                                "whose DMA wait waits for an interrupt");
    if (!ata_async_usable(BoardroomSeatIndex(seat)))
        return test_steps_aside("the legacy IDE drive under the volume has no bus-master DMA, so "
                                "its reads are programmed I/O and wait for no interrupt");
    if (g_amp.total_cores < 2)
        return test_steps_aside("on one core the legacy channel is driven polled, and its wait "
                                "waits for no interrupt");

    buffer = kmalloc(STAND_DMA_SECTORS * TAGFS_SECTOR_SIZE);
    TEST_ASSERT(buffer != NULL, "a buffer for eight sectors, taken before interrupts go off");

    uint64_t flags  = StandFlagsOff();
    int      read   = tagfs_volume_read(0, STAND_DMA_SECTORS, buffer);
    uint64_t during = StandFlagsNow();
    StandFlagsBack(flags);
    TEST_ASSERT_EQ(read, 0, "eight sectors of the volume are read with interrupts off");

    bool turned_on = (during & STAND_RFLAGS_IF) != 0;
    TestEvidence("DMA reads that turned interrupts on under a caller that had them off", turned_on);
    if (turned_on)
        verdict = TEST_DEFECT;
out:
    kfree(buffer);
    return verdict;
}

// ============================================================================
// The suite — none of these grows a chain on purpose, so they walk before the
// ones that do (stand_chain_tests.c)
// ============================================================================

static const TestCase g_stand_lock_tests[] = {
    { .name  = "stand_registry_flush_holds_no_lock", .func = TestStandRegistryFlushHoldsNoLock,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.1",
      .caveat_defect = "the tag registry's flush holds its interrupts-off lock across every write "
                       "it makes to the medium, so a core that needs a tag spins with interrupts "
                       "off for as long as the medium takes" },
    { .name  = "stand_ledger_writes_outside_its_lock", .func = TestStandLedgerWritesOutsideItsLock,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.1",
      .caveat_defect = "the Ledger is written to the medium under its interrupts-off lock, so a "
                       "core that needs the Ledger spins with interrupts off for as long as the "
                       "medium takes" },
    { .name  = "stand_integrity_pages_outside_its_lock", .func = TestStandIntegrityPagesOutsideItsLock,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.1",
      .caveat_defect = "the integrity map pages itself in and out of the medium under its "
                       "interrupts-off lock, from inside every block read and write, so every "
                       "core that reads or writes a block spins with interrupts off for as long "
                       "as the medium takes" },
    { .name  = "stand_cow_question_asks_no_medium", .func = TestStandCowQuestionAsksNoMedium,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "with no snapshot of it standing, asking whether a file is copied on write "
                       "reads the file's metadata off the medium under the copy-on-write lock, "
                       "with interrupts off, on every block of every write" },
    { .name  = "stand_redirect_log_leaves_the_flush", .func = TestStandRedirectLogLeavesTheFlush,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "every 64th redirect the log takes pushes the medium's cache from inside "
                       "the copy-on-write lock, with interrupts off — on the drain core that "
                       "waits for completions only its own interrupt returns" },
    { .name  = "stand_dma_wait_keeps_the_callers_interrupts",
      .func  = TestStandDmaWaitKeepsTheCallersInterrupts,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a legacy-IDE DMA wait turns interrupts on under its caller's lock because "
                       "only the BSP's interrupt can finish it" },
};

const TestSuite g_stand_lock_suite = {
    .tests = g_stand_lock_tests, .count = ARRAY_SIZE(g_stand_lock_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
