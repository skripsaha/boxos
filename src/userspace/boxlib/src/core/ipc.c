#include "box/ipc.h"
#include "box/chain.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/system.h"
#include "box/string.h"
#include "box/file.h"
#include "box/cpu.h"

int send(uint32_t target_pid, const void* data, uint16_t size) {
    if (target_pid == 0) return -ERR_INVALID_ARGUMENT;

    route(target_pid);

    if (data && size > 0) {
        notify_data(data, size);
    }

    notify();

    result_entry_t result;
    if (!result_wait(&result, 500000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return OK;
}

int broadcast(const char* tag, const void* data, uint16_t size) {
    if (!tag || tag[0] == '\0') return -ERR_INVALID_ARGUMENT;

    route_tag(tag);

    if (data && size > 0) {
        notify_data(data, size);
    }

    notify();

    result_entry_t result;
    if (!result_wait(&result, 500000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return OK;
}

int listen(uint8_t source_type, uint8_t flags) {
    hw_listen(source_type, flags);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 500000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return OK;
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

bool receive_wait(result_entry_t* out_entry, uint32_t timeout_ms) {
    if (!out_entry) return false;

    if (result_pop_ipc(out_entry)) return true;

    result_page_t* rp = result_page();
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = ipc_rdtsc() + cpu_ms_to_tsc(timeout_ms);
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

int send_args(uint32_t target_pid, int argc, char** argv) {
    char buf[240];
    int pos = 0;
    buf[pos++] = (char)argc;
    for (int i = 0; i < argc && pos < 235; i++) {
        size_t len = strlen(argv[i]);
        if (pos + len + 1 >= 240) break;
        memcpy(buf + pos, argv[i], len);
        pos += len;
        buf[pos++] = '\0';
    }
    return send(target_pid, buf, (uint16_t)pos);
}

int receive_args(int* argc, char argv[][64], int max_args) {
    result_entry_t entry;
    if (!receive_wait(&entry, 1000)) return -1;
    const char* buf = (const char*)entry.payload;
    uint16_t total = entry.size;
    *argc = (uint8_t)buf[0];
    int pos = 1;
    for (int i = 0; i < *argc && i < max_args; i++) {
        size_t len = strlen(buf + pos);
        if (len >= 64) len = 63;
        memcpy(argv[i], buf + pos, len + 1);
        pos += len + 1;
    }

    // Apply context tags if present after args
    // Format: [ctx_count:1byte] [ctx_tag0\0] [ctx_tag1\0] ...
    if (pos < total) {
        uint8_t ctx_count = (uint8_t)buf[pos++];
        for (uint8_t i = 0; i < ctx_count && pos < total; i++) {
            context_set(buf + pos);
            pos += strlen(buf + pos) + 1;
        }
    }

    return 0;
}
