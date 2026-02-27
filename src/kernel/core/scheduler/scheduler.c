#include "scheduler.h"
#include "klib.h"
#include "process.h"
#include "context_switch.h"
#include "idt.h"
#include "tss.h"
#include "atomics.h"
#include "idle.h"
#include "guide/pending_results.h"
#include "guide/guide.h"
#include "../../drivers/usb/xhci/xhci_interrupt.h"

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

    // Hot Result bonus (decays with consecutive runs)
    if (proc->result_there) {
        int32_t result_bonus = SCHEDULER_BOOST_HOT_RESULT;
        if (proc->consecutive_runs > 0) {
            result_bonus -= (proc->consecutive_runs * 5);
            if (result_bonus < SCHEDULER_BOOST_STARVATION) {
                result_bonus = SCHEDULER_BOOST_STARVATION;
            }
        }
        score += result_bonus;
    }

    // CRITICAL priority for processes blocked on overflow
    if (proc->block_reason == PROC_BLOCK_RESULT_OVERFLOW) {
        score += 200;  // Highest priority to recover
    }

    // Use context match
    if (scheduler_matches_use_context(proc)) {
        score += SCHEDULER_BOOST_CONTEXT_MATCH;
    }

    // Enhanced starvation protection (wraparound-safe)
    uint64_t ticks_since_run = 0;
    if (sched.total_ticks >= proc->last_run_time) {
        ticks_since_run = sched.total_ticks - proc->last_run_time;
    }
    if (ticks_since_run >= SCHEDULER_CRITICAL_STARVATION_TICKS) {
        score += SCHEDULER_BOOST_CRITICAL_STARVATION;  // 1000ms = critical starvation
    } else if (ticks_since_run >= SCHEDULER_SEVERE_STARVATION_TICKS) {
        score += SCHEDULER_BOOST_SEVERE_STARVATION;   // 500ms = severe starvation
    } else if (ticks_since_run >= SCHEDULER_MILD_STARVATION_TICKS) {
        score += SCHEDULER_BOOST_STARVATION;   // 100ms = mild starvation
    }

    // Consecutive run penalty
    if (proc->consecutive_runs >= SCHEDULER_MAX_CONSECUTIVE_RUNS) {
        score -= SCHEDULER_BOOST_CRITICAL_STARVATION;
    }

    // Utility/system bonus
    if (process_has_tag(proc, "utility") || process_has_tag(proc, "system")) {
        score += 5;
    }

    if (score > INT32_MAX / 2) {
        score = INT32_MAX / 2;
    }

    return score;
}

void scheduler_reap_zombies(void) {
    process_t* proc = process_get_first();

    // read current_process under short lock, not held during entire loop
    spin_lock(&sched.scheduler_lock);
    process_t* current = sched.current_process;
    spin_unlock(&sched.scheduler_lock);

    // Now scheduler_lock is FREE - process_destroy() can safely acquire it

    while (proc) {
        process_t* next = proc->next;
        process_state_t state = process_get_state(proc);
        uint32_t refs = process_ref_count(proc);

        if (state == PROC_ZOMBIE && refs == 0 && proc != current) {
            process_destroy_safe(proc);
        }

        proc = next;
    }

    // Run deferred cleanup after reaping
    process_cleanup_deferred();
}

process_t* scheduler_select_next(void) {
    process_t* best = NULL;
    int32_t best_score = INT32_MIN;

    process_t* proc = process_get_first();
    while (proc) {
        // validate magic before accessing process
        if (proc->magic != PROCESS_MAGIC) {
            proc = proc->next;
            continue;
        }

        process_state_t state = process_get_state(proc);

        if (state == PROC_TERMINATED || state == PROC_ZOMBIE) {
            proc = proc->next;
            continue;
        }

        if (state == PROC_READY) {
            int32_t score = scheduler_calculate_score(proc);
            if (score > best_score) {
                best_score = score;
                best = proc;
            }
        }
        proc = proc->next;
    }

    if (!best) {
        best = idle_process_get();
    }

    return best;
}

void scheduler_yield(void) {
    scheduler_reap_zombies();

    spin_lock(&sched.scheduler_lock);
    process_t* current = sched.current_process;

    if (current && process_get_state(current) == PROC_RUNNING) {
        process_set_state(current, PROC_READY);
    }

    process_t* next = scheduler_select_next();

    if (next) {
        sched.current_process = next;
        process_set_state(next, PROC_RUNNING);
        next->last_run_time = sched.total_ticks;
    } else {
        sched.current_process = NULL;
    }

    spin_unlock(&sched.scheduler_lock);
}

void scheduler_yield_cooperative(void) {
    spin_lock(&sched.scheduler_lock);

    process_t* current = sched.current_process;

    if (current && process_get_state(current) == PROC_RUNNING) {
        process_set_state(current, PROC_READY);

        process_t* next = scheduler_select_next();

        if (next) {
            sched.current_process = next;
            process_set_state(next, PROC_RUNNING);
            next->last_run_time = sched.total_ticks;
        } else {
            process_set_state(current, PROC_RUNNING);
        }
    }

    spin_unlock(&sched.scheduler_lock);
}

void scheduler_yield_from_interrupt(void* frame_ptr) {
    interrupt_frame_t* frame = (interrupt_frame_t*)frame_ptr;

    __asm__ volatile("cli");

    // read current_process under lock
    spin_lock(&sched.scheduler_lock);
    process_t* current = sched.current_process;
    spin_unlock(&sched.scheduler_lock);

    if (current && process_get_state(current) == PROC_RUNNING) {
        // don't preempt if only runnable process
        // Quick scan: are there other READY processes?
        bool other_ready = false;
        process_t* scan = process_get_first();
        while (scan) {
            if (scan != current && process_get_state(scan) == PROC_READY) {
                other_ready = true;
                break;
            }
            scan = scan->next;
        }

        // Only context switch if competition exists
        if (!other_ready && !process_is_idle(current)) {
            sched.total_ticks++;
            return;  // Continue running current process
        }

        context_save_from_frame(current, frame);
        process_set_state(current, PROC_READY);

        if (!process_is_idle(current) && current->result_there) {
            __sync_synchronize();
            atomic_store_u8((volatile uint8_t*)&current->result_there, 0);
        }
    }

    if ((sched.total_ticks % SCHEDULER_CRITICAL_STARVATION_TICKS) == 0) {
        process_t* proc = process_get_first();
        while (proc) {
            process_t* next_proc = proc->next;

            process_state_t state = process_get_state(proc);
            uint32_t refs = process_ref_count(proc);

            if (state == PROC_TERMINATED && refs == 0) {
                process_destroy_safe(proc);
            } else if (state == PROC_ZOMBIE && refs == 0) {
                process_set_state(proc, PROC_TERMINATED);
                process_destroy_safe(proc);
            }

            proc = next_proc;
        }
    }

    // Audit zombies every 100 ticks (1 second @ 100Hz)
    static uint64_t tick_count = 0;
    tick_count++;
    if ((tick_count % SCHEDULER_CRITICAL_STARVATION_TICKS) == 0) {
        scheduler_audit_zombies();
    }

    process_t* next = scheduler_select_next();

    // Phase 2: Periodic Guide run to prevent deadlock
    static uint64_t guide_last_run = 0;
    uint64_t current_tick = sched.total_ticks;

    bool should_run_guide = false;

    // 1. Transition to idle (existing behavior)
    if (process_is_idle(next) && !process_is_idle(current)) {
        should_run_guide = true;
    }

    // 2. Periodic run every 5 ticks (50ms) for guaranteed processing
    if ((current_tick - guide_last_run) >= 5) {
        should_run_guide = true;
    }

    // 3. Priority run if processes waiting on EventRing
    extern event_ring_wait_queue_t event_ring_waiters;
    if (event_ring_waiters.count > 0) {
        should_run_guide = true;
    }

    if (should_run_guide) {
        guide_run();
        guide_last_run = current_tick;

        xhci_poll_events();

        // deferred cleanup on idle transition
        process_cleanup_deferred();

        // Re-select next process (may have unblocked processes)
        process_t* maybe_user = scheduler_select_next();
        if (maybe_user && !process_is_idle(maybe_user)) {
            next = maybe_user;
        }
    }

    // Update TSS RSP0 for userspace processes (not needed for idle)
    if (!process_is_idle(next) && next->kernel_stack_top) {
        tss_set_rsp0((uint64_t)next->kernel_stack_top);
    }

    spin_lock(&sched.scheduler_lock);
    sched.current_process = next;
    spin_unlock(&sched.scheduler_lock);
    process_set_state(next, PROC_RUNNING);
    next->last_run_time = sched.total_ticks;

    // Update consecutive run counters
    if (current && current != next) {
        // Process switched: reset current's counter
        current->consecutive_runs = 0;
        next->consecutive_runs = 1;
    } else if (next) {
        // Same process continues: increment counter
        next->consecutive_runs++;
    }

    // Fairness audit every 100 ticks
    if ((sched.total_ticks - sched.last_audit_tick) >= SCHEDULER_CRITICAL_STARVATION_TICKS) {
        scheduler_audit_fairness();
        sched.last_audit_tick = sched.total_ticks;
    }

    // Periodic cleanup (every 10 ticks = 100ms)
    if ((sched.total_ticks % 10) == 0) {
        pending_results_flush_all();
        process_cleanup_deferred();
    }

    context_restore_to_frame(next, frame);

    __asm__ volatile("sti");
}

void scheduler_tick(void) {
    sched.total_ticks++;

    if ((sched.total_ticks % SCHEDULER_CRITICAL_STARVATION_TICKS) == 0) {
        process_t* proc = process_get_first();
        while (proc) {
            process_t* next = proc->next;

            process_state_t state = process_get_state(proc);
            uint32_t refs = process_ref_count(proc);

            if (state == PROC_TERMINATED && refs == 0) {
                process_destroy_safe(proc);
            } else if (state == PROC_ZOMBIE && refs == 0) {
                process_set_state(proc, PROC_TERMINATED);
                process_destroy_safe(proc);
            }

            proc = next;
        }
    }

    // Check for timeout blocked processes on ResultRing and EventRing
    extern uint64_t cpu_get_tsc_freq_khz(void);
    process_t* proc = process_get_first();
    while (proc) {
        process_t* next = proc->next;

        // Check EventRing block timeout
        if (proc->block_reason == PROC_BLOCK_EVENT_RING_FULL &&
            proc->block_start_time != 0) {

            uint64_t tsc_khz = cpu_get_tsc_freq_khz();
            if (tsc_khz == 0) {
                proc = next;
                continue;
            }

            uint64_t now = rdtsc();
            if (now < proc->block_start_time) {
                proc->block_start_time = now;
                proc = next;
                continue;
            }

            uint64_t elapsed_tsc = now - proc->block_start_time;
            uint64_t elapsed_ms = elapsed_tsc / tsc_khz;

            if (elapsed_ms > CONFIG_EVENTRING_BLOCK_TIMEOUT_MS) {
                static uint64_t last_timeout_log = 0;
                uint64_t current_tick = sched.total_ticks;

                if ((current_tick - last_timeout_log) > SCHEDULER_SEVERE_STARVATION_TICKS) {
                    debug_printf("[SCHEDULER] TIMEOUT: PID %u blocked on EventRing for %llu ms, killing\n",
                                 proc->pid, elapsed_ms);
                    last_timeout_log = current_tick;
                }

                process_destroy_safe(proc);
            }

            proc = next;
            continue;
        }

        if (proc->block_reason == PROC_BLOCK_RESULT_OVERFLOW &&
            proc->block_start_time != 0) {

            uint64_t tsc_khz = cpu_get_tsc_freq_khz();
            if (tsc_khz == 0) {
                // TSC not calibrated - use tick-based timeout (50 ticks ~= 500ms at 100Hz)
                if (proc->last_run_time == 0) {
                    // Process just blocked, set baseline
                    proc->last_run_time = sched.total_ticks;
                    proc = next;
                    continue;
                }

                uint64_t blocked_ticks = sched.total_ticks - proc->last_run_time;
                if (blocked_ticks > SCHEDULER_SEVERE_STARVATION_TICKS) {
                    // rate-limited logging (max once per 50 ticks)
                    static uint64_t last_timeout_log_tick = 0;
                    uint64_t current_tick = sched.total_ticks;

                    if ((current_tick - last_timeout_log_tick) > SCHEDULER_SEVERE_STARVATION_TICKS) {
                        debug_printf("[SCHEDULER] CRITICAL: PID %u timeout (tick-based >%d ticks), killing\n",
                                     proc->pid, SCHEDULER_SEVERE_STARVATION_TICKS);
                        last_timeout_log_tick = current_tick;
                    }

                    process_destroy_safe(proc);
                }

                proc = next;
                continue;
            }

            uint64_t now = rdtsc();
            if (now < proc->block_start_time) {
                // rate-limited logging (max once per 100 ticks)
                static uint64_t last_tsc_overflow_log = 0;
                uint64_t current_tick = sched.total_ticks;

                if ((current_tick - last_tsc_overflow_log) > SCHEDULER_CRITICAL_STARVATION_TICKS) {
                    debug_printf("[SCHEDULER] ANOMALY: TSC wrap-around detected for PID %u (now=%llu, start=%llu)\n",
                                 proc->pid, now, proc->block_start_time);
                    last_tsc_overflow_log = current_tick;
                }

                proc->block_start_time = now;
                proc = next;
                continue;
            }

            uint64_t elapsed_tsc = now - proc->block_start_time;
            uint64_t elapsed_ms = elapsed_tsc / tsc_khz;

            if (elapsed_ms > 500) {
                // rate-limited logging (max once per 50 ticks)
                static uint64_t last_timeout_log_tsc = 0;
                uint64_t current_tick = sched.total_ticks;

                if ((current_tick - last_timeout_log_tsc) > SCHEDULER_SEVERE_STARVATION_TICKS) {
                    debug_printf("[SCHEDULER] CRITICAL: PID %u timeout on ResultRing (>500ms), killing\n",
                                 proc->pid);
                    last_timeout_log_tsc = current_tick;
                }

                process_destroy_safe(proc);
            }
        }

        proc = next;
    }

    // read current_process under lock
    spin_lock(&sched.scheduler_lock);
    process_t* current = sched.current_process;
    spin_unlock(&sched.scheduler_lock);

    if (current && process_get_state(current) == PROC_RUNNING) {
        scheduler_yield();
    }
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
    process_t* proc = process_get_first();
    bool found_starved = false;

    while (proc) {
        process_state_t state = process_get_state(proc);

        if (state == PROC_READY || state == PROC_RUNNING) {
            uint64_t ticks_since_run = sched.total_ticks - proc->last_run_time;

            if (ticks_since_run > SCHEDULER_SEVERE_STARVATION_TICKS) {
                if (!found_starved) {
                    debug_printf("[SCHEDULER] Fairness audit (tick %lu):\n",
                                 sched.total_ticks);
                    found_starved = true;
                }

                debug_printf("[SCHEDULER]   PID %u: starved for %lu ticks (%.1f ms), consecutive_runs=%u\n",
                             proc->pid,
                             ticks_since_run,
                             (float)ticks_since_run * 10.0f,
                             proc->consecutive_runs);
            }
        }

        proc = proc->next;
    }
}

void scheduler_audit_zombies(void) {
    static uint32_t warn_count = 0;
    const uint32_t MAX_ZOMBIE_WARNINGS = 5;

    process_t* proc = process_get_first();

    while (proc) {
        process_state_t state = process_get_state(proc);
        uint32_t refs = process_ref_count(proc);

        if (state == PROC_ZOMBIE && refs > 0) {
            if (warn_count < MAX_ZOMBIE_WARNINGS) {
                debug_printf("[SCHEDULER] WARNING: PID %u stuck in ZOMBIE with ref_count=%u\n",
                             proc->pid, refs);
                warn_count++;
                if (warn_count == MAX_ZOMBIE_WARNINGS) {
                    debug_printf("[SCHEDULER] Suppressing further zombie warnings\n");
                }
            }
        }

        proc = proc->next;
    }
}
