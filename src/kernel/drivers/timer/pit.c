#include "pit.h"
#include "io.h"
#include "klib.h"
#include "pic.h"

static volatile uint64_t pit_ticks = 0;
static uint32_t pit_frequency = 0;

void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0 || frequency_hz > PIT_FREQUENCY) {
        debug_printf("[PIT] Invalid frequency: %u Hz (max: %u Hz)\n", frequency_hz, PIT_FREQUENCY);
        return;
    }

    pit_frequency = frequency_hz;

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

    pic_enable_irq(0);

    debug_printf("[PIT] Timer initialized: %u Hz (%u ticks/sec)\n", pit_frequency, pit_frequency);
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}

void pit_tick(void) {
    pit_ticks++;
}

void pit_sleep_ms(uint32_t milliseconds) {
    if (pit_frequency == 0) {
        debug_printf("[PIT] ERROR: PIT not initialized!\n");
        return;
    }

    uint64_t ticks_to_wait = ((uint64_t)milliseconds * pit_frequency) / 1000;
    uint64_t start_tick = pit_ticks;
    uint64_t target_tick = start_tick + ticks_to_wait;

    while (pit_ticks < target_tick) {
        asm volatile("hlt");
    }
}

uint32_t pit_get_frequency(void) {
    return pit_frequency;
}

// Busy-wait using port 0x80 (~1us per access). Works without interrupts, used for TSC calibration.
void pit_delay_busy(uint32_t milliseconds) {
    if (pit_frequency == 0) {
        debug_printf("[PIT] ERROR: PIT not initialized!\n");
        return;
    }

    uint32_t iterations = milliseconds * 1000;
    for (uint32_t i = 0; i < iterations; i++) {
        asm volatile("outb %%al, $0x80" : : "a"(0));
    }
}
