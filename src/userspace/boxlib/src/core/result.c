#include "box/result.h"
#include "box/notify.h"
#include "box/system.h"

bool result_available(void) {
    result_page_t* rp = result_page();
    // memory barrier: kernel updates tail with barriers; without this, userspace may see stale tail
    __sync_synchronize();
    return rp->ring.head != rp->ring.tail;
}

uint32_t result_count(void) {
    result_page_t* rp = result_page();
    __sync_synchronize();
    uint32_t head = rp->ring.head;
    uint32_t tail = rp->ring.tail;

    if (tail >= head) {
        return tail - head;
    } else {
        return (RESULT_RING_SIZE - head) + tail;
    }
}

bool result_pop(result_entry_t* out_entry) {
    result_page_t* rp = result_page();

    if (!result_available()) {
        return false;
    }

    __sync_synchronize();

    uint32_t head = rp->ring.head;
    *out_entry = rp->ring.entries[head];

    __sync_synchronize();

    rp->ring.head = (head + 1) % RESULT_RING_SIZE;

    return true;
}


bool result_page_full(void) {
    notify_page_t* np = notify_page();
    return np->result_page_full != 0;
}

bool event_ring_full(void) {
    notify_page_t* np = notify_page();
    return np->event_ring_full != 0;
}

uint32_t get_overflow_count(bool reset) {
    notify_page_t* np = notify_page();

    np->magic = NOTIFY_MAGIC;
    np->prefix_count = 1;
    np->flags = 0;
    np->status = 0;
    np->prefixes[0] = (0xFF << 8) | 0xE0;
    np->data[0] = reset ? 1 : 0;

    __asm__ volatile("int $0x80");

    result_entry_t result;
    if (!result_wait(&result, 100000)) {
        return 0xFFFFFFFF;
    }

    uint32_t count = *((uint32_t*)result.payload);
    return count;
}

#define IPC_STASH_SIZE 16
static result_entry_t ipc_stash_buf[IPC_STASH_SIZE];
static uint32_t ipc_stash_cnt = 0;

static void ipc_stash_push(result_entry_t* entry) {
    if (ipc_stash_cnt < IPC_STASH_SIZE) {
        ipc_stash_buf[ipc_stash_cnt++] = *entry;
    }
}

static bool ipc_stash_shift(result_entry_t* out) {
    if (ipc_stash_cnt == 0) return false;
    *out = ipc_stash_buf[0];
    for (uint32_t i = 1; i < ipc_stash_cnt; i++) {
        ipc_stash_buf[i - 1] = ipc_stash_buf[i];
    }
    ipc_stash_cnt--;
    return true;
}

bool result_pop_non_ipc(result_entry_t* out_entry) {
    result_entry_t entry;
    while (result_pop(&entry)) {
        if (entry.source == 0x01) {  // ROUTE_SOURCE_PROCESS
            ipc_stash_push(&entry);
            continue;
        }
        *out_entry = entry;
        return true;
    }
    return false;
}

bool result_pop_ipc(result_entry_t* out_entry) {
    if (ipc_stash_shift(out_entry)) return true;

    result_entry_t entry;
    while (result_pop(&entry)) {
        if (entry.source == 0x01) {
            *out_entry = entry;
            return true;
        }
    }
    return false;
}

uint32_t result_ipc_stash_count(void) {
    return ipc_stash_cnt;
}
