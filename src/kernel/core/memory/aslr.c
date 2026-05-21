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

/* Uniformly sample a page-aligned offset in [0, max_bytes).
 *
 * The naive `rand % max_pages` form is biased whenever max_pages is
 * not a power of two: the low end of the range receives floor(2^64 /
 * max_pages) + 1 hits while the high end receives floor(2^64 /
 * max_pages), so an attacker with many spawn samples can statistically
 * narrow the layout to the low half of the range.
 *
 * Rejection sampling: compute the largest multiple of max_pages that
 * fits in 64 bits (`limit`), and resample any RNG read above it. The
 * fraction discarded is at most 1/max_pages — for the smallest
 * realistic ASLR range (~64 KiB = 16 pages) that's 6 %, for the
 * 32 GiB heap range (~2^23 pages) it's effectively zero. */
uint64_t aslr_random_offset(uint64_t max_bytes) {
    if (max_bytes < ASLR_PAGE_SIZE) return 0;

    uint64_t max_pages = max_bytes / ASLR_PAGE_SIZE;
    if (max_pages == 0) return 0;

    /* limit = largest multiple of max_pages ≤ UINT64_MAX. Using
     * (UINT64_MAX / max_pages) * max_pages avoids overflow. When
     * max_pages divides 2^64 (power-of-two ranges) the loop body
     * runs at most once. */
    uint64_t limit = (UINT64_MAX / max_pages) * max_pages;

    uint64_t rand_val;
    /* Bounded retry: we expect 0–1 rejections per call. 64 is more
     * than enough headroom even at the worst-case discard rate. */
    for (int tries = 0; tries < 64; tries++) {
        rand_val = aslr_random64();
        if (rand_val < limit) break;
    }
    /* On the (vanishingly improbable) all-rejections path, fall
     * through with the last value — biased but at worst uniform
     * over [0, UINT64_MAX % max_pages), still a valid offset. */

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
