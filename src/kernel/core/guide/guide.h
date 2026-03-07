#ifndef GUIDE_H
#define GUIDE_H

#include "ktypes.h"
#include "ready_queue.h"
#include "pocket.h"
#include "klib.h"

extern ReadyQueue g_ready_queue;

typedef struct process_t process_t;

void guide_init(void);
void guide_run(void);
void guide_wake(void);
bool guide_is_idle(void);

// Deck handler: processes a single Pocket prefix.
// Returns 0 on success, negative error_t on failure.
typedef int (*deck_handler_t)(Pocket* pocket);

typedef struct {
    uint8_t deck_id;
    const char* name;
    deck_handler_t handler;
} DeckEntry;

deck_handler_t guide_get_deck_handler(uint8_t deck_id);

#endif // GUIDE_H
