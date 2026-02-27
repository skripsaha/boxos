#ifndef PIT_H
#define PIT_H

#include "ktypes.h"

// PIT (Programmable Interval Timer) - Intel 8253/8254
// Base frequency: 1.193182 MHz (1193182 Hz)

#define PIT_FREQUENCY    1193182  // Base PIT frequency in Hz
#define PIT_CHANNEL0     0x40     // Channel 0 data port (system timer)
#define PIT_CHANNEL1     0x41     // Channel 1 data port (unused)
#define PIT_CHANNEL2     0x42     // Channel 2 data port (PC speaker)
#define PIT_COMMAND      0x43     // Command register

// Command byte format:
// Bits 7-6: Channel select (00=Ch0, 01=Ch1, 10=Ch2, 11=Read-back)
// Bits 5-4: Access mode (01=LSB only, 10=MSB only, 11=LSB then MSB)
// Bits 3-1: Operating mode (000=Mode 0, 010=Mode 2, 011=Mode 3, etc)
// Bit 0:    BCD mode (0=binary, 1=BCD)

#define PIT_CMD_BINARY   0x00     // Binary mode (not BCD)
#define PIT_CMD_MODE2    0x04     // Mode 2: Rate Generator
#define PIT_CMD_MODE3    0x06     // Mode 3: Square Wave Generator
#define PIT_CMD_RW_BOTH  0x30     // Read/Write LSB then MSB
#define PIT_CMD_CHANNEL0 0x00     // Select Channel 0

// Initialize PIT to generate interrupts at specified frequency
void pit_init(uint32_t frequency_hz);

// Increment tick counter (called from IRQ 0 handler)
void pit_tick(void);

// Get current tick count (updated by IRQ 0 handler)
uint64_t pit_get_ticks(void);

// Sleep for specified number of milliseconds (busy wait)
void pit_sleep_ms(uint32_t milliseconds);

// Busy-wait delay using PIT (works WITHOUT interrupts!)
// Used for calibration before interrupts are enabled
void pit_delay_busy(uint32_t milliseconds);

// Get PIT frequency
uint32_t pit_get_frequency(void);

#endif // PIT_H
