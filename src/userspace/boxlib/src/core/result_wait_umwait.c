#include "box/result.h"
#include "../arch/x86_64/cpu_wait.h"

#define TSC_FREQ_MHZ  1000

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

bool result_wait_umwait(result_entry_t* out_entry, uint32_t timeout_ms) {
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
