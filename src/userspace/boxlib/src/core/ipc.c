#include "box/ipc.h"
#include "box/chain.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/system.h"
#include "box/string.h"
#include "box/file.h"
#include "box/cpu.h"

// Scratch buffer for IPC data (must be in mapped memory)
static uint8_t g_ipc_buf[256] __attribute__((aligned(16)));

int send(uint32_t target_pid, const void* data, uint16_t size) {
    if (target_pid == 0) return -ERR_INVALID_ARGUMENT;

    Pocket p;
    pocket_prepare(&p);
    p.target_pid = target_pid;
    p.route_tag[0] = '\0';
    pocket_add_prefix(&p, DECK_SYSTEM, 0x40);

    if (data && size > 0) {
        uint16_t copy = size > sizeof(g_ipc_buf) ? sizeof(g_ipc_buf) : size;
        memcpy(g_ipc_buf, data, copy);
        pocket_set_data(&p, g_ipc_buf, copy);
    }

    pocket_submit(&p);

    Result result;
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

    Pocket p;
    pocket_prepare(&p);
    p.target_pid = 0;
    size_t tlen = strlen(tag);
    if (tlen > 31) tlen = 31;
    memcpy(p.route_tag, tag, tlen);
    p.route_tag[tlen] = '\0';
    pocket_add_prefix(&p, DECK_SYSTEM, 0x41);

    if (data && size > 0) {
        uint16_t copy = size > sizeof(g_ipc_buf) ? sizeof(g_ipc_buf) : size;
        memcpy(g_ipc_buf, data, copy);
        pocket_set_data(&p, g_ipc_buf, copy);
    }

    pocket_submit(&p);

    Result result;
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

    Result result;
    if (!result_wait(&result, 500000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    return OK;
}

bool receive(Result* out) {
    if (!out) return false;
    return result_pop_ipc(out);
}

// Wall-clock timeout via rdtsc
static inline uint64_t ipc_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

bool receive_wait(Result* out, uint32_t timeout_ms) {
    if (!out) return false;

    if (result_pop_ipc(out)) return true;

    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = ipc_rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    while (1) {
        __sync_synchronize();

        if (result_available() || result_ipc_stash_count() > 0) {
            if (result_pop_ipc(out)) {
                return true;
            }
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
    Result entry;
    if (!receive_wait(&entry, 1000)) return -1;

    if (entry.data_addr == 0 || entry.data_length == 0) return -1;
    const char* buf = (const char*)(uintptr_t)entry.data_addr;
    uint32_t total = entry.data_length;

    *argc = (uint8_t)buf[0];
    int pos = 1;
    for (int i = 0; i < *argc && i < max_args; i++) {
        size_t len = strlen(buf + pos);
        if (len >= 64) len = 63;
        memcpy(argv[i], buf + pos, len + 1);
        pos += len + 1;
    }

    // Apply context tags if present after args
    if ((uint32_t)pos < total) {
        uint8_t ctx_count = (uint8_t)buf[pos++];
        for (uint8_t i = 0; i < ctx_count && (uint32_t)pos < total; i++) {
            context_set(buf + pos);
            pos += strlen(buf + pos) + 1;
        }
    }

    return 0;
}
