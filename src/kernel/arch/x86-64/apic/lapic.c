#include "lapic.h"
#include "irqchip.h"
#include "io.h"
#include "klib.h"
#include "vmm.h"

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
