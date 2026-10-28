#include "block_fail.h"
#include "klib.h"
#include "cpu_calibrate.h"
#include "atomics.h"

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
    bool     bad;           /* every command of its kind from the nth: sectors the medium cannot read,
                             * or will not write */
    uint64_t first;
    uint64_t end;           /* 0: the whole seat */
    uint32_t nth;
    uint32_t more;          /* a bad-sector plan's ranges past the first */
    uint64_t more_first[BLOCK_FAIL_BAD_RANGES - 1u];
    uint64_t more_end[BLOCK_FAIL_BAD_RANGES - 1u];
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

void BlockFailArmBadSectors(uint8_t seat, uint64_t first_sector, uint64_t sectors)
{
    BlockFailPlan plan = {
        .seat  = seat,
        .kind  = (uint8_t)BLOCK_FAIL_READ,
        .bad   = true,
        .first = first_sector,
        .end   = sectors ? first_sector + sectors : 0,
        .nth   = 1u,
    };
    BlockFailStand(&plan);
}

void BlockFailArmRefusedWrites(uint8_t seat, uint64_t first_sector, uint64_t sectors)
{
    BlockFailPlan plan = {
        .seat  = seat,
        .kind  = (uint8_t)BLOCK_FAIL_WRITE,
        .bad   = true,
        .first = first_sector,
        .end   = sectors ? first_sector + sectors : 0,
        .nth   = 1u,
    };
    BlockFailStand(&plan);
}

bool BlockFailAddBadSectors(uint8_t seat, uint64_t first_sector, uint64_t sectors)
{
    uint64_t s = __atomic_load_n(&g_block_fail_state, __ATOMIC_ACQUIRE);
    BlockFailPlan plan = g_block_fail;
    if (!((s >> 32) & 1u) || !plan.bad || plan.seat != seat || plan.end == 0 || sectors == 0 ||
        plan.more == BLOCK_FAIL_BAD_RANGES - 1u)
        return false;
    plan.more_first[plan.more] = first_sector;
    plan.more_end[plan.more]   = first_sector + sectors;
    plan.more++;
    BlockFailStand(&plan);
    return true;
}

/* Whether [lba, lba + count) meets a range of the plan; and in *before the
 * sectors of it before the first sector of the plan it reaches — 0 when it
 * begins inside a range. */
static bool BlockFailMeets(const BlockFailPlan* plan, uint64_t lba, uint32_t count, uint64_t* before)
{
    bool met = false;
    *before = count;
    for (uint32_t i = 0; i <= plan->more; i++) {
        uint64_t first = i ? plan->more_first[i - 1u] : plan->first;
        uint64_t end   = i ? plan->more_end[i - 1u] : plan->end;
        if (lba + count <= first || lba >= end)
            continue;
        met = true;
        if (first <= lba)
            *before = 0;
        else if (first - lba < *before)
            *before = first - lba;
    }
    return met;
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
                                  uint32_t* keep, BlockFailPlan* out)
{
    if (keep)
        *keep = 0;
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
    uint64_t ahead = 0;
    if (kind != BLOCK_FAIL_FLUSH && plan.end != 0 && !BlockFailMeets(&plan, lba, count, &ahead))
        return false;

    /* Counted, and chosen or not, in one swap under the generation the plan
     * was read under. */
    uint32_t n;
    bool     fire;
    do {
        if ((s >> 32) != g)
            return false;       /* taken down, or armed again, since the plan was read */
        n    = (((uint32_t)(s >> 1) & BLOCK_FAIL_SEEN_MAX) + 1u) & BLOCK_FAIL_SEEN_MAX;
        fire = plan.bad ? n >= plan.nth : n == plan.nth && !(s & 1u);
    } while (!__atomic_compare_exchange_n(&g_block_fail_state, &s,
                                          (s & ~(BLOCK_FAIL_GEN_ONE - 1u)) |
                                              ((uint64_t)n << 1) | (s & 1u) | fire,
                                          true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    if (!fire)
        return false;

    uint32_t before = keep && kind != BLOCK_FAIL_FLUSH ? (uint32_t)ahead : 0;
    if (kind == BLOCK_FAIL_FLUSH && (plan.sense[0] | plan.sense[1] | plan.sense[2])) {
        kprintf("[BLOCKFAIL] seat %u: the cache flush is answered as refused with sense key "
                "0x%x, code 0x%02x/0x%02x — occurrence %u since arming, and it was never "
                "issued to the medium\n", seat, plan.sense[0], plan.sense[1], plan.sense[2], n);
    } else if (plan.bad && kind == BLOCK_FAIL_WRITE) {
        if (keep)
            *keep = before;
        kprintf("[BLOCKFAIL] seat %u: the write of sectors %llu..%llu is refused as a medium error "
                "from sector %llu on, as every write there is — occurrence %u since arming: the %u "
                "sector(s) before it reach the medium, and nothing from it on is issued\n", seat,
                (unsigned long long)lba, (unsigned long long)(lba + count - 1),
                (unsigned long long)(lba + before), n, before);
    } else if (plan.bad) {
        kprintf("[BLOCKFAIL] seat %u: the read of sectors %llu..%llu is answered as a sector "
                "the medium cannot read (UNC, MEDIUM ERROR) — occurrence %u since arming, and "
                "nothing of it was issued to the medium\n", seat, (unsigned long long)lba,
                (unsigned long long)(lba + count - 1), n);
    } else if (kind == BLOCK_FAIL_FLUSH) {
        kprintf("[BLOCKFAIL] seat %u: the cache flush is refused as a medium "
                "error — occurrence %u since arming, and it was never issued to "
                "the medium\n", seat, n);
    } else if (before) {
        *keep = before;
        kprintf("[BLOCKFAIL] seat %u: the %s of sectors %llu..%llu is refused as "
                "a medium error from sector %llu on — occurrence %u since arming: "
                "the %u sector(s) before it reach the medium, and nothing from it "
                "on is issued\n", seat,
                kind == BLOCK_FAIL_READ ? "read" : "write",
                (unsigned long long)lba, (unsigned long long)(lba + count - 1),
                (unsigned long long)(lba + before), n, before);
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

bool BlockFailStrikes(uint8_t seat, BlockFailKind kind, uint64_t lba, uint32_t count,
                      uint32_t* keep)
{
    BlockFailPlan plan;
    return BlockFailStrikesUnder(seat, kind, lba, count, keep, &plan);
}

bool BlockFailReadStrikes(uint8_t seat, uint64_t lba, uint32_t count, bool* bad)
{
    BlockFailPlan plan;
    bool          fired = BlockFailStrikesUnder(seat, BLOCK_FAIL_READ, lba, count, NULL, &plan);
    *bad = fired && plan.bad;
    return fired;
}

bool BlockFailFlushStrikes(uint8_t seat, uint8_t out_sense[3])
{
    BlockFailPlan plan;
    bool          fired = BlockFailStrikesUnder(seat, BLOCK_FAIL_FLUSH, 0, 0, NULL, &plan);
    memcpy(out_sense, fired ? plan.sense : (const uint8_t[3]){ 0, 0, 0 }, 3);
    return fired;
}

/* The seat a protection stands on, + 1; 0 for none. Said without the colon a
 * refusal is said with: a protection is a state the seat is put in, not a
 * command refused, and whoever counts the refusals counts those lines alone. */
static volatile uint32_t g_block_fail_protected;

void BlockFailProtect(uint8_t seat, bool on)
{
    __atomic_store_n(&g_block_fail_protected, on ? (uint32_t)seat + 1u : 0u, __ATOMIC_RELEASE);
    kprintf("[BLOCKFAIL] seat %u %s\n", seat,
            on ? "protects its medium — every write is refused from here as a protected "
                 "medium refuses it"
               : "protects its medium no longer — writes reach the medium again");
}

bool BlockFailProtects(uint8_t seat)
{
    return __atomic_load_n(&g_block_fail_protected, __ATOMIC_ACQUIRE) == (uint32_t)seat + 1u;
}

/* The seek the stand's medium is given: its microseconds above, the seat + 1
 * below — one word, so a read on any core sees a whole lag or none. */
static volatile uint64_t g_block_fail_lag;

void BlockFailReadLag(uint8_t seat, uint32_t us)
{
    __atomic_store_n(&g_block_fail_lag, us ? (uint64_t)us << 32 | ((uint32_t)seat + 1u) : 0u, __ATOMIC_RELEASE);
    kprintf("[BLOCKFAIL] seat %u %s %u us\n", seat, us ? "seeks before every read:" : "seeks no more: was", us);
}

/* The pace the stand names: written whole before it is pinned, and read
 * after the pin is seen — a test names it between cycles. */
static volatile uint64_t g_block_fail_rate;
static volatile uint64_t g_block_fail_seek_us;
static volatile uint8_t  g_block_fail_paced;

void BlockFailReadPace(bool pinned, uint64_t rate, uint64_t seek_us)
{
    __atomic_store_n(&g_block_fail_paced, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_block_fail_rate, rate, __ATOMIC_RELAXED);
    __atomic_store_n(&g_block_fail_seek_us, seek_us, __ATOMIC_RELAXED);
    __atomic_store_n(&g_block_fail_paced, pinned ? 1u : 0u, __ATOMIC_RELEASE);
}

bool BlockFailReadPaced(uint64_t* rate, uint64_t* seek_us)
{
    if (!__atomic_load_n(&g_block_fail_paced, __ATOMIC_ACQUIRE))
        return false;
    *rate    = __atomic_load_n(&g_block_fail_rate, __ATOMIC_RELAXED);
    *seek_us = __atomic_load_n(&g_block_fail_seek_us, __ATOMIC_RELAXED);
    return true;
}

void BlockFailReadLagged(uint8_t seat)
{
    uint64_t lag = __atomic_load_n(&g_block_fail_lag, __ATOMIC_ACQUIRE);
    if (!lag || (uint32_t)lag != (uint32_t)seat + 1u)
        return;
    uint64_t until = rdtsc() + cpu_us_to_tsc(lag >> 32);
    while (rdtsc() < until)
        cpu_pause();
}

#endif /* CONFIG_VOLUME_TESTS */
