#ifndef KCORE_H
#define KCORE_H

#include "ktypes.h"
#include "amp.h"
#include "error.h"

/* Sized to the PROCESS LIMIT, deliberately. kcore_pending dedup guarantees
 * a process occupies at most ONE slot across all queues, so a capacity of
 * MAX_PROCESSES makes "queue full" impossible by construction — even if
 * every process in the machine lands on the same K-Core at once. The old
 * 512 was reachable under strand churn (4096 possible processes into
 * 4×512 slots), and the failure branch left kcore_pending=1 with the
 * pocket queued NOWHERE: every later notify saw pending set and skipped
 * the submit, stranding the process forever — the all-idle mute wedge the
 * 16-core matrix kept hitting (K-Cores honestly asleep over truly empty
 * queues while the stranded strand spun in result_wait). Memory cost:
 * 32 KiB of slot pointers per core, from the PMM at boot. */
#include "boxos_limits.h"
#define KCORE_QUEUE_CAPACITY   MAX_PROCESSES
#define KCORE_QUEUE_MASK       (KCORE_QUEUE_CAPACITY - 1)

_Static_assert((KCORE_QUEUE_CAPACITY & (KCORE_QUEUE_CAPACITY - 1)) == 0,
               "KCORE_QUEUE_CAPACITY must be a power of two (ring mask)");

struct process_t;

typedef struct {
    struct process_t* volatile slots[KCORE_QUEUE_CAPACITY];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t count;
    uint8_t           kcore_idx;
    uint8_t           _pad[3];
    /* The strand this K-Core is serving right now; NULL between two. Held
     * around guide_process_one and read by Nightwatch: a pocket standing at
     * its ring's head while a K-Core serves its owner is being worked on,
     * however long that takes (a proc_exec reads its image inside the call);
     * one standing there with nobody serving it is unserved. */
    struct process_t* volatile serving;
    /* 1 while this K-Core is committed to HLT: set by the sleep gate BEFORE
     * its last look at the queue, cleared when it wakes. kcore_submit rings
     * the doorbell only when it reads 1 — an awake K-Core finds the pocket
     * on its next pop. Dekker pairing: the gate's store and the producer's
     * lock xadd on `count` are both full barriers, so either the gate sees
     * the claim or the producer sees the flag. */
    volatile uint32_t in_hlt;
    /* The strands this K-Core has taken out of `slots` and holds until it
     * serves them, the least Tip first: a binary heap only this K-Core
     * touches, so it takes no lock and needs no slot index — a held strand's
     * Tip moves only while it is served, and then it is held nowhere. `count`
     * goes on counting them; it falls when a strand is served, not when it
     * leaves the ring, and that is what every reader of a queue's depth asks.
     * Sized as the ring, and for the same reason. */
    struct process_t** held;
    uint32_t           held_count;
    /* This K-Core's Waterline: the least Tip among the strands it held when
     * it last served one, and never lower than it was. Written by this
     * K-Core; read by another when a strand comes from it (sched_rebase). */
    volatile uint64_t  waterline;
    /* Where this K-Core's time goes, counted by its own loop, which no
     * scheduler sees: the TSC ticks it spent awake — serving, pumping,
     * servicing — and in all, up to `count_mark`; the time since the mark is
     * awake unless in_hlt says it sleeps (kcore_cpu_usage). Written by this
     * K-Core alone, under `count_seq`, odd while it writes. */
    volatile uint32_t  count_seq;
    volatile uint64_t  count_mark;
    volatile uint64_t  count_awake;
    volatile uint64_t  count_total;
} __attribute__((aligned(64))) KCorePocketQueue;

extern KCorePocketQueue *g_kcore_queues;

void kcore_init(void);
error_t kcore_submit(struct process_t* proc);
void kcore_run_loop(void) __attribute__((noreturn));
uint32_t kcore_queue_depth(uint8_t core_idx);
/* True while some K-Core is inside guide_process_one for this strand. */
bool kcore_is_serving(const struct process_t* proc);
/* A K-Core's time, as scheduler_core_cpu_usage says an App Core's: the TSC
 * ticks it was awake and in all since it began its loop, counted to now.
 * False for a core that is no K-Core. */
bool kcore_cpu_usage(uint8_t core_idx, uint64_t* busy_tsc, uint64_t* total_tsc);
/* A K-Core asleep in its HLT gate woken to go round its loop again — for work
 * it finds at a service point rather than in its queue. The work is published
 * before the call; from any context, an interrupt's included — it is one load
 * and at most one IPI. Nothing for the calling core itself: whatever it is
 * in the middle of, it goes round its loop after it. */
void kcore_wake(uint8_t core_idx);

#endif // KCORE_H
