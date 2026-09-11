#ifndef BOX_CORE_STASH_H
#define BOX_CORE_STASH_H

#include "box/defs.h"


typedef struct StashChunk {
    struct StashChunk *next;
    uint32_t head;
    uint32_t count;
    uint8_t  entries[];
} StashChunk;

typedef struct {
    StashChunk *head;
    StashChunk *tail;
    StashChunk *spare;
    uint32_t    entry_size;
    uint32_t    chunk_cap;
    uint32_t    count;
} Stash;

void stash_init(Stash *s, uint32_t entry_size, uint32_t chunk_cap);

bool stash_reserve(Stash *s);

void stash_put(Stash *s, const void *entry);

bool stash_take(Stash *s, void *out);

bool stash_take_where(Stash *s, bool (*match)(const void *entry, const void *key),
                      const void *key, void *out);

void stash_clear(Stash *s);

void stash_free(Stash *s);

static inline uint32_t stash_count(const Stash *s) { return s ? s->count : 0; }

#endif