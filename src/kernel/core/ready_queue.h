#ifndef READY_QUEUE_H
#define READY_QUEUE_H

#include "ktypes.h"
#include "boxos_sizes.h"

// ReadyQueue: global FIFO queue of processes that have pending Pockets.
// Replaces the old EventRing — holds process_t* pointers instead of full Event copies.
// Syscall handler and ISR push here; Guide pops and processes.

typedef struct process_t process_t;

typedef struct {
    process_t* entries[READY_QUEUE_CAPACITY];
    volatile uint32_t head;
    volatile uint32_t tail;
    spinlock_t lock;
} ReadyQueue;

void ready_queue_init(ReadyQueue* rq);
bool ready_queue_push(ReadyQueue* rq, process_t* proc);
process_t* ready_queue_pop(ReadyQueue* rq);
bool ready_queue_is_empty(const ReadyQueue* rq);
uint32_t ready_queue_count(const ReadyQueue* rq);

#endif // READY_QUEUE_H
