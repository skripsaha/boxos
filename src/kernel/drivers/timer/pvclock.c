#include "pvclock.h"
#include "hypervisor.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "atomics.h"
#include "amp.h"          // amp_get_core_index() — whose slot this core reads

/*
 * kvmclock implementation.
 *
 * One pvclock_vcpu_time_info per VCPU; KVM updates them on every entry
 * to that VCPU. We allocate a single page that comfortably fits one
 * 32-byte struct per VCPU we will ever boot, then enable kvmclock on
 * each VCPU by writing MSR_KVM_SYSTEM_TIME_NEW with the *physical*
 * address of that VCPU's slot + bit 0 (enable). After enable, the
 * struct is updated by the hypervisor whenever the VCPU is scheduled.
 *
 * A READING BELONGS TO THE VCPU THAT TOOK IT. tsc_timestamp is, in the
 * words of Documentation/virt/kvm/x86/msr.rst, "the tsc value at the
 * current VCPU at the time of the update of this structure" — an epoch
 * private to one VCPU, published when THAT VCPU was last entered. So
 * pvclock_now_ns() reads the slot of the core it is executing on, exactly
 * as linux's kvmclock.c does through this_cpu_pvti(). Reading slot 0 from
 * core 3 subtracted core 0's epoch from core 3's rdtsc and produced a
 * nanosecond value wrong by the difference of their TSC offsets plus the
 * whole interval since vCPU 0 was last scheduled — and readings taken from
 * different cores went backwards against each other.
 *
 * The RATE is the one thing that is package-wide: KVM rate-synchronises
 * guest-visible TSC across VCPUs, so the (mul, shift) pair is identical in
 * every slot. That is why pvclock_tsc_khz() may read slot 0 from anywhere,
 * and why it is the only function here that does.
 *
 * Per-VCPU init is performed by pvclock_init_ap() from per_core.c,
 * which is invoked from each AP after lapic_enable but before any
 * pvclock_now_ns() consumer on that AP.
 */

#define MSR_KVM_SYSTEM_TIME_NEW   0x4b564d01u
#define MSR_KVM_WALL_CLOCK_NEW    0x4b564d00u

typedef struct __attribute__((packed, aligned(32))) {
    volatile uint32_t version;
    uint32_t          pad0;
    volatile uint64_t tsc_timestamp;
    volatile uint64_t system_time;     /* nanoseconds */
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

/* Backing pages — kept across the kernel lifetime. */
static uint64_t                   s_pvti_page_phys   = 0;
static pvclock_vcpu_time_info_t  *s_pvti_array       = NULL;  /* PVCLOCK_MAX_SLOTS entries */
static uint64_t                   s_wall_page_phys   = 0;
static pvclock_wall_clock_t      *s_wall             = NULL;

static volatile bool s_available = false;

/* The PMM page allocated below is 4 KB, which holds exactly 128 VCPU
 * slots of 32 bytes. Both pvclock_init_ap() and pvclock_now_ns() hold to
 * this bound: a core numbered past it simply has no pvclock, which is a
 * fact to report rather than a page to read past. */
#define PVCLOCK_MAX_SLOTS  128
_Static_assert(PVCLOCK_MAX_SLOTS * sizeof(pvclock_vcpu_time_info_t) <= 4096u,
               "pvclock slots must fit the single page they are allocated in");

/* Hypervisor latency on the populate-after-WRMSR write: KVM updates
 * the pvti slot synchronously inside the WRMSR exit handler, so a few
 * cache reloads are all that's needed. 1000 PAUSE iterations is ~20 µs
 * on a 2 GHz CPU — generous bound that has never been hit in practice
 * on real KVM hosts and still bails before the boot timer matters. */
#define PVCLOCK_POPULATE_SPIN_MAX  1000u

/* Seqlock-read retry bound.
 *
 * Linux's loop is unbounded, and msr.rst says why: "guest has to check
 * version before and after grabbing time information and check that they
 * are both equal and even. An odd version indicates an in-progress update."
 * There is no sanctioned way to consume any field without a matching even
 * pair, so a reading that never settles is not a reading.
 *
 * The bound here exists so that the kernel's lock-free clock read cannot
 * wedge the core that takes it, and it is an ITERATION count rather than a
 * span on a clock for the one reason that cannot be argued with: this IS a
 * clock, and it has nothing else to ask. What the bound must never do is
 * invent a time when it expires — see the end of pvclock_now_ns(), where
 * exhaustion is reported as "no reading" and nothing else. The previous
 * justification for the number claimed field telemetry ("observed: 1-2
 * retries under load, never seen >10") that nothing in this project
 * measured. */
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
    /* Idempotent — double-init would leak the two backing pages and
     * potentially confuse the hypervisor with conflicting MSR writes
     * across re-enable. */
    if (s_available) return true;

    if (!hv_has_kvmclock()) {
        debug_printf("[PVCLOCK] kvmclock not advertised by hypervisor\n");
        return false;
    }

    /* Allocate two pages: one for the VCPU array, one for the
     * wall-clock structure. Both are zeroed; the hypervisor fills
     * them in when we enable. */
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

    /* Enable per-BSP. The MSR write installs the VCPU-0 slot. APs are
     * activated by pvclock_init_ap() — see per_core.c.
     *
     * MSR value layout (KVM docs):
     *   bits 63..2  guest physical address of the pvti slot (4-byte aligned)
     *   bit 1       reserved (must be 0)
     *   bit 0       enable */
    uint64_t addr0 = s_pvti_page_phys + 0 * sizeof(pvclock_vcpu_time_info_t);
    wrmsr_pv(MSR_KVM_SYSTEM_TIME_NEW, addr0 | 1u);

    /* Activate wall clock. */
    wrmsr_pv(MSR_KVM_WALL_CLOCK_NEW, s_wall_page_phys);

    /* Spin briefly waiting for the hypervisor to populate VCPU 0. Linux's
     * KVM performs the update inside the WRMSR exit, so in practice the
     * slot is filled by the time that instruction retires — but msr.rst
     * declines to promise it ("The interval between updates of this
     * structure is arbitrary and implementation-dependent"), so a slot
     * still empty here is a conforming hypervisor and not a broken one.
     * The branch below treats it as "no pvclock on this machine". */
    for (unsigned i = 0; i < PVCLOCK_POPULATE_SPIN_MAX && s_pvti_array[0].version == 0; i++)
        __asm__ volatile("pause");

    if (s_pvti_array[0].version == 0) {
        debug_printf("[PVCLOCK] hypervisor did not populate VCPU 0 slot — disabling MSRs and freeing pages\n");
        /* Detach the hypervisor before releasing the backing pages —
         * KVM treats bit 0 of MSR_KVM_SYSTEM_TIME_NEW as enable;
         * writing 0 unbinds the GPA. Without this we'd free pages
         * the hypervisor might still write to. */
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

    /* Register both pages as shared, and not one line earlier. The
     * shared-phys registry is append-only — vmm.h declares
     * vmm_register_shared_phys and vmm_is_shared_phys and no way to undo
     * one — and its meaning is "this frame lives for the kernel lifetime",
     * which vmm_destroy_context() honours by clearing the PTE and NOT
     * returning the frame to the buddy. Registered before the populate
     * check, the two frames the branch above hands back to the allocator
     * kept their entries: the first cabin to be given one of them as an
     * ordinary page would lose it for good at teardown. Nothing has entered
     * the registry until the pages are certainly ours to keep. */
    vmm_register_shared_phys(s_pvti_page_phys);
    vmm_register_shared_phys(s_wall_page_phys);

    s_available = true;
    debug_printf("[PVCLOCK] kvmclock active: tsc_to_ns_mul=0x%x shift=%d flags=0x%x\n",
                 s_pvti_array[0].tsc_to_system_mul,
                 (int)s_pvti_array[0].tsc_shift,
                 s_pvti_array[0].flags);
    return true;
}

/* Per-AP activation. Called once on each AP after its LAPIC comes up.
 * Each VCPU needs its own slot — KVM updates only the slot the MSR on
 * THAT VCPU points to.
 *
 * Defensive ordering: re-test hv_has_kvmclock() — if the BSP raced
 * with us during pvclock_init() and ended up disabling pvclock (e.g.
 * populate-spin timeout above freed pages and cleared state), we must
 * NOT issue a WRMSR with a stale GPA. The s_pvti_page_phys != 0
 * check defends against init-then-fail order, while hv_has_kvmclock()
 * defends against the feature being unavailable in the first place. */
void pvclock_init_ap(uint8_t core_index)
{
    if (!s_available) return;
    if (core_index >= PVCLOCK_MAX_SLOTS) return;
    if (s_pvti_page_phys == 0) return;          /* init failed/freed */
    if (!hv_has_kvmclock()) return;             /* feature gone (shouldn't happen) */

    uint64_t addr = s_pvti_page_phys + core_index * sizeof(pvclock_vcpu_time_info_t);
    wrmsr_pv(MSR_KVM_SYSTEM_TIME_NEW, addr | 1u);
}

bool pvclock_is_available(void)
{
    return s_available;
}

/* Compute (cycles * mul) >> 32 in a way that doesn't lose precision.
 * cycles is at most ~2^48 (a few months of TSC), mul is 32-bit; the
 * 128-bit product is taken via the (small `lo`, scaled `hi`) split. */
static uint64_t mul_shift_32(uint64_t cycles, uint32_t mul)
{
    uint64_t lo = (cycles & 0xFFFFFFFFu) * (uint64_t)mul;
    uint64_t hi = (cycles >> 32)         * (uint64_t)mul;
    return (lo >> 32) + hi;
}

uint64_t pvclock_now_ns(void)
{
    if (!s_available) return 0;

    /* This core's own slot — the one pvclock_init_ap() registered for it,
     * and the only one whose tsc_timestamp is an epoch on this core's TSC.
     * A core past the slot array has no slot registered (pvclock_init_ap
     * declines it with the same bound), so it has no pvclock reading to
     * give and says so rather than reading a neighbour's page. */
    uint8_t core = amp_get_core_index();
    if (core >= PVCLOCK_MAX_SLOTS) return 0;

    /* Per Linux Documentation/virt/kvm/x86/msr.rst recommended-read:
     * read version → barrier → fields → barrier → version. If version
     * changed (or is odd), retry. Odd value = update in progress on
     * the hypervisor side; even = consistent snapshot. */
    pvclock_vcpu_time_info_t *pvti = &s_pvti_array[core];

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

        /* Guard against rdtsc < tsc_timestamp (host updates mid-read,
         * or a TSC reset across migration). Cap delta at 0 — gives a
         * monotonically increasing reading instead of garbage. */
        uint64_t delta = (tsc_now > tsc_timestamp)
                             ? (tsc_now - tsc_timestamp) : 0;

        if (shift >= 0)
            delta <<= shift;
        else
            delta >>= -shift;

        ns = mul_shift_32(delta, mul) + system_time;
        return ns;
    }
    /* No consistent snapshot. There is nothing here to hand back: the only
     * value in reach is pvti->system_time, and reading it is precisely what
     * msr.rst forbids — no version pair, from a structure the exit condition
     * says is being rewritten. It is also not what it used to claim to be.
     * It is the update ANCHOR, without the mul_shift_32(delta, mul) term
     * every successful return above adds, so it can sit up to a full
     * hypervisor update interval behind the value this function last
     * returned — "last known system_time ... better than 0 for monotonicity"
     * was wrong on all three counts.
     *
     * 0 is the answer this function's contract already has for "no reading"
     * (pvclock.h: "Returns 0 if pvclock is not initialised"), and a caller
     * that gets it falls back to another clocksource instead of trusting a
     * number nobody stands behind. */
    return 0;
}

bool pvclock_walltime(uint64_t *out_sec, uint32_t *out_nsec)
{
    if (!s_available || !s_wall) return false;
    if (!out_sec || !out_nsec)   return false;

    /* Same seqlock pattern, this time on the wall-clock structure. */
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

    /* Slot 0 from any core, deliberately: this reads the (mul, shift) RATE,
     * which KVM keeps identical in every slot, never the per-VCPU epoch. */
    pvclock_vcpu_time_info_t *pvti = &s_pvti_array[0];
    uint32_t mul   = pvti->tsc_to_system_mul;
    int8_t   shift = pvti->tsc_shift;
    if (mul == 0) return 0;

    /* Derivation (matches linux/arch/x86/kernel/kvmclock.c
     * `pvclock_tsc_khz`):
     *
     *   ns       = ((delta_tsc << shift) × mul) >> 32                 (shift ≥ 0)
     *   ns       = ((delta_tsc >> -shift) × mul) >> 32                (shift < 0)
     *
     * Both collapse to ns = (delta_tsc × mul) >> (32 - shift).
     *
     * Solve for the TSC rate that turns 1 second (1e9 ns) into one
     * second worth of TSC cycles:
     *
     *   1e9 ns = (tsc_hz × mul) >> (32 - shift)
     *   tsc_hz = (1e9 << (32 - shift)) / mul
     *   tsc_khz = tsc_hz / 1000 = (1e6 << (32 - shift)) / mul
     *
     * To stay in 64-bit range across all realistic pvclock (mul,
     * shift) pairs, compute the (1e6 << 32) / mul reciprocal FIRST,
     * THEN apply the shift. This is the canonical Linux ordering.
     * (1e6 × 2^32 = 4.3e15 — comfortably in uint64_t.) */
    uint64_t tsc_khz = ((uint64_t)1000000ULL) << 32;
    tsc_khz /= (uint64_t)mul;

    /* Guard against pathological shift values. int8_t range is -128
     * to 127; realistic pvclock values are |shift| ≤ 32 — Linux uses
     * the same implicit assumption. A shift outside that range almost
     * certainly means corruption; reject. */
    if (shift > 63 || shift < -63) return 0;

    if (shift < 0)
        tsc_khz <<= -shift;
    else
        tsc_khz >>= shift;
    return tsc_khz;
}
