#include "pit.h"
#include "idt.h"   /* irq_register_handler + the kernel's own answerers */
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

    /* When HPET LegacyReplacement is sourcing IRQ0 there is nothing here
     * for the 8254 to do. HPET 1.0a §2.4.2.1, Mapping Option #1: with
     * LEG_RT_CNF set, timer 0 is forced onto IRQ0 (INTI2 through the I/O
     * APIC) and "the 8254 timer will not cause any interrupts" — the part
     * is disconnected from the pin, so programming it could not
     * double-count the tick as this comment used to claim; it simply would
     * not reach the interrupt the kernel receives. We leave the 8254 as
     * firmware left it — which is also why pit_read_count() has no reload
     * value of ours to divide by in this mode, see pit_delay_busy() — and
     * only enable IRQ0 routing so the HPET-delivered pulses arrive. */
    if (hpet_tick_active()) {
        debug_printf("[PIT] HPET legacy tick active — leaving 8254 idle, "
                     "registering IRQ0 only (%u Hz logical)\n", pit_frequency);
        irq_register_handler(0, IrqTheTickAnswers, RIGHT_OF_WAY_TIME_IS_LOST);
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

    /* The tick answers its own line now, through the same registry every
     * other line uses. It used to be dispatched by `case 0` of a switch on
     * `vector - 32` inside irq_handler, which stopped being a way to find it
     * the moment a vector stopped being 32 + its ISA number. */
    irq_register_handler(0, IrqTheTickAnswers, RIGHT_OF_WAY_TIME_IS_LOST);
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
    __atomic_add_fetch(&pit_ticks, 1, __ATOMIC_RELAXED);

    /* Advance microsecond counter. freq may change between ticks (scheduler
     * reprograms PIT under load), but each tick contributes 1_000_000/freq
     * microseconds at the freq IT was issued at — preserving monotonicity.
     * This accumulator is the machine's clock only on a board with no HPET;
     * where an HPET exists, pit_get_uptime_us() reads the counter instead and
     * this total is never consulted. */
    uint32_t freq = pit_frequency;
    if (freq > 0) {
        __atomic_add_fetch(&pit_uptime_us,
                           1000000ULL / (uint64_t)freq,
                           __ATOMIC_RELAXED);
    }

    /* Refresh the page userspace reads.
     *
     * It is deliberately NOT handed the accumulator above. That counter only
     * starts moving when IRQ0 starts arriving, and on a board where the HPET
     * carries the clock the kernel has already been up for the whole boot by
     * then — twelve seconds on the 2026-09-20 i5-9400F run. The page said one
     * thing and the same question asked through the deck said another, because
     * one fact had two carriers with two different zeros. The publisher now
     * reads the kernel's one clock itself, so there is no number left to pass
     * in and nowhere for a second answer to come from. */
    clockboard_publish_uptime();
}

uint64_t pit_get_uptime_ms(void) {
    /* When HPET is online its main counter is monotonic and finer than a
     * microsecond — use it transparently behind the legacy PIT uptime
     * API so every caller automatically gets the higher-resolution
     * clock. Falls back to PIT-accumulated us on boards without HPET
     * (pre-PIIX4 etc.).
     *
     * The tick is not the "10 ns" this comment used to name: that is
     * QEMU's period and no shipping part's. The controller states its
     * own period in femtoseconds in GCAP_ID[63:32], HPET 1.0a §2.3.4
     * permits anything up to 0x05F5E100 fs (10^8 fs = 100 ns), and
     * every IA-PC part carries the canonical 69,841,279 fs — 69.84 ns
     * at 14.31818 MHz. Sub-microsecond is true of all of them, which is
     * the whole of what this choice rests on; a named tick length is
     * not, and hpet_now_us() divides by the reported period rather than
     * by any constant (see hpet.c, hpet_ticks_to_us). */
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
//
// When HPET LegacyReplacement is active, pit_init() never programmed the
// 8254: channel 0 holds whatever reload value firmware left it with, so
// the `divisor` computed below is not the one the counter is actually
// counting down from and `elapsed` accumulates nonsense — delays of
// microseconds instead of the requested milliseconds, or none at all if
// the firmware left the channel stopped. In that mode we delegate to the
// HPET-counter busy-wait, which is always reliable. This single chokepoint
// keeps every existing `pit_delay_busy()` caller correct on modern
// hardware that ships without a functional 8254.
void pit_delay_busy(uint32_t milliseconds) {
    if (hpet_tick_active() && hpet_is_present()) {
        hpet_busy_wait_us((uint64_t)milliseconds * 1000ULL);
        return;
    }
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
