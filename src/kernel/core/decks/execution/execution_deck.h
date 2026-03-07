#ifndef EXECUTION_DECK_H
#define EXECUTION_DECK_H

#include "pocket.h"
#include "boxos_decks.h"

#define EXECUTION_DECK_ID DECK_EXECUTION

// Writes a Result to the process's ResultRing based on Pocket state.
// Called at the end of the prefix chain (or on error/security denial).
int execution_deck_handler(Pocket* pocket);

#endif // EXECUTION_DECK_H
