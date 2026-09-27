#include "block_fail.h"
#include "klib.h"

#ifdef CONFIG_VOLUME_TESTS

/*
 * The plan, under a sequence word.
 *
 * The plan is only ever rewritten while the word is even (nothing armed), and
 * a reader takes a copy between two looks at the word: an odd word that did
 * not move is a plan that was whole the whole time. So the question every
 * command asks costs one load of one word while nothing is armed, and no lock
 * is taken on a road that runs on every core.
 */
typedef struct {
    uint8_t  seat;
    uint8_t  kind;
    uint64_t first;
    uint64_t end;           /* 0: the whole seat */
    uint32_t nth;
} BlockFailPlan;

static BlockFailPlan     g_block_fail;
static volatile uint32_t g_block_fail_gen;      /* odd while a plan stands */
static volatile uint32_t g_block_fail_seen;
static volatile uint32_t g_block_fail_fired;

bool BlockFailDisarm(void)
{
    uint32_t g = __atomic_load_n(&g_block_fail_gen, __ATOMIC_ACQUIRE);
    while (g & 1u) {
        if (__atomic_compare_exchange_n(&g_block_fail_gen, &g, g + 1u, false,
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
    __atomic_store_n(&g_block_fail_seen, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_block_fail_fired, 0u, __ATOMIC_RELAXED);

    __atomic_add_fetch(&g_block_fail_gen, 1u, __ATOMIC_RELEASE);   /* even -> odd */
}

uint32_t BlockFailSeen(void)
{
    return __atomic_load_n(&g_block_fail_seen, __ATOMIC_ACQUIRE);
}

bool BlockFailFired(void)
{
    return __atomic_load_n(&g_block_fail_fired, __ATOMIC_ACQUIRE) != 0;
}

bool BlockFailStrikes(uint8_t seat, BlockFailKind kind, uint64_t lba, uint32_t count)
{
    uint32_t g = __atomic_load_n(&g_block_fail_gen, __ATOMIC_ACQUIRE);
    if (!(g & 1u))
        return false;

    BlockFailPlan plan = g_block_fail;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_block_fail_gen, __ATOMIC_RELAXED) != g)
        return false;           /* rewritten while it was read: not this plan's command */

    if (plan.seat != seat || plan.kind != (uint8_t)kind)
        return false;
    if (kind != BLOCK_FAIL_FLUSH && plan.end != 0 &&
        (lba + count <= plan.first || lba >= plan.end))
        return false;

    uint32_t n = __atomic_add_fetch(&g_block_fail_seen, 1u, __ATOMIC_ACQ_REL);
    if (n != plan.nth)
        return false;
    if (__atomic_exchange_n(&g_block_fail_fired, 1u, __ATOMIC_ACQ_REL) != 0)
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

#endif /* CONFIG_VOLUME_TESTS */
