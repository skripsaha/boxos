#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../integrity/integrity.h"
#include "block_fail.h"
#include "boardroom.h"
#include "ata_async.h"
#include "amp.h"
#include "../settle/settle.h"

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

    uint32_t fresh_id;
    TEST_ASSERT(tagfs_tag_intern(StandFreshFlushTag(), &fresh_id) == OK,
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

/* One Ledger write, made by its one writer — a Settle cycle — and the medium's
 * newest copy read back to prove it was made. */
static TestResult TestStandLedgerWritesOutsideItsLock(void)
{
    TestResult verdict   = TEST_PASS;
    uint64_t   seq       = StandLedgerSeq();
    uint32_t   witnessed = g_ledger_lock.Witnessed;
    error_t    wrote     = TagFSSettle();
    uint32_t   crossed   = g_ledger_lock.Witnessed - witnessed;

    TEST_ASSERT(seq > 0, "a Ledger copy reads before the write");
    TEST_ASSERT_OK(wrote, "a Settle cycle writes the Ledger");
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
 * others push them aside. A Settle cycle first leaves no page owed from before.
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
    uint64_t    first   = 0;
    bool        taken   = false;
    error_t     wrote   = OK;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(IntegrityIsInitialized(), "the volume's integrity map is up");
    TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle first, so no page of the map is owed to the medium");
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
      .caveat_defect = "the tag registry's flush holds its interrupts-off lock across every write "
                       "it makes to the medium, so a core that needs a tag spins with interrupts "
                       "off for as long as the medium takes" },
    { .name  = "stand_ledger_writes_outside_its_lock", .func = TestStandLedgerWritesOutsideItsLock,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "the Ledger is written to the medium under its interrupts-off lock, so a "
                       "core that needs the Ledger spins with interrupts off for as long as the "
                       "medium takes" },
    { .name  = "stand_integrity_pages_outside_its_lock", .func = TestStandIntegrityPagesOutsideItsLock,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "the integrity map pages itself in and out of the medium under its "
                       "interrupts-off lock, from inside every block read and write, so every "
                       "core that reads or writes a block spins with interrupts off for as long "
                       "as the medium takes" },
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
