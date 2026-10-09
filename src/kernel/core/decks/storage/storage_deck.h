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

/* A file id as a program is handed it, in an answer's crate: the programs'
 * file_id_t (box/types.h) is a u32 until their ABI widens with wave 3, and the
 * kernel holds ids in 64 bits. OK and *out when the id fits; when it does not,
 * ERR_TAGFS_FID_EXHAUSTED — said, the first time — and *out untouched: an id
 * is never cut to another file's number. No format v1 volume issues one past
 * it (LedgerIssueFileId). */
error_t StorageFileIdOut(uint64_t fid, uint32_t *out);

#endif /* STORAGE_DECK_H */
