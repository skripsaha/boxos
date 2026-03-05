#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "ktypes.h"
#include "process.h"
#include "klib.h"

/*
 * Lock ordering: scheduler_lock must be acquired BEFORE process_lock.
 * See process/process.h for the full hierarchy.
 */

#define MAX_USE_CONTEXT_TAGS 16
#define TAG_LENGTH 64

#define SCHEDULER_MAX_CONSECUTIVE_RUNS 5
#define SCHEDULER_PENALTY_DURATION_TICKS 3

#define SCHEDULER_BOOST_HOT_RESULT           50
#define SCHEDULER_BOOST_CONTEXT_MATCH        20
#define SCHEDULER_BOOST_STARVATION           10
#define SCHEDULER_BOOST_CRITICAL_STARVATION 100
#define SCHEDULER_BOOST_SEVERE_STARVATION    30

#define SCHEDULER_CRITICAL_STARVATION_TICKS 100
#define SCHEDULER_SEVERE_STARVATION_TICKS    50
#define SCHEDULER_MILD_STARVATION_TICKS      10

typedef struct {
    char active_tags[MAX_USE_CONTEXT_TAGS][TAG_LENGTH];
    uint32_t tag_count;
    bool enabled;
} use_context_t;

typedef struct {
    process_t* current_process;
    spinlock_t scheduler_lock;   // protects current_process
    use_context_t use_context;
    spinlock_t context_lock;
    uint64_t total_ticks;
    uint64_t last_audit_tick;
} scheduler_state_t;

void scheduler_init(void);

void scheduler_cleanup_finished(void);

process_t* scheduler_select_next(void);

void scheduler_yield(void);
void scheduler_yield_from_interrupt(void* frame);
void scheduler_yield_cooperative(void);

void scheduler_set_use_context(const char* tags[], uint32_t count);
void scheduler_clear_use_context(void);
bool scheduler_matches_use_context(process_t* proc);
int32_t scheduler_calculate_score(process_t* proc);
void scheduler_audit_fairness(void);
void scheduler_audit_finished(void);

scheduler_state_t* scheduler_get_state(void);

#endif
