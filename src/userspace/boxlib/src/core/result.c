#include "box/result.h"
#include "box/notify.h"
#include "box/system.h"

bool result_available(void) {
    ResultRing* rr = result_ring();
    __sync_synchronize();
    return rr->hdr.head != rr->hdr.tail;
}

uint32_t result_count(void) {
    ResultRing* rr = result_ring();
    __sync_synchronize();
    uint64_t n = rr->hdr.tail - rr->hdr.head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

bool result_pop(Result* out) {
    ResultRing* rr = result_ring();
    if (!result_available()) {
        return false;
    }
    __sync_synchronize();

    uint64_t idx = rr->hdr.head;
    Result *slot = (Result *)(uintptr_t)
        (rr->hdr.slots_base + (idx % rr->hdr.slot_count_max) * rr->hdr.slot_size);
    *out = *slot;

    __sync_synchronize();
    rr->hdr.head = idx + 1;
    return true;
}

#define IPC_STASH_SIZE 64
static Result ipc_stash_buf[IPC_STASH_SIZE];
static uint32_t ipc_stash_cnt = 0;

static void ipc_stash_push(Result* entry) {
    if (ipc_stash_cnt >= IPC_STASH_SIZE) {
        /* Stash full: drop oldest to make room */
        for (uint32_t i = 1; i < ipc_stash_cnt; i++) {
            ipc_stash_buf[i - 1] = ipc_stash_buf[i];
        }
        ipc_stash_cnt--;
    }
    ipc_stash_buf[ipc_stash_cnt++] = *entry;
}

static bool ipc_stash_shift(Result* out) {
    if (ipc_stash_cnt == 0) return false;
    *out = ipc_stash_buf[0];
    for (uint32_t i = 1; i < ipc_stash_cnt; i++) {
        ipc_stash_buf[i - 1] = ipc_stash_buf[i];
    }
    ipc_stash_cnt--;
    return true;
}

#define NON_IPC_STASH_SIZE 64
static Result non_ipc_stash_buf[NON_IPC_STASH_SIZE];
static uint32_t non_ipc_stash_cnt = 0;

static void non_ipc_stash_push(Result* entry) {
    if (non_ipc_stash_cnt >= NON_IPC_STASH_SIZE) {
        for (uint32_t i = 1; i < non_ipc_stash_cnt; i++) {
            non_ipc_stash_buf[i - 1] = non_ipc_stash_buf[i];
        }
        non_ipc_stash_cnt--;
    }
    non_ipc_stash_buf[non_ipc_stash_cnt++] = *entry;
}

static bool non_ipc_stash_shift(Result* out) {
    if (non_ipc_stash_cnt == 0) return false;
    *out = non_ipc_stash_buf[0];
    for (uint32_t i = 1; i < non_ipc_stash_cnt; i++) {
        non_ipc_stash_buf[i - 1] = non_ipc_stash_buf[i];
    }
    non_ipc_stash_cnt--;
    return true;
}

bool result_pop_non_ipc(Result* out) {
    if (non_ipc_stash_shift(out)) return true;

    Result entry;
    while (result_pop(&entry)) {
        if (entry.sender_pid != 0) {
            ipc_stash_push(&entry);
            continue;
        }
        *out = entry;
        return true;
    }
    return false;
}

bool result_pop_ipc(Result* out) {
    if (ipc_stash_shift(out)) return true;

    Result entry;
    while (result_pop(&entry)) {
        if (entry.sender_pid != 0) {
            *out = entry;
            return true;
        }
        non_ipc_stash_push(&entry);
    }
    return false;
}

uint32_t result_ipc_stash_count(void) {
    return ipc_stash_cnt;
}
