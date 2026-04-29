#ifndef STORAGE_DECK_H
#define STORAGE_DECK_H

#include "ktypes.h"
#include "error.h"
#include "boxos_decks.h"

#define STORAGE_DECK_ID         DECK_STORAGE

/* Manifest opcodes (used by storage_ops.c handlers + boxlib wrappers). */
#define STORAGE_TAG_QUERY       0x01
#define STORAGE_TAG_SET         0x02
#define STORAGE_TAG_UNSET       0x03
#define STORAGE_OBJ_READ        0x05
#define STORAGE_OBJ_WRITE       0x06
#define STORAGE_OBJ_CREATE      0x07
#define STORAGE_OBJ_DELETE      0x08
#define STORAGE_OBJ_RENAME      0x09
#define STORAGE_OBJ_GET_INFO    0x0A
#define STORAGE_CONTEXT_SET     0x10
#define STORAGE_CONTEXT_CLEAR   0x11

void    storage_deck_init(void);
error_t StorageDeckRegister(void);

#endif /* STORAGE_DECK_H */
