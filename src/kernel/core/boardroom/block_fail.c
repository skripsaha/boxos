#include "block_fail.h"
#include "klib.h"

#ifdef CONFIG_VOLUME_TESTS

/*
 * The plan, and everything a strike counts against it, under one word.
 *
 *   bits 63..32  the generation — odd while a plan stands
 *   bits 31..1   the matching commands the plan has met
 *   bit  0       whether it has refused one
 *
 * The plan itself is only ever rewritten while the generation is even, and a
 * reader copies it between two looks at the word: an odd generation that did
 * not move is a plan that was whole the whole time. The count and the refusal
 * are in the same word as the generation, and a strike moves them with one
 * compare-and-swap that also asks the generation it read the plan under. So a
 * strike that read one plan can never count toward the plan armed after it,
 * nor fire as it: the moment the generation moves, its swap fails and its
 * command goes through uncounted. The unarmed question still costs one load
 * of one word, and no lock is taken on a road that runs on every core.
 */
typedef struct {
    uint8_t  seat;
    uint8_t  kind;
    uint64_t first;
    uint64_t end;           /* 0: the whole seat */
    uint32_t nth;
} BlockFailPlan;

#define BLOCK_FAIL_GEN_ONE  ((uint64_t)1 << 32)
#define BLOCK_FAIL_SEEN_MAX 0x7FFFFFFFu

static BlockFailPlan     g_block_fail;
static volatile uint64_t g_block_fail_state;

bool BlockFailDisarm(void)
{
    uint64_t s = __atomic_load_n(&g_block_fail_state, __ATOMIC_ACQUIRE);
    while ((s >> 32) & 1u) {
        if (__atomic_compare_exchange_n(&g_block_fail_state, &s, s + BLOCK_FAIL_GEN_ONE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

void BlockFailArm(uint8_t seat, BlockFailKind kind,
                  uint64_t first_sector, uint64_t sectors, uint32_t nth)
{
    BlockFailDisarm();

    g_block_fail.seat  = seat;
    g_block_fail.kind  = (uint8_t)kind;
    g_block_fail.first = first_sector;
    g_block_fail.end   = sectors ? first_sector + sectors : 0;
    g_block_fail.nth   = nth ? nth : 1u;

    /* Even -> odd, with the count and the refusal cleared in the same store.
     * Nothing else writes the word while the generation is even: a strike
     * swaps only under the odd generation it read. */
    uint64_t s = __atomic_load_n(&g_block_fail_state, __ATOMIC_RELAXED);
    __atomic_store_n(&g_block_fail_state, (s & ~(BLOCK_FAIL_GEN_ONE - 1u)) + BLOCK_FAIL_GEN_ONE,
                     __ATOMIC_RELEASE);
}

uint32_t BlockFailSeen(void)
{
    return (uint32_t)(__atomic_load_n(&g_block_fail_state, __ATOMIC_ACQUIRE) >> 1) &
           BLOCK_FAIL_SEEN_MAX;
}

bool BlockFailFired(void)
{
    return (__atomic_load_n(&g_block_fail_state, __ATOMIC_ACQUIRE) & 1u) != 0;
}

bool BlockFailStrikes(uint8_t seat, BlockFailKind kind, uint64_t lba, uint32_t count)
{
    uint64_t s = __atomic_load_n(&g_block_fail_state, __ATOMIC_ACQUIRE);
    uint64_t g = s >> 32;
    if (!(g & 1u))
        return false;

    BlockFailPlan plan = g_block_fail;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if ((__atomic_load_n(&g_block_fail_state, __ATOMIC_RELAXED) >> 32) != g)
        return false;           /* rewritten while it was read: not this plan's command */

    if (plan.seat != seat || plan.kind != (uint8_t)kind)
        return false;
    if (kind != BLOCK_FAIL_FLUSH && plan.end != 0 &&
        (lba + count <= plan.first || lba >= plan.end))
        return false;

    /* Counted, and chosen or not, in one swap under the generation the plan
     * was read under. */
    uint32_t n;
    bool     fire;
    do {
        if ((s >> 32) != g)
            return false;       /* taken down, or armed again, since the plan was read */
        n    = (((uint32_t)(s >> 1) & BLOCK_FAIL_SEEN_MAX) + 1u) & BLOCK_FAIL_SEEN_MAX;
        fire = n == plan.nth && !(s & 1u);
    } while (!__atomic_compare_exchange_n(&g_block_fail_state, &s,
                                          (s & ~(BLOCK_FAIL_GEN_ONE - 1u)) |
                                              ((uint64_t)n << 1) | (s & 1u) | fire,
                                          true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    if (!fire)
        return false;

    if (kind == BLOCK_FAIL_FLUSH) {
        kprintf("[BLOCKFAIL] seat %u: the cache flush is refused as a medium "
                "error — occurrence %u since arming, and it was never issued to "
                "the medium\n", seat, n);
    } else {
        kprintf("[BLOCKFAIL] seat %u: the %s of sectors %llu..%llu is refused as "
                "a medium error — occurrence %u since arming, and nothing of it "
                "was issued to the medium\n", seat,
                kind == BLOCK_FAIL_READ ? "read" : "write",
                (unsigned long long)lba,
                (unsigned long long)(lba + count - 1), n);
    }
    return true;
}

/* The seat a protection stands on, + 1; 0 for none. */
static volatile uint32_t g_block_fail_protected;

void BlockFailProtect(uint8_t seat, bool on)
{
    __atomic_store_n(&g_block_fail_protected, on ? (uint32_t)seat + 1u : 0u, __ATOMIC_RELEASE);
    kprintf("[BLOCKFAIL] seat %u: %s\n", seat,
            on ? "every write is refused from here as a protected medium refuses it"
               : "writes reach the medium again");
}

bool BlockFailProtects(uint8_t seat)
{
    return __atomic_load_n(&g_block_fail_protected, __ATOMIC_ACQUIRE) == (uint32_t)seat + 1u;
}

#endif /* CONFIG_VOLUME_TESTS */
