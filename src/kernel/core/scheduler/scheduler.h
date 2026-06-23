#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "ktypes.h"
#include "kernel_config.h"
#include "process.h"
#include "klib.h"
#include "runqueue.h"
#include "error.h"

// ============================================================================
// Dynamic Scheduler Parameters (auto-tuned at runtime)
// ============================================================================

extern const uint32_t g_scheduler_fairness_base;
extern const uint32_t g_scheduler_starvation_base;
extern const uint32_t g_scheduler_steal_cooldown_base;
extern uint32_t g_timer_frequency;

// Calculated dynamic values (updated on timer IRQ)
extern uint32_t g_dynamic_fairness_ratio;
extern uint32_t g_dynamic_starvation_ticks;
extern uint32_t g_dynamic_steal_cooldown;

// Adaptive tick rate bounds — sourced from kernel_config.h
#define SCHEDULER_MIN_TICK_HZ     CONFIG_SCHED_MIN_TICK_HZ
#define SCHEDULER_MAX_TICK_HZ     CONFIG_SCHED_MAX_TICK_HZ
#define SCHEDULER_DEFAULT_TICK_HZ CONFIG_SCHED_DEFAULT_TICK_HZ

// Core parking thresholds
#define SCHEDULER_PARK_IDLE_TICKS    CONFIG_SCHED_PARK_IDLE_TICKS
#define SCHEDULER_UNPARK_LOAD_THRESH CONFIG_SCHED_UNPARK_LOAD_THRESH

// Affinity: cache-warm threshold
#define SCHEDULER_AFFINITY_WARM_TICKS CONFIG_SCHED_AFFINITY_WARM_TICKS

// Recalc interval
#define SCHED_RECALC_INTERVAL CONFIG_SCHED_RECALC_INTERVAL

// Limits
#define SCHEDULER_MAX_CONSECUTIVE_RUNS CONFIG_SCHED_MAX_CONSECUTIVE_RUNS
#define SCHEDULER_MIN_FAIRNESS         CONFIG_SCHED_MIN_FAIRNESS
#define SCHEDULER_MAX_FAIRNESS         CONFIG_SCHED_MAX_FAIRNESS
#define SCHEDULER_MIN_STARVATION       CONFIG_SCHED_MIN_STARVATION
#define SCHEDULER_MAX_STARVATION       CONFIG_SCHED_MAX_STARVATION

// ============================================================================
// Scheduler State
// ============================================================================

extern volatile uint64_t g_global_tick;

typedef struct {
    process_t  *current_process;
    spinlock_t  scheduler_lock;
    uint64_t    total_ticks;
    uint64_t    last_steal_tick;
    uint32_t    normal_ticks;          // Counter for adaptive fairness
    RunQueue    runqueue;
    _Atomic bool is_parked;            // Written by BSP recalc, read by AP timer IRQ — must be atomic
    uint32_t    idle_tick_count;       // Consecutive idle ticks
    /* Quiescence sequence (reap-vs-switch safety). Bumped at the top of every
     * schedule() on this core. An evicted strand stamps the value it saw; that
     * strand's kernel stack is provably free once this counter has advanced
     * past the stamp, because the next schedule() runs on the incoming strand's
     * stack (post-iretq) — never the evicted one's. The reaper gates on it so a
     * strand's stack is never freed while this core is still in its epilogue. */
    volatile uint64_t quiesce_seq;
} scheduler_state_t;

// ============================================================================
// Scheduler Statistics (read-only, for debugging/monitoring)
// ============================================================================

typedef struct {
    uint64_t context_switches;
    uint64_t fairness_rounds;
    uint64_t starvation_boosts;
    uint64_t steal_attempts;
    uint64_t steal_successes;
    uint64_t affinity_hits;
    uint32_t parked_cores;
    uint32_t active_cores;
} scheduler_stats_t;

extern scheduler_stats_t g_sched_stats;

// ============================================================================
// Public API
// ============================================================================

error_t scheduler_init(void);
void scheduler_init_core(uint8_t core_index);
void scheduler_shutdown(void);
void schedule(void *frame);
process_t *scheduler_select_next(void);

// Dynamic parameter recalculation (called from timer IRQ)
void scheduler_recalc_parameters(void);

// Core parking
void scheduler_park_core(uint8_t core_idx);
void scheduler_unpark_core(uint8_t core_idx);
bool scheduler_is_core_parked(uint8_t core_idx);

// Statistics
scheduler_stats_t scheduler_get_stats(void);

int sched_determine_priority(process_t *proc);
error_t sched_enqueue(process_t *proc);
error_t sched_enqueue_on(uint8_t core_idx, process_t *proc);
error_t sched_dequeue(process_t *proc);
/* Remove proc from whichever core's runqueue holds it (referenced-nowhere
 * guarantee before process_destroy frees it). See scheduler.c. */
void sched_dequeue_all_cores(process_t *proc);

scheduler_state_t *scheduler_get_state(void);
scheduler_state_t *scheduler_get_core(uint8_t core_idx);

#endif // SCHEDULER_H
