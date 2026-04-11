#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "ktypes.h"
#include "process.h"
#include "klib.h"
#include "runqueue.h"
#include "error.h"

// ============================================================================
// Dynamic Scheduler Parameters (auto-tuned at runtime)
// ============================================================================

extern uint32_t g_scheduler_fairness_base;
extern uint32_t g_scheduler_starvation_base;
extern uint32_t g_scheduler_steal_cooldown_base;
extern uint32_t g_timer_frequency;

// Calculated dynamic values (updated on timer IRQ)
extern uint32_t g_dynamic_fairness_ratio;
extern uint32_t g_dynamic_starvation_ticks;
extern uint32_t g_dynamic_steal_cooldown;

// Adaptive tick rate bounds
#define SCHEDULER_MIN_TICK_HZ     10
#define SCHEDULER_MAX_TICK_HZ     500
#define SCHEDULER_DEFAULT_TICK_HZ 250

// Core parking thresholds
#define SCHEDULER_PARK_IDLE_TICKS    100
#define SCHEDULER_UNPARK_LOAD_THRESH 2

// Affinity: cache-warm threshold (ticks since last run on same core)
#define SCHEDULER_AFFINITY_WARM_TICKS 5

// Limits
#define SCHEDULER_MAX_CONSECUTIVE_RUNS 5
#define SCHEDULER_MIN_FAIRNESS         2
#define SCHEDULER_MAX_FAIRNESS         20
#define SCHEDULER_MIN_STARVATION       10
#define SCHEDULER_MAX_STARVATION       100

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
    bool        is_parked;             // Core is parked (no timer IRQ, no scheduling)
    uint32_t    idle_tick_count;       // Consecutive idle ticks
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

void scheduler_init(void);
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

scheduler_state_t *scheduler_get_state(void);
scheduler_state_t *scheduler_get_core(uint8_t core_idx);

#endif // SCHEDULER_H
