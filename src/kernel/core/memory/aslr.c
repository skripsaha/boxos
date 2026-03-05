#include "aslr.h"
#include "klib.h"
#include "cpuid.h"
#include "atomics.h"

static bool has_rdrand = false;
static volatile uint64_t aslr_counter = 0;

static inline bool cpu_has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx >> 30) & 1;  // CPUID.01H:ECX.RDRAND[bit 30]
}

static inline bool rdrand64(uint64_t* val) {
    uint8_t ok;
    __asm__ __volatile__(
        "rdrand %0\n\t"
        "setc %1"
        : "=r"(*val), "=qm"(ok)
    );
    return ok;
}

// TSC-based fallback PRNG (xorshift64)
static uint64_t prng_fallback(void) {
    uint64_t s = rdtsc() ^ atomic_fetch_add_u64(&aslr_counter, 1);
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

static uint64_t aslr_random64(void) {
    if (has_rdrand) {
        uint64_t val;
        // RDRAND can transiently fail; retry a few times
        for (int i = 0; i < 10; i++) {
            if (rdrand64(&val)) return val;
        }
        // hardware failure — fall through to PRNG
    }
    return prng_fallback();
}

void aslr_init(void) {
    has_rdrand = cpu_has_rdrand();
    aslr_counter = rdtsc();

    debug_printf("[ASLR] Initialized: RDRAND %s\n",
                 has_rdrand ? "available (hardware entropy)" : "unavailable (TSC fallback)");
}

uint64_t aslr_random_offset(uint64_t max_bytes) {
    if (max_bytes < ASLR_PAGE_SIZE) return 0;

    uint64_t max_pages = max_bytes / ASLR_PAGE_SIZE;
    uint64_t rand_val = aslr_random64();
    uint64_t page_offset = rand_val % max_pages;

    return page_offset * ASLR_PAGE_SIZE;
}

aslr_offsets_t aslr_generate(void) {
    aslr_offsets_t offsets;
    offsets.stack_offset    = aslr_random_offset(ASLR_STACK_RANGE);
    offsets.heap_offset     = aslr_random_offset(ASLR_HEAP_RANGE);
    offsets.buf_heap_offset = aslr_random_offset(ASLR_BUF_RANGE);
    return offsets;
}
