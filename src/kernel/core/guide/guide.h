#ifndef GUIDE_H
#define GUIDE_H

#include "ktypes.h"
#include "ready_queue.h"
#include "pocket.h"
#include "klib.h"

// g_ready_queue is declared in ready_queue.h — no extern needed here

typedef struct process_t process_t;

void guide_init(void);
void guide(void);

// Process all pending Pockets for one process (drain PocketRing, write Results).
// Used by K-Core guide loop. Does NOT change process state — caller manages that.
void guide_process_one(process_t *proc);

// Deck handler: processes a single Pocket prefix.
// Returns 0 on success, negative error_t on failure.
// proc = source process (already resolved by Guide, avoids redundant process_find).
typedef int (*deck_handler_t)(Pocket *pocket, process_t *proc);

typedef struct
{
    uint8_t deck_id;
    const char *name;
    deck_handler_t handler;
} DeckEntry;

deck_handler_t guide_get_deck_handler(uint8_t deck_id);

#endif // GUIDE_H
