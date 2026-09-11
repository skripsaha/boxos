#include "pit.h"
#include "hpet.h"
#include "io.h"
#include "klib.h"
#include "irqchip.h"
#include "scheduler.h"
#include "clockboard.h"

static volatile uint64_t pit_ticks = 0;
static uint32_t pit_frequency = 0;

static volatile uint64_t pit_uptime_us = 0;

void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0 || frequency_hz > PIT_FREQUENCY) {
        debug_printf("[PIT] Invalid frequency: %u Hz (max: %u Hz)\n", frequency_hz, PIT_FREQUENCY);
        return;
    }

    pit_frequency = frequency_hz;
    g_timer_frequency = frequency_hz;

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
        return;

    uint32_t divisor = PIT_FREQUENCY / frequency_hz;
    if (divisor > 65535)
        divisor = 65535;

    uint8_t command = PIT_CMD_CHANNEL0 | PIT_CMD_RW_BOTH | PIT_CMD_MODE2 | PIT_CMD_BINARY;
    outb(PIT_COMMAND, command);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    pit_frequency = frequency_hz;
    g_timer_frequency = frequency_hz;
}

uint64_t pit_get_ticks(void) {
    return __atomic_load_n(&pit_ticks, __ATOMIC_RELAXED);
}

void pit_tick(void) {
    uint64_t new_ticks = __atomic_add_fetch(&pit_ticks, 1, __ATOMIC_RELAXED);

    uint32_t freq = pit_frequency;
    uint64_t new_us = 0;
    if (freq > 0) {
        new_us = __atomic_add_fetch(&pit_uptime_us,
                                     1000000ULL / (uint64_t)freq,
                                     __ATOMIC_RELAXED);
    } else {
        new_us = __atomic_load_n(&pit_uptime_us, __ATOMIC_RELAXED);
    }

    clockboard_tick_update(new_us, new_ticks);
}

uint64_t pit_get_uptime_ms(void) {
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

static uint16_t pit_read_count(void) {
    outb(PIT_COMMAND, 0x00);
    uint8_t lo = inb(PIT_CHANNEL0);
    uint8_t hi = inb(PIT_CHANNEL0);
    return ((uint16_t)hi << 8) | lo;
}

void pit_delay_busy(uint32_t milliseconds) {
    if (hpet_tick_active() && hpet_is_present()) {
        hpet_busy_wait_us((uint64_t)milliseconds * 1000ULL);
        return;
    }
    if (pit_frequency == 0) {
        debug_printf("[PIT] ERROR: PIT not initialized!\n");
        return;
    }

    uint32_t target_ticks = (uint32_t)(((uint64_t)milliseconds * PIT_FREQUENCY) / 1000);

    uint32_t divisor = PIT_FREQUENCY / pit_frequency;

    uint32_t elapsed = 0;
    uint16_t last_count = pit_read_count();

    while (elapsed < target_ticks) {
        uint16_t current = pit_read_count();

        if (current <= last_count) {
            elapsed += (last_count - current);
        } else {
            elapsed += (last_count + (divisor - current));
        }

        last_count = current;
    }
}