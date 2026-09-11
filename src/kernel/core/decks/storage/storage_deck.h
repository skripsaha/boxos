#ifndef STORAGE_DECK_H
#define STORAGE_DECK_H

#include "ktypes.h"
#include "error.h"
#include "boxos_decks.h"

#define STORAGE_DECK_ID         DECK_STORAGE


void    storage_deck_init(void);
error_t StorageDeckRegister(void);

#endif