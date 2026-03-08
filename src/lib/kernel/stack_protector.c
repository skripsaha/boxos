#include "klib.h"
#include "process.h"
#include "cpuid.h"
#include "atomics.h"

// Initial value used before randomization (during early boot before CPUID).
// stack_canary_init() replaces this with RDRAND/TSC entropy.
uintptr_t __stack_chk_guard = 0x00000AFF0DEADC0DEULL;

static inline bool _rdrand64(uint64_t* val) {
    uint8_t ok;
    __asm__ __volatile__(
        "rdrand %0\n\t"
        "setc %1"
        : "=r"(*val), "=qm"(ok)
    );
    return ok;
}

__attribute__((optimize("no-stack-protector")))
void stack_canary_init(void) {
    uint64_t canary = 0;

    // Try RDRAND first (hardware entropy)
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    bool has_rdrand = (ecx >> 30) & 1;

    if (has_rdrand) {
        for (int i = 0; i < 10; i++) {
            if (_rdrand64(&canary) && canary != 0) {
                goto done;
            }
        }
    }

    // Fallback: TSC-based entropy (xorshift64)
    {
        uint64_t s = rdtsc();
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        // Mix in more TSC samples for better entropy
        s ^= rdtsc() << 31;
        s ^= rdtsc() >> 11;
        canary = s;
    }

done:
    // Ensure low byte is 0x00 (null terminator) — prevents string-based overflows
    // from guessing the canary via string reads
    canary = (canary & ~0xFFULL);
    if (canary == 0) canary = 0xDEADC0DE00ULL; // never use zero

    __stack_chk_guard = canary;
}

#pragma GCC push_options
#pragma GCC optimize("no-stack-protector")

void __attribute__((noreturn)) __stack_chk_fail(void) {
    process_t* proc = process_get_current();

    if (proc && proc->pid != 0) {
        kprintf("\n[STACK PROTECTOR] Stack smashing detected in PID %u (%s)\n",
                proc->pid, proc->tags);
        process_set_state(proc, PROC_CRASHED);
        // Timer IRQ will call schedule() and pick a different process
    }

    kprintf("\n");
    kprintf("====================================================================\n");
    kprintf("KERNEL PANIC: Stack smashing detected\n");
    kprintf("====================================================================\n");

    while (1) {
        asm volatile("cli; hlt");
    }
}

#pragma GCC pop_options
