#ifndef KCORE_H
#define KCORE_H

#include "ktypes.h"
#include "amp.h"
#include "error.h"

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
    struct process_t* volatile serving;
} __attribute__((aligned(64))) KCorePocketQueue;

extern KCorePocketQueue *g_kcore_queues;

void kcore_init(void);
error_t kcore_submit(struct process_t* proc);
void kcore_run_loop(void) __attribute__((noreturn));
uint32_t kcore_queue_depth(uint8_t core_idx);
bool kcore_is_serving(const struct process_t* proc);

#endif