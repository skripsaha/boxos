#ifndef PENDING_RESULTS_H
#define PENDING_RESULTS_H

#include "ktypes.h"
#include "klib.h"
#include "result_page.h"

#define PENDING_QUEUE_SIZE 512  // Scaled for 4096-process limit

typedef struct {
    uint32_t pid;
    result_entry_t entry;
    uint64_t timestamp;
} pending_result_t;

typedef struct {
    pending_result_t entries[PENDING_QUEUE_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    spinlock_t lock;
} pending_queue_t;

void pending_results_init(void);
int pending_results_enqueue(uint32_t pid, const result_entry_t* entry);
int pending_results_try_deliver(uint32_t pid);
void pending_results_flush_all(void);

#endif // PENDING_RESULTS_H
