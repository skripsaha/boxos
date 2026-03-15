#ifndef IDLE_H
#define IDLE_H

#include "ktypes.h"
#include "process.h"

#define IDLE_PID 0

// Initialize BSP idle process (PID 0).  Called once from kernel_main().
void idle_process_init(void);

// Initialize an idle process for an AP core.  Called from ap_entry_c().
void idle_process_init_core(uint8_t core_index);

// Return the idle process for the calling core.
process_t* idle_process_get(void);

bool process_is_idle(process_t* proc);

#endif // IDLE_H
