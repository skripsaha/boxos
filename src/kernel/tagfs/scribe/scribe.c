#include "scribe.h"
#include "../tagfs.h"
#include "../lock_rank/lock_rank.h"
#include "amp.h"
#include "atomics.h"
#include "baton.h"
#include "cpu_calibrate.h"
#include "scheduler.h"
#include "system_halt.h"
#include "kheap.h"
#include "vmm.h"

// ----------------------------------------------------------------------------
// Flush tickets
// ----------------------------------------------------------------------------

static volatile uint64_t g_flush_asked;   /* tickets handed to flushes about to be issued */
static volatile uint64_t g_flush_kept;    /* the highest ticket a kept flush carried */

uint64_t VolumeFlushBegin(void)
{
    return __atomic_add_fetch(&g_flush_asked, 1u, __ATOMIC_SEQ_CST);
}

/* Two flushes may answer in either order; the mark is the highest kept. */
void VolumeFlushEnd(uint64_t ticket, bool kept)
{
    if (!kept)
        return;
    uint64_t cur = __atomic_load_n(&g_flush_kept, __ATOMIC_RELAXED);
    while (cur < ticket &&
           !__atomic_compare_exchange_n(&g_flush_kept, &cur, ticket, false,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        ;
}

uint64_t VolumeFlushNeed(void)
{
    return __atomic_load_n(&g_flush_asked, __ATOMIC_SEQ_CST) + 1u;
}

bool VolumeFlushKept(uint64_t need)
{
    return __atomic_load_n(&g_flush_kept, __ATOMIC_ACQUIRE) >= need;
}

error_t VolumeFlushFor(uint64_t need)
{
    if (VolumeFlushKept(need))
        return OK;
    return tagfs_flush_cache();
}

static volatile uint64_t g_write_need;    /* the highest need a seen write named */
static volatile uint64_t g_writes_seen;   /* every write seen, this boot */

void VolumeWriteSeen(void)
{
    __atomic_add_fetch(&g_writes_seen, 1u, __ATOMIC_RELEASE);
    uint64_t need = VolumeFlushNeed();
    uint64_t cur  = __atomic_load_n(&g_write_need, __ATOMIC_RELAXED);
    while (cur < need &&
           !__atomic_compare_exchange_n(&g_write_need, &cur, need, false,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        ;
}

bool VolumeWritesKept(void)
{
    return VolumeFlushKept(__atomic_load_n(&g_write_need, __ATOMIC_ACQUIRE));
}

uint64_t VolumeWritesAnswered(void)
{
    return __atomic_load_n(&g_writes_seen, __ATOMIC_ACQUIRE);
}

// ----------------------------------------------------------------------------
// Scribe
// ----------------------------------------------------------------------------

/* How many Scribes of each rank this core holds. Written only by the core
 * itself, with its interrupts off, so the halt's interrupt on the same core
 * reads a count that is whole. Counts, not bits: two Scribes of one rank held
 * at once are out of order, and a bit would call the core free after the
 * first of them went. */
static uint8_t g_scribe_held[MAX_CORES][SCRIBE_RANK_LAST + 1];

/* Each out-of-order pair of ranks — held, then asked for — named once. */
static volatile uint8_t g_scribe_order_said[SCRIBE_RANK_LAST + 1][SCRIBE_RANK_LAST + 1];

/* Each rank asked for inside a baton continuation, named once. */
static volatile uint8_t g_scribe_continuation_said[SCRIBE_RANK_LAST + 1];

static inline uint64_t ScribeIrqOff(void)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static inline void ScribeIrqRestore(uint64_t flags)
{
    if (flags & (1u << 9))
        asm volatile("sti" ::: "memory");
}

static bool ScribeAnyHeldOn(uint8_t core)
{
    for (uint32_t rank = SCRIBE_RANK_SETTLE; rank <= SCRIBE_RANK_LAST; rank++) {
        if (g_scribe_held[core][rank])
            return true;
    }
    return false;
}

/* Every Scribe is taken and waited for with no TagFS lock held on this core;
 * one that is not is named by the witness, and the work goes on. */
static void ScribeWitness(uintptr_t asked_by)
{
    if (LockRankAnyHeldHere())
        LockRankWitness("wait", asked_by);
}

/* A Scribe asked for while this core holds one of the same or a later rank.
 * Its own holder asking again is not an order question: that is SELF. */
static void ScribeOrderCheck(const Scribe *s, uint8_t core)
{
    if (__atomic_load_n(&s->holder, __ATOMIC_ACQUIRE) == (uint32_t)core + 1u)
        return;
    for (uint32_t rank = s->rank; rank <= SCRIBE_RANK_LAST; rank++) {
        if (!g_scribe_held[core][rank])
            continue;
        if (__atomic_exchange_n(&g_scribe_order_said[rank][s->rank], (uint8_t)1,
                                __ATOMIC_ACQ_REL) == 0)
            kprintf("[Scribe] %s asked for on core %u, which holds a Scribe of rank %u — "
                    "out of order: Scribes nest the Settle, then the registry\n",
                    s->what, (unsigned)core, (unsigned)rank);
    }
}

/* A Scribe asked for inside a continuation that will return (rule 6): the
 * drain core runs those while it waits for a Scribe, so such an ask can be
 * answered SELF by a pass this very core holds beneath it. Named once per
 * rank; the work goes on. */
static void ScribeContinuationCheck(const Scribe *s, uint8_t core)
{
    if (!BatonRunningHere())
        return;
    if (__atomic_exchange_n(&g_scribe_continuation_said[s->rank], (uint8_t)1,
                            __ATOMIC_ACQ_REL) == 0)
        kprintf("[Scribe] %s asked for inside a baton continuation on core %u — a "
                "continuation never takes a Scribe\n", s->what, (unsigned)core);
}

/* Answered by a pass that already ended? SCRIBE_HELD says not yet. The kept
 * answer is looked at first: a later pass that was kept wrote this ticket's
 * change again even when an earlier one failed on it. */
static ScribeTurn ScribeCovered(const Scribe *s, uint64_t ticket, error_t *refusal)
{
    if (__atomic_load_n(&s->written, __ATOMIC_ACQUIRE) >= ticket)
        return SCRIBE_WRITTEN;
    if (__atomic_load_n(&s->refused, __ATOMIC_ACQUIRE) >= ticket) {
        *refusal = (error_t)__atomic_load_n(&s->refusal, __ATOMIC_RELAXED);
        return SCRIBE_REFUSED;
    }
    return SCRIBE_HELD;
}

/* The take itself, with interrupts off from the exchange to the count: the
 * halt's interrupt must never find this core holding a Scribe it has not
 * counted yet, or it would stop the core with the Scribe held for good. */
static ScribeTurn ScribeTryHere(Scribe *s, uint8_t core, uint64_t ticket,
                                uint64_t *cover, error_t *refusal)
{
    ScribeTurn turn = ScribeCovered(s, ticket, refusal);
    if (turn != SCRIBE_HELD)
        return turn;

    uint32_t me    = (uint32_t)core + 1u;
    uint32_t seen  = 0;
    uint64_t flags = ScribeIrqOff();

    if (!__atomic_compare_exchange_n(&s->holder, &seen, me, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        ScribeIrqRestore(flags);
        return seen == me ? SCRIBE_SELF : SCRIBE_HELD;
    }

    /* The pass that covered the ticket may have ended between the first look
     * and the exchange. */
    turn = ScribeCovered(s, ticket, refusal);
    if (turn != SCRIBE_HELD) {
        __atomic_store_n(&s->holder, 0u, __ATOMIC_RELEASE);
        ScribeIrqRestore(flags);
        return turn;
    }

    g_scribe_held[core][s->rank]++;
    *cover = __atomic_load_n(&s->asked, __ATOMIC_SEQ_CST);
    SchedulerHoldHere();
    /* The pass is one of the kernel's own paths: what it allocates is served
     * from the heap's reserve first (kheap.h), until the Give. */
    KHeapReserveOpen();
    ScribeIrqRestore(flags);
    return SCRIBE_MINE;
}

/* One turn of every wait: the drain core runs the continuations it owes — a
 * holder on another core may be waiting for one of them — and every core
 * answers a shootdown aimed at it by hand, as spin_lock's contended loop
 * does: a waiter whose caller left its interrupts off takes no shootdown IPI,
 * and a holder's pass may take as long as its medium's patience. */
static inline void ScribeWaitTurn(uint8_t core)
{
    vmm_tlb_shootdown_poll();
    if (core == g_amp.bsp_index)
        BatonPump(core);
    cpu_pause();
}

uint64_t ScribeAsk(Scribe *s)
{
    return __atomic_add_fetch(&s->asked, 1u, __ATOMIC_SEQ_CST);
}

ScribeTurn ScribeTry(Scribe *s, uint64_t ticket, uint64_t *cover, error_t *refusal)
{
    uint8_t core = amp_get_core_index();

    ScribeWitness((uintptr_t)__builtin_return_address(0));
    ScribeOrderCheck(s, core);
    ScribeContinuationCheck(s, core);
    return ScribeTryHere(s, core, ticket, cover, refusal);
}

ScribeTurn ScribeWait(Scribe *s, uint64_t ticket, uint64_t *cover, error_t *refusal)
{
    uint8_t core = amp_get_core_index();

    ScribeWitness((uintptr_t)__builtin_return_address(0));
    ScribeOrderCheck(s, core);
    ScribeContinuationCheck(s, core);
    for (;;) {
        ScribeTurn turn = ScribeTryHere(s, core, ticket, cover, refusal);
        if (turn != SCRIBE_HELD)
            return turn;
        ScribeWaitTurn(core);
    }
}

/* The holder's core is no longer running: the pass is taken over for this
 * core, with interrupts off from the exchange to the count as a take is. The
 * stopped core's count stays where it stood — it never runs again. */
static bool ScribeTakeOver(Scribe *s, uint8_t core, uint32_t dead, uint64_t *cover)
{
    uint32_t seen  = dead;
    uint64_t flags = ScribeIrqOff();
    if (!__atomic_compare_exchange_n(&s->holder, &seen, (uint32_t)core + 1u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        ScribeIrqRestore(flags);
        return false;
    }
    g_scribe_held[core][s->rank]++;
    *cover = __atomic_load_n(&s->asked, __ATOMIC_SEQ_CST);
    SchedulerHoldHere();
    KHeapReserveOpen();
    ScribeIrqRestore(flags);
    kprintf("[Scribe] %s was held by core %u, which is no longer running — its pass is "
            "abandoned where it stood and begun again from its start on core %u\n",
            s->what, (unsigned)(dead - 1u), (unsigned)core);
    return true;
}

ScribeTurn ScribeWaitTakingOver(Scribe *s, uint64_t ticket, uint64_t *cover, error_t *refusal)
{
    uint8_t core = amp_get_core_index();

    ScribeWitness((uintptr_t)__builtin_return_address(0));
    ScribeOrderCheck(s, core);
    ScribeContinuationCheck(s, core);
    for (;;) {
        ScribeTurn turn = ScribeTryHere(s, core, ticket, cover, refusal);
        if (turn != SCRIBE_HELD)
            return turn;
        uint32_t holder = __atomic_load_n(&s->holder, __ATOMIC_ACQUIRE);
        if (holder && holder - 1u < g_amp.total_cores &&
            !amp_core_online(&g_amp.cores[holder - 1u]) && ScribeTakeOver(s, core, holder, cover))
            return SCRIBE_MINE;
        ScribeWaitTurn(core);
    }
}

ScribeTurn ScribeWaitFor(const volatile uint32_t *holder, bool (*settled)(const void *ctx),
                         const void *ctx)
{
    uint8_t  core = amp_get_core_index();
    uint32_t me   = (uint32_t)core + 1u;

    ScribeWitness((uintptr_t)__builtin_return_address(0));
    for (;;) {
        if (settled(ctx))
            return SCRIBE_WRITTEN;
        if (__atomic_load_n(holder, __ATOMIC_ACQUIRE) == me)
            return SCRIBE_SELF;
        ScribeWaitTurn(core);
    }
}

/* The last Scribe of a core an orderly halt asked to stop has gone: the pass
 * is whole on the medium, or refused whole, and the core stops here, saying
 * how long the halt waited for it. Interrupts are off. */
static void ScribeStopAfterPass(const Scribe *s, uint8_t core)
{
    uint64_t waited = rdtsc() - g_amp.cores[core].freeze_asked_tsc;

    kprintf("[HALT] core %u stops after its pass of %s, %lu us after the halt asked it to stop\n",
            (unsigned)core, s->what, (unsigned long)cpu_tsc_to_us(waited));
    HaltFreezeHere();
}

void ScribeGive(Scribe *s, uint64_t cover, error_t result)
{
    uint64_t flags = ScribeIrqOff();
    uint8_t  core  = amp_get_core_index();

    if (__atomic_load_n(&s->holder, __ATOMIC_ACQUIRE) != (uint32_t)core + 1u) {
        ScribeIrqRestore(flags);
        kprintf("[Scribe] %s given back on core %u, which does not hold it — nothing is "
                "published\n", s->what, (unsigned)core);
        return;
    }

    if (result == OK) {
        __atomic_store_n(&s->written, cover, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&s->refusal, (uint32_t)result, __ATOMIC_RELAXED);
        __atomic_store_n(&s->refused, cover, __ATOMIC_RELEASE);
    }
    g_scribe_held[core][s->rank]--;
    SchedulerLetGoHere();
    KHeapReserveClose();
    __atomic_store_n(&s->holder, 0u, __ATOMIC_RELEASE);

    if (__atomic_load_n(&g_amp.cores[core].freeze_owed, __ATOMIC_RELAXED) &&
        !SchedulerHeldHere())
        ScribeStopAfterPass(s, core);
    ScribeIrqRestore(flags);
}

bool ScribeHeldHere(const Scribe *s)
{
    return __atomic_load_n(&s->holder, __ATOMIC_ACQUIRE) ==
           (uint32_t)amp_get_core_index() + 1u;
}

bool ScribeAnyHeldHere(void)
{
    uint64_t flags = ScribeIrqOff();
    bool     any   = ScribeAnyHeldOn(amp_get_core_index());
    ScribeIrqRestore(flags);
    return any;
}

// ----------------------------------------------------------------------------
// Spare
// ----------------------------------------------------------------------------

error_t SpareStamp(uint32_t magic, void *image, uint32_t *out_block, uint64_t *out_need)
{
    uint32_t block = 0;
    error_t  took  = magic == TAGFS_MPOOL_MAGIC ? BlockMapTakeSpare(&block, __func__)
                                                : BlockMapTake(1, 0, &block, __func__);
    if (took != OK)
        return took;

    memset(image, 0, TAGFS_BLOCK_SIZE);
    memcpy(image, &magic, sizeof(magic));
    error_t wrote = TagFSMetaBlockWrite(block, image);
    if (wrote != OK) {
        tagfs_free_blocks(block, 1);
        return wrote;
    }

    *out_block = block;
    *out_need  = VolumeFlushNeed();
    return OK;
}
