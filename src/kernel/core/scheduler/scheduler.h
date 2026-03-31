#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "ktypes.h"
#include "process.h"
#include "klib.h"
#include "runqueue.h"
#include "error.h"

extern volatile uint64_t g_global_tick;

#define SCHEDULER_MAX_CONSECUTIVE_RUNS 5
#define SCHEDULER_FAIRNESS_INTERVAL 5

#define SCHEDULER_BOOST_STARVATION 10
#define SCHEDULER_BOOST_CRITICAL_STARVATION 100
#define SCHEDULER_BOOST_SEVERE_STARVATION 30

#define SCHEDULER_CRITICAL_STARVATION_TICKS 100
#define SCHEDULER_SEVERE_STARVATION_TICKS 50
#define SCHEDULER_MILD_STARVATION_TICKS 10

#define STEAL_COOLDOWN_TICKS 3

typedef struct
{
    uint64_t context_bits;
    uint16_t *overflow_ids;
    uint16_t overflow_count;
    uint16_t overflow_capacity;
    bool enabled;
} use_context_t;

typedef struct
{
    process_t *current_process;
    spinlock_t scheduler_lock;
    uint64_t total_ticks;
    uint64_t last_steal_tick;
    RunQueue runqueue;
} scheduler_state_t;

void scheduler_init(void);
void scheduler_init_core(uint8_t core_index);
void schedule(void *frame);
process_t *scheduler_select_next(void);

void scheduler_set_use_context(const char *tags[], uint32_t count);
void scheduler_clear_use_context(void);
bool scheduler_matches_use_context(process_t *proc);

int sched_determine_priority(process_t *proc);
error_t sched_enqueue(process_t *proc);
error_t sched_enqueue_on(uint8_t core_idx, process_t *proc);
error_t sched_dequeue(process_t *proc);

scheduler_state_t *scheduler_get_state(void);
scheduler_state_t *scheduler_get_core(uint8_t core_idx);

#endif
