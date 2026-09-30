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
    uint8_t  sense[3];      /* a flush's SCSI answer; all zero: a medium error */
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

/* The plan written while the generation is even, then stood up: even -> odd,
 * with the count and the refusal cleared in the same store. Nothing else
 * writes the word while the generation is even: a strike swaps only under the
 * odd generation it read. */
static void BlockFailStand(const BlockFailPlan* plan)
{
    BlockFailDisarm();
    g_block_fail = *plan;

    uint64_t s = __atomic_load_n(&g_block_fail_state, __ATOMIC_RELAXED);
    __atomic_store_n(&g_block_fail_state, (s & ~(BLOCK_FAIL_GEN_ONE - 1u)) + BLOCK_FAIL_GEN_ONE,
                     __ATOMIC_RELEASE);
}

void BlockFailArm(uint8_t seat, BlockFailKind kind,
                  uint64_t first_sector, uint64_t sectors, uint32_t nth)
{
    BlockFailPlan plan = {
        .seat  = seat,
        .kind  = (uint8_t)kind,
        .first = first_sector,
        .end   = sectors ? first_sector + sectors : 0,
        .nth   = nth ? nth : 1u,
    };
    BlockFailStand(&plan);
}

void BlockFailArmFlushSense(uint8_t seat, uint8_t key, uint8_t asc, uint8_t ascq, uint32_t nth)
{
    BlockFailPlan plan = {
        .seat  = seat,
        .kind  = (uint8_t)BLOCK_FAIL_FLUSH,
        .sense = { key, asc, ascq },
        .nth   = nth ? nth : 1u,
    };
    BlockFailStand(&plan);
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

/* The strike, and in *out the plan it struck under. */
static bool BlockFailStrikesUnder(uint8_t seat, BlockFailKind kind, uint64_t lba, uint32_t count,
                                  BlockFailPlan* out)
{
    uint64_t s = __atomic_load_n(&g_block_fail_state, __ATOMIC_ACQUIRE);
    uint64_t g = s >> 32;
    if (!(g & 1u))
        return false;

    BlockFailPlan plan = g_block_fail;
    *out = plan;
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

    if (kind == BLOCK_FAIL_FLUSH && (plan.sense[0] | plan.sense[1] | plan.sense[2])) {
        kprintf("[BLOCKFAIL] seat %u: the cache flush is answered as refused with sense key "
                "0x%x, code 0x%02x/0x%02x — occurrence %u since arming, and it was never "
                "issued to the medium\n", seat, plan.sense[0], plan.sense[1], plan.sense[2], n);
    } else if (kind == BLOCK_FAIL_FLUSH) {
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

bool BlockFailStrikes(uint8_t seat, BlockFailKind kind, uint64_t lba, uint32_t count)
{
    BlockFailPlan plan;
    return BlockFailStrikesUnder(seat, kind, lba, count, &plan);
}

bool BlockFailFlushStrikes(uint8_t seat, uint8_t out_sense[3])
{
    BlockFailPlan plan;
    bool          fired = BlockFailStrikesUnder(seat, BLOCK_FAIL_FLUSH, 0, 0, &plan);
    memcpy(out_sense, fired ? plan.sense : (const uint8_t[3]){ 0, 0, 0 }, 3);
    return fired;
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
