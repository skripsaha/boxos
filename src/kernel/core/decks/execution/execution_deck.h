#ifndef EXECUTION_DECK_H
#define EXECUTION_DECK_H

#include "pocket.h"
#include "boxos_decks.h"

typedef struct process_t process_t;

#define EXECUTION_DECK_ID DECK_EXECUTION

// Writes a Result to the process's ResultRing based on Pocket state.
// Called at the end of the prefix chain (or on error/security denial).
// proc = source process (already resolved by Guide).
int execution_deck_handler(Pocket* pocket, process_t* proc);

#endif // EXECUTION_DECK_H
