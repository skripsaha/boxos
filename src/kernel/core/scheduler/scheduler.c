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

static scheduler_state_t sched;

void scheduler_init(void) {
    debug_printf("[SCHEDULER] Initializing Smart Score Scheduler...\n");

    memset(&sched, 0, sizeof(scheduler_state_t));
    sched.current_process = NULL;
    spinlock_init(&sched.scheduler_lock);
    sched.use_context.enabled = false;
    sched.use_context.tag_count = 0;
    sched.total_ticks = 0;
    sched.last_audit_tick = 0;
    spinlock_init(&sched.context_lock);

    debug_printf("[SCHEDULER] Scheduler initialized\n");
    debug_printf("[SCHEDULER] Score system: +%d (hot result), +%d (context match), +%d/+%d/+%d (starvation), +5 (utility)\n",
                 SCHEDULER_BOOST_HOT_RESULT, SCHEDULER_BOOST_CONTEXT_MATCH, SCHEDULER_BOOST_STARVATION,
                 SCHEDULER_BOOST_SEVERE_STARVATION, SCHEDULER_BOOST_CRITICAL_STARVATION);
    debug_printf("[SCHEDULER] Starvation protection: consecutive run limit=%d, penalty=%d ticks\n",
                 SCHEDULER_MAX_CONSECUTIVE_RUNS, SCHEDULER_PENALTY_DURATION_TICKS);
}

bool scheduler_matches_use_context(process_t* proc) {
    if (!proc) {
        return false;
    }

    spin_lock(&sched.context_lock);

    if (!sched.use_context.enabled || sched.use_context.tag_count == 0) {
        spin_unlock(&sched.context_lock);
        return false;
    }

    bool match = false;
    for (uint32_t i = 0; i < sched.use_context.tag_count; i++) {
        if (process_has_tag(proc, sched.use_context.active_tags[i])) {
            match = true;
            break;
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

void scheduler_cleanup_finished(void) {
    spin_lock(&sched.scheduler_lock);
    process_t* current = sched.current_process;
    spin_unlock(&sched.scheduler_lock);

    // Collect candidates under lock, then destroy after releasing
    uint32_t to_destroy[16];
    uint32_t count = 0;

    process_list_lock();
    process_t* proc = process_get_first();
    while (proc && count < 16) {
        process_state_t state = process_get_state(proc);
        uint32_t refs = process_ref_count(proc);

        if (state == PROC_DONE && refs == 0 && proc != current) {
            to_destroy[count++] = proc->pid;
        }
        proc = proc->next;
    }
    process_list_unlock();

    for (uint32_t i = 0; i < count; i++) {
        process_t* p = process_find(to_destroy[i]);
        if (p) {
            process_destroy_safe(p);
        }
    }

    process_cleanup_deferred();
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

void scheduler_yield(void) {
    spin_lock(&sched.scheduler_lock);
    process_t* current = sched.current_process;

    process_t* next = scheduler_select_next();

    if (next) {
        sched.current_process = next;
        next->last_run_time = sched.total_ticks;
    } else {
        sched.current_process = NULL;
    }

    spin_unlock(&sched.scheduler_lock);
}

void scheduler_yield_cooperative(void) {
    spin_lock(&sched.scheduler_lock);

    process_t* current = sched.current_process;

    if (current && process_get_state(current) == PROC_WORKING) {
        process_t* next = scheduler_select_next();

        if (next) {
            sched.current_process = next;
            next->last_run_time = sched.total_ticks;
        }
    }

    spin_unlock(&sched.scheduler_lock);
}

void scheduler_yield_from_interrupt(void* frame_ptr) {
    interrupt_frame_t* frame = (interrupt_frame_t*)frame_ptr;

    __asm__ volatile("cli");

    spin_lock(&sched.scheduler_lock);
    process_t* current = sched.current_process;
    spin_unlock(&sched.scheduler_lock);

    if (current && process_get_state(current) == PROC_WORKING) {
        bool other_ready = false;
        process_list_lock();
        process_t* scan = process_get_first();
        while (scan) {
            if (scan != current && process_get_state(scan) == PROC_WORKING) {
                other_ready = true;
                break;
            }
            scan = scan->next;
        }
        process_list_unlock();

        if (!other_ready && !process_is_idle(current)) {
            // no other process ready — keep running current process
            // do NOT increment total_ticks here; the timer IRQ handler already did it
            return;
        }

        context_save_from_frame(current, frame);
    }

    if ((sched.total_ticks % SCHEDULER_CRITICAL_STARVATION_TICKS) == 0) {
        uint32_t cleanup_pids[16];
        uint32_t cleanup_count = 0;

        process_list_lock();
        process_t* proc = process_get_first();
        while (proc && cleanup_count < 16) {
            process_state_t state = process_get_state(proc);
            uint32_t refs = process_ref_count(proc);

            if ((state == PROC_CRASHED || state == PROC_DONE) && refs == 0) {
                cleanup_pids[cleanup_count++] = proc->pid;
            }
            proc = proc->next;
        }
        process_list_unlock();

        for (uint32_t i = 0; i < cleanup_count; i++) {
            process_t* p = process_find(cleanup_pids[i]);
            if (p) {
                if (process_get_state(p) == PROC_DONE) {
                    process_set_state(p, PROC_CRASHED);
                }
                process_destroy_safe(p);
            }
        }
    }

    // audit finished processes every 100 ticks (~1 second at 100 Hz)
    // SMP: these static counters are safe on single-core (cli context),
    // but must become per-CPU variables for multi-core support.
    static uint64_t tick_count = 0;
    tick_count++;
    if ((tick_count % SCHEDULER_CRITICAL_STARVATION_TICKS) == 0) {
        scheduler_audit_finished();
    }

    process_t* next = scheduler_select_next();

    static uint64_t guide_last_run = 0;
    uint64_t current_tick = sched.total_ticks;

    bool should_run_guide = false;

    // run guide on idle transition
    if (process_is_idle(next) && !process_is_idle(current)) {
        should_run_guide = true;
    }

    // periodic run every 5 ticks (50 ms) for guaranteed event processing
    if ((current_tick - guide_last_run) >= 5) {
        should_run_guide = true;
    }

    // run if ReadyQueue has processes with pending Pockets
    if (!ready_queue_is_empty(&g_ready_queue)) {
        should_run_guide = true;
    }

    if (should_run_guide) {
        guide_run();
        guide_last_run = current_tick;

        xhci_poll_events();

        process_cleanup_deferred();

        process_t* maybe_user = scheduler_select_next();
        if (maybe_user && !process_is_idle(maybe_user)) {
            next = maybe_user;
        }
    }

    if (!process_is_idle(next) && next->kernel_stack_top) {
        tss_set_rsp0((uint64_t)next->kernel_stack_top);
    }

    spin_lock(&sched.scheduler_lock);
    sched.current_process = next;
    spin_unlock(&sched.scheduler_lock);
    if (process_get_state(next) == PROC_CREATED) {
        process_set_state(next, PROC_WORKING);
    }
    next->last_run_time = sched.total_ticks;

    if (current && current != next) {
        current->consecutive_runs = 0;
        next->consecutive_runs = 1;
    } else if (next) {
        next->consecutive_runs++;
    }

    if ((sched.total_ticks - sched.last_audit_tick) >= SCHEDULER_CRITICAL_STARVATION_TICKS) {
        scheduler_audit_fairness();
        sched.last_audit_tick = sched.total_ticks;
    }

    if ((sched.total_ticks % 10) == 0) {
        process_cleanup_deferred();
    }

    context_restore_to_frame(next, frame);

    // do NOT sti here — iretq restores RFLAGS atomically including IF=1;
    // an sti before iretq creates a window where a timer fires in kernel mode
    // and corrupts the process context being restored
}

void scheduler_set_use_context(const char* tags[], uint32_t count) {
    if (!tags || count == 0 || count > MAX_USE_CONTEXT_TAGS) {
        debug_printf("[SCHEDULER] ERROR: Invalid use context parameters\n");
        return;
    }

    spin_lock(&sched.context_lock);

    sched.use_context.tag_count = 0;

    for (uint32_t i = 0; i < count && i < MAX_USE_CONTEXT_TAGS; i++) {
        if (tags[i]) {
            strncpy(sched.use_context.active_tags[i], tags[i], TAG_LENGTH - 1);
            sched.use_context.active_tags[i][TAG_LENGTH - 1] = '\0';
            sched.use_context.tag_count++;
        }
    }

    sched.use_context.enabled = true;

    spin_unlock(&sched.context_lock);

    debug_printf("[SCHEDULER] Use context set: %u tags\n", sched.use_context.tag_count);
    for (uint32_t i = 0; i < sched.use_context.tag_count; i++) {
        debug_printf("[SCHEDULER]   - %s\n", sched.use_context.active_tags[i]);
    }
}

void scheduler_clear_use_context(void) {
    spin_lock(&sched.context_lock);
    sched.use_context.enabled = false;
    sched.use_context.tag_count = 0;
    spin_unlock(&sched.context_lock);
    debug_printf("[SCHEDULER] Use context cleared\n");
}

scheduler_state_t* scheduler_get_state(void) {
    return &sched;
}

void scheduler_audit_fairness(void) {
    bool found_starved = false;

    process_list_lock();
    process_t* proc = process_get_first();
    while (proc) {
        process_state_t state = process_get_state(proc);

        if (state == PROC_WORKING) {
            uint64_t ticks_since_run = sched.total_ticks - proc->last_run_time;

            if (ticks_since_run > SCHEDULER_SEVERE_STARVATION_TICKS) {
                if (!found_starved) {
                    debug_printf("[SCHEDULER] Fairness audit (tick %lu):\n",
                                 sched.total_ticks);
                    found_starved = true;
                }

                debug_printf("[SCHEDULER]   PID %u: starved for %lu ticks (%lu ms), consecutive_runs=%u\n",
                             proc->pid,
                             ticks_since_run,
                             ticks_since_run * 10,
                             proc->consecutive_runs);
            }
        }

        proc = proc->next;
    }
    process_list_unlock();
}

void scheduler_audit_finished(void) {
    static uint32_t warn_count = 0;
    const uint32_t MAX_DONE_WARNINGS = 5;

    process_list_lock();
    process_t* proc = process_get_first();
    while (proc) {
        process_state_t state = process_get_state(proc);
        uint32_t refs = process_ref_count(proc);

        if (state == PROC_DONE && refs > 0) {
            if (warn_count < MAX_DONE_WARNINGS) {
                debug_printf("[SCHEDULER] WARNING: PID %u stuck in DONE with ref_count=%u\n",
                             proc->pid, refs);
                warn_count++;
                if (warn_count == MAX_DONE_WARNINGS) {
                    debug_printf("[SCHEDULER] Suppressing further warnings\n");
                }
            }
        }

        proc = proc->next;
    }
    process_list_unlock();
}
