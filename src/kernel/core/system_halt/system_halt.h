#ifndef SYSTEM_HALT_H
#define SYSTEM_HALT_H

#include "ktypes.h"

// Unified system shutdown sequence.
// If `reboot` is true, reset CPU after cleanup. Otherwise, power off via ACPI.
// This function never returns.
void system_halt(bool reboot) __attribute__((noreturn));

#endif // SYSTEM_HALT_H
