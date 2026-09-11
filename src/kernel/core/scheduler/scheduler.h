#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "ktypes.h"
#include "kernel_config.h"
#include "process.h"
#include "klib.h"
#include "runqueue.h"
#include "error.h"


extern const uint32_t g_scheduler_fairness_base;
extern const uint32_t g_scheduler_starvation_base;
extern const uint32_t g_scheduler_steal_cooldown_base;
extern uint32_t g_timer_frequency;

extern uint32_t g_dynamic_fairness_ratio;
extern uint32_t g_dynamic_starvation_ticks;
extern uint32_t g_dynamic_steal_cooldown;

#define SCHEDULER_MIN_TICK_HZ     CONFIG_SCHED_MIN_TICK_HZ
#define SCHEDULER_MAX_TICK_HZ     CONFIG_SCHED_MAX_TICK_HZ
#define SCHEDULER_DEFAULT_TICK_HZ CONFIG_SCHED_DEFAULT_TICK_HZ

#define SCHEDULER_PARK_IDLE_TICKS    CONFIG_SCHED_PARK_IDLE_TICKS
#define SCHEDULER_UNPARK_LOAD_THRESH CONFIG_SCHED_UNPARK_LOAD_THRESH

#define SCHEDULER_AFFINITY_WARM_TICKS CONFIG_SCHED_AFFINITY_WARM_TICKS

#define SCHED_RECALC_INTERVAL CONFIG_SCHED_RECALC_INTERVAL

#define SCHEDULER_MAX_CONSECUTIVE_RUNS CONFIG_SCHED_MAX_CONSECUTIVE_RUNS
#define SCHEDULER_MIN_FAIRNESS         CONFIG_SCHED_MIN_FAIRNESS
#define SCHEDULER_MAX_FAIRNESS         CONFIG_SCHED_MAX_FAIRNESS
#define SCHEDULER_MIN_STARVATION       CONFIG_SCHED_MIN_STARVATION
#define SCHEDULER_MAX_STARVATION       CONFIG_SCHED_MAX_STARVATION


extern volatile uint64_t g_global_tick;

typedef struct {
    process_t  *current_process;
    spinlock_t  scheduler_lock;
    uint64_t    total_ticks;
    uint64_t    last_steal_tick;
    uint32_t    normal_ticks;
    RunQueue    runqueue;
    _Atomic bool is_parked;
    uint32_t    idle_tick_count;
    volatile uint64_t quiesce_seq;
} scheduler_state_t;


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


error_t scheduler_init(void);
void scheduler_init_core(uint8_t core_index);
void scheduler_shutdown(void);
void schedule(void *frame);
process_t *scheduler_select_next(void);

void scheduler_recalc_parameters(void);

void scheduler_park_core(uint8_t core_idx);
void scheduler_unpark_core(uint8_t core_idx);
bool scheduler_is_core_parked(uint8_t core_idx);

scheduler_stats_t scheduler_get_stats(void);

int sched_determine_priority(process_t *proc);
error_t sched_enqueue(process_t *proc);
error_t sched_enqueue_on(uint8_t core_idx, process_t *proc);
error_t sched_dequeue(process_t *proc);
void sched_dequeue_all_cores(process_t *proc);

scheduler_state_t *scheduler_get_state(void);
scheduler_state_t *scheduler_get_core(uint8_t core_idx);

#endif