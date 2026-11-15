#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
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

#define STAND_DMA_SECTORS     8u      /* one legacy-IDE DMA transfer (ATA_ASYNC_MAX_SECTORS) */
#define STAND_RFLAGS_IF       (1u << 9)

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
