#ifndef LISTEN_TABLE_H
#define LISTEN_TABLE_H

#include "ktypes.h"
#include "klib.h"

// listen_table: slab-allocated intrusive linked list of event listeners.
// No static capacity limit.

#define LISTEN_KEYBOARD  0
#define LISTEN_MOUSE     1
#define LISTEN_NETWORK   2

#define LISTEN_FLAG_EXCLUSIVE 0x01

typedef struct ListenNode {
    uint32_t          pid;
    uint8_t           source_type;
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
int      listen_table_add(uint32_t pid, uint8_t source_type, uint8_t flags);
int      listen_table_remove(uint32_t pid, uint8_t source_type);
uint32_t listen_table_find(uint8_t source_type, uint32_t *out_pids, uint32_t max_pids);
void     listen_table_remove_pid(uint32_t pid);

#endif
