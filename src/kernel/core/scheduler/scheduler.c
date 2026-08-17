#include "scheduler.h"
#include "nightwatch.h"
#include "use_context.h"
#include "klib.h"
#include "process.h"
#include "context_switch.h"
#include "idt.h"
#include "tss.h"
#include "atomics.h"
#include "idle.h"
#include "guide.h"
#include "ready_queue.h"
#include "xhci_interrupt.h"
#include "notify.h"
#include "tagfs.h"
#include "per_core.h"
#include "amp.h"
#include "pit.h"
#include "lapic.h"     /* lapic_send_ipi — directed reschedule IPI on cross-core enqueue */
#include "irqchip.h"   /* IPI_WAKE_VECTOR */

// ---------------------------------------------------------------------------
// Dynamic Scheduler Parameters
// ---------------------------------------------------------------------------

const uint32_t g_scheduler_fairness_base       = 4;
const uint32_t g_scheduler_starvation_base     = 25;
const uint32_t g_scheduler_steal_cooldown_base = 10;
uint32_t g_timer_frequency                     = SCHEDULER_DEFAULT_TICK_HZ;

// Calculated dynamic values
uint32_t g_dynamic_fairness_ratio;
uint32_t g_dynamic_starvation_ticks;
uint32_t g_dynamic_steal_cooldown;

// Per-core process counters — atomic, lock-free
static volatile uint32_t *g_core_context_count;
static volatile uint32_t *g_core_normal_count;

// ---------------------------------------------------------------------------
// Global monotonic tick
// ---------------------------------------------------------------------------
volatile uint64_t g_global_tick = 0;

// ---------------------------------------------------------------------------
// Per-core scheduler array — dynamically allocated
// ---------------------------------------------------------------------------
static scheduler_state_t *g_core_sched;
static uint32_t g_sched_core_count;

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------
scheduler_stats_t g_sched_stats;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

static void sched_init_one(scheduler_state_t *s)
{
    memset(s, 0, sizeof(scheduler_state_t));
    s->current_process = NULL;
    spinlock_init(&s->scheduler_lock);
    s->total_ticks = 0;
    s->normal_ticks = 0;
    s->last_steal_tick = 0;
    s->is_parked = false;
    s->idle_tick_count = 0;
    runqueue_init(&s->runqueue);
}

error_t scheduler_init(void)
{
    debug_printf("[SCHEDULER] Initializing per-core O(1) scheduler...\n");

    UseContextInit();

    memset(&g_sched_stats, 0, sizeof(g_sched_stats));

    g_sched_core_count = g_amp.total_cores;
    if (g_sched_core_count == 0 || g_sched_core_count > MAX_CORES) {
        debug_printf("[SCHEDULER] ERROR: Invalid core count %u\n", g_sched_core_count);
        return ERR_INVALID_ARGUMENT;
    }

    g_core_sched         = kmalloc(sizeof(scheduler_state_t) * g_sched_core_count);
    g_core_context_count = kmalloc(sizeof(uint32_t) * g_sched_core_count);
    g_core_normal_count  = kmalloc(sizeof(uint32_t) * g_sched_core_count);

    if (!g_core_sched || !g_core_context_count || !g_core_normal_count) {
        debug_printf("[SCHEDULER] ERROR: Failed to allocate scheduler structures\n");
        if (g_core_sched)         { kfree(g_core_sched);                g_core_sched = NULL; }
        if (g_core_context_count) { kfree((void *)g_core_context_count); g_core_context_count = NULL; }
        if (g_core_normal_count)  { kfree((void *)g_core_normal_count);  g_core_normal_count = NULL; }
        g_sched_core_count = 0;
        return ERR_NO_MEMORY;
    }

    for (uint32_t i = 0; i < g_sched_core_count; i++)
    {
        sched_init_one(&g_core_sched[i]);
        __atomic_store_n(&g_core_context_count[i], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_core_normal_count[i], 0, __ATOMIC_RELAXED);
    }

    scheduler_recalc_parameters();

    debug_printf("[SCHEDULER] Per-core O(1) scheduler ready (%u Hz, %u cores, init_cap=%u)\n",
                 g_timer_frequency, g_sched_core_count, RUNQUEUE_INITIAL_CAP);
    return OK;
}

void scheduler_init_core(uint8_t core_index)
{
    if (!g_core_sched || core_index >= g_sched_core_count) return;
    scheduler_state_t *s = &g_core_sched[core_index];

    /* scheduler_init() already called sched_init_one() on every slot during
     * BSP startup; calling it again here would memset the state to zero and
     * re-kmalloc every runqueue->queues[i].procs array, leaking the original
     * memory and clobbering whatever's already enqueued. APs only need to
     * notice their state was already prepared. */
    if (s->runqueue.queues[0].procs != NULL) {
        debug_printf("[SCHEDULER] Core %u already initialized — skipping double init\n", core_index);
        return;
    }

    sched_init_one(s);
    debug_printf("[SCHEDULER] Core %u scheduler initialized\n", core_index);
}

void scheduler_shutdown(void)
{
    if (!g_core_sched) return;

    for (uint32_t i = 0; i < g_sched_core_count; i++) {
        runqueue_shutdown(&g_core_sched[i].runqueue);
    }
    kfree(g_core_sched);
    g_core_sched = NULL;
    if (g_core_context_count) {
        kfree((void *)g_core_context_count);
        g_core_context_count = NULL;
    }
    if (g_core_normal_count) {
        kfree((void *)g_core_normal_count);
        g_core_normal_count = NULL;
    }
    g_sched_core_count = 0;
    UseContextShutdown();
    debug_printf("[SCHEDULER] Shutdown complete\n");
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

scheduler_state_t *scheduler_get_state(void)
{
    if (!g_core_sched) return NULL;
    uint8_t idx = amp_get_core_index();
    if (idx >= g_sched_core_count) return NULL;
    return &g_core_sched[idx];
}

scheduler_state_t *scheduler_get_core(uint8_t core_idx)
{
    if (!g_core_sched || core_idx >= g_sched_core_count) return NULL;
    return &g_core_sched[core_idx];
}

// ---------------------------------------------------------------------------
// Core Parking
// ---------------------------------------------------------------------------

void scheduler_park_core(uint8_t core_idx)
{
    if (!g_core_sched || core_idx >= g_sched_core_count) return;
    scheduler_state_t *s = &g_core_sched[core_idx];
    if (!__atomic_load_n(&s->is_parked, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s->is_parked, true, __ATOMIC_RELEASE);
        g_sched_stats.parked_cores++;
        g_sched_stats.active_cores--;
        debug_printf("[SCHED] Core %u parked\n", core_idx);
    }
}

void scheduler_unpark_core(uint8_t core_idx)
{
    if (!g_core_sched || core_idx >= g_sched_core_count) return;
    scheduler_state_t *s = &g_core_sched[core_idx];
    if (__atomic_load_n(&s->is_parked, __ATOMIC_ACQUIRE)) {
        s->idle_tick_count = 0;
        __atomic_store_n(&s->is_parked, false, __ATOMIC_RELEASE);
        g_sched_stats.parked_cores--;
        g_sched_stats.active_cores++;
        debug_printf("[SCHED] Core %u unparked\n", core_idx);
    }
}

bool scheduler_is_core_parked(uint8_t core_idx)
{
    if (!g_core_sched || core_idx >= g_sched_core_count)
        return false;
    return __atomic_load_n(&g_core_sched[core_idx].is_parked, __ATOMIC_ACQUIRE);
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

scheduler_stats_t scheduler_get_stats(void)
{
    return g_sched_stats;
}

// ---------------------------------------------------------------------------
// Dynamic Parameter Recalculation — O(1), lock-free, called every tick
// Uses pre-computed atomic counters updated on enqueue/dequeue
// ---------------------------------------------------------------------------

void scheduler_recalc_parameters(void)
{
    if (!g_core_sched || !g_core_context_count || !g_core_normal_count || g_sched_core_count == 0)
        return;

    // Throttle: only recalculate every SCHED_RECALC_INTERVAL global ticks.
    // Called from BSP PIT IRQ only, so no concurrent access to s_last_recalc.
    static uint64_t s_last_recalc = 0;
    uint64_t now = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    if (now - s_last_recalc < SCHED_RECALC_INTERVAL)
        return;
    s_last_recalc = now;

    // Sum atomic counters — O(cores) but very fast (no locks)
    uint32_t total_context = 0;
    uint32_t total_normal = 0;
    uint32_t busy_cores = 0;

    for (uint32_t c = 0; c < g_sched_core_count; c++) {
        total_context += __atomic_load_n(&g_core_context_count[c], __ATOMIC_RELAXED);
        total_normal += __atomic_load_n(&g_core_normal_count[c], __ATOMIC_RELAXED);

        scheduler_state_t *s = &g_core_sched[c];
        uint32_t rq_count = RunqueueAtomicTotal(&s->runqueue);

        // Read current_process under lock to prevent torn pointer reads on SMP
        spin_lock(&s->scheduler_lock);
        process_t *cur = s->current_process;
        spin_unlock(&s->scheduler_lock);

        if (rq_count > 0 || (cur && !process_is_idle(cur))) {
            busy_cores++;
        }

        // Core parking
        bool parked = __atomic_load_n(&s->is_parked, __ATOMIC_ACQUIRE);
        if (!parked && rq_count == 0 && (!cur || process_is_idle(cur))) {
            s->idle_tick_count++;
            if (s->idle_tick_count >= SCHEDULER_PARK_IDLE_TICKS) {
                scheduler_park_core(c);
            }
        } else if (parked && rq_count >= SCHEDULER_UNPARK_LOAD_THRESH) {
            scheduler_unpark_core(c);
        } else if (!parked) {
            s->idle_tick_count = 0;
        }
    }

    // Dynamic fairness
    if (total_context > 0 && total_normal > 0) {
        g_dynamic_fairness_ratio = g_scheduler_fairness_base +
                                   (total_context / (total_normal + 1));
    } else {
        g_dynamic_fairness_ratio = 1;
    }

    if (g_dynamic_fairness_ratio < SCHEDULER_MIN_FAIRNESS)
        g_dynamic_fairness_ratio = SCHEDULER_MIN_FAIRNESS;
    if (g_dynamic_fairness_ratio > SCHEDULER_MAX_FAIRNESS)
        g_dynamic_fairness_ratio = SCHEDULER_MAX_FAIRNESS;

    // Dynamic starvation
    uint32_t total_procs = total_context + total_normal;
    g_dynamic_starvation_ticks = g_scheduler_starvation_base + (total_procs / 4);

    if (g_dynamic_starvation_ticks < SCHEDULER_MIN_STARVATION)
        g_dynamic_starvation_ticks = SCHEDULER_MIN_STARVATION;
    if (g_dynamic_starvation_ticks > SCHEDULER_MAX_STARVATION)
        g_dynamic_starvation_ticks = SCHEDULER_MAX_STARVATION;

    // Dynamic steal cooldown
    g_dynamic_steal_cooldown = g_scheduler_steal_cooldown_base + (g_sched_core_count / 2);

    // Update stats
    g_sched_stats.active_cores = busy_cores;

    /* PIT frequency policy.
     *
     * Past revisions adaptively reprogrammed the 8254 PIT (10-500 Hz
     * range) based on `busy_cores` to "save power when idle". In
     * practice this caused three real-HW problems:
     *
     *  1. Latency spikes when load dropped to 0: PIT at 10 Hz means
     *     100 ms before the next scheduler tick can wake new work.
     *     User-visible as "system feels frozen" right after a job
     *     ends — exactly what the user reported on Bochs/QEMU.
     *
     *  2. Non-deterministic timing for any subsystem that derives
     *     its rate from g_timer_frequency (keyboard typematic ticks,
     *     irq_defer batching, scheduler starvation thresholds). The
     *     timer rate becoming load-dependent makes every other
     *     subsystem load-dependent too.
     *
     *  3. PIT reprogramming itself is not free — outb to ports 0x43,
     *     0x40 takes hundreds of cycles and races the in-flight IRQ
     *     stream on some chipsets (real PCH and Bochs both observed
     *     to occasionally drop or double-count ticks across a
     *     reprogram window).
     *
     * Linux's dynticks (NO_HZ_FULL) achieves the same power goal
     * WITHOUT reprogramming hardware — it just skips waking idle
     * CPUs. BoxOS doesn't have that infrastructure yet, so the
     * correct production-grade choice is "stable PIT at the boot
     * frequency".
     *
     * Build override: -DCONFIG_SCHED_ADAPTIVE_PIT=1 re-enables the
     * legacy adaptive path (kept for power-tuning experiments). The
     * default (0) is the production policy. */
#if defined(CONFIG_SCHED_ADAPTIVE_PIT) && CONFIG_SCHED_ADAPTIVE_PIT
    uint32_t target_frequency;
    if (busy_cores == 0 && g_sched_stats.context_switches == 0) {
        target_frequency = SCHEDULER_DEFAULT_TICK_HZ;
    } else if (busy_cores == 0) {
        target_frequency = SCHEDULER_MIN_TICK_HZ;
    } else if (busy_cores >= g_sched_core_count) {
        target_frequency = SCHEDULER_MAX_TICK_HZ;
    } else {
        target_frequency = SCHEDULER_MIN_TICK_HZ +
            ((SCHEDULER_MAX_TICK_HZ - SCHEDULER_MIN_TICK_HZ) * busy_cores) / g_sched_core_count;
    }
    if (target_frequency < SCHEDULER_MIN_TICK_HZ)
        target_frequency = SCHEDULER_MIN_TICK_HZ;
    if (target_frequency > SCHEDULER_MAX_TICK_HZ)
        target_frequency = SCHEDULER_MAX_TICK_HZ;
    if (target_frequency != g_timer_frequency) {
        pit_set_frequency(target_frequency);
    }
#endif
    /* else: PIT stays at SCHEDULER_DEFAULT_TICK_HZ (250 Hz) for the
     * lifetime of the kernel. Stable. */
}

// ---------------------------------------------------------------------------
// Priority + RunQueue
// ---------------------------------------------------------------------------

int sched_determine_priority(process_t *proc)
{
    if (!proc)
        return SCHED_PRIO_NORMAL;

    // Starvation check
    uint64_t now = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    uint64_t ticks_since_run = 0;
    if (now >= proc->last_run_time)
    {
        ticks_since_run = now - proc->last_run_time;
    }
    if (ticks_since_run >= g_dynamic_starvation_ticks)
    {
        g_sched_stats.starvation_boosts++;
        return SCHED_PRIO_STARVED;
    }

    // Tag affinity: track cache-warmth for future scheduling decisions
    // This does NOT boost priority — it tracks whether process cache is warm
    // on this core, which helps the scheduler make better placement decisions.
    uint8_t my_core = amp_get_core_index();
    if (my_core < g_sched_core_count && proc->home_core == my_core && ticks_since_run < SCHEDULER_AFFINITY_WARM_TICKS) {
        g_sched_stats.affinity_hits++;
        // Cache is warm — process stays on this core (no priority change)
    }

    // Context match boost
    if (UseContextMatches(proc))
    {
        return SCHED_PRIO_CONTEXT;
    }

    return SCHED_PRIO_NORMAL;
}

error_t sched_enqueue(process_t *proc)
{
    if (!proc || process_is_idle(proc))
        return ERR_INVALID_ARGUMENT;

    if (!g_core_sched || proc->home_core >= g_sched_core_count)
    {
        debug_printf("[SCHED] Invalid home_core %u for PID %u\n",
                     proc->home_core, proc->pid);
        return ERR_HOME_CORE_INVALID;
    }

    // Affinity: if process is cache-warm on home_core, keep it there
    // If cache-cold, consider migration ONLY if load imbalance is significant
    uint8_t target_core = proc->home_core;
    uint64_t ticks_since_run = 0;
    uint64_t now = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    if (now >= proc->last_run_time) {
        ticks_since_run = now - proc->last_run_time;
    }

    // If cache-cold (not run recently), consider load balancing
    // Only migrate if another core has ≥2 fewer processes (avoid thrashing)
    if (ticks_since_run >= SCHEDULER_AFFINITY_WARM_TICKS) {
        uint32_t home_load = RunqueueAtomicTotal(&g_core_sched[target_core].runqueue);
        uint32_t min_load = home_load;
        uint8_t best_core = target_core;

        for (uint8_t c = 0; c < g_sched_core_count; c++) {
            if (g_amp.cores[c].is_kcore) continue;
            if (scheduler_is_core_parked(c)) continue;
            uint32_t load = RunqueueAtomicTotal(&g_core_sched[c].runqueue);
            if (load + 1 < min_load) {  // +1 because we're about to enqueue here
                min_load = load;
                best_core = c;
            }
        }

        if (best_core != target_core) {
            target_core = best_core;
            proc->home_core = target_core;
            g_sched_stats.affinity_hits++;
        }
    }

    // Unpark core if needed
    if (scheduler_is_core_parked(target_core)) {
        scheduler_unpark_core(target_core);
    }

    scheduler_state_t *home = &g_core_sched[target_core];
    int prio = sched_determine_priority(proc);

    spin_lock(&home->runqueue.lock);
    error_t result = ERR_RUNQUEUE_FULL;

    if (!runqueue_contains(&home->runqueue, proc))
    {
        if (runqueue_enqueue(&home->runqueue, proc, prio))
        {
            result = OK;
            proc->current_prio = (int8_t)prio;
            if (prio == SCHED_PRIO_CONTEXT) {
                __atomic_fetch_add(&g_core_context_count[target_core], 1, __ATOMIC_RELAXED);
            } else {
                __atomic_fetch_add(&g_core_normal_count[target_core], 1, __ATOMIC_RELAXED);
            }
        }
    }
    else
    {
        result = OK;
    }
    spin_unlock(&home->runqueue.lock);

    /* Directed reschedule IPI: if the strand landed on a REMOTE core, kick
     * that core so it leaves MWAIT/idle and re-enters schedule() at once,
     * instead of waiting up to one LAPIC tick (≈10 ms) — or, if the core
     * was parked, never re-checking after scheduler_unpark_core cleared the
     * flag without a wake.  This is the same primitive SysAddrWake /
     * touch_queue_fire_wake use.  Without it a strand_spawn'd worker can sit
     * unscheduled while the spawner parks, hanging the cabin until a
     * timeout. */
    if (result == OK && g_amp.total_cores > 1)
    {
        uint8_t self = amp_get_core_index();
        if (target_core != self && target_core < g_amp.total_cores)
            lapic_send_ipi(g_amp.cores[target_core].lapic_id, IPI_WAKE_VECTOR);
    }

    return result;
}

error_t sched_enqueue_on(uint8_t core_idx, process_t *proc)
{
    if (!proc || process_is_idle(proc))
        return ERR_INVALID_ARGUMENT;

    if (!g_core_sched || core_idx >= g_sched_core_count)
        return ERR_HOME_CORE_INVALID;

    // Unpark core if needed
    if (scheduler_is_core_parked(core_idx)) {
        scheduler_unpark_core(core_idx);
    }

    scheduler_state_t *target = &g_core_sched[core_idx];
    int prio = sched_determine_priority(proc);

    spin_lock(&target->runqueue.lock);
    error_t result = ERR_RUNQUEUE_FULL;

    if (!runqueue_contains(&target->runqueue, proc))
    {
        if (runqueue_enqueue(&target->runqueue, proc, prio))
        {
            result = OK;
            proc->current_prio = (int8_t)prio;
            if (prio == SCHED_PRIO_CONTEXT) {
                __atomic_fetch_add(&g_core_context_count[core_idx], 1, __ATOMIC_RELAXED);
            } else {
                __atomic_fetch_add(&g_core_normal_count[core_idx], 1, __ATOMIC_RELAXED);
            }
        }
    }
    else
    {
        result = OK;
    }
    spin_unlock(&target->runqueue.lock);

    /* Directed reschedule IPI for a remote target core — see sched_enqueue. */
    if (result == OK && g_amp.total_cores > 1)
    {
        uint8_t self = amp_get_core_index();
        if (core_idx != self && core_idx < g_amp.total_cores)
            lapic_send_ipi(g_amp.cores[core_idx].lapic_id, IPI_WAKE_VECTOR);
    }

    return result;
}

error_t sched_dequeue(process_t *proc)
{
    if (!proc || process_is_idle(proc))
        return ERR_INVALID_ARGUMENT;

    if (!g_core_sched || proc->home_core >= g_sched_core_count)
        return ERR_HOME_CORE_INVALID;

    scheduler_state_t *home = &g_core_sched[proc->home_core];

    spin_lock(&home->runqueue.lock);
    // Read priority UNDER lock to avoid race with concurrent enqueue
    int prio = proc->current_prio;
    runqueue_remove(&home->runqueue, proc);
    if (prio >= 0 && prio < SCHED_PRIO_LEVELS) {
        if (prio == SCHED_PRIO_CONTEXT) {
            __atomic_fetch_sub(&g_core_context_count[proc->home_core], 1, __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_sub(&g_core_normal_count[proc->home_core], 1, __ATOMIC_RELAXED);
        }
    }
    spin_unlock(&home->runqueue.lock);

    return OK;
}

/* Remove `proc` from whichever core's runqueue currently holds it, if any,
 * keeping that core's tier counter consistent. process_destroy calls this so a
 * torn-down strand is referenced by NO runqueue before it is freed: a dangling
 * runqueue slot would otherwise dispatch a freed/recycled process_t (the
 * strandtest recycle-race). sched_dequeue() only covers proc->home_core; this
 * sweeps every core for the corpse case — process_destroy reaches a DONE strand
 * whose PROC_CRASHED transition never ran sched_dequeue (old state != WORKING),
 * and for the rare event a strand is enqueued off its home core.
 *
 * A process is in at most one runqueue (single rq_prio/rq_index), and
 * runqueue_contains/runqueue_remove validate slot==proc, so the sweep removes it
 * from the one holder and no-ops on every other core. The tier-counter
 * decrement is gated on runqueue_contains() being true, so this never
 * double-counts against sched_dequeue()'s home-core removal (which clears
 * rq_prio/rq_index, making contains() false here). Each runqueue lock is taken
 * alone — no nesting, no lock-order inversion. */
void sched_dequeue_all_cores(process_t *proc)
{
    if (!proc || process_is_idle(proc) || !g_core_sched)
        return;

    for (uint32_t c = 0; c < g_sched_core_count; c++)
    {
        scheduler_state_t *s = &g_core_sched[c];
        spin_lock(&s->runqueue.lock);
        if (runqueue_contains(&s->runqueue, proc))
        {
            int prio = proc->current_prio;
            runqueue_remove(&s->runqueue, proc);
            if (prio >= 0 && prio < SCHED_PRIO_LEVELS)
            {
                if (prio == SCHED_PRIO_CONTEXT)
                    __atomic_fetch_sub(&g_core_context_count[c], 1, __ATOMIC_RELAXED);
                else
                    __atomic_fetch_sub(&g_core_normal_count[c], 1, __ATOMIC_RELAXED);
            }
        }
        spin_unlock(&s->runqueue.lock);
    }
}

// ---------------------------------------------------------------------------
// Process selection (per-core)
// ---------------------------------------------------------------------------

process_t *scheduler_select_next(void)
{
    scheduler_state_t *s = scheduler_get_state();
    if (!s) return idle_process_get();

    // Skip parked cores
    if (__atomic_load_n(&s->is_parked, __ATOMIC_ACQUIRE)) {
        return idle_process_get();
    }

    bool fairness_round = (s->normal_ticks >= g_dynamic_fairness_ratio);

    uint8_t my_core = amp_get_core_index();

    spin_lock(&s->runqueue.lock);

    process_t *next = NULL;

    if (fairness_round)
    {
        uint32_t fair_bitmap = s->runqueue.active_bitmap & ~(1u << SCHED_PRIO_CONTEXT);
        if (fair_bitmap != 0)
        {
            int prio = 31 - __builtin_clz(fair_bitmap);
            SchedQueue *q = &s->runqueue.queues[prio];
            next = q->procs[q->head];
            q->procs[q->head] = NULL;
            next->rq_prio = -1;
            next->rq_index = -1;
            q->head = (q->head + 1) % q->capacity;
            q->count--;
            if (q->count == 0)
                s->runqueue.active_bitmap &= ~(1u << prio);
            __atomic_fetch_sub(&s->runqueue.total, 1, __ATOMIC_RELEASE);
            // Decrement tier counter: fairness round always pulls from non-CONTEXT queues
            if (my_core < g_sched_core_count)
                __atomic_fetch_sub(&g_core_normal_count[my_core], 1, __ATOMIC_RELAXED);
        }
        else
        {
            next = runqueue_dequeue_best(&s->runqueue);
            if (next && !process_is_idle(next) && my_core < g_sched_core_count) {
                if (next->current_prio == SCHED_PRIO_CONTEXT)
                    __atomic_fetch_sub(&g_core_context_count[my_core], 1, __ATOMIC_RELAXED);
                else
                    __atomic_fetch_sub(&g_core_normal_count[my_core], 1, __ATOMIC_RELAXED);
            }
        }
        s->normal_ticks = 0;
        g_sched_stats.fairness_rounds++;
    }
    else
    {
        next = runqueue_dequeue_best(&s->runqueue);
        if (next && !process_is_idle(next) && my_core < g_sched_core_count) {
            if (next->current_prio == SCHED_PRIO_CONTEXT)
                __atomic_fetch_sub(&g_core_context_count[my_core], 1, __ATOMIC_RELAXED);
            else
                __atomic_fetch_sub(&g_core_normal_count[my_core], 1, __ATOMIC_RELAXED);
        }
        s->normal_ticks++;
    }

    // Skip destroying processes.
    // `next` was already dequeued and its counter decremented above.
    // Each replacement also needs its counter decremented.
    while (next && !process_is_idle(next) &&
           __atomic_load_n(&next->destroying, __ATOMIC_ACQUIRE))
    {
        next = runqueue_dequeue_best(&s->runqueue);
        if (next && !process_is_idle(next) && my_core < g_sched_core_count) {
            if (next->current_prio == SCHED_PRIO_CONTEXT)
                __atomic_fetch_sub(&g_core_context_count[my_core], 1, __ATOMIC_RELAXED);
            else
                __atomic_fetch_sub(&g_core_normal_count[my_core], 1, __ATOMIC_RELAXED);
        }
    }

    spin_unlock(&s->runqueue.lock);

    if (!next)
    {
        next = idle_process_get();
    }

    return next;
}

// ---------------------------------------------------------------------------
// Work stealing
// ---------------------------------------------------------------------------

static process_t *sched_try_steal(uint8_t my_core)
{
    if (!g_core_sched) return NULL;

    uint8_t victim = 0xFF;
    uint32_t max_count = 1;

    for (uint8_t c = 0; c < g_sched_core_count; c++)
    {
        if (c == my_core) continue;
        if (g_amp.cores[c].is_kcore) continue;
        if (!amp_core_online(&g_amp.cores[c])) continue;
        if (scheduler_is_core_parked(c)) continue;

        uint32_t cnt = RunqueueAtomicTotal(&g_core_sched[c].runqueue);
        if (cnt > max_count)
        {
            max_count = cnt;
            victim = c;
        }
    }

    if (victim == 0xFF)
        return NULL;

    scheduler_state_t *vs = &g_core_sched[victim];
    spin_lock(&vs->runqueue.lock);

    if (RunqueueAtomicTotal(&vs->runqueue) <= 1)
    {
        spin_unlock(&vs->runqueue.lock);
        return NULL;
    }

    /* Snapshot the stolen process AND the priority we are pulling it
     * from INSIDE the victim's lock. The historical bug: prio was
     * read from `stolen->current_prio` AFTER the unlock — between
     * unlock and that read, another path on the victim core could
     * mutate current_prio (e.g. a parallel priority recalculation),
     * leading to a tier-counter decrement on the wrong queue and
     * persistent counter drift (eventually wraps negative, makes
     * `recalc_parameters` think every core is busy, parks live
     * cores). Reading inside the lock + transferring both counters
     * before unlocking eliminates the window. */
    process_t *stolen = NULL;
    int        stolen_prio = -1;
    for (int prio = SCHED_PRIO_NORMAL; prio >= SCHED_PRIO_IDLE; prio--)
    {
        SchedQueue *q = &vs->runqueue.queues[prio];
        if (q->count == 0) continue;

        stolen = q->procs[q->head];
        q->procs[q->head] = NULL;
        stolen->rq_prio = -1;
        stolen->rq_index = -1;
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        if (q->count == 0)
            vs->runqueue.active_bitmap &= ~(1u << prio);
        __atomic_fetch_sub(&vs->runqueue.total, 1, __ATOMIC_RELEASE);
        stolen_prio = stolen->current_prio;
        break;
    }

    /* Transfer the tier counter from victim to stealer while still
     * holding the victim's lock — no other path on the victim can
     * be modifying current_prio for this process here because we
     * already pulled it off the runqueue. */
    if (stolen && stolen_prio >= 0 && stolen_prio < SCHED_PRIO_LEVELS) {
        uint8_t core = amp_get_core_index();
        if (core < g_sched_core_count) {
            if (stolen_prio == SCHED_PRIO_CONTEXT) {
                __atomic_fetch_sub(&g_core_context_count[victim], 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_core_context_count[core], 1, __ATOMIC_RELAXED);
            } else {
                __atomic_fetch_sub(&g_core_normal_count[victim], 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_core_normal_count[core], 1, __ATOMIC_RELAXED);
            }
        }
    }

    spin_unlock(&vs->runqueue.lock);

    if (stolen) {
        stolen->home_core = amp_get_core_index();
        g_sched_stats.steal_successes++;
    }

    return stolen;
}

// ---------------------------------------------------------------------------
// schedule() — per-core unified scheduling
// ---------------------------------------------------------------------------

void schedule(void *frame_ptr)
{
    interrupt_frame_t *frame = (interrupt_frame_t *)frame_ptr;

    __asm__ volatile("cli");

    scheduler_state_t *s = scheduler_get_state();
    if (!s) {
        return;
    }

    /* Quiescence tick — bumped on EVERY schedule entry, BEFORE the parked
     * early-return (a parked-but-still-ticking core must advance it too;
     * is_parked does not mask the LAPIC timer). A strand switched out below
     * stamps this value; the reaper frees its kernel stack only once this core's
     * quiesce_seq EXCEEDS the stamp.
     *
     * Why "exceeds the stamp" ⇒ "this core has left the evicted strand's kernel
     * stack": schedule() runs with IF=0 (cli above) all the way through to the
     * iretq that returns from THIS dispatch, so the NEXT schedule() entry on this
     * core — the one that bumps past the stamp — cannot begin until that iretq
     * has retired. The iretq is exactly what moves the core off the outgoing
     * strand's kernel stack (onto the incoming context's, or idle's on the
     * parked/bail path). So the bump-before-parked-check is both necessary
     * (else a core that evicts then parks would freeze its epoch and never let
     * that corpse reap) and safe (the epoch only advances after the stack is
     * left). See process.c strand_stack_in_use(). */
    uint64_t my_qseq = __atomic_add_fetch(&s->quiesce_seq, 1, __ATOMIC_ACQ_REL);

    // Skip scheduling on parked cores
    if (__atomic_load_n(&s->is_parked, __ATOMIC_ACQUIRE)) {
        return;
    }

    spin_lock(&s->scheduler_lock);
    process_t *current = s->current_process;
    spin_unlock(&s->scheduler_lock);

    // Save context
    if (current)
    {
        process_state_t cur_state = process_get_state(current);
        if (cur_state == PROC_WORKING || cur_state == PROC_WAITING)
        {
            context_save_from_frame(current, frame);
        }
    }

    // Re-enqueue current if still WORKING
    if (current && !process_is_idle(current) &&
        process_get_state(current) == PROC_WORKING)
    {
        int prio = sched_determine_priority(current);
        spin_lock(&s->runqueue.lock);
        /* Re-check state UNDER the runqueue lock before committing the enqueue.
         * process_set_state's WORKING→non-WORKING transition takes THIS same
         * runqueue lock for its sched_dequeue (on proc->home_core == this core
         * for a running strand), so observing WORKING here is decisive: either
         * the transition has not run yet (we enqueue; its later dequeue, under
         * this lock, removes us) or it already ran (we see non-WORKING and skip).
         * Without it, the unlocked state read in the `if` above races a
         * concurrent exit/kill and can leave a DONE/CRASHED strand in the
         * runqueue — which the runtime reaper then frees while a dangling slot
         * still points at it, so a freed/recycled process_t gets dispatched
         * (the strand-churn fault that runtime full-process reaping unmasked).
         * Read state atomically — NOT process_get_state(), which takes
         * state_lock and would invert the state_lock→runqueue_lock order. */
        if (__atomic_load_n(&current->state, __ATOMIC_ACQUIRE) == PROC_WORKING &&
            !runqueue_contains(&s->runqueue, current))
        {
            runqueue_enqueue(&s->runqueue, current, prio);
            current->current_prio = (int8_t)prio;
            uint8_t core = amp_get_core_index();
            if (core < g_sched_core_count) {
                if (prio == SCHED_PRIO_CONTEXT) {
                    __atomic_fetch_add(&g_core_context_count[core], 1, __ATOMIC_RELAXED);
                } else {
                    __atomic_fetch_add(&g_core_normal_count[core], 1, __ATOMIC_RELAXED);
                }
            }
        }
        spin_unlock(&s->runqueue.lock);
    }

    // Select next
    process_t *next = scheduler_select_next();

    // Work stealing
    if (process_is_idle(next) && amp_is_appcore() && g_amp.multicore_active)
    {
        uint64_t ticks_since_steal = s->total_ticks - s->last_steal_tick;
        if (ticks_since_steal >= g_dynamic_steal_cooldown)
        {
            s->last_steal_tick = s->total_ticks;
            g_sched_stats.steal_attempts++;
            process_t *stolen = sched_try_steal(amp_get_core_index());
            if (stolen)
            {
                next = stolen;
            }
        }
    }

    // Set kernel stack (top + floor) for THIS core — including idle, so the
    // REACT headroom guard reads the correct geometry while idle runs.
    if (next->kernel_stack_top)
    {
        per_core_set_kernel_rsp((uint64_t)next->kernel_stack_top,
                                (uint64_t)next->kernel_stack);
    }

    // Switch
    spin_lock(&s->scheduler_lock);
    /* TOCTOU re-check: between scheduler_select_next (which filtered
     * destroying processes) and this store, another core may have
     * called process_destroy on `next` and marked destroying=1. If
     * we commit `next` as current_process here, process_destroy's
     * scan finds the just-stored pointer AND the destroyer races
     * with us through the alive-ref window — net effect is a UAF
     * when destroy eventually frees the process struct.
     *
     * Closing the window: re-read destroying under the same lock
     * that process_destroy's scan synchronises with (via the
     * ACQUIRE load on s->current_process in process_destroy.c:662).
     * If destroying is set we fall back to idle for this tick;
     * scheduler_select_next will pick a fresh candidate next tick. */
    if (next && !process_is_idle(next) &&
        __atomic_load_n(&next->destroying, __ATOMIC_ACQUIRE)) {
        /* Selected task is being destroyed — don't dispatch it. Fall back to
         * THIS core's idle task for this tick, NOT to NULL: with next == NULL,
         * context_restore_to_frame is a no-op, so the IRQ frame is left holding
         * the OUTGOING `current` and the iretq would resume `current` here —
         * while `current` was just re-enqueued above, leaving it both running
         * AND in a runqueue (stealable → the same context dispatched on two
         * cores). Running idle keeps the running-XOR-runqueue invariant; the
         * next tick re-runs scheduler_select_next and picks a fresh candidate. */
        next = idle_process_get();
    }

    /* Running-XOR-runqueue interlock: claim `next` exclusively before committing
     * it as current_process. If another core still owns it (mid-eviction there —
     * e.g. a strand transiently re-enqueued while still current), do NOT dispatch
     * it here: two cores would then run context_save/restore over one shared
     * proc->context and tear it (the strandtest UEFI-16c corruption). Fall back to
     * idle; the next tick re-selects. The owner releases on_cpu below when it
     * switches the strand out. on_cpu == me means a consecutive run (already
     * ours) — keep it without re-claiming. */
    if (next && !process_is_idle(next)) {
        int16_t me8 = (int16_t)amp_get_core_index();
        if (__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE) != me8) {
            int16_t expect = -1;
            if (!__atomic_compare_exchange_n(&next->on_cpu, &expect, me8,
                                             false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                /* Another core still owns `next` — averting double-dispatch. */
                next = idle_process_get();
            }
        }
    }

    /* Stamp the outgoing strand for reap-vs-switch safety. It leaves the run
     * state at the store below, but THIS core keeps executing on its kernel
     * stack until the post-return iretq — so the reaper must hold off freeing
     * it until this core's quiesce_seq advances past my_qseq (i.e. its next
     * schedule(), which runs on `next`'s stack). The RELEASE store pairs with
     * the reaper's ACQUIRE and is ordered before current_process=next, so any
     * core that observes the strand no longer current also observes this stamp. */
    if (current && current != next && !process_is_idle(current)) {
        current->quiesce_core = amp_get_core_index();
        __atomic_store_n(&current->quiesce_seq, my_qseq, __ATOMIC_RELEASE);
    }
    s->current_process = next;
    /* Release the outgoing strand's dispatch claim AFTER current_process moves to
     * `next`: once on_cpu is -1 the strand is no longer current_process on this
     * core, so a core that then claims it dispatches it singly (no double). */
    if (current && current != next && !process_is_idle(current)) {
        __atomic_store_n(&current->on_cpu, (int16_t)-1, __ATOMIC_RELEASE);
    }
    spin_unlock(&s->scheduler_lock);

    /* Nightwatch: real work dispatched here, so this core is no longer idle
     * and a later stall counts as a new episode. One byte store. */
    if (!process_is_idle(next))
        nightwatch_core_busy(amp_get_core_index());

    g_sched_stats.context_switches++;

    // PROC_CREATED → PROC_WORKING.  `next` is never NULL: scheduler_select_next
    // returns the idle task when nothing is runnable, and the destroying-task
    // re-check above also falls back to idle — so no NULL guard is needed here.
    if (process_get_state(next) == PROC_CREATED)
    {
        spin_lock(&next->state_lock);
        next->state = PROC_WORKING;
        spin_unlock(&next->state_lock);
    }
    next->last_run_time = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);

    // Track consecutive runs
    if (current && current != next)
    {
        current->consecutive_runs = 0;
        next->consecutive_runs = 1;
    }
    else if (next)
    {
        next->consecutive_runs++;
    }

    // Restore context
    context_restore_to_frame(next, frame);
}
