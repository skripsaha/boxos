#include "box/core/stash.h"
#include "box/memory.h"
#include "box/string.h"

static inline uint8_t *slot_at(const Stash *s, const StashChunk *c, uint32_t i)
{
    return (uint8_t *)c->entries + (size_t)(i % s->chunk_cap) * s->entry_size;
}

void stash_init(Stash *s, uint32_t entry_size, uint32_t chunk_cap)
{
    memset(s, 0, sizeof(*s));
    s->entry_size = entry_size;
    s->chunk_cap  = chunk_cap;
}

static StashChunk *chunk_take_or_new(Stash *s)
{
    StashChunk *c = s->spare;
    if (c) {
        s->spare = NULL;
    } else {
        c = malloc(sizeof(StashChunk) + (size_t)s->chunk_cap * s->entry_size);
        if (!c) return NULL;
    }
    c->next  = NULL;
    c->head  = 0;
    c->count = 0;
    return c;
}

bool stash_reserve(Stash *s)
{
    if (!s || s->entry_size == 0 || s->chunk_cap == 0) return false;
    if (s->tail && s->tail->count < s->chunk_cap) return true;
    StashChunk *c = chunk_take_or_new(s);
    if (!c) return false;
    if (s->tail) s->tail->next = c;
    else         s->head       = c;
    s->tail = c;
    return true;
}

void stash_put(Stash *s, const void *entry)
{
    StashChunk *c = s->tail;
    memcpy(slot_at(s, c, c->head + c->count), entry, s->entry_size);
    c->count++;
    s->count++;
}

static void drop_empty_heads(Stash *s)
{
    while (s->head && s->head->count == 0 && s->head != s->tail) {
        StashChunk *c = s->head;
        s->head = c->next;
        if (s->spare) free(c);
        else          s->spare = c;
    }
    if (s->head && s->head->count == 0) s->head->head = 0;
}

bool stash_take(Stash *s, void *out)
{
    if (!s || s->count == 0) return false;
    drop_empty_heads(s);
    StashChunk *c = s->head;
    memcpy(out, slot_at(s, c, c->head), s->entry_size);
    c->head = (c->head + 1) % s->chunk_cap;
    c->count--;
    s->count--;
    drop_empty_heads(s);
    return true;
}

bool stash_take_where(Stash *s, bool (*match)(const void *, const void *),
                      const void *key, void *out)
{
    if (!s || s->count == 0) return false;
    for (StashChunk *c = s->head; c; c = c->next) {
        for (uint32_t i = 0; i < c->count; i++) {
            uint8_t *e = slot_at(s, c, c->head + i);
            if (!match(e, key)) continue;
            memcpy(out, e, s->entry_size);
            for (uint32_t j = i; j + 1 < c->count; j++) {
                memcpy(slot_at(s, c, c->head + j),
                       slot_at(s, c, c->head + j + 1), s->entry_size);
            }
            c->count--;
            s->count--;
            drop_empty_heads(s);
            return true;
        }
    }
    return false;
}

void stash_clear(Stash *s)
{
    if (!s) return;
    StashChunk *c = s->head;
    while (c) {
        StashChunk *next = c->next;
        if (c == s->head) { c->next = NULL; c->head = 0; c->count = 0; }
        else if (!s->spare) { s->spare = c; }
        else free(c);
        c = next;
    }
    s->tail  = s->head;
    s->count = 0;
}

void stash_free(Stash *s)
{
    if (!s) return;
    StashChunk *c = s->head;
    while (c) {
        StashChunk *next = c->next;
        free(c);
        c = next;
    }
    free(s->spare);
    s->head = s->tail = s->spare = NULL;
    s->count = 0;
}