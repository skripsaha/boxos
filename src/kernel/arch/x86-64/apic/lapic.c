#include "lapic.h"
#include "irqchip.h"
#include "klib.h"
#include "vmm.h"
#include "acpi_madt.h"
#include "cpu_calibrate.h"   // cpu_ms_to_tsc / cpu_get_tsc_freq_khz
#include "atomics.h"         // rdtsc
#include "cpuid.h"           // g_cpu_caps.has_tsc_deadline
#include "hourglass.h"       // the ICR wait is bounded by real time, not by spins

static volatile uint32_t* lapic_base_virt = NULL;
static uintptr_t lapic_base_phys = 0;
static bool lapic_enabled = false;

/* Cached x2APIC mode flag.
 *
 * Sampled once from IA32_APIC_BASE.EXTD (bit 10) at lapic_init time.
 * BoxOS never disables x2APIC after enabling it (the spec also makes
 * the transition extd→xapic require disabling-then-reenabling APIC,
 * which we do not do), so this cache is stable for the kernel
 * lifetime.
 *
 * It decides EVERY register access, not only the ICR and the ID.
 * Intel SDM Vol 3A, "x2APIC Register Availability" and the table
 * "MSR/MMIO Interface of a Local x2APIC in Different Modes of
 * Operation": in x2APIC mode "the memory mapped interface is not
 * available and any access to the MMIO interface will behave similar
 * to that of a legacy xAPIC in globally disabled state" — the store is
 * DROPPED, silently, with no fault to announce it. The
 * general-protection exception runs the OTHER way: RDMSR/WRMSR in the
 * 0800H-0BFFH window while the APIC is NOT in x2APIC mode.
 *
 * That asymmetry is the whole danger. A kernel that believes a wrong
 * ICR access would trap leaves EOI, SVR, TPR, the LVTs and the timer on
 * a dead window, and on the first interrupt the ISR bit is never
 * cleared and the machine goes permanently deaf with nothing printed. */
static bool g_x2apic_active = false;

/* TSC-deadline timer state (Intel SDM Vol 3A, "TSC-Deadline Mode" — section
 * numbers are not used here because they move between SDM revisions). When
 * the CPU supports
 * the deadline timer, App Cores run the scheduler tick in deadline mode instead
 * of periodic: it is driven by the invariant TSC (exact, low-jitter) rather
 * than the bus-clock-derived LAPIC counter. The mode is one-shot per deadline,
 * so lapic_timer_rearm() reloads the next deadline from the IRQ handler.
 * g_lapic_tsc_period is the per-tick interval in TSC cycles. Both are written
 * by every App Core in lapic_timer_init() to the same values (has_tsc_deadline
 * is intersected across cores), so the shared writes are benign. */
static bool     g_lapic_tsc_deadline = false;
static uint64_t g_lapic_tsc_period   = 0;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* Legacy xAPIC MMIO offset -> x2APIC MSR index.
 *
 * The map is one-to-one on 16-byte boundaries: MSR = 0x800 + (offset >> 4)
 * (Intel SDM Vol 3A, table "Local APIC Register Address Map Supported by
 * x2APIC"; EDK2's BaseXApicX2ApicLib computes the identical expression).
 * Two offsets have no MSR behind them and must never arrive here:
 *
 *   DFR (0x0E0) — the register does not exist in x2APIC mode. BoxOS never
 *                 programs it: every destination it issues is physical.
 *   ICR (0x300/0x310) — one 64-bit MSR 0x830 instead, written whole by
 *                 lapic_icr_write. Both halves are handled explicitly
 *                 below rather than left to fault or, worse, to send. */
static inline uint32_t x2apic_msr_for(uint32_t reg) {
    return MSR_X2APIC_BASE + (reg >> 4);
}

uint32_t lapic_read(uint32_t reg) {
    if (g_x2apic_active) {
        /* ICR_HIGH is the upper half of MSR 0x830, not an MSR of its own. */
        if (reg == LAPIC_REG_ICR_HIGH)
            return (uint32_t)(rdmsr(MSR_X2APIC_ICR) >> 32);
        return (uint32_t)rdmsr(x2apic_msr_for(reg));
    }
    return lapic_base_virt[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t value) {
    if (g_x2apic_active) {
        if (reg == LAPIC_REG_ICR_LOW || reg == LAPIC_REG_ICR_HIGH) {
            /* The x2APIC ICR is a single 64-bit MSR: half of it cannot be
             * written without sending the IPI the other half describes.
             * Every ICR write goes through lapic_icr_write, so reaching
             * here means a caller bypassed it — say so instead of either
             * faulting or firing an IPI nobody asked for. */
            kprintf("[LAPIC] %[E]ICR offset 0x%x written directly in x2APIC "
                    "mode — request dropped, use lapic_icr_write%[D]\n", reg);
            return;
        }
        /* WRMSR to an APIC register does not carry the serializing
         * semantics of the uncacheable MMIO store it replaces (Intel SDM
         * Vol 3A, "x2APIC Register Address Space"), so callers that
         * ordered work before this write would lose that order. Fence,
         * exactly as EDK2's WriteLocalApicReg does, so a caller written
         * against the xAPIC path keeps behaving the same here. */
        __asm__ volatile("mfence" ::: "memory");
        wrmsr(x2apic_msr_for(reg), value);
        return;
    }
    lapic_base_virt[reg / 4] = value;
}

void lapic_init(uintptr_t base_addr) {
    lapic_base_phys = base_addr;

    debug_printf("[LAPIC] Initializing Local APIC at phys 0x%lx\n", base_addr);

    // Enable LAPIC via MSR (set global enable bit) — leave EXTD alone:
    // if firmware/BSP enabled x2APIC, we honour it; if not, we stay xAPIC.
    uint64_t apic_base_msr = rdmsr(MSR_APIC_BASE);
    apic_base_msr |= MSR_APIC_BASE_ENABLE;
    wrmsr(MSR_APIC_BASE, apic_base_msr);

    // Re-sample the EXTD bit AFTER enabling — some firmwares only expose
    // x2APIC once the APIC global enable is set.
    //
    // This question is asked BEFORE the first register touch, and before the
    // MMIO window is mapped, because the answer decides whether that window
    // exists at all. In x2APIC mode it is not decoded, so mapping it would
    // reserve kernel VA for a page nothing answers — and making the whole
    // bring-up conditional on that mapping succeeding would refuse to run an
    // APIC that is perfectly reachable through its MSRs.
    g_x2apic_active = (rdmsr(MSR_APIC_BASE) & MSR_APIC_BASE_EXTD) != 0;

    if (g_x2apic_active) {
        kprintf("[LAPIC] x2APIC mode is live (firmware handover) — every "
                "register goes through the MSR window at 0x800\n");
    } else {
        // Map LAPIC MMIO region (4KB, uncacheable)
        lapic_base_virt = (volatile uint32_t*)vmm_map_mmio(
            base_addr, 4096,
            VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_CACHE_DISABLE
        );

        if (!lapic_base_virt) {
            debug_printf("[LAPIC] %[E]Failed to map LAPIC MMIO%[D]\n");
            return;
        }
    }

    // Set Spurious Interrupt Vector Register: enable APIC + set spurious vector
    uint32_t svr = lapic_read(LAPIC_REG_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr = (svr & ~0xFF) | LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_REG_SVR, svr);

    // Clear Task Priority Register (accept all interrupts)
    lapic_write(LAPIC_REG_TPR, 0);

    // Mask LINT0 and LINT1 by default
    lapic_write(LAPIC_REG_LINT0_LVT, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LINT1_LVT, LAPIC_LVT_MASKED);

    // Mask timer LVT by default
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_LVT_MASKED);

    // Clear any pending errors
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_ESR, 0);

    // Send EOI to clear any pending interrupts from before init
    lapic_write(LAPIC_REG_EOI, 0);

    lapic_enabled = true;

    uint32_t id = lapic_get_id();
    uint32_t ver = lapic_read(LAPIC_REG_VERSION);

    debug_printf("[LAPIC] APIC ID=%u, Version=0x%x, MaxLVT=%u\n",
                 id, ver & 0xFF, ((ver >> 16) & 0xFF) + 1);
    debug_printf("[LAPIC] SVR=0x%x (spurious vector=%u)\n",
                 lapic_read(LAPIC_REG_SVR), LAPIC_SPURIOUS_VECTOR);
    debug_printf("[LAPIC] %[S]Local APIC enabled successfully%[D]\n");
}

void lapic_send_eoi(void) {
    /* The guard asks "is a register reachable", not "is the MMIO window
     * mapped": in x2APIC mode there is no window and EOI is MSR 0x80B. An
     * EOI that does not land leaves the ISR bit set and the core deaf from
     * that vector's priority class down, which is why this is the one write
     * that must never be skipped on the mistaken belief it cannot work. */
    if (g_x2apic_active || lapic_base_virt) {
        lapic_write(LAPIC_REG_EOI, 0);
    }
}

uint32_t lapic_get_id(void) {
    /* x2APIC: the local APIC ID lives in the read-only MSR
     * IA32_X2APIC_APICID (0x802) and is the FULL 32-bit register value —
     * it is NOT shifted by 24 like the xAPIC MMIO form. The memory-mapped
     * APIC register window (offset 0x20) is not decoded while EXTD=1, so an
     * MMIO read here returns whatever the bus gives an unclaimed read and
     * would make every core resolve to the same bogus index (Intel SDM
     * Vol 3A, "x2APIC Register Availability"). */
    if (g_x2apic_active) {
        return (uint32_t)rdmsr(MSR_X2APIC_APICID);
    }
    /* xAPIC: 8-bit APIC ID in bits 31:24 of the MMIO ID register. */
    return (lapic_read(LAPIC_REG_ID) >> 24) & 0xFF;
}

void lapic_enable(void) {
    /* This is an App/K Core's first touch of its own APIC, and the mode it
     * finds itself in has to agree with the one the kernel cached from the
     * BSP — otherwise every register access below is an RDMSR/WRMSR in the
     * 0800H-0BFFH window on a CPU that is not in x2APIC mode, which is a
     * general-protection exception, on a core whose IDT has only just been
     * loaded.
     *
     * Usually they already agree: Intel SDM Vol 3A, "x2APIC State
     * Transitions" — "an INIT in x2APIC state keeps the x2APIC in the
     * x2APIC mode", so a core firmware left in x2APIC is still there after
     * our INIT/SIPI. The case this covers is firmware that enabled x2APIC
     * on the BSP alone. Linux makes the same call for the same reason, from
     * cpu_init() via x2apic_setup().
     *
     * The climb is taken ONE LEGAL EDGE AT A TIME, and that is the whole
     * subtlety. The state diagram in the Intel 64 Architecture x2APIC
     * Specification, "x2APIC State Transitions", has no edge from the
     * disabled state (EN=0) straight to x2APIC (EN=1, EXTD=1): that arrow
     * is labelled an illegal transition and the write raises #GP. From
     * disabled the only legal step is into xAPIC. And this core can be
     * disabled: a globally disabled local APIC still answers INIT and
     * STARTUP, which is exactly how amp_boot_aps got a firmware-parked AP
     * as far as this line. So the walk is disabled -> xAPIC -> x2APIC, in
     * two writes. One write would fault a core whose IDT is seconds old.
     *
     * Bits only ever go up. Leaving x2APIC would require clearing EN and
     * EXTD together (a write with EN=1, EXTD=0 while in x2APIC mode is
     * itself a #GP), and BoxOS never leaves it. */
    uint64_t apic_base = rdmsr(MSR_APIC_BASE);

    if (!(apic_base & MSR_APIC_BASE_ENABLE)) {
        /* Step one: disabled -> xAPIC. EXTD is forced off in this write
         * because EN=0 with EXTD=1 is not a state the APIC can be in —
         * whatever that bit reads as here, it names no mode worth
         * preserving, and carrying it along is the illegal edge itself. */
        apic_base = (apic_base | MSR_APIC_BASE_ENABLE) &
                    ~(uint64_t)MSR_APIC_BASE_EXTD;
        wrmsr(MSR_APIC_BASE, apic_base);
    }

    if (g_x2apic_active && !(apic_base & MSR_APIC_BASE_EXTD)) {
        /* Step two: xAPIC -> x2APIC, the one legal way in. */
        apic_base |= MSR_APIC_BASE_EXTD;
        wrmsr(MSR_APIC_BASE, apic_base);
    }

    uint32_t svr = lapic_read(LAPIC_REG_SVR);
    svr |= LAPIC_SVR_ENABLE;
    lapic_write(LAPIC_REG_SVR, svr);
    lapic_enabled = true;
}

void lapic_disable(void) {
    uint32_t svr = lapic_read(LAPIC_REG_SVR);
    svr &= ~LAPIC_SVR_ENABLE;
    lapic_write(LAPIC_REG_SVR, svr);
    lapic_enabled = false;
}

bool lapic_is_enabled(void) {
    return lapic_enabled;
}

uintptr_t lapic_get_base(void) {
    return lapic_base_phys;
}

/* True once lapic_read/lapic_write are safe to call — which in x2APIC mode
 * means the MSR window, not a mapping: there is no MMIO window there and
 * asking for one would report the APIC unusable on exactly the machines
 * whose APIC works best. Not the same question as lapic_get_base(): the
 * physical base is recorded before the mapping is attempted, and the mapping
 * can fail, so a non-zero base is no promise that a register read will land
 * anywhere.
 *
 * lapic_read() is `lapic_base_virt[reg / 4]`, which with a null base turns the
 * APIC ID register at offset 0x20 into a load from virtual address 0x20. That
 * is where BoxOS's first panic on real hardware died — the panic path prints
 * the core index, amp_get_core_index() fell back to the APIC ID, and the dump
 * we had asked for was replaced by "Unhandled kernel #PF at 0x20 err=0x0". */
bool lapic_is_mapped(void) {
    return g_x2apic_active || lapic_base_virt != NULL;
}

void lapic_timer_init(uint8_t vector, uint32_t frequency_hz) {
    debug_printf("[LAPIC] Calibrating APIC timer for %u Hz...\n", frequency_hz);

    /* Preferred on modern hardware: TSC-deadline mode. The timer is driven by
     * the invariant TSC (exact, low-jitter) instead of the bus-clock-derived
     * LAPIC counter, and needs no counter calibration — we derive the per-tick
     * interval directly from the already-calibrated TSC frequency. It is a
     * one-shot per deadline, so lapic_timer_rearm() reloads it each IRQ.
     * Intel SDM Vol 3A, "TSC-Deadline Mode".
     *
     * Hard precondition: invariant TSC. Without it the TSC rate changes
     * with P-states and the precomputed g_lapic_tsc_period drifts. Intel
     * SDM Vol 3B, "Invariant TSC" — invariance is reported via
     * CPUID.80000007H:EDX[8].
     * Real silicon from Nehalem (2008) onward is invariant; QEMU TCG with
     * `-cpu qemu64` is NOT. Fall back to periodic mode for non-invariant
     * targets — the periodic LAPIC counter is bus-clock-derived and
     * already independent of P-states. */
    if (g_cpu_caps.has_tsc_deadline && g_cpu_caps.has_invariant_tsc) {
        uint64_t cyc_per_sec = cpu_get_tsc_freq_khz() * 1000ULL;
        g_lapic_tsc_period = (frequency_hz > 0) ? (cyc_per_sec / frequency_hz)
                                                : cyc_per_sec;
        if (g_lapic_tsc_period == 0)
            g_lapic_tsc_period = cyc_per_sec;   // guard against bad calibration
        g_lapic_tsc_deadline = true;

        lapic_write(LAPIC_REG_TIMER_LVT, vector | LAPIC_LVT_TIMER_TSC_DEADLINE);
        /* The LVT-mode store must be ordered before the first deadline write —
         * WRMSR(IA32_TSC_DEADLINE) is not otherwise serialized against the LVT
         * MMIO store (Intel SDM Vol 3A, "TSC-Deadline Mode"). */
        __asm__ volatile("mfence" ::: "memory");
        wrmsr(MSR_IA32_TSC_DEADLINE, rdtsc() + g_lapic_tsc_period);

        debug_printf("[LAPIC] Timer configured: vector=%u, %u Hz TSC-deadline\n",
                     vector, frequency_hz);
        return;
    }

    /* Fallback: periodic mode, calibrated against the TSC, NOT PIT channel 2.
     *
     * Each AP runs this from per_core_init_ap() while the BSP is concurrently
     * busy-waiting on PIT channel 2 (pit_delay_us) inside amp_boot_aps() to
     * pace INIT/SIPI and to poll the AP-online flag. PIT channel 2 is a single
     * shared resource (ports 0x42/0x43, gate via 0x61); two CPUs programming
     * it at once corrupt each other's window, so the old channel-2 calibration
     * yielded a wrong APIC timer period on real hardware (QEMU's looser timing
     * masked it). The TSC is per-core and was already calibrated by
     * cpu_calibrate_tsc() before any AP boots (main.c), so timing the window
     * with rdtsc() needs no shared hardware and cannot race. */
    #define LAPIC_CAL_MS 10u

    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_16);

    // Free-run the count-down from max; the LVT stays masked so no IRQ fires
    // during measurement.
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    uint64_t tsc_deadline = rdtsc() + cpu_ms_to_tsc(LAPIC_CAL_MS);
    while (rdtsc() < tsc_deadline)
        __asm__ volatile("pause");

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_REG_TIMER_CCR);

    // Stop timer
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_LVT_MASKED);

    uint32_t ticks_per_sec    = elapsed * (1000u / LAPIC_CAL_MS);
    uint32_t ticks_per_period = (frequency_hz > 0) ? (ticks_per_sec / frequency_hz) : 0;
    if (ticks_per_period == 0)
        ticks_per_period = 1;   // never load ICR=0 — that stops the timer

    debug_printf("[LAPIC] Timer: %u ticks/%ums, %u ticks/s, period=%u ticks\n",
                 elapsed, LAPIC_CAL_MS, ticks_per_sec, ticks_per_period);

    // Configure periodic timer
    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_REG_TIMER_LVT, vector | LAPIC_LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_ICR, ticks_per_period);

    debug_printf("[LAPIC] Timer configured: vector=%u, %u Hz periodic\n",
                 vector, frequency_hz);
}

void lapic_timer_stop(void) {
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_ICR, 0);
}

void lapic_timer_rearm(void) {
    /* TSC-deadline is one-shot: the IA32_TSC_DEADLINE MSR self-clears when it
     * fires, so the timer IRQ handler must program the next deadline or the
     * tick stops. Relative to "now" (rdtsc) so it self-corrects if a tick ran
     * long. No-op in periodic mode (the LAPIC reloads the count itself). */
    if (g_lapic_tsc_deadline)
        wrmsr(MSR_IA32_TSC_DEADLINE, rdtsc() + g_lapic_tsc_period);
}

bool lapic_is_x2apic_active(void) {
    return g_x2apic_active;
}

/* How long the xAPIC ICR may stay busy before the previous IPI is called
 * stuck rather than slow. Linux draws the same line at the same place —
 * apic_mem_wait_icr_idle_timeout() in arch/x86/kernel/apic/ipi.c polls for
 * 1000 x 100 us — and a delivery that has not been accepted after a tenth
 * of a second is not late, it is stuck. The number is a SPAN OF REAL TIME
 * measured by HourGlass against a proven clock, never a spin count: a spin
 * count means a different wait on every machine, and it was the counting
 * that let this path declare the ICR free and write over a live IPI. */
#define LAPIC_ICR_IDLE_SPAN_US  100000u

/* Wait for the PREVIOUS xAPIC IPI to clear delivery-status (bit 12) before
 * ICR_LOW is written, because writing ICR_LOW is what sends: a write while
 * delivery is pending replaces an IPI that was still on its way, and
 * nothing anywhere reports the one that was lost.
 *
 * What the span MEANS depends on who is waiting, and Linux splits it the
 * same way for the same reason.
 *
 *   may_refuse = true — bring-up only (amp.c INIT/SIPI). The destination
 *     may be an APIC ID the MADT promised and the board does not have, in
 *     which case nothing will ever accept the IPI and an unbounded wait
 *     hangs the boot on a core that does not exist. That caller CAN act on
 *     a refusal: it stops launching that core and names it.
 *     Linux: wakeup_secondary_cpu_via_init -> the _timeout variant.
 *
 *   may_refuse = false — every runtime IPI. The destination is a core we
 *     booted and are already talking to, so the bit will clear; and the
 *     callers (scheduler wake, TLB shootdown, Touch, panic fan-out) are
 *     void, with no way to hear a refusal and nothing to do with one.
 *     Dropping the IPI there would be a lost wake — the exact class this
 *     kernel has hunted again and again — so the wait ends on the FACT,
 *     not on the clock. The span still earns its keep: when it passes, the
 *     stall is named once, loudly, with the core and the command, and then
 *     the wait goes on. A stuck ICR becomes a machine that says where it
 *     is stuck instead of a machine that quietly stops waking anybody.
 *     Linux: apic_mem_wait_icr_idle(), unbounded.
 *
 * Returns false only when a refusal was allowed and the span ran out. */
static bool icr_wait_for_previous(uint32_t dest_id, uint32_t cmd,
                                  bool may_refuse) {
    if (!(lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_SEND_PENDING))
        return true;

    HourGlass glass;
    bool measured = HourGlassTurn(&glass, LAPIC_ICR_IDLE_SPAN_US);
    if (!measured) {
        /* No proven clock on this machine yet. HourGlass forbids inventing
         * a substitute span, so the wait is on the fact alone — which is
         * what the runtime path does in any case. Said once. */
        static bool told_unruled = false;
        if (!told_unruled) {
            told_unruled = true;
            kprintf("[LAPIC] no proven time ruler yet — the ICR wait is "
                    "unbounded until one exists\n");
        }
    }

    while (lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_SEND_PENDING) {
        if (measured && HourGlassRunOut(&glass)) {
            const char *why = HourGlassSourceDied(&glass) ? "ruler died"
                                                          : "span ran out";
            if (may_refuse) {
                kprintf("[LAPIC] %[E]ICR still busy after %lu us on %s (%s) "
                        "— IPI 0x%x to LAPIC %u NOT sent%[D]\n",
                        HourGlassElapsedUs(&glass), HourGlassRulerName(&glass),
                        why, cmd, dest_id);
                return false;
            }
            kprintf("[LAPIC] %[E]ICR still busy after %lu us on %s (%s) — the "
                    "previous IPI is stuck; 0x%x for LAPIC %u waits for it "
                    "instead of overwriting it%[D]\n",
                    HourGlassElapsedUs(&glass), HourGlassRulerName(&glass),
                    why, cmd, dest_id);
            measured = false;   /* said once; from here the fact decides */
        }
        __asm__ volatile("pause");
    }
    return true;
}

static bool icr_send(uint32_t dest_id, uint32_t cmd, bool may_refuse) {
    if (g_x2apic_active) {
        /* x2APIC: one 64-bit MSR write to 0x830 — destination at full
         * width in the upper half, command in the lower.
         *
         * There is no delivery-status bit to wait on, and the reason is
         * NOT that delivery is finished when WRMSR retires. Intel 64
         * Architecture x2APIC Specification, "ICR Operation in x2APIC
         * Mode": "The Delivery Status bit of the ICR has been removed",
         * and "completion of the WRMSR instruction to the ICR does not
         * guarantee that the interrupt to be dispatched has been received
         * by the targeted processors". So nothing here can be polled and
         * nothing can be overwritten — the APIC queues the send itself —
         * which is why this path neither waits nor refuses.
         *
         * The fence IS load-bearing, and it carries the CALLER's ordering,
         * not the APIC's. The same specification relaxes the serializing
         * semantics of WRMSR when writing APIC registers, so the stores a
         * sender published for the target to read — a run-queue entry
         * before a wake, the VA and page count of a TLB shootdown before
         * the shootdown IPI — can still be invisible to the target when
         * the interrupt arrives. The uncacheable MMIO store this replaces
         * carried that ordering for free; WRMSR does not. Linux fences
         * every x2APIC send for exactly this reason (weak_wrmsr_fence in
         * x2apic_send_IPI). lapic_write fences its own x2APIC writes; this
         * path bypasses it because the ICR is one MSR rather than two
         * registers, so the fence is spelled out here. */
        __asm__ volatile("mfence" ::: "memory");
        wrmsr(MSR_X2APIC_ICR, ((uint64_t)dest_id << 32) | (uint64_t)cmd);
        return true;
    }

    /* A destination the xAPIC ICR cannot name.
     *
     * The xAPIC ICR destination field is eight bits (Intel SDM Vol 3A,
     * "Interrupt Command Register (ICR)": bits 56 through 63 of the ICR
     * hold the APIC ID of the target processor). Masking an ID of 0x100
     * down to 0x00 does not lose the interrupt — it DELIVERS it, to a
     * different, running processor. An IPI_WAKE that arrives at the wrong
     * core leaves the intended one asleep over queued work and gives the
     * wrong one a wake it never needed; an INIT would reset a live core.
     * Neither is something to do quietly, so the send is refused and
     * named. amp_boot_aps already declines to start such a core for the
     * same reason; this is the floor under that decision, for every path
     * that did not ask first. Such an ID is addressable only in x2APIC
     * mode, where the destination is the full 32 bits of MSR 0x830. */
    if (dest_id > 0xFFu) {
        kprintf("[LAPIC] %[E]APIC ID %u does not fit the 8-bit xAPIC ICR "
                "destination — IPI 0x%x NOT sent, because sending would "
                "deliver it to the core with ID %u instead%[D]\n",
                dest_id, cmd, dest_id & 0xFFu);
        return false;
    }

    /* ICR_HIGH then ICR_LOW is one indivisible send, and the pair must not
     * be split by an interrupt.
     *
     * Intel SDM Vol 3A, "Interrupt Command Register (ICR)": "The act of
     * writing to the low doubleword of the ICR causes the IPI to be sent",
     * and the destination it is sent to is whatever ICR_HIGH holds AT THAT
     * MOMENT. An interrupt taken between the two writes whose handler
     * sends its own directed IPI leaves ICR_HIGH holding the handler's
     * destination; our ICR_LOW write then delivers our command to the
     * handler's core, and ours never hears it. That is live on the BSP,
     * which runs kcore_run_loop with interrupts enabled and is also the
     * core that takes IRQ0 — whose tick path sends directed wakes of its
     * own.
     *
     * Linux brackets exactly this pair the same way: default_send_IPI_
     * single_phys wraps __default_send_IPI_dest_field (the idle wait plus
     * the ICR2/ICR writes) in local_irq_save/local_irq_restore.
     *
     * The wait for the previous IPI is INSIDE the region on purpose: the
     * delivery-status bit it polls is the same ICR, so observing it idle
     * and then being preempted before the write buys nothing. The waits
     * are on free-running counters (HourGlass reads TSC / PM-timer / PIT
     * counters directly), so none of them needs an interrupt to advance.
     *
     * The x2APIC branch above returns before this: one WRMSR carries
     * destination and command together and cannot be split. */
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    bool sent = icr_wait_for_previous(dest_id, cmd, may_refuse);
    if (sent) {
        lapic_write(LAPIC_REG_ICR_HIGH, dest_id << 24);
        lapic_write(LAPIC_REG_ICR_LOW,  cmd);
    }

    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
    return sent;
}

bool lapic_icr_write(uint32_t dest_id, uint32_t cmd) {
    return icr_send(dest_id, cmd, true);
}

void lapic_send_ipi(uint32_t dest_lapic_id, uint8_t vector) {
    /* icr_send picks the mode-correct path: xAPIC puts dest[7:0] in
     * ICR_HIGH[31:24], x2APIC the full 32-bit dest. No refusal is allowed
     * here — this function is void and so is every caller above it, so a
     * refused send would be a wake nobody is told about. */
    (void)icr_send(dest_lapic_id, (uint32_t)vector, false);
}

void lapic_send_ipi_all_excluding_self(uint8_t vector) {
    /* Shorthand bits 18-19 = 11b ("all excluding self") makes the LAPIC
     * ignore the destination field; pass dest=0 on both modes, the field
     * has the same layout in each. Same no-refusal rule as the directed
     * send above. */
    (void)icr_send(0u, (uint32_t)vector | (3u << 18), false);
}

/*
 * Translate ACPI 6.5 §5.2.12.5 MPS INTI Flags into LVT bits 13/15.
 *
 *   bits[1:0] Polarity:
 *     00 = conforms to bus     -> treat as active high (ISA default)
 *     01 = active high
 *     10 = reserved
 *     11 = active low
 *
 *   bits[3:2] Trigger Mode:
 *     00 = conforms to bus     -> NMI is edge-triggered by hardware design,
 *                                  so default to edge
 *     01 = edge
 *     10 = reserved
 *     11 = level
 *
 * For NMI the trigger should almost always be edge; some firmware
 * publishes "conforms" and trusts the OS to know that. We honour
 * whatever the firmware explicitly states.
 */
static uint32_t mps_flags_to_lvt(uint16_t mps_flags) {
    uint32_t lvt = 0;
    uint16_t polarity = mps_flags & 0x3;
    uint16_t trigger  = (mps_flags >> 2) & 0x3;

    if (polarity == 0x3) lvt |= LAPIC_LVT_PIN_POLARITY_LOW;
    /* 0x0 (conforms) and 0x1 (active high) map to "no polarity bit". */

    if (trigger == 0x3) lvt |= LAPIC_LVT_TRIGGER_LEVEL;
    /* 0x0 (conforms) and 0x1 (edge) map to edge — LVT bit 15 = 0. */

    return lvt;
}

void lapic_apply_madt_nmi(const struct madt_info *info,
                          uint8_t acpi_processor_id) {
    if (!lapic_enabled || !info) return;

    uint8_t applied[2] = { 0, 0 };  /* LINT0, LINT1 — track which we wrote */

    for (uint8_t i = 0; i < info->nmi_count; i++) {
        const madt_nmi_entry_t *e = &info->nmi[i];
        if (!e->valid) continue;
        if (e->acpi_processor_id != MADT_NMI_PROCESSOR_ALL &&
            e->acpi_processor_id != acpi_processor_id)
            continue;
        if (e->lint > 1) continue;

        uint32_t lvt = LAPIC_LVT_DELIVERY_NMI | mps_flags_to_lvt(e->mps_flags);
        uint32_t reg = (e->lint == 0) ? LAPIC_REG_LINT0_LVT
                                      : LAPIC_REG_LINT1_LVT;
        lapic_write(reg, lvt);
        applied[e->lint] = 1;

        debug_printf("[LAPIC] LINT%u programmed NMI (mps=0x%04x lvt=0x%08x) for proc=%u\n",
                     e->lint, e->mps_flags, lvt, e->acpi_processor_id);
    }

    if (!applied[0] && !applied[1]) {
        debug_printf("[LAPIC] No MADT NMI entry matched proc=%u (LINT0/LINT1 stay masked)\n",
                     acpi_processor_id);
    }
}
