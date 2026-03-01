#ifndef PIT_H
#define PIT_H

#include "ktypes.h"

// Intel 8253/8254 PIT base frequency: 1.193182 MHz
#define PIT_FREQUENCY    1193182
#define PIT_CHANNEL0     0x40
#define PIT_CHANNEL1     0x41
#define PIT_CHANNEL2     0x42   // PC speaker
#define PIT_COMMAND      0x43

// Command byte bits [7:6]=channel [5:4]=access [3:1]=mode [0]=BCD
#define PIT_CMD_BINARY   0x00
#define PIT_CMD_MODE2    0x04   // Rate Generator
#define PIT_CMD_MODE3    0x06   // Square Wave Generator
#define PIT_CMD_RW_BOTH  0x30   // Read/Write LSB then MSB
#define PIT_CMD_CHANNEL0 0x00

void pit_init(uint32_t frequency_hz);
void pit_tick(void);
uint64_t pit_get_ticks(void);
void pit_sleep_ms(uint32_t milliseconds);
// Busy-wait that works without interrupts (for TSC calibration)
void pit_delay_busy(uint32_t milliseconds);
uint32_t pit_get_frequency(void);

#endif // PIT_H
