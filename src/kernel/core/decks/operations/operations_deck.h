#ifndef OPERATIONS_DECK_H
#define OPERATIONS_DECK_H

#include "ktypes.h"
#include "boxos_decks.h"   /* OP_BUF_*, OP_BIT_SWAP, OP_VAL_ADD — the deck's opcodes, single source */
#include "error.h"

#define OPERATIONS_DECK_ID DECK_OPERATIONS

error_t OperationsDeckRegister(void);

#endif /* OPERATIONS_DECK_H */
