#ifndef GUIDE_H
#define GUIDE_H

#include "ktypes.h"
#include "events.h"
#include "event_ring.h"
#include "klib.h"

extern EventRingBuffer* kernel_event_ring;

typedef struct process_t process_t;

typedef struct {
    process_t* head;
    process_t* tail;
    spinlock_t lock;
    volatile uint32_t count;
} event_ring_wait_queue_t;

extern event_ring_wait_queue_t event_ring_waiters;

void guide_init_wait_queue(void);
void guide_block_on_event_ring(process_t* proc);
void guide_wakeup_waiters(void);

void guide_init(void);
void guide_run(void);
void guide_wake(void);
bool guide_is_idle(void);

typedef int (*deck_handler_t)(Event* event);

typedef struct {
    uint8_t deck_id;
    const char* name;
    deck_handler_t handler;
} DeckEntry;

deck_handler_t guide_get_deck_handler(uint8_t deck_id);
uint32_t guide_alloc_event_id(void);

void guide_testrun(void);
void test_full_cycle(void);

#endif // GUIDE_H
