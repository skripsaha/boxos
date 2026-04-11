#include "scheduler.h"
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

// ---------------------------------------------------------------------------
// Dynamic Scheduler Parameters
// ---------------------------------------------------------------------------

uint32_t g_scheduler_fairness_base       = 4;
uint32_t g_scheduler_starvation_base     = 25;
uint32_t g_scheduler_steal_cooldown_base = 10;
uint32_t g_timer_frequency               = SCHEDULER_DEFAULT_TICK_HZ;

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

void scheduler_init(void)
{
    debug_printf("[SCHEDULER] Initializing per-core O(1) scheduler...\n");

    UseContextInit();

    memset(&g_sched_stats, 0, sizeof(g_sched_stats));

    // Allocate scheduler structures based on actual core count
    g_sched_core_count = g_amp.total_cores;
    if (g_sched_core_count == 0 || g_sched_core_count > MAX_CORES) {
        debug_printf("[SCHEDULER] ERROR: Invalid core count %u\n", g_sched_core_count);
        return;
    }

    g_core_sched = kmalloc(sizeof(scheduler_state_t) * g_sched_core_count);
    g_core_context_count = kmalloc(sizeof(uint32_t) * g_sched_core_count);
    g_core_normal_count = kmalloc(sizeof(uint32_t) * g_sched_core_count);

    if (!g_core_sched || !g_core_context_count || !g_core_normal_count) {
        debug_printf("[SCHEDULER] ERROR: Failed to allocate scheduler structures\n");
        // Cleanup partial allocations
        if (g_core_sched) { kfree(g_core_sched); g_core_sched = NULL; }
        if (g_core_context_count) { kfree((void *)g_core_context_count); g_core_context_count = NULL; }
        if (g_core_normal_count) { kfree((void *)g_core_normal_count); g_core_normal_count = NULL; }
        g_sched_core_count = 0;
        return;
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
}

void scheduler_init_core(uint8_t core_index)
{
    if (!g_core_sched || core_index >= g_sched_core_count) return;
    scheduler_state_t *s = &g_core_sched[core_index];
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
    if (!s->is_parked) {
        s->is_parked = true;
        g_sched_stats.parked_cores++;
        g_sched_stats.active_cores--;
        debug_printf("[SCHED] Core %u parked\n", core_idx);
    }
}

void scheduler_unpark_core(uint8_t core_idx)
{
    if (!g_core_sched || core_idx >= g_sched_core_count) return;
    scheduler_state_t *s = &g_core_sched[core_idx];
    if (s->is_parked) {
        s->is_parked = false;
        s->idle_tick_count = 0;
        g_sched_stats.parked_cores--;
        g_sched_stats.active_cores++;
        debug_printf("[SCHED] Core %u unparked\n", core_idx);
    }
}

bool scheduler_is_core_parked(uint8_t core_idx)
{
    if (!g_core_sched || core_idx >= g_sched_core_count)
        return false;
    return g_core_sched[core_idx].is_parked;
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
        if (rq_count > 0 ||
            (s->current_process && !process_is_idle(s->current_process))) {
            busy_cores++;
        }

        // Core parking
        if (!s->is_parked && rq_count == 0 &&
            (!s->current_process || process_is_idle(s->current_process))) {
            s->idle_tick_count++;
            if (s->idle_tick_count >= SCHEDULER_PARK_IDLE_TICKS) {
                scheduler_park_core(c);
            }
        } else if (s->is_parked && rq_count >= SCHEDULER_UNPARK_LOAD_THRESH) {
            scheduler_unpark_core(c);
        } else if (!s->is_parked) {
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

    // Adaptive tick rate: reprogram PIT hardware based on system load
    // This is unique to BoxOS — no other OS does this dynamically
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

    // Bounds check
    if (target_frequency < SCHEDULER_MIN_TICK_HZ)
        target_frequency = SCHEDULER_MIN_TICK_HZ;
    if (target_frequency > SCHEDULER_MAX_TICK_HZ)
        target_frequency = SCHEDULER_MAX_TICK_HZ;

    // Reprogram PIT only if frequency actually changed
    if (target_frequency != g_timer_frequency) {
        pit_set_frequency(target_frequency);
    }
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
        debug_printf("[SCHED] PID %u STARVED (ticks=%u >= threshold=%u)\n",
                      proc->pid, (uint32_t)ticks_since_run, g_dynamic_starvation_ticks);
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
        debug_printf("[SCHED] PID %u CONTEXT match\n", proc->pid);
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

// ---------------------------------------------------------------------------
// Process selection (per-core)
// ---------------------------------------------------------------------------

process_t *scheduler_select_next(void)
{
    scheduler_state_t *s = scheduler_get_state();
    if (!s) return idle_process_get();

    // Skip parked cores
    if (s->is_parked) {
        return idle_process_get();
    }

    bool fairness_round = (s->normal_ticks >= g_dynamic_fairness_ratio);

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
        }
        else
        {
            next = runqueue_dequeue_best(&s->runqueue);
        }
        s->normal_ticks = 0;
        g_sched_stats.fairness_rounds++;
        debug_printf("[SCHED] FAIRNESS round: selected PID %u (prio=%d)\n",
                      next ? next->pid : 0, next ? sched_determine_priority(next) : -1);
    }
    else
    {
        next = runqueue_dequeue_best(&s->runqueue);
        s->normal_ticks++;
        debug_printf("[SCHED] Select: PID %u (normal_ticks=%u/%u)\n",
                      next ? next->pid : 0, s->normal_ticks, g_dynamic_fairness_ratio);
    }

    // Skip destroying processes
    while (next && !process_is_idle(next) &&
           __atomic_load_n(&next->destroying, __ATOMIC_ACQUIRE))
    {
        debug_printf("[SCHED] Skip destroying PID %u\n", next->pid);
        next = runqueue_dequeue_best(&s->runqueue);
    }

    spin_unlock(&s->runqueue.lock);

    if (!next)
    {
        next = idle_process_get();
        debug_printf("[SCHED] IDLE selected\n");
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
        if (!g_amp.cores[c].online) continue;
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

    process_t *stolen = NULL;
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
        break;
    }

    spin_unlock(&vs->runqueue.lock);

    if (stolen)
    {
        uint8_t core = amp_get_core_index();
        int prio = stolen->current_prio;
        // Validate priority before updating counters
        if (prio >= 0 && prio < SCHED_PRIO_LEVELS && core < g_sched_core_count) {
            if (prio == SCHED_PRIO_CONTEXT) {
                __atomic_fetch_sub(&g_core_context_count[victim], 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_core_context_count[core], 1, __ATOMIC_RELAXED);
            } else {
                __atomic_fetch_sub(&g_core_normal_count[victim], 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_core_normal_count[core], 1, __ATOMIC_RELAXED);
            }
        }
        stolen->home_core = core;
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

    // Skip scheduling on parked cores
    if (s->is_parked) {
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
        if (!runqueue_contains(&s->runqueue, current))
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

    // Set kernel stack
    if (!process_is_idle(next) && next->kernel_stack_top)
    {
        per_core_set_kernel_rsp((uint64_t)next->kernel_stack_top);
    }

    // Switch
    spin_lock(&s->scheduler_lock);
    s->current_process = next;
    spin_unlock(&s->scheduler_lock);

    debug_printf("[SCHED] Switch: PID %u -> PID %u\n",
                  current ? current->pid : 0, next->pid);
    g_sched_stats.context_switches++;

    // PROC_CREATED → PROC_WORKING
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
