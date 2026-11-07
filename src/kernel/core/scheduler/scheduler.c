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
#include "per_core.h"
#include "amp.h"
#include "lapic.h"     /* lapic_send_ipi — directed reschedule IPI on cross-core enqueue */
#include "irqchip.h"   /* IPI_WAKE_VECTOR */
#include "kring.h"     /* KResultStepAside — the ask to yield a core */

/* TSC frequency for per-process processor-time accounting (see schedule()). */
extern uint64_t cpu_get_tsc_freq_khz(void);

uint32_t g_timer_frequency = SCHEDULER_DEFAULT_TICK_HZ;

/* Ticks an App Core lets pass between two looks for work to steal: ten, and
 * one more for every two cores there are to look at. Set once, at init. */
static uint32_t g_steal_cooldown;

/* What a strand pays for the time it holds a core is its Fare: the time
 * itself, the less the more of the Use Context its cabin wears — a third of
 * it for every tag of the context, half for half of them. The person's work
 * is given three quarters of a core it shares with one outsider, the share
 * Linux gives nice -5 over nice 0 (3121 to 1024). */
#define SCHED_CONTEXT_FARE_SHARE 3

// ---------------------------------------------------------------------------
// Global monotonic tick
// ---------------------------------------------------------------------------
volatile uint64_t g_global_tick = 0;

// ---------------------------------------------------------------------------
// Per-core scheduler array — dynamically allocated
// ---------------------------------------------------------------------------
static scheduler_state_t *g_core_sched;
static uint32_t g_sched_core_count;

static void scheduler_account_current(scheduler_state_t *s, uint64_t now);

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

static void sched_init_one(scheduler_state_t *s)
{
    memset(s, 0, sizeof(scheduler_state_t));
    s->current_process = NULL;
    spinlock_init(&s->scheduler_lock);
    s->total_ticks = 0;
    s->last_steal_tick = 0;
    s->is_parked = false;
    s->idle_tick_count = 0;
    s->accounting_last_tsc = rdtsc();
    runqueue_init(&s->runqueue);
}

error_t scheduler_init(void)
{
    debug_printf("[SCHEDULER] Initializing per-core Fare scheduler...\n");

    UseContextInit();

    g_sched_core_count = g_amp.total_cores;
    if (g_sched_core_count == 0 || g_sched_core_count > MAX_CORES) {
        debug_printf("[SCHEDULER] ERROR: Invalid core count %u\n", g_sched_core_count);
        return ERR_INVALID_ARGUMENT;
    }

    g_core_sched = kmalloc(sizeof(scheduler_state_t) * g_sched_core_count);
    if (!g_core_sched) {
        debug_printf("[SCHEDULER] ERROR: Failed to allocate scheduler structures\n");
        g_sched_core_count = 0;
        return ERR_NO_MEMORY;
    }

    for (uint32_t i = 0; i < g_sched_core_count; i++)
        sched_init_one(&g_core_sched[i]);

    g_steal_cooldown = 10 + g_sched_core_count / 2;

    debug_printf("[SCHEDULER] Per-core Fare scheduler ready (%u Hz, %u cores, init_cap=%u)\n",
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
    if (s->runqueue.procs != NULL) {
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

/* The count is read by this core's own interrupts, so it is changed with them
 * shut, whatever state the caller left them in. */
static inline uint64_t SchedulerIrqShut(void)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static inline void SchedulerIrqBack(uint64_t flags)
{
    if (flags & (1u << 9))
        asm volatile("sti" ::: "memory");
}

void SchedulerHoldHere(void)
{
    uint64_t flags = SchedulerIrqShut();
    scheduler_state_t *s = scheduler_get_state();
    if (s)
        s->held++;
    SchedulerIrqBack(flags);
}

/* A span that began before the scheduler existed held nothing, and lets go of
 * nothing. */
void SchedulerLetGoHere(void)
{
    uint64_t flags = SchedulerIrqShut();
    scheduler_state_t *s = scheduler_get_state();
    if (s && s->held)
        s->held--;
    SchedulerIrqBack(flags);
}

bool SchedulerHeldHere(void)
{
    uint64_t flags = SchedulerIrqShut();
    scheduler_state_t *s = scheduler_get_state();
    bool held = s && s->held;
    SchedulerIrqBack(flags);
    return held;
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
// Processor accounting
// ---------------------------------------------------------------------------

static void scheduler_account_current(scheduler_state_t *s, uint64_t now)
{
    uint64_t last = s->accounting_last_tsc;
    if (last != 0 && now > last) {
        uint64_t delta = now - last;
        __atomic_fetch_add(&s->accounting_total_tsc, delta, __ATOMIC_RELAXED);
        process_t *current = __atomic_load_n(&s->current_process, __ATOMIC_ACQUIRE);
        if (current && !process_is_idle(current))
            __atomic_fetch_add(&s->accounting_busy_tsc, delta, __ATOMIC_RELAXED);
    }
    s->accounting_last_tsc = now;
}

void scheduler_cpu_usage(uint64_t *busy_tsc, uint64_t *total_tsc)
{
    uint64_t busy = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < g_sched_core_count; i++) {
        scheduler_state_t *s = &g_core_sched[i];
        uint64_t core_total = __atomic_load_n(&s->accounting_total_tsc, __ATOMIC_RELAXED);
        uint64_t core_busy = __atomic_load_n(&s->accounting_busy_tsc, __ATOMIC_RELAXED);
        total += core_total;
        busy += core_busy;
    }
    if (busy_tsc) *busy_tsc = busy;
    if (total_tsc) *total_tsc = total;
}

bool scheduler_core_cpu_usage(uint8_t core_idx, uint64_t *busy_tsc,
                              uint64_t *total_tsc, bool *parked)
{
    if (!g_core_sched || core_idx >= g_sched_core_count)
        return false;

    scheduler_state_t *s = &g_core_sched[core_idx];
    if (busy_tsc)
        *busy_tsc = __atomic_load_n(&s->accounting_busy_tsc, __ATOMIC_RELAXED);
    if (total_tsc)
        *total_tsc = __atomic_load_n(&s->accounting_total_tsc, __ATOMIC_RELAXED);
    if (parked)
        *parked = __atomic_load_n(&s->is_parked, __ATOMIC_ACQUIRE);
    return true;
}

// ---------------------------------------------------------------------------
// Core parking — every SCHED_RECALC_INTERVAL global ticks, from the BSP's IRQ0
// ---------------------------------------------------------------------------

void scheduler_recalc_parameters(void)
{
    if (!g_core_sched || g_sched_core_count == 0)
        return;

    // Called from BSP PIT IRQ only, so no concurrent access to s_last_recalc.
    static uint64_t s_last_recalc = 0;
    uint64_t now = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    if (now - s_last_recalc < SCHED_RECALC_INTERVAL)
        return;
    s_last_recalc = now;

    for (uint32_t c = 0; c < g_sched_core_count; c++) {
        scheduler_state_t *s = &g_core_sched[c];
        uint32_t rq_count = RunqueueAtomicTotal(&s->runqueue);

        // Read current_process under lock to prevent torn pointer reads on SMP
        spin_lock(&s->scheduler_lock);
        process_t *cur = s->current_process;
        spin_unlock(&s->scheduler_lock);

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
}

// ---------------------------------------------------------------------------
// Fare and Waterline
// ---------------------------------------------------------------------------

/* One turn on a core — an App Core's tick — in TSC cycles. */
static inline uint64_t sched_turn(void)
{
    return (uint64_t)cpu_get_tsc_freq_khz() * 1000u / CONFIG_SCHED_APP_TICK_HZ;
}

/* The cycles over the strand's weight: one, and as much more as its Fit —
 * up to SCHED_CONTEXT_FARE_SHARE. */
uint64_t sched_fare_for(const process_t *proc, uint64_t cycles)
{
    uint64_t fit = UseContextFit(proc);
    return cycles * USE_CONTEXT_FIT_FULL /
           (USE_CONTEXT_FIT_FULL + (SCHED_CONTEXT_FARE_SHARE - 1) * fit);
}

uint64_t sched_rebase(uint64_t paid, bool known, uint64_t there, uint64_t here)
{
    uint64_t turn   = sched_turn();
    uint64_t lowest = here > turn ? here - turn : 0;
    uint64_t level  = here;

    if (known) {
        int64_t ahead = (int64_t)(paid - there);
        level = (ahead < 0 && (uint64_t)-ahead > here) ? 0 : here + (uint64_t)ahead;
    }
    return level < lowest ? lowest : level;
}

/*
 * A strand comes to `core` from outside its runqueue — woken, new, moved,
 * stolen, handed back — and its Fare is brought onto that core's scale
 * (sched_rebase): a strand that slept is served soon after it wakes, but a
 * long sleep buys it one turn, not the core. Under `core`'s runqueue lock,
 * which is what this core's Waterline and a queued strand's Fare are read
 * and written under.
 */
static void sched_place(process_t *proc, uint8_t core)
{
    int16_t  from  = proc->fare_core;
    bool     known = from >= 0 && (uint32_t)from < g_sched_core_count;
    uint64_t there = known ? __atomic_load_n(&g_core_sched[from].waterline, __ATOMIC_RELAXED) : 0;

    proc->fare = sched_rebase(proc->fare, known, there, g_core_sched[core].waterline);
    __atomic_store_n(&proc->fare_core, (int16_t)core, __ATOMIC_RELAXED);
}

/* The Waterline rises to the Fare of the strand this core dispatches from its
 * runqueue — the least there, the strand it switched out among them — and
 * never falls. Under this core's runqueue lock. */
static inline void sched_raise_waterline(scheduler_state_t *s, uint64_t fare)
{
    if (fare > s->waterline)
        __atomic_store_n(&s->waterline, fare, __ATOMIC_RELAXED);
}

/* The least Fare queued on this core, `proc` aside; false when nobody else is
 * queued. The heap's root, or — when `proc` is the root — the lesser of its
 * two children. Under this core's runqueue lock. */
static bool sched_least_other(RunQueue *rq, const process_t *proc, uint64_t *fare)
{
    process_t *first = runqueue_at(rq, 0);
    if (first == proc) {
        process_t *left  = runqueue_at(rq, 1);
        process_t *right = runqueue_at(rq, 2);
        first = (right && (!left || right->fare < left->fare)) ? right : left;
    }
    if (!first)
        return false;
    *fare = first->fare;
    return true;
}

/*
 * Whether the core that last switched `proc` out has left its kernel stack.
 * A core stands on the stack of the strand it switches out until the iretq
 * that ends its schedule() — the interrupt frame it returns through is at the
 * top of that stack — and a strand run on another core before then has its
 * first interrupt frame laid over that one: the core returns into another
 * strand's registers. The evicting core's quiesce_seq passing the strand's
 * stamp says it has gone (process.c, strand_stack_in_use, the reaper's own
 * question). Until then a strand is run where it is, by that core.
 */
static bool sched_stack_left(const process_t *proc)
{
    uint8_t qc = proc->quiesce_core;
    if (qc == QUIESCE_CORE_NONE || qc >= g_sched_core_count)
        return true;
    return __atomic_load_n(&g_core_sched[qc].quiesce_seq, __ATOMIC_ACQUIRE) >
           __atomic_load_n(&proc->quiesce_seq, __ATOMIC_ACQUIRE);
}

void sched_yield_turn(void)
{
    scheduler_state_t *s = scheduler_get_state();
    if (s)
        s->turn_given = true;
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

    /* A strand still on a CPU belongs to that core's runqueue and to no other.
     *
     * The wake that reaches here is process_set_state's WAITING->WORKING flip,
     * and a strand can be flipped while it is still current: a K-Core parks it
     * and answers it while its App-Core has not yet switched it out. Only the
     * core that owns it can take it out of a runqueue safely — schedule()'s
     * dispatch claim (on_cpu) refuses a strand another core still holds, and
     * the strand refused there used to be dropped. So while on_cpu names a
     * core, that core is the target, and cold migration is not considered: the
     * cache is not cold, it is in use. And a moment longer, until that core
     * has left the strand's kernel stack (sched_stack_left): switched out, the
     * strand is under its feet until its iretq, and run on another core first
     * it would have its interrupt frames laid over the one that core returns
     * through.
     *
     * Read under the strand's state_lock (process_set_state holds it), so
     * on_cpu can only move from a core to -1 from here on, never to a new
     * core: a WAITING strand is in no runqueue and nobody can dispatch it. */
    int16_t  owner       = __atomic_load_n(&proc->on_cpu, __ATOMIC_ACQUIRE);
    uint8_t  target_core = proc->home_core;
    uint64_t ticks_since_run = 0;
    uint64_t now = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    if (now >= proc->last_run_time) {
        ticks_since_run = now - proc->last_run_time;
    }

    if (owner >= 0 && (uint32_t)owner < g_sched_core_count) {
        target_core = (uint8_t)owner;
    }
    else if (!sched_stack_left(proc)) {
        target_core = proc->quiesce_core;
    }
    // If cache-cold (not run recently), consider load balancing
    // Only migrate if another core has ≥2 fewer processes (avoid thrashing)
    else if (ticks_since_run >= SCHEDULER_AFFINITY_WARM_TICKS) {
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
        }
    }

    // Unpark core if needed
    if (scheduler_is_core_parked(target_core)) {
        scheduler_unpark_core(target_core);
    }

    scheduler_state_t *home = &g_core_sched[target_core];

    spin_lock(&home->runqueue.lock);
    error_t result = OK;
    if (!runqueue_contains(&home->runqueue, proc))
    {
        sched_place(proc, target_core);
        if (!runqueue_enqueue(&home->runqueue, proc))
            result = ERR_RUNQUEUE_FULL;
    }
    spin_unlock(&home->runqueue.lock);

    /* Ask the strand holding that core to step aside. A strand that runs does
     * not need this — the IPI below brings a schedule() — but one that SPINS
     * waiting on another strand's work (a hot look, a reply wait) may be
     * spinning on exactly the strand just queued behind it, and would hold the
     * core for its whole budget. Read under the target core's scheduler_lock,
     * so its current strand cannot be switched out and torn down while its
     * ring header is written. Lock order: state_lock (held by the caller,
     * process_set_state) -> scheduler_lock; nothing takes them the other way
     * round. */
    if (result == OK)
    {
        scheduler_state_t *ts = &g_core_sched[target_core];
        spin_lock(&ts->scheduler_lock);
        process_t *cur = ts->current_process;
        if (cur && cur != proc && !process_is_idle(cur))
            KResultStepAside(cur, true);
        spin_unlock(&ts->scheduler_lock);
    }

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

/* One transition, one bell. The wake paths (kring, touch_ring, touch,
 * touch_queue, sync_ops) each carried their own copy of "IPI the home core",
 * and each rang it AFTER process_set_state had already rung it from
 * sched_enqueue — two IPIs, and on an App-Core each IPI_WAKE was a full
 * schedule(). They ring here now, and only when the flip did not.
 *
 * A strand on a CPU is not rung at all. It is running: whatever it waits on it
 * reads from memory — a spin on its ring, or UMWAIT on the very cacheline the
 * producer just wrote — and an interrupt would only take the core from it for
 * a schedule() that has nothing new to decide. A strand parked while still on
 * its CPU is told to step aside through its ring header instead
 * (KResultStepAside), which is the one it is already watching.
 *
 * Ordering: every caller has published (a RELEASE store) and then passed
 * through the target's state_lock — a locked instruction, a full barrier on
 * x86 — before on_cpu is read here. A strand leaving its CPU after that read is
 * re-queued by its own core (WORKING) and dispatched on a later schedule(); a
 * stale on_cpu >= 0 therefore costs latency up to that schedule, never a wake. */
void sched_ring_home(process_t *proc)
{
    if (!proc || g_amp.total_cores <= 1)
        return;
    if (__atomic_load_n(&proc->on_cpu, __ATOMIC_ACQUIRE) >= 0)
        return;
    uint8_t core = proc->home_core;
    if (core >= g_amp.total_cores || core == amp_get_core_index())
        return;
    lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
}

error_t sched_dequeue(process_t *proc)
{
    if (!proc || process_is_idle(proc))
        return ERR_INVALID_ARGUMENT;

    if (!g_core_sched || proc->home_core >= g_sched_core_count)
        return ERR_HOME_CORE_INVALID;

    /* A queued strand is in the runqueue of the core its Fare is reckoned on
     * (sched_place), which is not its home while it was queued on the core
     * that still ran it. Not queued, there is nothing to take out. */
    int16_t core = __atomic_load_n(&proc->fare_core, __ATOMIC_RELAXED);
    if (core < 0 || (uint32_t)core >= g_sched_core_count)
        core = proc->home_core;
    scheduler_state_t *holder = &g_core_sched[core];

    spin_lock(&holder->runqueue.lock);
    runqueue_remove(&holder->runqueue, proc);
    spin_unlock(&holder->runqueue.lock);

    return OK;
}

/* Remove `proc` from whichever core's runqueue currently holds it, if any.
 * process_destroy calls this so a torn-down strand is referenced by NO
 * runqueue before it is freed: a dangling runqueue slot would otherwise
 * dispatch a freed/recycled process_t (the strandtest recycle-race).
 * sched_dequeue() looks in one runqueue; this sweeps
 * every core for the corpse case — process_destroy reaches a DONE strand whose
 * PROC_CRASHED transition never ran sched_dequeue (old state != WORKING).
 *
 * A process is in at most one runqueue (one rq_index), and runqueue_remove
 * validates slot==proc, so the sweep removes it from the one holder and no-ops
 * on every other core. Each runqueue lock is taken alone — no nesting, no
 * lock-order inversion. */
void sched_dequeue_all_cores(process_t *proc)
{
    if (!proc || process_is_idle(proc) || !g_core_sched)
        return;

    for (uint32_t c = 0; c < g_sched_core_count; c++)
    {
        scheduler_state_t *s = &g_core_sched[c];
        spin_lock(&s->runqueue.lock);
        runqueue_remove(&s->runqueue, proc);
        spin_unlock(&s->runqueue.lock);
    }
}

// ---------------------------------------------------------------------------
// Process selection (per-core)
// ---------------------------------------------------------------------------

/* May `proc` be put on a core? Only a strand that is to run — not yet started
 * (PROC_CREATED, which its first dispatch makes WORKING) or WORKING — and not
 * being torn down. Exited, crashed, parked or stopped, it may not, wherever it
 * was found: a runqueue can still hold a strand whose state changed while it
 * sat there, because a change of state takes a strand out of its home core's
 * runqueue only on the way out of WORKING (process_set_state), and not at all
 * from a strand already taken out to be run. Read without the state lock; the
 * order that makes the answer good enough at dispatch is told in schedule(). */
static inline bool SchedMayDispatch(process_t *proc)
{
    process_state_t st = __atomic_load_n(&proc->state, __ATOMIC_ACQUIRE);
    return (st == PROC_CREATED || st == PROC_WORKING) &&
           !__atomic_load_n(&proc->destroying, __ATOMIC_ACQUIRE);
}

process_t *scheduler_select_next(void)
{
    scheduler_state_t *s = scheduler_get_state();
    if (!s) return idle_process_get();

    // Skip parked cores
    if (__atomic_load_n(&s->is_parked, __ATOMIC_ACQUIRE)) {
        return idle_process_get();
    }

    spin_lock(&s->runqueue.lock);

    /* The least Fare first. A strand that may not run (SchedMayDispatch)
     * leaves the runqueue here, and its wake — if it has one coming — queues
     * it again. */
    process_t *next = runqueue_dequeue_best(&s->runqueue);
    while (next && !SchedMayDispatch(next))
        next = runqueue_dequeue_best(&s->runqueue);
    if (next)
        sched_raise_waterline(s, next->fare);

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

    /* Any strand the victim holds, whatever it pays — looked at along the
     * heap, from the least Fare on. */
    process_t *stolen = NULL;
    for (uint32_t i = 0; (stolen = runqueue_at(&vs->runqueue, i)) != NULL; i++)
    {
        /* Never the strand the victim is still running. schedule() puts its
         * outgoing strand back into its own runqueue BEFORE it picks the next
         * one, so for a moment that strand is both queued and on_cpu. Taken
         * here, the thief's dispatch claim loses to the owner and the strand
         * is in no runqueue and on no core — WORKING, and never run again.
         * The victim releases it a few instructions later; it is not ours to
         * take until then. Read under the victim's runqueue lock: a queued
         * strand's claim can only be released, never taken, while we hold it. */
        if (__atomic_load_n(&stolen->on_cpu, __ATOMIC_ACQUIRE) >= 0)
            continue;

        /* Nor one that may not run: the victim's own selection lets it go
         * (scheduler_select_next), and a thief that took it would have nothing
         * to dispatch. */
        if (!SchedMayDispatch(stolen))
            continue;

        /* Nor one whose stack the victim may still stand on: switched out in
         * the victim's schedule() still running, re-queued before the victim
         * picked its next, released before the victim's iretq. */
        if (!sched_stack_left(stolen))
            continue;

        runqueue_remove(&vs->runqueue, stolen);
        break;
    }

    spin_unlock(&vs->runqueue.lock);

    if (stolen) {
        scheduler_state_t *mine = &g_core_sched[my_core];
        spin_lock(&mine->runqueue.lock);
        sched_place(stolen, my_core);
        spin_unlock(&mine->runqueue.lock);
        stolen->home_core = my_core;
    }

    return stolen;
}

/*
 * The person's work keeps a core to itself while outsiders can share one. When
 * this core has just dispatched a strand wearing the Use Context and an
 * outsider — a strand of no Fit — waits in its runqueue, that outsider moves to
 * the least loaded application core running no strand of the context (what
 * waits there and what runs; an idle core first): the outsiders squeeze up
 * among themselves, and a game is not handed a turn in four of a core while an
 * outsider has one of its own. One move a dispatch, so nothing ping-pongs.
 *
 * The move is one step, under both runqueue locks — the lower core's first,
 * the only place two are held — and under the outsider's state_lock, taken
 * without waiting since everywhere else it is taken before a runqueue lock. A
 * park takes a strand out of the runqueue of the core its Fare is reckoned on
 * and a wake puts it into its home core's, each under that lock: either sees
 * the outsider wholly here or wholly there, never in neither — to be queued
 * here again by a wake and then there by the move, in two runqueues at once.
 * Only a strand whose stack this core has left (sched_stack_left): never the
 * one it is switching out right now. Returns whether one moved.
 */
static bool sched_make_room(scheduler_state_t *s, uint8_t me)
{
    if (RunqueueAtomicTotal(&s->runqueue) == 0)
        return false;

    uint8_t  target    = 0xFF;
    uint32_t best_load = UINT32_MAX;
    for (uint8_t c = 0; c < g_sched_core_count; c++) {
        if (c == me || g_amp.cores[c].is_kcore || !amp_core_online(&g_amp.cores[c]))
            continue;
        if (__atomic_load_n(&g_core_sched[c].context_here, __ATOMIC_RELAXED))
            continue;
        uint32_t load = RunqueueAtomicTotal(&g_core_sched[c].runqueue) +
                        (__atomic_load_n(&g_core_sched[c].busy_here, __ATOMIC_RELAXED) ? 1u : 0u);
        if (load < best_load) {
            best_load = load;
            target    = c;
        }
    }
    if (target == 0xFF)
        return false;

    scheduler_state_t *t      = &g_core_sched[target];
    spinlock_t        *first  = me < target ? &s->runqueue.lock : &t->runqueue.lock;
    spinlock_t        *second = me < target ? &t->runqueue.lock : &s->runqueue.lock;
    spin_lock(first);
    spin_lock(second);

    /* Its state is read under its state_lock, where it is decisive. */
    process_t *outsider = NULL;
    for (uint32_t i = 0; (outsider = runqueue_at(&s->runqueue, i)) != NULL; i++) {
        if (__atomic_load_n(&outsider->on_cpu, __ATOMIC_ACQUIRE) >= 0 ||
            UseContextFit(outsider) != 0 || !sched_stack_left(outsider) ||
            !spin_trylock(&outsider->state_lock))
            continue;
        if (SchedMayDispatch(outsider))
            break;
        spin_unlock(&outsider->state_lock);
    }

    bool moved = false;
    if (outsider) {
        uint64_t fare      = outsider->fare;
        int16_t  fare_core = outsider->fare_core;
        runqueue_remove(&s->runqueue, outsider);
        sched_place(outsider, target);
        moved = runqueue_enqueue(&t->runqueue, outsider);
        if (moved) {
            outsider->home_core = target;
        } else {
            /* The target's runqueue at its ceiling: the outsider stays here at
             * the Fare it had, in the room it left under these same locks. */
            outsider->fare = fare;
            __atomic_store_n(&outsider->fare_core, fare_core, __ATOMIC_RELAXED);
            (void)runqueue_enqueue(&s->runqueue, outsider);
        }
        spin_unlock(&outsider->state_lock);
    }
    spin_unlock(second);
    spin_unlock(first);

    /* A parked core is woken for it, as an enqueue wakes one. */
    if (moved) {
        if (scheduler_is_core_parked(target))
            scheduler_unpark_core(target);
        lapic_send_ipi(g_amp.cores[target].lapic_id, IPI_WAKE_VECTOR);
    }
    return moved;
}

/* A dispatch claim lost to the core that still runs the strand. By
 * construction this cannot happen — a wake enqueues a running strand only onto
 * its owner (sched_enqueue) and a steal skips one (sched_try_steal) — so
 * reaching here is a defect, and it is said. The strand is not dropped for it:
 * it is back in the owner's runqueue, where the owner, which is the only core
 * entitled to take it, will find it. Under the owner's runqueue lock the state
 * read is decisive (the same reasoning as the re-enqueue in schedule()): a
 * strand parked meanwhile is left to its wake. */
static void sched_hand_back(process_t *proc, int16_t owner)
{
    static uint32_t said;
    if (__atomic_fetch_add(&said, 1u, __ATOMIC_RELAXED) == 0)
        kprintf("[SCHED] DEFECT: core %u lost the dispatch claim on PID %u to "
                "core %d, which still runs it — handed back to that core\n",
                (unsigned)amp_get_core_index(), proc->pid, (int)owner);

    if (owner < 0 || (uint32_t)owner >= g_sched_core_count)
        owner = (int16_t)proc->home_core;
    scheduler_state_t *os = &g_core_sched[owner];

    spin_lock(&os->runqueue.lock);
    bool queued = false;
    if (__atomic_load_n(&proc->state, __ATOMIC_ACQUIRE) == PROC_WORKING &&
        !runqueue_contains(&os->runqueue, proc))
    {
        sched_place(proc, (uint8_t)owner);
        queued = runqueue_enqueue(&os->runqueue, proc);
    }
    spin_unlock(&os->runqueue.lock);

    if (queued && owner != (int16_t)amp_get_core_index())
        lapic_send_ipi(g_amp.cores[owner].lapic_id, IPI_WAKE_VECTOR);
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

    uint64_t now = rdtsc();
    scheduler_account_current(s, now);

    /* Read and cleared at every entry: a turn given away is settled by the
     * schedule() that takes it, and by no later one. */
    bool turn_given = s->turn_given;
    s->turn_given = false;

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

    /* A kernel context in the middle of a Scribe pass, or standing over a
     * command on a medium, stays on this core until it is done: the idle
     * context of an application core, or of the only core, writes the volume
     * from TagFSServiceIfPending, and a switch away from it here would stop
     * the pass mid-write — or keep a command slot from a caller on this same
     * core whose interrupts are shut — for as long as the process switched to
     * ran. Ring 3 never holds one. */
    if (frame && (frame->cs & 3) == 0 && s->held) {
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

    /* The strand switched out pays its Fare for the time it held this core —
     * and one that gave its turn away stands behind the strand that pays the
     * least — and goes back among this core's strands at what it now owes,
     * while it still works. Paid under the runqueue lock: a strand woken while
     * it still held this core is queued here already (sched_enqueue), and its
     * slot follows its Fare. */
    if (current && !process_is_idle(current))
    {
        uint8_t me = amp_get_core_index();
        spin_lock(&s->runqueue.lock);
        if (current->fare_tsc != 0 && now > current->fare_tsc)
            current->fare += sched_fare_for(current, now - current->fare_tsc);
        uint64_t next_fare;
        if (turn_given && sched_least_other(&s->runqueue, current, &next_fare) &&
            current->fare <= next_fare)
            current->fare = next_fare + 1;
        /* Re-check state UNDER the runqueue lock before committing the enqueue.
         * process_set_state's WORKING→non-WORKING transition takes THIS same
         * runqueue lock for its sched_dequeue (the core a running strand's
         * Fare is reckoned on is the core running it), so observing WORKING
         * here is decisive: either the transition has not run yet (we enqueue;
         * its later dequeue, under this lock, removes us) or it already ran
         * (we see non-WORKING and skip).
         * Read without the lock, the state races a concurrent exit/kill and
         * can leave a DONE/CRASHED strand in the runqueue — which the runtime
         * reaper then frees while a dangling slot still points at it, so a
         * freed/recycled process_t gets dispatched
         * (the strand-churn fault that runtime full-process reaping unmasked).
         * Read state atomically — NOT process_get_state(), which takes
         * state_lock and would invert the state_lock→runqueue_lock order. */
        if (runqueue_contains(&s->runqueue, current))
        {
            runqueue_settle(&s->runqueue, current);
        }
        else if (__atomic_load_n(&current->state, __ATOMIC_ACQUIRE) == PROC_WORKING)
        {
            if (current->fare_core != (int16_t)me)
                sched_place(current, me);
            runqueue_enqueue(&s->runqueue, current);
        }
        spin_unlock(&s->runqueue.lock);
    }

    /* RT-10: on one core idle may be owed the next quantum (idle.c, IdleOwed). */
    process_t *next = (current && !process_is_idle(current) && IdleOwed())
                          ? idle_process_get()
                          : scheduler_select_next();

    // Work stealing
    if (process_is_idle(next) && amp_is_appcore() && g_amp.multicore_active)
    {
        uint64_t ticks_since_steal = s->total_ticks - s->last_steal_tick;
        if (ticks_since_steal >= g_steal_cooldown)
        {
            s->last_steal_tick = s->total_ticks;
            process_t *stolen = sched_try_steal(amp_get_core_index());
            if (stolen)
            {
                next = stolen;
            }
        }
    }

    // Switch
    spin_lock(&s->scheduler_lock);

    /* Running-XOR-runqueue interlock: claim `next` exclusively before committing
     * it as current_process. If another core still owns it (mid-eviction there —
     * e.g. a strand transiently re-enqueued while still current), do NOT dispatch
     * it here: two cores would then run context_save/restore over one shared
     * proc->context and tear it (the strandtest UEFI-16c corruption). Fall back to
     * idle; the next tick re-selects. The owner releases on_cpu below when it
     * switches the strand out. on_cpu == me means a consecutive run (already
     * ours) — keep it without re-claiming. */
    process_t *refused = NULL;
    int16_t    refused_owner = -1;
    if (next && !process_is_idle(next)) {
        int16_t me8     = (int16_t)amp_get_core_index();
        bool    claimed = false;
        if (__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE) != me8) {
            int16_t expect = -1;
            if (__atomic_compare_exchange_n(&next->on_cpu, &expect, me8,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                claimed = true;
            } else {
                /* Another core still owns `next` — averting double-dispatch.
                 * `next` is already out of every runqueue, so it is handed back
                 * to its owner below, once this core's scheduler_lock is gone. */
                refused       = next;
                refused_owner = expect;
                next = idle_process_get();
            }
        }

        /* Only now, holding the claim, is `next` asked whether it may run.
         *
         * Between scheduler_select_next taking it out of the runqueue and this
         * point it was in no runqueue and on no core, and a change of state made
         * then — its exit (SysProcKill: PROC_DONE), a park (SysAddrPark,
         * SysTurnIn: PROC_WAITING), its destruction — found nothing to take it
         * out of: sched_dequeue looks in a runqueue. Asked before the claim, as
         * this was, the answer could be stale by the time the strand ran, and
         * this core started a strand that had exited — whose teardown, seeing it
         * on no core, then freed its FPU buffer and kernel stack under it
         * (measured: fxrstor through a NULL fpu_state in context_restore_to_frame,
         * and a return address read from a kernel stack already freed).
         *
         * The claim is a locked compare-exchange, a full barrier, so the reads
         * below come after it. Whoever changes the state stores first and looks
         * at on_cpu after a full barrier of its own (process_set_state for a
         * park, process_destroy for the teardown), so one of the two always sees
         * the other: either this core sees the new state and lets the strand
         * go, or the other side sees it claimed and treats it as running — a
         * strand parked on its CPU is asked to step aside, a strand on a CPU is
         * not torn down. A consecutive run holds its claim already and needs no
         * barrier: nothing can tear down the strand this core runs.
         *
         * A strand let go is in no runqueue, which is where it belongs: an exited
         * one is the reaper's, a parked one is queued again by its wake. Idle runs
         * instead, never NULL — with NULL the IRQ frame would resume `current`,
         * re-enqueued above, both running and queued. */
        if (!process_is_idle(next) && !SchedMayDispatch(next)) {
            if (claimed)
                __atomic_store_n(&next->on_cpu, (int16_t)-1, __ATOMIC_RELEASE);
            next = idle_process_get();
        }
    }

    // Set kernel stack (top + floor) for THIS core, for the strand that will run
    // here — including idle, so the REACT headroom guard reads the correct
    // geometry while idle runs. Set only once `next` is final: a strand let go
    // above must not leave this core pointing at its stack.
    if (next->kernel_stack_top)
    {
        per_core_set_kernel_rsp((uint64_t)next->kernel_stack_top,
                                (uint64_t)next->kernel_stack);
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

        /* Processor time. total_cpu_time was a field that had been zeroed at
         * creation and never written again since it was added — a promise in a
         * struct, and the reason std::clock() had nothing true to return until
         * Ф41-e.
         *
         * Measured with the TSC, not with the scheduler tick, and the first
         * attempt used the tick: at that granularity whoever is current when
         * the tick fires is credited the WHOLE tick, so two processes that
         * alternate every tick are EACH credited 100% of the wall clock. That
         * is not a rounding error, it is a factor of two per participant, and
         * on a one-core boot it made std::clock() read exactly equal to wall
         * time — including across a 300 ms sleep. The TSC is the only clock
         * here fine enough to say who actually had the core.
         *
         * Both ends of the delta are taken on THIS core (a process is switched
         * away by the core running it), so cross-core TSC skew cannot enter.
         * Two RDTSCs per switch, ~25 cycles each, against a switch rate of a
         * few hundred per second per core. */
        if (current->cpu_tsc_stamp != 0) {
            const uint64_t khz = cpu_get_tsc_freq_khz();
            const uint64_t now_tsc = rdtsc();
            if (khz != 0 && now_tsc > current->cpu_tsc_stamp) {
                const uint64_t used = ((now_tsc - current->cpu_tsc_stamp) * 1000ULL) / khz;
                __atomic_store_n(&current->total_cpu_time,
                                 __atomic_load_n(&current->total_cpu_time, __ATOMIC_RELAXED) + used,
                                 __ATOMIC_RELAXED);
            }
            current->cpu_tsc_stamp = 0;   /* no longer holding the core */
        }
    }
    s->current_process = next;
    bool busy_now    = !process_is_idle(next);
    bool context_now = busy_now && UseContextFit(next) != 0;
    __atomic_store_n(&s->busy_here, busy_now, __ATOMIC_RELAXED);
    __atomic_store_n(&s->context_here, context_now, __ATOMIC_RELAXED);
    /* The ask to step aside is recomputed at every dispatch: true exactly when
     * somebody is left waiting in this core's runqueue. */
    if (!process_is_idle(next))
        KResultStepAside(next, RunqueueAtomicTotal(&s->runqueue) != 0);
    /* Release the outgoing strand's dispatch claim AFTER current_process moves to
     * `next`: once on_cpu is -1 the strand is no longer current_process on this
     * core, so a core that then claims it dispatches it singly (no double). */
    if (current && current != next && !process_is_idle(current)) {
        __atomic_store_n(&current->on_cpu, (int16_t)-1, __ATOMIC_RELEASE);
    }
    spin_unlock(&s->scheduler_lock);

    if (refused)
        sched_hand_back(refused, refused_owner);

    /* The person's work keeps its core to itself while an outsider can move —
     * and the ask to step aside is asked again if nobody is left to step
     * aside for. */
    if (context_now && g_amp.multicore_active && sched_make_room(s, amp_get_core_index())) {
        spin_lock(&s->scheduler_lock);
        KResultStepAside(next, RunqueueAtomicTotal(&s->runqueue) != 0);
        spin_unlock(&s->scheduler_lock);
    }

    /* Nightwatch: real work dispatched here, so this core is no longer idle
     * and a later stall counts as a new episode. One byte store. */
    if (!process_is_idle(next))
        nightwatch_core_busy(amp_get_core_index());

    // PROC_CREATED → PROC_WORKING.  `next` is never NULL: scheduler_select_next
    // returns the idle task when nothing is runnable, and the destroying-task
    // re-check above also falls back to idle — so no NULL guard is needed here.
    // Looked at and written in one hold of the lock: a program ended between
    // a look and a later write would have its CRASHED written over, and death
    // is terminal (process_set_state).
    spin_lock(&next->state_lock);
    if (next->state == PROC_CREATED)
        next->state = PROC_WORKING;
    spin_unlock(&next->state_lock);
    next->last_run_time = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    /* What it pays from here: charged at this core's next schedule(). */
    if (!process_is_idle(next)) next->fare_tsc = rdtsc();
    /* Start of this strand's processor-time slice. Only when it is actually
     * taking the core from someone else: on a consecutive run the slice never
     * ended, and restamping would discard the part already served. The idle
     * task is excluded — time spent idle is not time given to anyone. */
    if (next != current && !process_is_idle(next)) next->cpu_tsc_stamp = rdtsc();

    // Restore context
    context_restore_to_frame(next, frame);
}
