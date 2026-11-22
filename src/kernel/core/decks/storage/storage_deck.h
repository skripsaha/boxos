#ifndef STORAGE_DECK_H
#define STORAGE_DECK_H

#include "ktypes.h"
#include "error.h"
#include "boxos_decks.h"
#include "tagfs.h"
#include "boxos_manifest.h"
#include "klib.h"

#define STORAGE_DECK_ID         DECK_STORAGE

/* A call's params read at byte `off` — alignment-safe: the manifest lays them
 * packed, whatever the width. The caller has measured param_size first. */
static inline uint16_t param_u16(const ManifestOp *op, uint16_t off)
{
    uint16_t v;
    memcpy(&v, op->params + off, sizeof(v));
    return v;
}

static inline uint32_t param_u32(const ManifestOp *op, uint16_t off)
{
    uint32_t v;
    memcpy(&v, op->params + off, sizeof(v));
    return v;
}

static inline uint64_t param_u64(const ManifestOp *op, uint16_t off)
{
    uint64_t v;
    memcpy(&v, op->params + off, sizeof(v));
    return v;
}

/* The opcodes and the scope byte live in boxos_decks.h, the single source
 * both this deck's handler table and boxlib's wrappers read. */

void    storage_deck_init(void);
error_t StorageDeckRegister(void);

/* An event about a file — a write, a span, an anchor — said to everyone who
 * listens for it: on every one of the person's tags the file wears — Touch
 * speaks the person's tags, and an event on a program's own would tell every
 * listener what it keeps to itself (tagfs.h, TagOwner) — and on the words its name
 * answers to (the whole name and its stem, the name index's), so a program
 * that watches the files it knows by name hears them without the name being a
 * tag of the file. The one fan-out of the deck's file events: what the
 * listeners hear is the same whichever road changed the file. `meta` is the
 * file's card; `pid` the caller who made the change, 0 for none. */
void    StorageFileSays(const TagFSMetadata *meta, const void *event, size_t size, uint32_t pid);

#endif /* STORAGE_DECK_H */
