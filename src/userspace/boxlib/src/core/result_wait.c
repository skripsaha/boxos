// result_wait — unified wait strategy with automatic hardware detection
// Uses UMONITOR/UMWAIT on CPUs with WAITPKG, falls back to cooperative yield

#include "box/result.h"
#include "box/cpu.h"
#include "box/system.h"
#include "../arch/x86_64/cpu_wait.h"

#define TSC_FREQ_MHZ  1000

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool result_wait_umwait(result_entry_t* out_entry, uint32_t timeout_ms) {
    result_page_t* rp = result_page();
    volatile uint8_t* flag_addr = &rp->notification_flag;

    while (1) {
        __sync_synchronize();

        if (*flag_addr != 0 || result_available()) {
            if (result_pop_non_ipc(out_entry)) {
                *flag_addr = 0;
                return true;
            }
            *flag_addr = 0;
        }

        umonitor((volatile void*)flag_addr);
        __sync_synchronize();

        if (*flag_addr == 0 && !result_available()) {
            uint64_t deadline_tsc;
            if (timeout_ms == 0) {
                deadline_tsc = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                uint64_t tsc_now = rdtsc();
                uint64_t tsc_delta = (uint64_t)timeout_ms * TSC_FREQ_MHZ * 1000;
                deadline_tsc = tsc_now + tsc_delta;
            }

            int wake_reason = umwait(0, deadline_tsc);

            if (wake_reason == 1 && timeout_ms > 0) {
                __sync_synchronize();
                if (*flag_addr == 0 && !result_available()) {
                    return false;
                }
            }
        }
    }
}

static bool result_wait_yield(result_entry_t* out_entry, uint32_t timeout_ms) {
    result_page_t* rp = result_page();

    // Use rdtsc for wall-clock timeout instead of iteration counting.
    // With multiple yielding processes, voluntary yields cycle in microseconds
    // (not the assumed 10ms), making iteration-based timeouts 100-1000x too short.
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = rdtsc() + (uint64_t)timeout_ms * TSC_FREQ_MHZ * 1000;
    }

    while (1) {
        __sync_synchronize();

        if (rp->notification_flag != 0 || result_available()) {
            if (result_pop_non_ipc(out_entry)) {
                rp->notification_flag = 0;
                return true;
            }
            rp->notification_flag = 0;
        }

        if (timeout_ms > 0 && rdtsc() >= deadline) {
            return false;
        }

        yield();
    }
}

bool result_wait(result_entry_t* out_entry, uint32_t timeout_ms) {
    if (result_pop_non_ipc(out_entry)) {
        return true;
    }

    if (cpu_has_waitpkg()) {
        return result_wait_umwait(out_entry, timeout_ms);
    } else {
        return result_wait_yield(out_entry, timeout_ms);
    }
}
