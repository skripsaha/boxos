#include "pit.h"
#include "io.h"
#include "klib.h"
#include "pic.h"

// Global tick counter (updated by IRQ 0 handler)
static volatile uint64_t pit_ticks = 0;
static uint32_t pit_frequency = 0;

// Initialize PIT to generate interrupts at specified frequency
void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0 || frequency_hz > PIT_FREQUENCY) {
        debug_printf("[PIT] Invalid frequency: %u Hz (max: %u Hz)\n", frequency_hz, PIT_FREQUENCY);
        return;
    }

    pit_frequency = frequency_hz;

    // Calculate divisor for desired frequency
    // Divisor = PIT_FREQUENCY / desired_frequency
    uint32_t divisor = PIT_FREQUENCY / frequency_hz;

    if (divisor > 65535) {
        divisor = 65535; // Max 16-bit value
        pit_frequency = PIT_FREQUENCY / divisor;
        debug_printf("[PIT] WARNING: Frequency too low, adjusted to %u Hz\n", pit_frequency);
    }

    debug_printf("[PIT] Initializing timer: %u Hz (divisor=%u)\n", pit_frequency, divisor);

    // Send command byte: Channel 0, Access mode (LSB+MSB), Mode 2 (rate generator), Binary
    uint8_t command = PIT_CMD_CHANNEL0 | PIT_CMD_RW_BOTH | PIT_CMD_MODE2 | PIT_CMD_BINARY;
    outb(PIT_COMMAND, command);

    // Send divisor (LSB first, then MSB)
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));        // Low byte
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF)); // High byte

    // Enable Timer IRQ (IRQ 0) in PIC
    pic_enable_irq(0);

    debug_printf("[PIT] Timer initialized: %u Hz (%u ticks/sec)\n", pit_frequency, pit_frequency);
}

// Get current tick count
uint64_t pit_get_ticks(void) {
    return pit_ticks;
}

// Called by IRQ 0 handler to increment tick counter
void pit_tick(void) {
    pit_ticks++;
}

// Sleep for specified milliseconds (busy wait using PIT ticks)
void pit_sleep_ms(uint32_t milliseconds) {
    if (pit_frequency == 0) {
        debug_printf("[PIT] ERROR: PIT not initialized!\n");
        return;
    }

    // Calculate how many ticks we need to wait
    // ticks = (milliseconds * frequency) / 1000
    uint64_t ticks_to_wait = ((uint64_t)milliseconds * pit_frequency) / 1000;
    uint64_t start_tick = pit_ticks;
    uint64_t target_tick = start_tick + ticks_to_wait;

    // Busy wait until target tick is reached
    while (pit_ticks < target_tick) {
        asm volatile("hlt"); // Wait for interrupt (saves power)
    }
}

// Get frequency in Hz
uint32_t pit_get_frequency(void) {
    return pit_frequency;
}

// ============================================================================
// BUSY-WAIT DELAY (WORKS WITHOUT INTERRUPTS!)
// ============================================================================

// This function provides accurate delays WITHOUT requiring interrupts
// Perfect for CPU calibration before interrupt enabling
void pit_delay_busy(uint32_t milliseconds) {
    if (pit_frequency == 0) {
        debug_printf("[PIT] ERROR: PIT not initialized!\n");
        return;
    }

    // We'll use a simple busy loop based on I/O port delays
    // Each inb() takes ~1μs on typical hardware
    // So for 1ms we need ~1000 iterations
    // This is approximate but good enough for calibration

    uint32_t iterations = milliseconds * 1000;  // ~1μs per iteration
    for (uint32_t i = 0; i < iterations; i++) {
        // Read from an unused port to create delay
        // Port 0x80 is typically used for POST codes and creates ~1μs delay
        asm volatile("outb %%al, $0x80" : : "a"(0));
    }
}
