#include "pvclock.h"
#include "hypervisor.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "atomics.h"


#define MSR_KVM_SYSTEM_TIME_NEW   0x4b564d01u
#define MSR_KVM_WALL_CLOCK_NEW    0x4b564d00u

#define PVCLOCK_TSC_STABLE_BIT    (1u << 0)

typedef struct __attribute__((packed, aligned(32))) {
    volatile uint32_t version;
    uint32_t          pad0;
    volatile uint64_t tsc_timestamp;
    volatile uint64_t system_time;
    volatile uint32_t tsc_to_system_mul;
    volatile int8_t   tsc_shift;
    volatile uint8_t  flags;
    uint8_t           pad[2];
} pvclock_vcpu_time_info_t;

typedef struct __attribute__((packed, aligned(4))) {
    volatile uint32_t version;
    volatile uint32_t sec;
    volatile uint32_t nsec;
} pvclock_wall_clock_t;

static uint64_t                   s_pvti_page_phys   = 0;
static pvclock_vcpu_time_info_t  *s_pvti_array       = NULL;
static uint64_t                   s_wall_page_phys   = 0;
static pvclock_wall_clock_t      *s_wall             = NULL;

static volatile bool s_available = false;

#define PVCLOCK_MAX_SLOTS  128

#define PVCLOCK_POPULATE_SPIN_MAX  1000u

#define PVCLOCK_SEQLOCK_RETRY_MAX  1024

static inline uint64_t rdmsr_pv(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr_pv(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"((uint32_t)v),
                     "d"((uint32_t)(v >> 32)));
}

bool pvclock_init(void)
{
    if (s_available) return true;

    if (!hv_has_kvmclock()) {
        debug_printf("[PVCLOCK] kvmclock not advertised by hypervisor\n");
        return false;
    }

    void *pvti_phys = pmm_alloc_zero(1);
    void *wall_phys = pmm_alloc_zero(1);
    if (!pvti_phys || !wall_phys) {
        if (pvti_phys) pmm_free(pvti_phys, 1);
        if (wall_phys) pmm_free(wall_phys, 1);
        debug_printf("[PVCLOCK] pmm_alloc failed\n");
        return false;
    }

    s_pvti_page_phys = (uint64_t)pvti_phys;
    s_pvti_array     = (pvclock_vcpu_time_info_t *)vmm_phys_to_virt(s_pvti_page_phys);
    s_wall_page_phys = (uint64_t)wall_phys;
    s_wall           = (pvclock_wall_clock_t *)vmm_phys_to_virt(s_wall_page_phys);

    vmm_register_shared_phys(s_pvti_page_phys);
    vmm_register_shared_phys(s_wall_page_phys);

    uint64_t addr0 = s_pvti_page_phys + 0 * sizeof(pvclock_vcpu_time_info_t);
    wrmsr_pv(MSR_KVM_SYSTEM_TIME_NEW, addr0 | 1u);

    wrmsr_pv(MSR_KVM_WALL_CLOCK_NEW, s_wall_page_phys);

    for (unsigned i = 0; i < PVCLOCK_POPULATE_SPIN_MAX && s_pvti_array[0].version == 0; i++)
        __asm__ volatile("pause");

    if (s_pvti_array[0].version == 0) {
        debug_printf("[PVCLOCK] hypervisor did not populate VCPU 0 slot — disabling MSRs and freeing pages\n");
        wrmsr_pv(MSR_KVM_SYSTEM_TIME_NEW, 0);
        wrmsr_pv(MSR_KVM_WALL_CLOCK_NEW,  0);
        pmm_free((void *)(uintptr_t)s_pvti_page_phys, 1);
        pmm_free((void *)(uintptr_t)s_wall_page_phys, 1);
        s_pvti_page_phys = 0;
        s_pvti_array     = NULL;
        s_wall_page_phys = 0;
        s_wall           = NULL;
        return false;
    }

    s_available = true;
    debug_printf("[PVCLOCK] kvmclock active: tsc_to_ns_mul=0x%x shift=%d flags=0x%x\n",
                 s_pvti_array[0].tsc_to_system_mul,
                 (int)s_pvti_array[0].tsc_shift,
                 s_pvti_array[0].flags);
    return true;
}

void pvclock_init_ap(uint8_t core_index)
{
    if (!s_available) return;
    if (core_index >= PVCLOCK_MAX_SLOTS) return;
    if (s_pvti_page_phys == 0) return;
    if (!hv_has_kvmclock()) return;

    uint64_t addr = s_pvti_page_phys + core_index * sizeof(pvclock_vcpu_time_info_t);
    wrmsr_pv(MSR_KVM_SYSTEM_TIME_NEW, addr | 1u);
}

bool pvclock_is_available(void)
{
    return s_available;
}

static uint64_t mul_shift_32(uint64_t cycles, uint32_t mul)
{
    uint64_t lo = (cycles & 0xFFFFFFFFu) * (uint64_t)mul;
    uint64_t hi = (cycles >> 32)         * (uint64_t)mul;
    return (lo >> 32) + hi;
}

uint64_t pvclock_now_ns(void)
{
    if (!s_available) return 0;

    pvclock_vcpu_time_info_t *pvti = &s_pvti_array[0];

    uint64_t ns;
    for (int attempts = 0; attempts < PVCLOCK_SEQLOCK_RETRY_MAX; attempts++) {
        uint32_t v0 = __atomic_load_n(&pvti->version, __ATOMIC_ACQUIRE);
        if (v0 & 1u) { __asm__ volatile("pause"); continue; }

        uint64_t tsc_now        = rdtsc();
        uint64_t tsc_timestamp  = pvti->tsc_timestamp;
        uint64_t system_time    = pvti->system_time;
        uint32_t mul            = pvti->tsc_to_system_mul;
        int8_t   shift          = pvti->tsc_shift;

        __asm__ volatile("" ::: "memory");
        uint32_t v1 = __atomic_load_n(&pvti->version, __ATOMIC_ACQUIRE);
        if (v1 != v0) continue;

        uint64_t delta = (tsc_now > tsc_timestamp)
                             ? (tsc_now - tsc_timestamp) : 0;

        if (shift >= 0)
            delta <<= shift;
        else
            delta >>= -shift;

        ns = mul_shift_32(delta, mul) + system_time;
        return ns;
    }
    return pvti->system_time;
}

bool pvclock_walltime(uint64_t *out_sec, uint32_t *out_nsec)
{
    if (!s_available || !s_wall) return false;
    if (!out_sec || !out_nsec)   return false;

    for (int attempts = 0; attempts < PVCLOCK_SEQLOCK_RETRY_MAX; attempts++) {
        uint32_t v0 = __atomic_load_n(&s_wall->version, __ATOMIC_ACQUIRE);
        if (v0 & 1u) { __asm__ volatile("pause"); continue; }

        uint32_t sec  = s_wall->sec;
        uint32_t nsec = s_wall->nsec;

        __asm__ volatile("" ::: "memory");
        uint32_t v1 = __atomic_load_n(&s_wall->version, __ATOMIC_ACQUIRE);
        if (v1 != v0) continue;

        *out_sec  = sec;
        *out_nsec = nsec;
        return true;
    }
    return false;
}

uint64_t pvclock_tsc_khz(void)
{
    if (!s_available) return 0;

    pvclock_vcpu_time_info_t *pvti = &s_pvti_array[0];
    uint32_t mul   = pvti->tsc_to_system_mul;
    int8_t   shift = pvti->tsc_shift;
    if (mul == 0) return 0;

    uint64_t tsc_khz = ((uint64_t)1000000ULL) << 32;
    tsc_khz /= (uint64_t)mul;

    if (shift > 63 || shift < -63) return 0;

    if (shift < 0)
        tsc_khz <<= -shift;
    else
        tsc_khz >>= shift;
    return tsc_khz;
}