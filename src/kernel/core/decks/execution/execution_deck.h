#ifndef EXECUTION_DECK_H
#define EXECUTION_DECK_H

#include "pocket.h"
#include "boxos_decks.h"

typedef struct process_t process_t;

#define EXECUTION_DECK_ID DECK_EXECUTION

int execution_deck_handler(Pocket* pocket, process_t* proc);

#endif