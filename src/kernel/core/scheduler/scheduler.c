#include "scheduler.h"
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

// ---------------------------------------------------------------------------
// Global monotonic tick — incremented by every core on every timer tick.
// Used for starvation detection so migrated processes are treated fairly.
// ---------------------------------------------------------------------------
volatile uint64_t g_global_tick = 0;

// ---------------------------------------------------------------------------
// Per-core scheduler array
// ---------------------------------------------------------------------------
static scheduler_state_t g_core_sched[MAX_CORES];

// ---------------------------------------------------------------------------
// Global use_context (tag-based focus) — shared across all cores
// ---------------------------------------------------------------------------
static use_context_t g_use_context;
static spinlock_t g_context_lock;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

static void sched_init_one(scheduler_state_t *s)
{
    memset(s, 0, sizeof(scheduler_state_t));
    s->current_process = NULL;
    spinlock_init(&s->scheduler_lock);
    s->total_ticks = 0;
    runqueue_init(&s->runqueue);
}

void scheduler_init(void)
{
    debug_printf("[SCHEDULER] Initializing per-core O(1) scheduler...\n");

    // Initialize all slots to a clean state
    for (int i = 0; i < MAX_CORES; i++)
    {
        sched_init_one(&g_core_sched[i]);
    }

    // Global use_context
    memset(&g_use_context, 0, sizeof(g_use_context));
    g_use_context.enabled = false;
    spinlock_init(&g_context_lock);

    debug_printf("[SCHEDULER] Per-core O(1) scheduler ready (4 priority levels, capacity=%d/level)\n",
                 RUNQUEUE_CAPACITY);
}

void scheduler_init_core(uint8_t core_index)
{
    scheduler_state_t *s = &g_core_sched[core_index];
    sched_init_one(s);

    debug_printf("[SCHEDULER] Core %u scheduler initialized\n", core_index);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

scheduler_state_t *scheduler_get_state(void)
{
    uint8_t idx = amp_get_core_index();
    return &g_core_sched[idx];
}

scheduler_state_t *scheduler_get_core(uint8_t core_idx)
{
    return &g_core_sched[core_idx];
}

// ---------------------------------------------------------------------------
// Use context (global tag-based focus)
// ---------------------------------------------------------------------------

bool scheduler_matches_use_context(process_t *proc)
{
    if (!proc)
        return false;

    // All reads of g_use_context must be under g_context_lock.
    // Without the lock, a concurrent scheduler_clear_use_context() could
    // free overflow_ids between our enabled check and the overflow loop,
    // causing use-after-free.
    spin_lock(&g_context_lock);

    if (!g_use_context.enabled)
    {
        spin_unlock(&g_context_lock);
        return false;
    }

    // AND semantics: process must have ALL context tags
    uint64_t ctx_bits = g_use_context.context_bits;
    if (ctx_bits && (proc->tag_bits & ctx_bits) != ctx_bits)
    {
        spin_unlock(&g_context_lock);
        return false;
    }

    // Overflow tags (ids >= 64)
    // CRITICAL: Lock-free read pattern for tag_overflow_ids.
    //
    // Writer (process_set_tag_bit) uses:
    //   1. Atomic store-release of new pointer
    //   2. Atomic store-release of new capacity
    //   3. mfence() before kfree(old_ids)
    //
    // We use atomic load-acquire to ensure:
    //   - We see the new pointer AND all data written before it
    //   - We see a consistent (pointer, count) pair
    //   - Old buffer is not freed while we read (protected by mfence in writer)
    //
    // This is safe without process_lock because:
    //   - We only read, never write
    //   - Writer publishes atomically with release semantics
    //   - Writer delays kfree until after mfence
    uint16_t overflow_count = __atomic_load_n(&proc->tag_overflow_count, __ATOMIC_ACQUIRE);
    uint16_t *overflow_ids = __atomic_load_n(&proc->tag_overflow_ids, __ATOMIC_ACQUIRE);

    bool match = true;
    for (uint16_t j = 0; j < g_use_context.overflow_count && match; j++)
    {
        bool found = false;
        for (uint16_t i = 0; i < overflow_count && found == false; i++)
        {
            if (overflow_ids[i] == g_use_context.overflow_ids[j])
            {
                found = true;
            }
        }
        if (!found)
            match = false;
    }

    spin_unlock(&g_context_lock);
    return match;
}

// ---------------------------------------------------------------------------
// Priority + RunQueue
// ---------------------------------------------------------------------------

// Determine priority level for a process.  Uses the global tick counter
// for starvation detection so core migrations don't confuse the calculation.
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
    if (ticks_since_run >= SCHEDULER_MILD_STARVATION_TICKS)
    {
        return SCHED_PRIO_STARVED;
    }

    // Context match boost (scheduler_matches_use_context acquires g_context_lock
    // internally and returns false when disabled, so no lock-free pre-check needed)
    if (scheduler_matches_use_context(proc))
    {
        return SCHED_PRIO_CONTEXT;
    }

    return SCHED_PRIO_NORMAL;
}

// Enqueue a process into its home core's RunQueue.
// Returns: true on success, false on error (invalid proc, idle proc, or bad home_core)
bool sched_enqueue(process_t *proc)
{
    if (!proc || process_is_idle(proc))
        return false;

    // CRITICAL: Bounds-check home_core before indexing g_core_sched[]
    if (proc->home_core >= g_amp.total_cores)
    {
        debug_printf("[SCHED] ERROR: sched_enqueue: PID %u has invalid home_core %u (max %u)\n",
                     proc->pid, proc->home_core, g_amp.total_cores);
        return false;
    }

    scheduler_state_t *home = &g_core_sched[proc->home_core];

    int prio = sched_determine_priority(proc);

    spin_lock(&home->runqueue.lock);
    bool result = false;
    if (!runqueue_contains(&home->runqueue, proc))
    {
        result = runqueue_enqueue(&home->runqueue, proc, prio);
        if (!result)
        {
            debug_printf("[SCHED] ERROR: sched_enqueue: RunQueue full for PID %u on core %u\n",
                         proc->pid, proc->home_core);
        }
    }
    else
    {
        result = true; // Already enqueued, consider success
    }
    spin_unlock(&home->runqueue.lock);

    return result;
}

// Enqueue a process onto a specific core's RunQueue (cross-core safe).
void sched_enqueue_on(uint8_t core_idx, process_t *proc)
{
    if (!proc || process_is_idle(proc))
        return;

    scheduler_state_t *target = &g_core_sched[core_idx];

    int prio = sched_determine_priority(proc);

    spin_lock(&target->runqueue.lock);
    if (!runqueue_contains(&target->runqueue, proc))
    {
        runqueue_enqueue(&target->runqueue, proc, prio);
    }
    spin_unlock(&target->runqueue.lock);
}

// Remove a process from its home core's RunQueue.
void sched_dequeue(process_t *proc)
{
    if (!proc || process_is_idle(proc))
        return;

    // CRITICAL: Bounds-check home_core before indexing g_core_sched[]
    if (proc->home_core >= g_amp.total_cores)
    {
        debug_printf("[SCHED] ERROR: sched_dequeue: PID %u has invalid home_core %u (max %u)\n",
                     proc->pid, proc->home_core, g_amp.total_cores);
        return;
    }

    scheduler_state_t *home = &g_core_sched[proc->home_core];

    spin_lock(&home->runqueue.lock);
    runqueue_remove(&home->runqueue, proc);
    spin_unlock(&home->runqueue.lock);
}

// ---------------------------------------------------------------------------
// Process selection (per-core)
// ---------------------------------------------------------------------------

// O(1) process selection from the CURRENT core's RunQueue.
process_t *scheduler_select_next(void)
{
    scheduler_state_t *s = scheduler_get_state();
    bool fairness_round = (s->total_ticks % SCHEDULER_FAIRNESS_INTERVAL == 0);

    spin_lock(&s->runqueue.lock);

    process_t *next = NULL;

    if (fairness_round)
    {
        // Mask out CONTEXT priority — let STARVED/NORMAL win
        uint32_t fair_bitmap = s->runqueue.active_bitmap & ~(1u << SCHED_PRIO_CONTEXT);
        if (fair_bitmap != 0)
        {
            int prio = 31 - __builtin_clz(fair_bitmap);
            SchedQueue *q = &s->runqueue.queues[prio];
            next = q->procs[q->head];
            q->procs[q->head] = NULL;
            q->head = (q->head + 1) % RUNQUEUE_CAPACITY;
            q->count--;
            if (q->count == 0)
                s->runqueue.active_bitmap &= ~(1u << prio);
        }
        else
        {
            next = runqueue_dequeue_best(&s->runqueue);
        }
    }
    else
    {
        next = runqueue_dequeue_best(&s->runqueue);
    }

    // CRITICAL: Skip processes marked for destruction.
    // A process can be selected between being dequeued and returned.
    // Check destroying flag atomically to avoid race with process_destroy().
    //
    // FIX: Do NOT enqueue skipped processes back — they will be cleaned up
    // by process_destroy when ref_count reaches 0. Re-enqueuing creates a
    // race where the process could be selected again while being destroyed.
    while (next && !process_is_idle(next) &&
           __atomic_load_n(&next->destroying, __ATOMIC_ACQUIRE))
    {
        // Process is being destroyed — skip it and get next.
        // Do NOT put it back — let process_destroy handle cleanup.
        process_t *skipped = next;
        next = runqueue_dequeue_best(&s->runqueue);
        // skipped will be freed when its ref_count reaches 0
    }

    spin_unlock(&s->runqueue.lock);

    if (!next)
    {
        next = idle_process_get();
    }

    return next;
}

// ---------------------------------------------------------------------------
// Work stealing: idle App Cores steal from the busiest neighbor
// ---------------------------------------------------------------------------

static process_t *sched_try_steal(uint8_t my_core)
{
    // Find the App Core with the most processes in its RunQueue
    uint8_t victim = 0xFF;
    uint32_t max_count = 1; // only steal if victim has > 1

    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        if (c == my_core)
            continue;
        if (g_amp.cores[c].is_kcore)
            continue;
        if (!g_amp.cores[c].online)
            continue;

        // Read count under lock to avoid torn reads during concurrent
        // enqueue/dequeue on the remote core's runqueue.
        spin_lock(&g_core_sched[c].runqueue.lock);
        uint32_t cnt = runqueue_total_count(&g_core_sched[c].runqueue);
        spin_unlock(&g_core_sched[c].runqueue.lock);
        if (cnt > max_count)
        {
            max_count = cnt;
            victim = c;
        }
    }

    if (victim == 0xFF)
        return NULL;

    // Lock victim's RunQueue and steal the lowest-priority process
    scheduler_state_t *vs = &g_core_sched[victim];
    spin_lock(&vs->runqueue.lock);

    // Re-check under lock
    if (runqueue_total_count(&vs->runqueue) <= 1)
    {
        spin_unlock(&vs->runqueue.lock);
        return NULL;
    }

    // Steal from the lowest priority level that has processes (least impactful)
    process_t *stolen = NULL;
    for (int prio = SCHED_PRIO_NORMAL; prio >= SCHED_PRIO_IDLE; prio--)
    {
        SchedQueue *q = &vs->runqueue.queues[prio];
        if (q->count == 0)
            continue;

        stolen = q->procs[q->head];
        q->procs[q->head] = NULL;
        q->head = (q->head + 1) % RUNQUEUE_CAPACITY;
        q->count--;
        if (q->count == 0)
            vs->runqueue.active_bitmap &= ~(1u << prio);
        break;
    }

    spin_unlock(&vs->runqueue.lock);

    if (stolen)
    {
        // Reparent: future enqueues will target our core
        stolen->home_core = my_core;
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

    spin_lock(&s->scheduler_lock);
    process_t *current = s->current_process;
    spin_unlock(&s->scheduler_lock);

    // Save context of current process
    if (current)
    {
        process_state_t cur_state = process_get_state(current);
        if (cur_state == PROC_WORKING || cur_state == PROC_WAITING)
        {
            context_save_from_frame(current, frame);
        }
    }

    // Re-enqueue current process into THIS core's RunQueue if still WORKING
    if (current && !process_is_idle(current) &&
        process_get_state(current) == PROC_WORKING)
    {
        int prio = sched_determine_priority(current);
        spin_lock(&s->runqueue.lock);
        if (!runqueue_contains(&s->runqueue, current))
        {
            runqueue_enqueue(&s->runqueue, current, prio);
        }
        spin_unlock(&s->runqueue.lock);
    }

    // O(1) select next process from THIS core's RunQueue
    process_t *next = scheduler_select_next();

    // Work stealing: if our RunQueue is empty (idle selected) and we're an
    // App Core in multi-core mode, try to steal from the busiest neighbor.
    if (process_is_idle(next) && amp_is_appcore() && g_amp.multicore_active)
    {
        uint64_t ticks_since_steal = s->total_ticks - s->last_steal_tick;
        if (ticks_since_steal >= STEAL_COOLDOWN_TICKS)
        {
            s->last_steal_tick = s->total_ticks;
            process_t *stolen = sched_try_steal(amp_get_core_index());
            if (stolen)
            {
                next = stolen;
            }
        }
    }

    // Set kernel stack for ring 3 -> ring 0 transitions
    if (!process_is_idle(next) && next->kernel_stack_top)
    {
        per_core_set_kernel_rsp((uint64_t)next->kernel_stack_top);
    }

    // Switch current process
    spin_lock(&s->scheduler_lock);
    s->current_process = next;
    spin_unlock(&s->scheduler_lock);

    // PROC_CREATED → PROC_WORKING: set state directly to avoid spurious enqueue
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

    // Cleanup is handled by K-Cores in kcore_run_loop() (distributed).
    // App Cores spend their time running user processes, not cleaning.

    // Restore next process context to frame
    context_restore_to_frame(next, frame);

    // do NOT sti here — iretq restores RFLAGS atomically including IF=1
}

// ---------------------------------------------------------------------------
// Use context set/clear (global)
// ---------------------------------------------------------------------------

void scheduler_set_use_context(const char *tags[], uint32_t count)
{
    if (!tags || count == 0)
    {
        debug_printf("[SCHEDULER] ERROR: Invalid use context parameters\n");
        return;
    }

    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry)
        return;

    spin_lock(&g_context_lock);

    g_use_context.context_bits = 0;
    g_use_context.overflow_count = 0;

    for (uint32_t i = 0; i < count; i++)
    {
        if (!tags[i])
            continue;

        char key[256], value[256];
        tagfs_parse_tag(tags[i], key, sizeof(key), value, sizeof(value));

        uint16_t tid = tag_registry_intern(fs->registry, key, value[0] ? value : NULL);
        if (tid == TAGFS_INVALID_TAG_ID)
            continue;

        if (tid < 64)
        {
            g_use_context.context_bits |= ((uint64_t)1 << tid);
        }
        else
        {
            if (g_use_context.overflow_count >= g_use_context.overflow_capacity)
            {
                uint16_t new_cap = g_use_context.overflow_capacity == 0
                                       ? 8
                                       : g_use_context.overflow_capacity * 2;
                uint16_t *new_ids = kmalloc(sizeof(uint16_t) * new_cap);
                if (new_ids)
                {
                    if (g_use_context.overflow_ids)
                    {
                        memcpy(new_ids, g_use_context.overflow_ids,
                               sizeof(uint16_t) * g_use_context.overflow_count);
                        kfree(g_use_context.overflow_ids);
                    }
                    g_use_context.overflow_ids = new_ids;
                    g_use_context.overflow_capacity = new_cap;
                }
            }
            if (g_use_context.overflow_count < g_use_context.overflow_capacity)
            {
                g_use_context.overflow_ids[g_use_context.overflow_count++] = tid;
            }
        }
    }

    g_use_context.enabled = true;
    spin_unlock(&g_context_lock);

    debug_printf("[SCHEDULER] Use context set: %u tags\n", count);
}

void scheduler_clear_use_context(void)
{
    spin_lock(&g_context_lock);
    g_use_context.enabled = false;
    g_use_context.context_bits = 0;
    if (g_use_context.overflow_ids)
    {
        kfree(g_use_context.overflow_ids);
        g_use_context.overflow_ids = NULL;
    }
    g_use_context.overflow_count = 0;
    g_use_context.overflow_capacity = 0;
    spin_unlock(&g_context_lock);
    debug_printf("[SCHEDULER] Use context cleared\n");
}
