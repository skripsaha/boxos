#include "hvclock.h"
#include "hypervisor.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "atomics.h"


#define MSR_HV_REFERENCE_TSC   0x40000021u
#define MSR_HV_TIME_REF_COUNT  0x40000020u

#define HVCLOCK_POPULATE_SPIN_MAX  1000u

#define HVCLOCK_SEQLOCK_RETRY_MAX  1024

typedef struct __attribute__((packed, aligned(4096))) {
    volatile uint32_t TscSequence;
    uint32_t          Reserved1;
    volatile uint64_t TscScale;
    volatile int64_t  TscOffset;
    uint64_t          Reserved2[509];
} hv_reference_tsc_page_t;

_Static_assert(sizeof(hv_reference_tsc_page_t) == 4096,
               "hv_reference_tsc_page_t must be exactly one page");

static uint64_t                  s_page_phys = 0;
static hv_reference_tsc_page_t  *s_page      = NULL;
static volatile bool             s_available = false;

static inline uint64_t rdmsr_hv(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr_hv(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"((uint32_t)v),
                     "d"((uint32_t)(v >> 32)));
}

bool hvclock_init(void)
{
    if (s_available) return true;

    if (!hv_has_hyperv_tsc_page()) {
        debug_printf("[HVCLOCK] Hyper-V reference TSC page not available\n");
        return false;
    }

    void *page = pmm_alloc_zero(1);
    if (!page) {
        debug_printf("[HVCLOCK] pmm_alloc failed\n");
        return false;
    }
    s_page_phys = (uint64_t)page;
    s_page      = (hv_reference_tsc_page_t *)vmm_phys_to_virt(s_page_phys);
    vmm_register_shared_phys(s_page_phys);

    uint64_t enable = (s_page_phys & ~0xFFFULL) | 1ULL;
    wrmsr_hv(MSR_HV_REFERENCE_TSC, enable);

    for (unsigned i = 0; i < HVCLOCK_POPULATE_SPIN_MAX && s_page->TscSequence == 0; i++)
        __asm__ volatile("pause");

    if (s_page->TscSequence == 0) {
        debug_printf("[HVCLOCK] Hyper-V left page TscSequence=0 — falling back to MSR\n");
    }

    s_available = true;
    debug_printf("[HVCLOCK] reference TSC page active (seq=%u scale=0x%lx offset=%ld)\n",
                 s_page->TscSequence,
                 (unsigned long)s_page->TscScale,
                 (long)s_page->TscOffset);
    return true;
}

bool hvclock_is_available(void)
{
    return s_available;
}

static inline uint64_t mulhi_u64(uint64_t a, uint64_t b)
{
    uint64_t lo, hi;
    __asm__ volatile("mulq %3"
                     : "=a"(lo), "=d"(hi)
                     : "0"(a), "rm"(b));
    (void)lo;
    return hi;
}

uint64_t hvclock_now_ns(void)
{
    if (!s_available) return 0;

    uint64_t scale, offset, tsc;
    uint32_t seq0;
    for (int attempts = 0; attempts < HVCLOCK_SEQLOCK_RETRY_MAX; attempts++) {
        seq0 = __atomic_load_n(&s_page->TscSequence, __ATOMIC_ACQUIRE);
        if (seq0 == 0) {
            uint64_t hund = rdmsr_hv(MSR_HV_TIME_REF_COUNT);
            return hund * 100ULL;
        }

        tsc    = rdtsc();
        scale  = s_page->TscScale;
        offset = (uint64_t)s_page->TscOffset;

        __asm__ volatile("" ::: "memory");
        uint32_t seq1 = __atomic_load_n(&s_page->TscSequence, __ATOMIC_ACQUIRE);
        if (seq1 == seq0) {
            uint64_t hund = mulhi_u64(tsc, scale) + offset;
            return hund * 100ULL;
        }
    }
    uint64_t hund = rdmsr_hv(MSR_HV_TIME_REF_COUNT);
    return hund * 100ULL;
}