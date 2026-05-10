#include "lapic.h"
#include "irqchip.h"
#include "io.h"
#include "klib.h"
#include "vmm.h"
#include "acpi_madt.h"

static volatile uint32_t* lapic_base_virt = NULL;
static uintptr_t lapic_base_phys = 0;
static bool lapic_enabled = false;

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

    // Map LAPIC MMIO region (4KB, uncacheable)
    lapic_base_virt = (volatile uint32_t*)vmm_map_mmio(
        base_addr, 4096,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_CACHE_DISABLE
    );

    if (!lapic_base_virt) {
        debug_printf("[LAPIC] %[E]Failed to map LAPIC MMIO%[D]\n");
        return;
    }

    // Enable LAPIC via MSR (set global enable bit)
    uint64_t apic_base_msr = rdmsr(MSR_APIC_BASE);
    apic_base_msr |= MSR_APIC_BASE_ENABLE;
    wrmsr(MSR_APIC_BASE, apic_base_msr);

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
    if (lapic_base_virt) {
        lapic_write(LAPIC_REG_EOI, 0);
    }
}

uint32_t lapic_get_id(void) {
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

void lapic_timer_init(uint8_t vector, uint32_t frequency_hz) {
    debug_printf("[LAPIC] Calibrating APIC timer for %u Hz...\n", frequency_hz);

    // Use PIT channel 2 for calibration (one-shot, ~10ms)
    // PIT frequency = 1193182 Hz, count for 10ms = 11932
    #define PIT_CALIBRATION_TICKS 11932
    #define PIT_CALIBRATION_MS    10

    // Set timer divider to 16
    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_16);

    // Setup PIT channel 2 for one-shot calibration
    outb(0x61, (inb(0x61) & 0xFD) | 0x01);    // Gate high, speaker off
    outb(0x43, 0xB0);                           // Channel 2, lobyte/hibyte, one-shot
    outb(0x42, PIT_CALIBRATION_TICKS & 0xFF);
    outb(0x42, (PIT_CALIBRATION_TICKS >> 8) & 0xFF);

    // Reset PIT one-shot counter
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp & 0xFE);
    outb(0x61, tmp | 0x01);

    // Start LAPIC timer with max count
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    // Wait for PIT to finish (bit 5 of port 0x61 goes high)
    while (!(inb(0x61) & 0x20)) {
        __asm__ volatile("pause");
    }

    // Read how many LAPIC ticks elapsed
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_REG_TIMER_CCR);

    // Stop timer
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_LVT_MASKED);

    // Calculate ticks per desired period
    // elapsed ticks in 10ms, so ticks_per_second = elapsed * 100
    uint32_t ticks_per_sec = elapsed * (1000 / PIT_CALIBRATION_MS);
    uint32_t ticks_per_period = ticks_per_sec / frequency_hz;

    debug_printf("[LAPIC] Timer: %u ticks/10ms, %u ticks/s, period=%u ticks\n",
                 elapsed, ticks_per_sec, ticks_per_period);

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

void lapic_send_ipi(uint8_t dest_lapic_id, uint8_t vector) {
    while (lapic_read(LAPIC_REG_ICR_LOW) & (1 << 12)) {
        __asm__ volatile("pause");
    }
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)dest_lapic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, (uint32_t)vector);
}

void lapic_send_ipi_all_excluding_self(uint8_t vector) {
    while (lapic_read(LAPIC_REG_ICR_LOW) & (1 << 12)) {
        __asm__ volatile("pause");
    }
    lapic_write(LAPIC_REG_ICR_HIGH, 0);
    lapic_write(LAPIC_REG_ICR_LOW, (uint32_t)vector | (3 << 18));
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
