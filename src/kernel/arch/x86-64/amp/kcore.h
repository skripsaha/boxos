#ifndef KCORE_H
#define KCORE_H

#include "ktypes.h"
#include "amp.h"

// Per-K-Core MPSC PocketQueue.
// Multiple App Cores push (CAS on tail), one K-Core pops (single consumer).
// Capacity must be a power of two for efficient modulo.
#define KCORE_QUEUE_CAPACITY  512
#define KCORE_QUEUE_MASK      (KCORE_QUEUE_CAPACITY - 1)

struct process_t;

typedef struct {
    struct process_t* volatile slots[KCORE_QUEUE_CAPACITY];
    volatile uint32_t head;           // K-Core advances (single consumer)
    volatile uint32_t tail;           // App Cores CAS-advance (multiple producers)
    volatile uint32_t count;          // approximate depth (atomic inc/dec)
    uint8_t           kcore_idx;      // which K-Core owns this queue
    uint8_t           _pad[3];
} __attribute__((aligned(64))) KCorePocketQueue;

// One queue per K-Core (indexed by core_index, not sequential K-Core number).
extern KCorePocketQueue g_kcore_queues[MAX_CORES];

// Initialize all K-Core queues. Must be called before amp_boot_aps().
void kcore_init(void);

// App Core: enqueue proc into least-loaded K-Core queue + IPI_WAKE.
// Caller must have set proc->kcore_pending = 1 via atomic CAS beforehand.
void kcore_submit(struct process_t* proc);

// K-Core: run the infinite guide loop. Never returns.
// Called from ap_entry_c() for AP K-Cores and from process_start_initial() for BSP.
void kcore_run_loop(void) __attribute__((noreturn));

// Approximate depth of a K-Core's queue (for least-loaded selection).
uint32_t kcore_queue_depth(uint8_t core_idx);

#endif // KCORE_H
