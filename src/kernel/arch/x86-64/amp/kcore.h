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
} __attribute__((aligned(64))) KCorePocketQueue;

extern KCorePocketQueue *g_kcore_queues;

void kcore_init(void);
error_t kcore_submit(struct process_t* proc);
void kcore_run_loop(void) __attribute__((noreturn));
uint32_t kcore_queue_depth(uint8_t core_idx);
/* True while some K-Core is inside guide_process_one for this strand. */
bool kcore_is_serving(const struct process_t* proc);

#endif // KCORE_H
