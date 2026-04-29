#include "box/result.h"
#include "box/notify.h"
#include "box/system.h"
#include "box/cpu.h"
#include "../arch/x86_64/cpu_wait.h"

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

/* ===========================================================================
 * Blocking waits — was result_wait.c (merged Stage 2).
 * UMWAIT-based fast path on CPUs with WAITPKG; pause-spin fallback otherwise.
 * =========================================================================== */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool result_wait_umwait(Result* out, uint32_t timeout_ms) {
    ResultRing* rr = result_ring();
    volatile uint32_t* tail_addr;
    { char* base = (char*)rr; tail_addr = (volatile uint32_t*)(base + 4); }

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            if (result_pop_non_ipc(out)) return true;
        }

        umonitor((volatile void*)tail_addr);
        __sync_synchronize();

        if (!result_available()) {
            uint64_t deadline_tsc;
            if (timeout_ms == 0) {
                deadline_tsc = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                uint64_t tsc_now   = rdtsc();
                uint64_t tsc_delta = cpu_ms_to_tsc(timeout_ms);
                deadline_tsc = tsc_now + tsc_delta;
            }

            int wake_reason = umwait(0, deadline_tsc);

            if (wake_reason == 1 && timeout_ms > 0) {
                __sync_synchronize();
                if (!result_available()) return false;
            }
        }
    }
}

static bool result_wait_yield(Result* out, uint32_t timeout_ms) {
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            if (result_pop_non_ipc(out)) return true;
        }

        if (timeout_ms > 0 && rdtsc() >= deadline) return false;

        /* Pause keeps multi-core friendly: K-Core writes Result directly;
         * we just spin until it appears. No kernel re-entry needed. */
        __asm__ volatile("pause");
    }
}

bool result_wait(Result* out, uint32_t timeout_ms) {
    if (result_pop_non_ipc(out)) return true;

    if (cpu_has_waitpkg()) return result_wait_umwait(out, timeout_ms);
    return result_wait_yield(out, timeout_ms);
}

/* Block until ANY result arrives — no IPC/non-IPC filtering.
 * For IPC servers (display daemon) that receive both kernel results
 * (from their own VGA/keyboard calls) and IPC messages.
 * Drains both stashes first, then pops from the ring. */
bool result_wait_any(Result* out, uint32_t timeout_ms) {
    if (result_pop_ipc(out))     return true;
    if (result_pop_non_ipc(out)) return true;
    if (result_pop(out))         return true;

    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    while (1) {
        __sync_synchronize();

        if (result_available()) {
            if (result_pop(out)) return true;
        }

        if (timeout_ms > 0 && rdtsc() >= deadline) return false;

        __asm__ volatile("pause");
    }
}
