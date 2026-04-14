#ifndef LISTEN_TABLE_H
#define LISTEN_TABLE_H

#include "ktypes.h"
#include "klib.h"

// listen_table: slab-allocated intrusive linked list of event listeners.
// Listeners register a required_tags bitmask and receive events where
// (event_tags & required_tags) == required_tags (subset match).

// Built-in event tag bits.
// Bits 0-7 reserved for core system events.
// Bits 8-63 available for drivers, virtual devices, and user-defined sources.
#define EVENT_TAG_KEYBOARD   (1ULL << 0)
#define EVENT_TAG_MOUSE      (1ULL << 1)
#define EVENT_TAG_NETWORK    (1ULL << 2)
#define EVENT_TAG_STORAGE    (1ULL << 3)
#define EVENT_TAG_TIMER      (1ULL << 4)

#define LISTEN_FLAG_EXCLUSIVE 0x01

typedef struct ListenNode {
    uint32_t          pid;
    uint64_t          required_tags;   // match when (event_tags & required_tags) == required_tags
    uint8_t           flags;
    struct ListenNode *next;
} ListenNode;

typedef struct {
    ListenNode *head;
    uint32_t    count;
    spinlock_t  lock;
} listen_table_t;

extern listen_table_t g_listen_table;

void     listen_table_init(void);
int      listen_table_add(uint32_t pid, uint64_t required_tags, uint8_t flags);
int      listen_table_remove(uint32_t pid, uint64_t required_tags);
// Find all listeners whose required_tags are a subset of event_tags.
// Fills out_pids[], returns count of matches.
uint32_t listen_table_find(uint64_t event_tags, uint32_t *out_pids, uint32_t max_pids);
void     listen_table_remove_pid(uint32_t pid);

#endif // LISTEN_TABLE_H
