#ifndef STORAGE_DECK_H
#define STORAGE_DECK_H

#include "ktypes.h"
#include "error.h"
#include "boxos_decks.h"

#define STORAGE_DECK_ID         DECK_STORAGE

/* The opcodes and the scope byte live in boxos_decks.h, the single source
 * both this deck's handler table and boxlib's wrappers read. */

void    storage_deck_init(void);
error_t StorageDeckRegister(void);

#endif /* STORAGE_DECK_H */
