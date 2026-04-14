#include "listen_table.h"
#include "slab.h"
#include "klib.h"
#include "error.h"

listen_table_t g_listen_table;

static uint32_t hash_bucket(uint32_t pid, uint64_t required_tags) {
    uint64_t h = (uint64_t)pid * 2654435761ULL ^ required_tags;
    return (uint32_t)(h % LISTEN_HASH_BUCKETS);
}

void listen_table_init(void) {
    g_listen_table.head  = NULL;
    g_listen_table.count = 0;
    spinlock_init(&g_listen_table.lock);
    for (int i = 0; i < LISTEN_HASH_BUCKETS; i++) {
        g_listen_table.hash_index[i] = NULL;
    }
    debug_printf("[LISTEN] Listen table initialized\n");
}

static ListenNode* hash_find(uint32_t pid, uint64_t required_tags) {
    uint32_t bucket = hash_bucket(pid, required_tags);
    for (ListenNode* n = g_listen_table.hash_index[bucket]; n; n = n->hash_next) {
        if (n->pid == pid && n->required_tags == required_tags) {
            return n;
        }
    }
    return NULL;
}

static void hash_insert(ListenNode* node) {
    uint32_t bucket = hash_bucket(node->pid, node->required_tags);
    node->hash_next = g_listen_table.hash_index[bucket];
    g_listen_table.hash_index[bucket] = node;
}

static void hash_remove(ListenNode* node) {
    uint32_t bucket = hash_bucket(node->pid, node->required_tags);
    ListenNode** pp = &g_listen_table.hash_index[bucket];
    while (*pp) {
        if (*pp == node) {
            *pp = node->hash_next;
            node->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

error_t listen_table_add(uint32_t pid, uint64_t required_tags, uint8_t flags) {
    spin_lock(&g_listen_table.lock);

    if (g_listen_table.count >= LISTEN_TABLE_MAX_ENTRIES) {
        spin_unlock(&g_listen_table.lock);
        return ERR_LISTEN_TABLE_FULL;
    }

    if (hash_find(pid, required_tags) != NULL) {
        spin_unlock(&g_listen_table.lock);
        return ERR_LISTEN_ALREADY;
    }

    ListenNode* node = slab_alloc(sizeof(ListenNode));
    if (!node) {
        spin_unlock(&g_listen_table.lock);
        return ERR_NO_MEMORY;
    }

    node->pid           = pid;
    node->required_tags = required_tags;
    node->flags         = flags;
    node->hash_next     = NULL;
    node->next          = g_listen_table.head;
    g_listen_table.head = node;
    g_listen_table.count++;
    hash_insert(node);

    spin_unlock(&g_listen_table.lock);

    debug_printf("[LISTEN] PID %u registered for tags=0x%llx flags=0x%02x\n",
                 pid, required_tags, flags);
    return OK;
}

error_t listen_table_remove(uint32_t pid, uint64_t required_tags) {
    spin_lock(&g_listen_table.lock);

    ListenNode **pp = &g_listen_table.head;
    while (*pp) {
        ListenNode *n = *pp;
        if (n->pid == pid && n->required_tags == required_tags) {
            *pp = n->next;
            g_listen_table.count--;
            hash_remove(n);
            spin_unlock(&g_listen_table.lock);
            slab_free(n);
            return OK;
        }
        pp = &n->next;
    }

    spin_unlock(&g_listen_table.lock);
    return ERR_PROCESS_NOT_FOUND;
}

uint32_t listen_table_find(uint64_t event_tags, uint32_t *out_pids, uint32_t max_pids) {
    spin_lock(&g_listen_table.lock);

    uint32_t found = 0;
    for (ListenNode *n = g_listen_table.head; n && found < max_pids; n = n->next) {
        if ((n->required_tags & event_tags) == n->required_tags) {
            out_pids[found++] = n->pid;
        }
    }

    spin_unlock(&g_listen_table.lock);
    return found;
}

void listen_table_remove_pid(uint32_t pid) {
    spin_lock(&g_listen_table.lock);

    ListenNode **pp = &g_listen_table.head;
    while (*pp) {
        ListenNode *n = *pp;
        if (n->pid == pid) {
            *pp = n->next;
            g_listen_table.count--;
            hash_remove(n);
            slab_free(n);
        } else {
            pp = &n->next;
        }
    }

    spin_unlock(&g_listen_table.lock);
}
