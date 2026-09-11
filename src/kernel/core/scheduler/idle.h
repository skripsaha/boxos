#ifndef IDLE_H
#define IDLE_H

#include "ktypes.h"
#include "process.h"

#define IDLE_PID 0

void idle_process_init(void);

void idle_process_init_core(uint8_t core_index);

process_t *idle_process_get(void);

bool process_is_idle(process_t *proc);

void idle_loop(void);

void cpu_idle(void);

#endif