#include "listen_table.h"
#include "klib.h"
#include "error.h"

listen_table_t g_listen_table;

void listen_table_init(void) {
    memset(&g_listen_table, 0, sizeof(listen_table_t));
    spinlock_init(&g_listen_table.lock);
    debug_printf("[LISTEN] Listen table initialized (max %d entries)\n", MAX_LISTENERS);
}

int listen_table_add(uint32_t pid, uint8_t source_type, uint8_t flags) {
    spin_lock(&g_listen_table.lock);

    for (uint32_t i = 0; i < g_listen_table.count; i++) {
        if (g_listen_table.entries[i].pid == pid &&
            g_listen_table.entries[i].source_type == source_type) {
            spin_unlock(&g_listen_table.lock);
            return -ERR_LISTEN_ALREADY;
        }
    }

    if (g_listen_table.count >= MAX_LISTENERS) {
        spin_unlock(&g_listen_table.lock);
        return -ERR_LISTEN_TABLE_FULL;
    }

    listen_entry_t* entry = &g_listen_table.entries[g_listen_table.count];
    entry->pid = pid;
    entry->source_type = source_type;
    entry->flags = flags;
    g_listen_table.count++;

    spin_unlock(&g_listen_table.lock);

    debug_printf("[LISTEN] PID %u registered for source_type=%u flags=0x%02x\n",
                 pid, source_type, flags);
    return 0;
}

int listen_table_remove(uint32_t pid, uint8_t source_type) {
    spin_lock(&g_listen_table.lock);

    for (uint32_t i = 0; i < g_listen_table.count; i++) {
        if (g_listen_table.entries[i].pid == pid &&
            g_listen_table.entries[i].source_type == source_type) {
            if (i < g_listen_table.count - 1) {
                g_listen_table.entries[i] = g_listen_table.entries[g_listen_table.count - 1];
            }
            g_listen_table.count--;
            spin_unlock(&g_listen_table.lock);
            return 0;
        }
    }

    spin_unlock(&g_listen_table.lock);
    return -1;
}

uint32_t listen_table_find(uint8_t source_type, uint32_t* out_pids, uint32_t max_pids) {
    spin_lock(&g_listen_table.lock);

    uint32_t found = 0;
    for (uint32_t i = 0; i < g_listen_table.count && found < max_pids; i++) {
        if (g_listen_table.entries[i].source_type == source_type) {
            out_pids[found++] = g_listen_table.entries[i].pid;
        }
    }

    spin_unlock(&g_listen_table.lock);
    return found;
}

void listen_table_remove_pid(uint32_t pid) {
    spin_lock(&g_listen_table.lock);

    uint32_t i = 0;
    while (i < g_listen_table.count) {
        if (g_listen_table.entries[i].pid == pid) {
            if (i < g_listen_table.count - 1) {
                g_listen_table.entries[i] = g_listen_table.entries[g_listen_table.count - 1];
            }
            g_listen_table.count--;
        } else {
            i++;
        }
    }

    spin_unlock(&g_listen_table.lock);
}
