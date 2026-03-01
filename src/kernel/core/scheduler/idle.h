#ifndef IDLE_H
#define IDLE_H

#include "ktypes.h"
#include "process.h"

#define IDLE_PID 0

void idle_process_init(void);
process_t* idle_process_get(void);
bool process_is_idle(process_t* proc);

#endif // IDLE_H
