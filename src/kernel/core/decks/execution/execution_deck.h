#ifndef EXECUTION_DECK_H
#define EXECUTION_DECK_H

#include "events.h"
#include "boxos_decks.h"

#define EXECUTION_DECK_ID       DECK_EXECUTION
#define RESULT_OVERFLOW_MARKER  0xDEADBEEF

int execution_deck_handler(Event* event);

#endif // EXECUTION_DECK_H
