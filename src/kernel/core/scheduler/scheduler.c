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

static scheduler_state_t sched;

void scheduler_init(void) {
    debug_printf("[SCHEDULER] Initializing scheduler...\n");

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

    debug_printf("[SCHEDULER] Scheduler initialized\n");
}

bool scheduler_matches_use_context(process_t* proc) {
    if (!proc) return false;

    spin_lock(&sched.context_lock);

    if (!sched.use_context.enabled) {
        spin_unlock(&sched.context_lock);
        return false;
    }

    // Fast path: single AND for tag_ids 0-63
    bool match = (proc->tag_bits & sched.use_context.context_bits) != 0;

    if (!match) {
        // Slow path: check overflow arrays (tag_ids >= 64)
        for (uint16_t i = 0; i < proc->tag_overflow_count && !match; i++) {
            for (uint16_t j = 0; j < sched.use_context.overflow_count; j++) {
                if (proc->tag_overflow_ids[i] == sched.use_context.overflow_ids[j]) {
                    match = true;
                    break;
                }
            }
        }
    }

    spin_unlock(&sched.context_lock);
    return match;
}

int32_t scheduler_calculate_score(process_t* proc) {
    if (!proc) {
        return INT32_MIN;
    }

    int32_t score = 0;

    if (scheduler_matches_use_context(proc)) {
        score += SCHEDULER_BOOST_CONTEXT_MATCH;
    }

    uint64_t ticks_since_run = 0;
    if (sched.total_ticks >= proc->last_run_time) {
        ticks_since_run = sched.total_ticks - proc->last_run_time;
    }
    if (ticks_since_run >= SCHEDULER_CRITICAL_STARVATION_TICKS) {
        score += SCHEDULER_BOOST_CRITICAL_STARVATION;
    } else if (ticks_since_run >= SCHEDULER_SEVERE_STARVATION_TICKS) {
        score += SCHEDULER_BOOST_SEVERE_STARVATION;
    } else if (ticks_since_run >= SCHEDULER_MILD_STARVATION_TICKS) {
        score += SCHEDULER_BOOST_STARVATION;
    }

    if (proc->consecutive_runs >= SCHEDULER_MAX_CONSECUTIVE_RUNS) {
        score -= SCHEDULER_BOOST_CRITICAL_STARVATION;
    }

    if (process_has_tag(proc, "utility") || process_has_tag(proc, "system")) {
        score += 5;
    }

    if (score > INT32_MAX / 2) {
        score = INT32_MAX / 2;
    }

    return score;
}

process_t* scheduler_select_next(void) {
    process_t* best = NULL;
    int32_t best_score = INT32_MIN;

    process_list_lock();
    process_t* proc = process_get_first();
    while (proc) {
        if (proc->magic != PROCESS_MAGIC) {
            proc = proc->next;
            continue;
        }

        process_state_t state = process_get_state(proc);

        if (state == PROC_CRASHED || state == PROC_DONE) {
            proc = proc->next;
            continue;
        }

        if (state == PROC_WORKING || state == PROC_CREATED) {
            int32_t score = scheduler_calculate_score(proc);
            if (score > best_score) {
                best_score = score;
                best = proc;
            }
        }
        proc = proc->next;
    }
    process_list_unlock();

    if (!best) {
        best = idle_process_get();
    }

    return best;
}

// Single unified schedule() function.
// Saves context of current process, selects next, restores context.
void schedule(void* frame_ptr) {
    interrupt_frame_t* frame = (interrupt_frame_t*)frame_ptr;

    __asm__ volatile("cli");

    spin_lock(&sched.scheduler_lock);
    process_t* current = sched.current_process;
    spin_unlock(&sched.scheduler_lock);

    // Save context of current process if it's running
    if (current && process_get_state(current) == PROC_WORKING) {
        context_save_from_frame(current, frame);
    }

    // Select next process
    process_t* next = scheduler_select_next();

    // Set kernel stack for ring 3 -> ring 0 transitions (TSS for INT, PerCpuData for notify)
    if (!process_is_idle(next) && next->kernel_stack_top) {
        tss_set_rsp0((uint64_t)next->kernel_stack_top);
        notify_set_kernel_rsp((uint64_t)next->kernel_stack_top);
    }

    // Switch current process
    spin_lock(&sched.scheduler_lock);
    sched.current_process = next;
    spin_unlock(&sched.scheduler_lock);

    if (process_get_state(next) == PROC_CREATED) {
        process_set_state(next, PROC_WORKING);
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

        uint16_t tid = tag_registry_intern(fs->registry, key, value);
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
