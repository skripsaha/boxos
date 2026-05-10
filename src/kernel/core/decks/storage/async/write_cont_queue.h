#ifndef WRITE_CONT_QUEUE_H
#define WRITE_CONT_QUEUE_H

#include "ktypes.h"
#include "error.h"

/*
 * Write-Continuation Queue — per-K-Core unbounded MPSC linked-list.
 *
 * BoxOS philosophy "динамика" — no fixed limits. Producers race only on
 * a single atomic XCHG of the tail pointer (Vyukov's MPSC algorithm).
 * Consumer is the owning K-Core; it walks `head -> next` links. Memory
 * cost per pending continuation is one node (24 bytes), reclaimed on
 * consume. Heap exhaustion is the only natural bound, signalled as
 * ERR_NO_MEMORY to caller.
 */

typedef void (*WriteContFn)(void *job);

typedef struct WriteContNode {
    WriteContFn                       fn;
    void                             *job;
    struct WriteContNode * volatile   next;
} WriteContNode;

typedef struct {
    /* Producer-side cursor — every push XCHGs this. */
    WriteContNode * volatile  tail;
    /* Consumer-side cursor — only the owning K-Core touches it. */
    WriteContNode            *head;
    /* Dummy head node, never freed; keeps the list non-empty so push
     * doesn't need a head/tail special case. */
    WriteContNode             stub;
    /* Approximate depth, RELAXED — used only for routing decisions. */
    volatile uint32_t         pending;
    uint8_t                   kcore_idx;
    uint8_t                   _pad[3];
} __attribute__((aligned(64))) WriteContQueue;

extern WriteContQueue *g_write_cont_queues;

void WriteContQueueInit(void);
error_t WriteContEnqueue(WriteContFn fn, void *job);
uint32_t WriteContPump(uint8_t kcore_idx);
uint32_t WriteContDepth(uint8_t kcore_idx);

#endif /* WRITE_CONT_QUEUE_H */
