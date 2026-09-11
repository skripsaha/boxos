#ifndef SYSTEM_HALT_H
#define SYSTEM_HALT_H

#include "ktypes.h"

void system_halt(bool reboot) __attribute__((noreturn));

#endif