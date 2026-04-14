#ifndef LISTEN_TABLE_H
#define LISTEN_TABLE_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"

// listen_table: slab-allocated intrusive linked list of event listeners with
// a hash index for O(1) duplicate detection on (pid, required_tags).

#define EVENT_TAG_KEYBOARD   (1ULL << 0)
#define EVENT_TAG_MOUSE      (1ULL << 1)
#define EVENT_TAG_NETWORK    (1ULL << 2)
#define EVENT_TAG_STORAGE    (1ULL << 3)
#define EVENT_TAG_TIMER      (1ULL << 4)

#define LISTEN_FLAG_EXCLUSIVE  0x01
#define LISTEN_HASH_BUCKETS    256   // power of 2 for fast modulo
#define LISTEN_TABLE_MAX_ENTRIES 1024

typedef struct ListenNode {
    uint32_t          pid;
    uint64_t          required_tags;
    uint8_t           flags;
    struct ListenNode *next;
    struct ListenNode *hash_next;  // collision chain in hash bucket
} ListenNode;

typedef struct {
    ListenNode  *head;
    uint32_t     count;
    spinlock_t   lock;
    ListenNode  *hash_index[LISTEN_HASH_BUCKETS];
} listen_table_t;

extern listen_table_t g_listen_table;

void     listen_table_init(void);
error_t  listen_table_add(uint32_t pid, uint64_t required_tags, uint8_t flags);
error_t  listen_table_remove(uint32_t pid, uint64_t required_tags);
uint32_t listen_table_find(uint64_t event_tags, uint32_t *out_pids, uint32_t max_pids);
void     listen_table_remove_pid(uint32_t pid);

#endif // LISTEN_TABLE_H
