#ifndef IDLE_H
#define IDLE_H

#include "ktypes.h"
#include "process.h"

#define IDLE_PID 0

// Initialize idle process (called from main.c)
void idle_process_init(void);

// Get pointer to idle process for scheduler
process_t* idle_process_get(void);

// Check if a process is the idle process
bool process_is_idle(process_t* proc);

#endif // IDLE_H
