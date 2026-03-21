#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "ktypes.h"
#include "process.h"
#include "klib.h"
#include "runqueue.h"

/*
 * Lock ordering: scheduler_lock must be acquired BEFORE process_lock.
 * See process/process.h for the full hierarchy.
 *
 * Per-core scheduling (Phase 3):
 *   Each core has its own scheduler_state_t with independent RunQueue,
 *   current_process, and tick counter.  g_core_sched[MAX_CORES] is indexed
 *   by core_index.  scheduler_get_state() returns the calling core's entry.
 *
 *   use_context (tag-based focus) is a GLOBAL policy shared across all cores.
 */

extern volatile uint64_t g_global_tick;

#define SCHEDULER_MAX_CONSECUTIVE_RUNS 5
#define SCHEDULER_FAIRNESS_INTERVAL 5

#define SCHEDULER_BOOST_STARVATION 10
#define SCHEDULER_BOOST_CRITICAL_STARVATION 100
#define SCHEDULER_BOOST_SEVERE_STARVATION 30

#define SCHEDULER_CRITICAL_STARVATION_TICKS 100
#define SCHEDULER_SEVERE_STARVATION_TICKS 50
#define SCHEDULER_MILD_STARVATION_TICKS 10

// Work stealing: idle App Cores steal from the busiest neighbor.
// Cooldown prevents thundering-herd when multiple cores go idle.
#define STEAL_COOLDOWN_TICKS 3

typedef struct
{
    uint64_t context_bits;
    uint16_t *overflow_ids;
    uint16_t overflow_count;
    uint16_t overflow_capacity;
    bool enabled;
} use_context_t;

// Per-core scheduler state.
// Each core has its own instance in g_core_sched[].
typedef struct
{
    process_t *current_process;
    spinlock_t scheduler_lock; // protects current_process
    uint64_t total_ticks;
    uint64_t last_steal_tick; // tick of last work-steal attempt (cooldown)
    RunQueue runqueue;        // O(1) priority bitmap runqueue
} scheduler_state_t;

// Initialize BSP scheduler (core 0).  Called once from kernel_main().
void scheduler_init(void);

// Initialize scheduler for an AP core.  Called from ap_entry_c().
void scheduler_init_core(uint8_t core_index);

// Single unified scheduling function. Called from:
//   - syscall_handler (after guide())
//   - timer IRQ (PIT on BSP, LAPIC timer on APs)
//   - exception recovery
// Selects next process, saves/restores context via frame.
void schedule(void *frame);

process_t *scheduler_select_next(void);

void scheduler_set_use_context(const char *tags[], uint32_t count);
void scheduler_clear_use_context(void);
bool scheduler_matches_use_context(process_t *proc);

// O(1) RunQueue integration
int sched_determine_priority(process_t *proc);
bool sched_enqueue(process_t *proc); // Returns: true on success, false on error
void sched_dequeue(process_t *proc);

// Enqueue a process onto a specific core's RunQueue (cross-core safe).
void sched_enqueue_on(uint8_t core_idx, process_t *proc);

// Return the calling core's scheduler state (indexed by LAPIC ID).
scheduler_state_t *scheduler_get_state(void);

// Return a specific core's scheduler state by index.
scheduler_state_t *scheduler_get_core(uint8_t core_idx);

#endif
