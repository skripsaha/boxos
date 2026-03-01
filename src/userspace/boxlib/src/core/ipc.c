#include "box/ipc.h"
#include "box/chain.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/system.h"

int send(uint32_t target_pid, const void* data, uint16_t size) {
    if (target_pid == 0) return -BOX_ERR_INVALID_ARGUMENT;

    route(target_pid);

    if (data && size > 0) {
        notify_data(data, size);
    }

    notify();

    result_entry_t result;
    if (!result_wait(&result, 500000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return BOX_OK;
}

int broadcast(const char* tag, const void* data, uint16_t size) {
    if (!tag || tag[0] == '\0') return -BOX_ERR_INVALID_ARGUMENT;

    route_tag(tag);

    if (data && size > 0) {
        notify_data(data, size);
    }

    notify();

    result_entry_t result;
    if (!result_wait(&result, 500000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return BOX_OK;
}

int listen(uint8_t source_type, uint8_t flags) {
    hw_listen(source_type, flags);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 500000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return BOX_OK;
}

bool receive(result_entry_t* out_entry) {
    if (!out_entry) return false;
    return result_pop_ipc(out_entry);
}

// Wall-clock timeout via rdtsc (not iteration counting).
// Iteration counting breaks with multiple yielding processes: voluntary yields
// bypass the timer, so N processes cycling through yields burn iterations in
// microseconds instead of the assumed 10ms each — making timeouts 100-1000x
// shorter than intended.
static inline uint64_t ipc_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#define IPC_TSC_FREQ_MHZ 1000

bool receive_wait(result_entry_t* out_entry, uint32_t timeout_ms) {
    if (!out_entry) return false;

    if (result_pop_ipc(out_entry)) return true;

    result_page_t* rp = result_page();
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = ipc_rdtsc() + (uint64_t)timeout_ms * IPC_TSC_FREQ_MHZ * 1000;
    }

    while (1) {
        __sync_synchronize();

        if (rp->notification_flag != 0 || result_available() || result_ipc_stash_count() > 0) {
            if (result_pop_ipc(out_entry)) {
                rp->notification_flag = 0;
                return true;
            }
            rp->notification_flag = 0;
        }

        if (timeout_ms > 0 && ipc_rdtsc() >= deadline) {
            return false;
        }

        yield();
    }
}
