#include "box/result.h"
#include "box/cpu.h"
#include "box/system.h"
#include "../arch/x86_64/cpu_wait.h"

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
            if (result_pop_non_ipc(out)) {
                return true;
            }
        }

        umonitor((volatile void*)tail_addr);
        __sync_synchronize();

        if (!result_available()) {
            uint64_t deadline_tsc;
            if (timeout_ms == 0) {
                deadline_tsc = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                uint64_t tsc_now = rdtsc();
                uint64_t tsc_delta = cpu_ms_to_tsc(timeout_ms);
                deadline_tsc = tsc_now + tsc_delta;
            }

            int wake_reason = umwait(0, deadline_tsc);

            if (wake_reason == 1 && timeout_ms > 0) {
                __sync_synchronize();
                if (!result_available()) {
                    return false;
                }
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
            if (result_pop_non_ipc(out)) {
                return true;
            }
        }

        if (timeout_ms > 0 && rdtsc() >= deadline) {
            return false;
        }

        // Use pause instead of yield() to avoid re-entering the kernel.
        // On multi-core the K-Core writes results directly to ResultRing;
        // the process just spins until the result appears. pause reduces
        // power and avoids pipeline stalls from tight spin loops.
        __asm__ volatile("pause");
    }
}

bool result_wait(Result* out, uint32_t timeout_ms) {
    if (result_pop_non_ipc(out)) {
        return true;
    }

    if (cpu_has_waitpkg()) {
        return result_wait_umwait(out, timeout_ms);
    } else {
        return result_wait_yield(out, timeout_ms);
    }
}
