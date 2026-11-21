#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "ktypes.h"
#include "kernel_config.h"
#include "process.h"
#include "klib.h"
#include "runqueue.h"
#include "error.h"

extern uint32_t g_timer_frequency;

#define SCHEDULER_DEFAULT_TICK_HZ CONFIG_SCHED_DEFAULT_TICK_HZ

// Core parking thresholds
#define SCHEDULER_PARK_IDLE_TICKS    CONFIG_SCHED_PARK_IDLE_TICKS
#define SCHEDULER_UNPARK_LOAD_THRESH CONFIG_SCHED_UNPARK_LOAD_THRESH

// Affinity: cache-warm threshold
#define SCHEDULER_AFFINITY_WARM_TICKS CONFIG_SCHED_AFFINITY_WARM_TICKS

// Recalc interval
#define SCHED_RECALC_INTERVAL CONFIG_SCHED_RECALC_INTERVAL

// ============================================================================
// Scheduler State
// ============================================================================

extern volatile uint64_t g_global_tick;

typedef struct {
    process_t  *current_process;
    spinlock_t  scheduler_lock;
    RunQueue    runqueue;
    /* This App Core's turn: the TSC moment it began — the strand running here
     * took the core, or began another turn on it — and the moment the core's
     * one-shot is armed for, 0 for none: a strand alone on its core has no
     * end, and an idle core none but a look owed (scheduler.c). Written by
     * this core alone, with its interrupts off. */
    uint64_t    turn_began;
    uint64_t    turn_due;
    /* The Waterline: the least Fare among this core's strands — the one
     * running at what it owes now — and never lower than it was: the level a
     * strand that comes to this core is placed against (scheduler.c). Written
     * under this core's runqueue lock; read by others without. */
    volatile uint64_t waterline;
    /* The strand running here as this core dispatched it: what it owed then
     * (run_fare), that moment (run_since, 0 while idle runs) and its weight
     * (run_price, scheduler.c's sched_price) — for the Waterline to count what
     * it has run since. Written by this core alone at every dispatch, its
     * interrupts off, between two steps of run_seq — odd while it writes;
     * read by any core. */
    volatile uint32_t run_seq;
    uint32_t          run_price;
    uint64_t          run_fare;
    uint64_t          run_since;
    /* The strand running here gave its core away (sched_yield_turn): its
     * next schedule() stands it behind the next one. This core's alone. */
    bool        turn_given;
    /* What this core last dispatched: a strand at all (busy_here), and one
     * wearing the Use Context — its Fit not 0 (context_here). Written by this
     * core at every dispatch; read by the others, without a lock, when one of
     * them makes room for the person's work (scheduler.c, sched_make_room)
     * and looks for a core to send an outsider to. */
    volatile bool busy_here;
    volatile bool context_here;
    /* The strand running here gave its core away and, nobody else being
     * there, took it again (given_up) — or is one that holds a core only a
     * moment each time it has one (short_here, its hold_mean under a least
     * turn): either holds the core only until somebody wants it, and a wake
     * may land here as on a free core (scheduler.c). The one that gave the
     * core away is passed at once; a short one is asked to step aside and has
     * its least turn. Written by this core at every dispatch; read by others
     * without a lock. */
    volatile bool given_up;
    volatile bool short_here;
    _Atomic bool is_parked;            // Written by BSP recalc, read by AP timer IRQ — must be atomic
    uint32_t    idle_tick_count;       // Consecutive idle ticks
    /* Quiescence sequence (reap-vs-switch safety). Bumped at the top of every
     * schedule() on this core. An evicted strand stamps the value it saw; that
     * strand's kernel stack is provably free once this counter has advanced
     * past the stamp, because the next schedule() runs on the incoming strand's
     * stack (post-iretq) — never the evicted one's. The reaper gates on it so a
     * strand's stack is never freed while this core is still in its epilogue. */
    volatile uint64_t quiesce_seq;
    /* Spans this core is in that its kernel context must finish before being
     * switched away from (SchedulerHoldHere): Scribe passes, and commands
     * stood over on a medium. While it is not zero, a tick or a wake that
     * interrupted this core's kernel context does not switch away from it.
     * Written only by the core itself, with its interrupts off; read by its
     * own interrupts. */
    volatile uint32_t held;
    /* TSC accounting for system-wide CPU utilization. Each core updates its
     * own counters at scheduler entry; readers add the span since — a strand
     * alone on its core enters the scheduler only when it parks — and take
     * the three together, again if the core moved them meanwhile. */
    volatile uint64_t accounting_last_tsc;
    volatile uint64_t accounting_busy_tsc;
    volatile uint64_t accounting_total_tsc;
} scheduler_state_t;

// ============================================================================
// Public API
// ============================================================================

error_t scheduler_init(void);
void scheduler_init_core(uint8_t core_index);
void scheduler_shutdown(void);
void schedule(void *frame);
process_t *scheduler_select_next(process_t *passed);
/* A strand came to this core's runqueue (IPI_WAKE). The strand running here
 * keeps the core to the end of its least turn when the newcomer is owed it,
 * else to the end of its turn — its one-shot armed for that moment — and the
 * core is scheduled at once once it is due, or when nothing runs. */
void scheduler_woken(void *frame);

// Core parking, looked at from the BSP's timer IRQ
void scheduler_recalc_parameters(void);

// Core parking
void scheduler_park_core(uint8_t core_idx);
void scheduler_unpark_core(uint8_t core_idx);
bool scheduler_is_core_parked(uint8_t core_idx);

// Processor accounting
void scheduler_cpu_usage(uint64_t *busy_tsc, uint64_t *total_tsc);
bool scheduler_core_cpu_usage(uint8_t core_idx, uint64_t *busy_tsc,
                              uint64_t *total_tsc, bool *parked);

error_t sched_enqueue(process_t *proc);

/* What holding a core `cycles` long costs `proc`: the cycles, the less the
 * more of the Use Context its cabin wears (its Fit) — down to a third for all
 * of it. The one price of a core's time, an App Core's Fare and a K-Core's
 * Tip (kcore.c) alike. */
uint64_t sched_fare_for(const process_t *proc, uint64_t cycles);

/* `paid` — a Fare or a Tip — brought from the core it was reckoned on, whose
 * Waterline is `there` (`known` false: none), to the core whose Waterline is
 * `here`: it keeps how far above or below `there` it stood, a newcomer stands
 * at `here`, and none stands more than half a round below `here`. */
uint64_t sched_rebase(uint64_t paid, bool known, uint64_t there, uint64_t here);
error_t sched_dequeue(process_t *proc);
/* The strand running on this core gives its core away (GATE_YIELD): its next
 * schedule() passes it once for the least Fare queued after it, at the Fare it
 * has paid, so the one it steps aside for runs first and a waiter looks at its
 * answer again one turn later, not a round later. */
void sched_yield_turn(void);
/* The doorbell for a strand that is runnable but may be waiting its turn: IPI
 * its home core, unless it is on a CPU right now (it reads its rings itself)
 * or its home is this core. A wake whose state flip enqueued the strand has
 * already rung — sched_enqueue's IPI — and must not ring again. */
void sched_ring_home(process_t *proc);
/* The doorbell for a strand that has stopped working while it is on a CPU —
 * parked, ended, crashed: IPI that core, its own included, so its scheduler
 * takes the strand off now. Called after the new state is stored, with a full
 * barrier between (process_set_state). */
void sched_ring_running(process_t *proc);
/* Remove proc from whichever core's runqueue holds it (referenced-nowhere
 * guarantee before process_destroy frees it). See scheduler.c. */
void sched_dequeue_all_cores(process_t *proc);

scheduler_state_t *scheduler_get_state(void);
scheduler_state_t *scheduler_get_core(uint8_t core_idx);

/* A span this core's kernel context must not be switched away from begins
 * and ends here: a Scribe pass (tagfs/scribe), or a command stood over on a
 * medium from the moment it asks for a place until it gives the place back
 * (core/boardroom/roadstead.h). The tick does not switch away in between —
 * what an interrupts-off lock used to give, without the deafness: interrupts
 * stay on. Safe with interrupts in either state; in pairs, on one core. */
void SchedulerHoldHere(void);
void SchedulerLetGoHere(void);

/* Whether this core is inside such a span. An orderly halt that finds it so
 * lets it reach the end first (HaltStopIfOwedHere, system_halt.h). */
bool SchedulerHeldHere(void);

#endif // SCHEDULER_H
