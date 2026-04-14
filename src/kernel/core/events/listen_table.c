#include "listen_table.h"
#include "slab.h"
#include "klib.h"
#include "error.h"

listen_table_t g_listen_table;

void listen_table_init(void) {
    g_listen_table.head  = NULL;
    g_listen_table.count = 0;
    spinlock_init(&g_listen_table.lock);
    debug_printf("[LISTEN] Listen table initialized\n");
}

int listen_table_add(uint32_t pid, uint64_t required_tags, uint8_t flags) {
    spin_lock(&g_listen_table.lock);

    for (ListenNode *n = g_listen_table.head; n; n = n->next) {
        if (n->pid == pid && n->required_tags == required_tags) {
            spin_unlock(&g_listen_table.lock);
            return -ERR_LISTEN_ALREADY;
        }
    }

    ListenNode *node = slab_alloc(sizeof(ListenNode));
    if (!node) {
        spin_unlock(&g_listen_table.lock);
        return -ERR_LISTEN_TABLE_FULL;
    }
    node->pid           = pid;
    node->required_tags = required_tags;
    node->flags         = flags;
    node->next          = g_listen_table.head;
    g_listen_table.head = node;
    g_listen_table.count++;

    spin_unlock(&g_listen_table.lock);

    debug_printf("[LISTEN] PID %u registered for tags=0x%llx flags=0x%02x\n",
                 pid, required_tags, flags);
    return 0;
}

int listen_table_remove(uint32_t pid, uint64_t required_tags) {
    spin_lock(&g_listen_table.lock);

    ListenNode **pp = &g_listen_table.head;
    while (*pp) {
        ListenNode *n = *pp;
        if (n->pid == pid && n->required_tags == required_tags) {
            *pp = n->next;
            g_listen_table.count--;
            spin_unlock(&g_listen_table.lock);
            slab_free(n);
            return 0;
        }
        pp = &n->next;
    }

    spin_unlock(&g_listen_table.lock);
    return -1;
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
            slab_free(n);
        } else {
            pp = &n->next;
        }
    }

    spin_unlock(&g_listen_table.lock);
}
