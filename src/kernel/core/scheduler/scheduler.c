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

static scheduler_state_t sched;

void scheduler_init(void) {
    debug_printf("[SCHEDULER] Initializing O(1) scheduler...\n");

    memset(&sched, 0, sizeof(scheduler_state_t));
    sched.current_process = NULL;
    spinlock_init(&sched.scheduler_lock);
    sched.use_context.enabled = false;
    sched.use_context.context_bits = 0;
    sched.use_context.overflow_ids = NULL;
    sched.use_context.overflow_count = 0;
    sched.use_context.overflow_capacity = 0;
    sched.total_ticks = 0;
    spinlock_init(&sched.context_lock);
    runqueue_init(&sched.runqueue);

    debug_printf("[SCHEDULER] O(1) scheduler initialized (4 priority levels, capacity=%d/level)\n",
                 RUNQUEUE_CAPACITY);
}

bool scheduler_matches_use_context(process_t* proc) {
    if (!proc) return false;
    if (!sched.use_context.enabled) return false;

    // AND semantics: process must have ALL context tags
    // Fast path: check bitmap tags (ids < 64)
    uint64_t ctx_bits = sched.use_context.context_bits;
    if (ctx_bits && (proc->tag_bits & ctx_bits) != ctx_bits) return false;

    // Overflow tags (ids >= 64): process must contain every overflow context tag
    spin_lock(&sched.context_lock);
    bool match = true;
    for (uint16_t j = 0; j < sched.use_context.overflow_count && match; j++) {
        bool found = false;
        for (uint16_t i = 0; i < proc->tag_overflow_count; i++) {
            if (proc->tag_overflow_ids[i] == sched.use_context.overflow_ids[j]) {
                found = true;
                break;
            }
        }
        if (!found) match = false;
    }
    spin_unlock(&sched.context_lock);
    return match;
}

// Determine priority level for a process. Called at enqueue time.
// CONTEXT(3) > STARVED(2) > NORMAL(1) > IDLE(0)
int sched_determine_priority(process_t* proc) {
    if (!proc) return SCHED_PRIO_NORMAL;

    // Starvation check (highest non-context priority)
    uint64_t ticks_since_run = 0;
    if (sched.total_ticks >= proc->last_run_time) {
        ticks_since_run = sched.total_ticks - proc->last_run_time;
    }
    if (ticks_since_run >= SCHEDULER_MILD_STARVATION_TICKS) {
        return SCHED_PRIO_STARVED;
    }

    // Context match boost
    if (sched.use_context.enabled && scheduler_matches_use_context(proc)) {
        return SCHED_PRIO_CONTEXT;
    }

    return SCHED_PRIO_NORMAL;
}

// Enqueue a process into the O(1) RunQueue.
// Caller: process_set_state hook, schedule() re-enqueue.
void sched_enqueue(process_t* proc) {
    if (!proc || process_is_idle(proc)) return;

    // Don't enqueue the currently running process
    if (proc == sched.current_process) return;

    int prio = sched_determine_priority(proc);

    spin_lock(&sched.runqueue.lock);
    if (!runqueue_contains(&sched.runqueue, proc)) {
        runqueue_enqueue(&sched.runqueue, proc, prio);
    }
    spin_unlock(&sched.runqueue.lock);
}

// Remove a process from the RunQueue (e.g., when it leaves PROC_WORKING).
void sched_dequeue(process_t* proc) {
    if (!proc || process_is_idle(proc)) return;

    spin_lock(&sched.runqueue.lock);
    runqueue_remove(&sched.runqueue, proc);
    spin_unlock(&sched.runqueue.lock);
}

// O(1) process selection using priority bitmap.
// Fairness round (1 in every SCHEDULER_FAIRNESS_INTERVAL ticks):
//   skip CONTEXT level so starved/normal processes get a chance.
process_t* scheduler_select_next(void) {
    bool fairness_round = (sched.total_ticks % SCHEDULER_FAIRNESS_INTERVAL == 0);

    spin_lock(&sched.runqueue.lock);

    process_t* next = NULL;

    if (fairness_round) {
        // Mask out CONTEXT priority — let STARVED/NORMAL win
        uint32_t fair_bitmap = sched.runqueue.active_bitmap & ~(1u << SCHED_PRIO_CONTEXT);
        if (fair_bitmap != 0) {
            int prio = 31 - __builtin_clz(fair_bitmap);
            SchedQueue* q = &sched.runqueue.queues[prio];
            next = q->procs[q->head];
            q->procs[q->head] = NULL;
            q->head = (q->head + 1) % RUNQUEUE_CAPACITY;
            q->count--;
            if (q->count == 0)
                sched.runqueue.active_bitmap &= ~(1u << prio);
        } else {
            // Only CONTEXT processes available — dequeue normally
            next = runqueue_dequeue_best(&sched.runqueue);
        }
    } else {
        next = runqueue_dequeue_best(&sched.runqueue);
    }

    spin_unlock(&sched.runqueue.lock);

    if (!next) {
        next = idle_process_get();
    }

    return next;
}

// Single unified schedule() function — O(1) via priority bitmap RunQueue.
// Saves context of current process, selects next, restores context.
void schedule(void* frame_ptr) {
    interrupt_frame_t* frame = (interrupt_frame_t*)frame_ptr;

    __asm__ volatile("cli");

    spin_lock(&sched.scheduler_lock);
    process_t* current = sched.current_process;
    spin_unlock(&sched.scheduler_lock);

    // Save context of current process
    if (current) {
        process_state_t cur_state = process_get_state(current);
        if (cur_state == PROC_WORKING || cur_state == PROC_WAITING) {
            context_save_from_frame(current, frame);
        }
    }

    // Re-enqueue current process into RunQueue if still WORKING
    if (current && !process_is_idle(current) &&
        process_get_state(current) == PROC_WORKING) {
        int prio = sched_determine_priority(current);
        spin_lock(&sched.runqueue.lock);
        if (!runqueue_contains(&sched.runqueue, current)) {
            runqueue_enqueue(&sched.runqueue, current, prio);
        }
        spin_unlock(&sched.runqueue.lock);
    }

    // O(1) select next process
    process_t* next = scheduler_select_next();

    // Set kernel stack for ring 3 -> ring 0 transitions (TSS.rsp0 + PerCpuData.kernel_rsp)
    if (!process_is_idle(next) && next->kernel_stack_top) {
        per_core_set_kernel_rsp((uint64_t)next->kernel_stack_top);
    }

    // Switch current process
    spin_lock(&sched.scheduler_lock);
    sched.current_process = next;
    spin_unlock(&sched.scheduler_lock);

    // PROC_CREATED → PROC_WORKING: set state directly to avoid spurious enqueue
    if (process_get_state(next) == PROC_CREATED) {
        spin_lock(&next->state_lock);
        next->state = PROC_WORKING;
        spin_unlock(&next->state_lock);
    }
    next->last_run_time = sched.total_ticks;

    // Track consecutive runs
    if (current && current != next) {
        current->consecutive_runs = 0;
        next->consecutive_runs = 1;
    } else if (next) {
        next->consecutive_runs++;
    }

    // Periodic deferred cleanup
    if ((sched.total_ticks % 10) == 0) {
        process_cleanup_deferred();
    }

    // Restore next process context to frame
    context_restore_to_frame(next, frame);

    // do NOT sti here — iretq restores RFLAGS atomically including IF=1
}

void scheduler_set_use_context(const char* tags[], uint32_t count) {
    if (!tags || count == 0) {
        debug_printf("[SCHEDULER] ERROR: Invalid use context parameters\n");
        return;
    }

    TagFSState* fs = tagfs_get_state();
    if (!fs || !fs->registry) return;

    spin_lock(&sched.context_lock);

    sched.use_context.context_bits = 0;
    sched.use_context.overflow_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (!tags[i]) continue;

        char key[256], value[256];
        tagfs_parse_tag(tags[i], key, sizeof(key), value, sizeof(value));

        uint16_t tid = tag_registry_intern(fs->registry, key, value[0] ? value : NULL);
        if (tid == TAGFS_INVALID_TAG_ID) continue;

        if (tid < 64) {
            sched.use_context.context_bits |= ((uint64_t)1 << tid);
        } else {
            if (sched.use_context.overflow_count >= sched.use_context.overflow_capacity) {
                uint16_t new_cap = sched.use_context.overflow_capacity == 0
                    ? 8 : sched.use_context.overflow_capacity * 2;
                uint16_t* new_ids = kmalloc(sizeof(uint16_t) * new_cap);
                if (new_ids) {
                    if (sched.use_context.overflow_ids) {
                        memcpy(new_ids, sched.use_context.overflow_ids,
                               sizeof(uint16_t) * sched.use_context.overflow_count);
                        kfree(sched.use_context.overflow_ids);
                    }
                    sched.use_context.overflow_ids = new_ids;
                    sched.use_context.overflow_capacity = new_cap;
                }
            }
            if (sched.use_context.overflow_count < sched.use_context.overflow_capacity) {
                sched.use_context.overflow_ids[sched.use_context.overflow_count++] = tid;
            }
        }
    }

    sched.use_context.enabled = true;
    spin_unlock(&sched.context_lock);

    debug_printf("[SCHEDULER] Use context set: %u tags\n", count);
}

void scheduler_clear_use_context(void) {
    spin_lock(&sched.context_lock);
    sched.use_context.enabled = false;
    sched.use_context.context_bits = 0;
    if (sched.use_context.overflow_ids) {
        kfree(sched.use_context.overflow_ids);
        sched.use_context.overflow_ids = NULL;
    }
    sched.use_context.overflow_count = 0;
    sched.use_context.overflow_capacity = 0;
    spin_unlock(&sched.context_lock);
    debug_printf("[SCHEDULER] Use context cleared\n");
}

scheduler_state_t* scheduler_get_state(void) {
    return &sched;
}
