#ifndef PIT_H
#define PIT_H

#include "ktypes.h"

#define PIT_FREQUENCY    1193182
#define PIT_CHANNEL0     0x40
#define PIT_CHANNEL1     0x41
#define PIT_CHANNEL2     0x42
#define PIT_COMMAND      0x43

#define PIT_CMD_BINARY   0x00
#define PIT_CMD_MODE2    0x04
#define PIT_CMD_MODE3    0x06
#define PIT_CMD_RW_BOTH  0x30
#define PIT_CMD_CHANNEL0 0x00

void pit_init(uint32_t frequency_hz);
void pit_set_frequency(uint32_t frequency_hz);
void pit_tick(void);
uint64_t pit_get_ticks(void);
uint64_t pit_get_uptime_ms(void);
uint64_t pit_get_uptime_us(void);
void pit_sleep_ms(uint32_t milliseconds);
void pit_delay_busy(uint32_t milliseconds);
uint32_t pit_get_frequency(void);

#endif