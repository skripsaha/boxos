#include "pit.h"
#include "hpet.h"
#include "io.h"
#include "klib.h"
#include "irqchip.h"
#include "scheduler.h"  // For g_timer_frequency
#include "clockboard.h"

static volatile uint64_t pit_ticks = 0;
static uint32_t pit_frequency = 0;

/* Monotonic uptime in microseconds, advanced per tick by (1_000_000 / freq).
 * Decouples uptime from `pit_ticks * 1000 / current_freq` (which is NOT
 * monotonic when the scheduler reprograms the PIT — old ticks counted at
 * a slow rate get divided by a higher freq, yielding a smaller ms value
 * than a previous reading. The S1 underflow `elapsed=0xFFFF...` was this. */
static volatile uint64_t pit_uptime_us = 0;

void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0 || frequency_hz > PIT_FREQUENCY) {
        debug_printf("[PIT] Invalid frequency: %u Hz (max: %u Hz)\n", frequency_hz, PIT_FREQUENCY);
        return;
    }

    pit_frequency = frequency_hz;
    g_timer_frequency = frequency_hz;  // Update scheduler with actual frequency

    /* When HPET LegacyReplacement is sourcing IRQ0 we MUST NOT program
     * the 8254 — its counter output and HPET timer 0 would both clock
     * the IOAPIC's IRQ0 pin and the existing pit_tick() handler would
     * double-count. Leave the 8254 quiescent (no command byte, no
     * counter load) and just enable IRQ0 routing so the HPET-delivered
     * pulses reach the kernel. */
    if (hpet_tick_active()) {
        debug_printf("[PIT] HPET legacy tick active — leaving 8254 idle, "
                     "registering IRQ0 only (%u Hz logical)\n", pit_frequency);
        irqchip_enable_irq(0);
        return;
    }

    uint32_t divisor = PIT_FREQUENCY / frequency_hz;

    if (divisor > 65535) {
        divisor = 65535;
        pit_frequency = PIT_FREQUENCY / divisor;
        debug_printf("[PIT] WARNING: Frequency too low, adjusted to %u Hz\n", pit_frequency);
    }

    debug_printf("[PIT] Initializing timer: %u Hz (divisor=%u)\n", pit_frequency, divisor);

    // Channel 0, LSB+MSB access, Mode 2 (rate generator), binary
    uint8_t command = PIT_CMD_CHANNEL0 | PIT_CMD_RW_BOTH | PIT_CMD_MODE2 | PIT_CMD_BINARY;
    outb(PIT_COMMAND, command);

    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    irqchip_enable_irq(0);

    debug_printf("[PIT] Timer initialized: %u Hz (%u ticks/sec)\n", pit_frequency, pit_frequency);
}

void pit_set_frequency(uint32_t frequency_hz) {
    if (frequency_hz == 0 || frequency_hz > PIT_FREQUENCY)
        return;
    if (frequency_hz == pit_frequency)
        return;  // No change needed

    uint32_t divisor = PIT_FREQUENCY / frequency_hz;
    if (divisor > 65535)
        divisor = 65535;

    // Reprogram PIT Channel 0 on the fly
    // Channel 0, LSB+MSB access, Mode 2 (rate generator), binary
    uint8_t command = PIT_CMD_CHANNEL0 | PIT_CMD_RW_BOTH | PIT_CMD_MODE2 | PIT_CMD_BINARY;
    outb(PIT_COMMAND, command);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    pit_frequency = frequency_hz;
    g_timer_frequency = frequency_hz;
}

uint64_t pit_get_ticks(void) {
    /* Cross-core read of a counter the IRQ0 BSP increments. Aligned 64-bit
     * load is atomic on x86_64; explicit __atomic_load_n removes the formal
     * C11 data race with __atomic_fetch_add in pit_tick(). */
    return __atomic_load_n(&pit_ticks, __ATOMIC_RELAXED);
}

void pit_tick(void) {
    uint64_t new_ticks = __atomic_add_fetch(&pit_ticks, 1, __ATOMIC_RELAXED);

    /* Advance microsecond counter. freq may change between ticks (scheduler
     * reprograms PIT under load), but each tick contributes 1_000_000/freq
     * microseconds at the freq IT was issued at — preserving monotonicity. */
    uint32_t freq = pit_frequency;
    uint64_t new_us = 0;
    if (freq > 0) {
        new_us = __atomic_add_fetch(&pit_uptime_us,
                                     1000000ULL / (uint64_t)freq,
                                     __ATOMIC_RELAXED);
    } else {
        new_us = __atomic_load_n(&pit_uptime_us, __ATOMIC_RELAXED);
    }

    /* Mirror dynamic counters into the userspace-visible ClockBoard so a
     * read in userspace costs zero syscalls. Single-writer (this is the
     * BSP-only IRQ handler), so the plain stores inside the helper are
     * race-free against any cross-core reader. */
    clockboard_tick_update(new_us, new_ticks);
}

uint64_t pit_get_uptime_ms(void) {
    /* When HPET is online its 10 ns-class main counter is monotonic with
     * sub-microsecond resolution — use it transparently behind the
     * legacy PIT uptime API so every caller automatically gets the
     * higher-resolution clock. Falls back to PIT-accumulated us on
     * boards without HPET (pre-PIIX4 etc.). */
    if (hpet_is_present()) return hpet_now_us() / 1000ULL;
    return __atomic_load_n(&pit_uptime_us, __ATOMIC_RELAXED) / 1000ULL;
}

uint64_t pit_get_uptime_us(void) {
    if (hpet_is_present()) return hpet_now_us();
    return __atomic_load_n(&pit_uptime_us, __ATOMIC_RELAXED);
}

void pit_sleep_ms(uint32_t milliseconds) {
    if (pit_frequency == 0) {
        debug_printf("[PIT] ERROR: PIT not initialized!\n");
        return;
    }

    uint64_t ticks_to_wait = ((uint64_t)milliseconds * pit_frequency) / 1000;
    uint64_t start_tick = __atomic_load_n(&pit_ticks, __ATOMIC_RELAXED);
    uint64_t target_tick = start_tick + ticks_to_wait;

    while (__atomic_load_n(&pit_ticks, __ATOMIC_RELAXED) < target_tick) {
        asm volatile("hlt");
    }
}

uint32_t pit_get_frequency(void) {
    return pit_frequency;
}

// Read PIT channel 0 current counter value via latch command.
// Channel 0 is in mode 2 (rate generator): counts from divisor down to 1, then reloads.
static uint16_t pit_read_count(void) {
    // Latch channel 0 counter (command byte: channel=0, access=latch, mode/bcd ignored)
    outb(PIT_COMMAND, 0x00);
    uint8_t lo = inb(PIT_CHANNEL0);
    uint8_t hi = inb(PIT_CHANNEL0);
    return ((uint16_t)hi << 8) | lo;
}

// Hardware-accurate busy-wait using PIT counter readback.
// PIT crystal runs at exactly 1,193,182 Hz on all x86 hardware and is faithfully
// emulated by QEMU/VirtualBox/Bochs. No dependency on CPU frequency.
void pit_delay_busy(uint32_t milliseconds) {
    if (pit_frequency == 0) {
        debug_printf("[PIT] ERROR: PIT not initialized!\n");
        return;
    }

    // How many PIT ticks we need to wait
    // PIT_FREQUENCY = 1,193,182, so 1ms = 1193 ticks
    uint32_t target_ticks = (uint32_t)(((uint64_t)milliseconds * PIT_FREQUENCY) / 1000);

    // Divisor = how many ticks per full counter cycle (reload value)
    uint32_t divisor = PIT_FREQUENCY / pit_frequency;

    uint32_t elapsed = 0;
    uint16_t last_count = pit_read_count();

    while (elapsed < target_ticks) {
        uint16_t current = pit_read_count();

        if (current <= last_count) {
            // Normal countdown: elapsed ticks = last - current
            elapsed += (last_count - current);
        } else {
            // Counter wrapped past reload point: last -> 0 -> divisor -> current
            elapsed += (last_count + (divisor - current));
        }

        last_count = current;
    }
}
