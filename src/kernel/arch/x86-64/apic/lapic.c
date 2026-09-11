#include "lapic.h"
#include "irqchip.h"
#include "klib.h"
#include "vmm.h"
#include "acpi_madt.h"
#include "cpu_calibrate.h"
#include "atomics.h"
#include "cpuid.h"

static volatile uint32_t* lapic_base_virt = NULL;
static uintptr_t lapic_base_phys = 0;
static bool lapic_enabled = false;

static bool g_x2apic_active = false;

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

uint32_t lapic_read(uint32_t reg) {
    return lapic_base_virt[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t value) {
    lapic_base_virt[reg / 4] = value;
}

void lapic_init(uintptr_t base_addr) {
    lapic_base_phys = base_addr;

    debug_printf("[LAPIC] Initializing Local APIC at phys 0x%lx\n", base_addr);

    lapic_base_virt = (volatile uint32_t*)vmm_map_mmio(
        base_addr, 4096,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_CACHE_DISABLE
    );

    if (!lapic_base_virt) {
        debug_printf("[LAPIC] %[E]Failed to map LAPIC MMIO%[D]\n");
        return;
    }

    uint64_t apic_base_msr = rdmsr(MSR_APIC_BASE);
    apic_base_msr |= MSR_APIC_BASE_ENABLE;
    wrmsr(MSR_APIC_BASE, apic_base_msr);

    g_x2apic_active = (rdmsr(MSR_APIC_BASE) & MSR_APIC_BASE_EXTD) != 0;

    uint32_t svr = lapic_read(LAPIC_REG_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr = (svr & ~0xFF) | LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_REG_SVR, svr);

    lapic_write(LAPIC_REG_TPR, 0);

    lapic_write(LAPIC_REG_LINT0_LVT, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LINT1_LVT, LAPIC_LVT_MASKED);

    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_LVT_MASKED);

    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_ESR, 0);

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
    if (lapic_base_virt) {
        lapic_write(LAPIC_REG_EOI, 0);
    }
}

uint32_t lapic_get_id(void) {
    if (g_x2apic_active) {
        return (uint32_t)rdmsr(MSR_X2APIC_APICID);
    }
    return (lapic_read(LAPIC_REG_ID) >> 24) & 0xFF;
}

void lapic_enable(void) {
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

bool lapic_is_mapped(void) {
    return lapic_base_virt != NULL;
}

void lapic_timer_init(uint8_t vector, uint32_t frequency_hz) {
    debug_printf("[LAPIC] Calibrating APIC timer for %u Hz...\n", frequency_hz);

    if (g_cpu_caps.has_tsc_deadline && g_cpu_caps.has_invariant_tsc) {
        uint64_t cyc_per_sec = cpu_get_tsc_freq_khz() * 1000ULL;
        g_lapic_tsc_period = (frequency_hz > 0) ? (cyc_per_sec / frequency_hz)
                                                : cyc_per_sec;
        if (g_lapic_tsc_period == 0)
            g_lapic_tsc_period = cyc_per_sec;
        g_lapic_tsc_deadline = true;

        lapic_write(LAPIC_REG_TIMER_LVT, vector | LAPIC_LVT_TIMER_TSC_DEADLINE);
        __asm__ volatile("mfence" ::: "memory");
        wrmsr(MSR_IA32_TSC_DEADLINE, rdtsc() + g_lapic_tsc_period);

        debug_printf("[LAPIC] Timer configured: vector=%u, %u Hz TSC-deadline\n",
                     vector, frequency_hz);
        return;
    }

    #define LAPIC_CAL_MS 10u

    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_16);

    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    uint64_t tsc_deadline = rdtsc() + cpu_ms_to_tsc(LAPIC_CAL_MS);
    while (rdtsc() < tsc_deadline)
        __asm__ volatile("pause");

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_REG_TIMER_CCR);

    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_LVT_MASKED);

    uint32_t ticks_per_sec    = elapsed * (1000u / LAPIC_CAL_MS);
    uint32_t ticks_per_period = (frequency_hz > 0) ? (ticks_per_sec / frequency_hz) : 0;
    if (ticks_per_period == 0)
        ticks_per_period = 1;

    debug_printf("[LAPIC] Timer: %u ticks/%ums, %u ticks/s, period=%u ticks\n",
                 elapsed, LAPIC_CAL_MS, ticks_per_sec, ticks_per_period);

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
    if (g_lapic_tsc_deadline)
        wrmsr(MSR_IA32_TSC_DEADLINE, rdtsc() + g_lapic_tsc_period);
}

bool lapic_is_x2apic_active(void) {
    return g_x2apic_active;
}

void lapic_icr_write(uint32_t dest_id, uint32_t cmd) {
    if (g_x2apic_active) {
        wrmsr(MSR_X2APIC_ICR, ((uint64_t)dest_id << 32) | (uint64_t)cmd);
        return;
    }

    for (uint32_t spins = 0; spins < 100000u; spins++) {
        if (!(lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_SEND_PENDING)) break;
        __asm__ volatile("pause");
    }
    lapic_write(LAPIC_REG_ICR_HIGH, (dest_id & 0xFFu) << 24);
    lapic_write(LAPIC_REG_ICR_LOW,  cmd);
}

void lapic_send_ipi(uint32_t dest_lapic_id, uint8_t vector) {
    lapic_icr_write(dest_lapic_id, (uint32_t)vector);
}

void lapic_send_ipi_all_excluding_self(uint8_t vector) {
    lapic_icr_write(0u, (uint32_t)vector | (3u << 18));
}

static uint32_t mps_flags_to_lvt(uint16_t mps_flags) {
    uint32_t lvt = 0;
    uint16_t polarity = mps_flags & 0x3;
    uint16_t trigger  = (mps_flags >> 2) & 0x3;

    if (polarity == 0x3) lvt |= LAPIC_LVT_PIN_POLARITY_LOW;

    if (trigger == 0x3) lvt |= LAPIC_LVT_TRIGGER_LEVEL;

    return lvt;
}

void lapic_apply_madt_nmi(const struct madt_info *info,
                          uint8_t acpi_processor_id) {
    if (!lapic_enabled || !info) return;

    uint8_t applied[2] = { 0, 0 };

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