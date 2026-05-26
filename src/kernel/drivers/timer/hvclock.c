#include "hvclock.h"
#include "hypervisor.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "atomics.h"

/*
 * Hyper-V reference TSC page driver.
 *
 * Spec: Microsoft TLFS §10.7.2, "Partition Reference TSC Page". The
 * page is allocated by us, registered with the hypervisor via
 * HV_X64_MSR_REFERENCE_TSC, then updated by the hypervisor whenever
 * the underlying TSC frequency or offset changes (live migration,
 * VM pause/resume).
 *
 * A guest read is the seqlock pattern from TLFS code-sample on the
 * same page:
 *
 *   do {
 *       seq0   = page->TscSequence;
 *       if (seq0 == 0) return rdmsr(0x40000020);   // fallback
 *       tsc    = rdtsc();
 *       scale  = page->TscScale;
 *       offset = page->TscOffset;
 *       seq1   = page->TscSequence;
 *   } while (seq0 != seq1);
 *   time_100ns = ((tsc * scale) >> 64) + offset;
 */

#define MSR_HV_REFERENCE_TSC   0x40000021u
#define MSR_HV_TIME_REF_COUNT  0x40000020u

/* Same rationale as PVCLOCK_POPULATE_SPIN_MAX in pvclock.c — bounded
 * wait for the hypervisor to populate the reference TSC page. Hyper-V
 * typically updates inside the WRMSR exit; 1000 PAUSEs ≈ 20 µs is a
 * safe upper bound. */
#define HVCLOCK_POPULATE_SPIN_MAX  1000u

/* TLFS recommends an unbounded seqlock loop. We cap at 1024 — beyond
 * that the hypervisor is wedged updating and we fall through to the
 * MSR_HV_TIME_REF_COUNT read, the architectural fallback. */
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
    /* Idempotent. Double-init would leak the backing page and could
     * wedge the hypervisor on a second WRMSR with a different GPA. */
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

    /* MSR layout (TLFS §10.7.2.3):
     *   bit 0       enable
     *   bits 11:1   reserved (preserve)
     *   bits 63:12  guest physical page number */
    uint64_t enable = (s_page_phys & ~0xFFFULL) | 1ULL;
    wrmsr_hv(MSR_HV_REFERENCE_TSC, enable);

    /* Hyper-V populates the page synchronously on the WRMSR; spin
     * briefly looking for a non-zero TscSequence. */
    for (unsigned i = 0; i < HVCLOCK_POPULATE_SPIN_MAX && s_page->TscSequence == 0; i++)
        __asm__ volatile("pause");

    if (s_page->TscSequence == 0) {
        debug_printf("[HVCLOCK] Hyper-V left page TscSequence=0 — falling back to MSR\n");
        /* Still mark available — pvclock_now_ns will use the MSR path. */
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

/* 128-bit unsigned multiply (a × b), return the high 64 bits.
 *
 * x86-64 mulq SRC: RDX:RAX = RAX × SRC. So `a` MUST go into RAX
 * (input operand tied to the "=a" output) and `b` is the source
 * operand. The low 64 bits land in RAX (captured as `lo` and
 * discarded); we want the high half from RDX.
 *
 * Earlier revision tied `a` to RDX via "0"(a) on a "=d" operand 0,
 * which left RAX uninitialised — mulq then multiplied undefined
 * garbage by b. The bug manifested only on Hyper-V (the only caller),
 * which is why the QEMU stress matrix didn't catch it. */
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

    /* MSR fallback when the page is invalidated. The MSR delivers
     * partition reference count in 100-ns units already. */
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
    /* Spec note: if we cannot read a stable snapshot in 1024 retries the
     * hypervisor is wedged updating; the partition reference count MSR
     * is the architectural fallback. */
    uint64_t hund = rdmsr_hv(MSR_HV_TIME_REF_COUNT);
    return hund * 100ULL;
}
