#ifndef READY_QUEUE_H
#define READY_QUEUE_H

#include "ktypes.h"
#include "klib.h"


typedef struct process_t process_t;

typedef struct {
    process_t  *head;
    process_t  *tail;
    uint32_t    count;
    spinlock_t  lock;
} ReadyQueue;

extern ReadyQueue g_ready_queue;

void      ready_queue_init(ReadyQueue *rq);
bool      ready_queue_push(ReadyQueue *rq, process_t *proc);
process_t *ready_queue_pop(ReadyQueue *rq);
bool      ready_queue_is_empty(const ReadyQueue *rq);
uint32_t  ready_queue_count(const ReadyQueue *rq);

#endif