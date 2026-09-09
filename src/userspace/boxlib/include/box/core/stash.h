#ifndef BOX_CORE_STASH_H
#define BOX_CORE_STASH_H

#include "box/defs.h"

/*
 * A stash is where a strand keeps what its ring handed it that was not for the
 * caller at hand: an IPC message met while waiting for a reply, a ferry
 * completion met by an IPC receive, a Touch of another tag. It is a FIFO that
 * grows by the chunk and never drops.
 *
 * What stood here were four fixed rings — 8192 entries for the main strand,
 * 256 for a spawned one, 256 Touches — that dropped the oldest entry or shed
 * the newest when full, in silence. This one adds a chunk when the last is
 * full. A chunk is as long as the ring the entries came from: that number is
 * the kernel's, read from the ring header, so a stash holds a whole ring's
 * worth before it asks the heap for more, and no number of ours stands between
 * an accepted message and its reader.
 *
 * Room is asked for BEFORE an entry leaves its ring (stash_reserve). When the
 * heap has none, the entry stays in the ring, where nothing is lost, and the
 * strand says so. Single-writer: one strand, its own stash, no lock.
 */

typedef struct StashChunk {
    struct StashChunk *next;
    uint32_t head;        /* oldest entry of this chunk (index)   */
    uint32_t count;       /* live entries in this chunk           */
    uint8_t  entries[];   /* chunk_cap * entry_size bytes, a ring */
} StashChunk;

typedef struct {
    StashChunk *head;       /* taken from */
    StashChunk *tail;       /* put into   */
    StashChunk *spare;      /* one emptied chunk kept for the next fill */
    uint32_t    entry_size;
    uint32_t    chunk_cap;
    uint32_t    count;      /* live entries across every chunk */
} Stash;

void stash_init(Stash *s, uint32_t entry_size, uint32_t chunk_cap);

/* Room for one more entry. False only when the heap has none — the caller then
 * leaves its entry where it is. A reservation that goes unused is not lost: it
 * is the room the next put finds. */
bool stash_reserve(Stash *s);

/* Put after a successful reserve. Cannot fail. */
void stash_put(Stash *s, const void *entry);

/* The oldest entry. */
bool stash_take(Stash *s, void *out);

/* The oldest entry `match` says yes to, taken out of order; the rest keep
 * their order. */
bool stash_take_where(Stash *s, bool (*match)(const void *entry, const void *key),
                      const void *key, void *out);

/* Everything out; one chunk's memory kept for the next fill. */
void stash_clear(Stash *s);

/* Every chunk back to the heap. The stash is empty and may be used again. */
void stash_free(Stash *s);

static inline uint32_t stash_count(const Stash *s) { return s ? s->count : 0; }

#endif /* BOX_CORE_STASH_H */
