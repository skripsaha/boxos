#ifndef ALARM_OPS_H
#define ALARM_OPS_H

#include "error.h"

struct process_t;

/* Register SYSTEM_OP_ALARM with the op registry. Called from
 * SystemDeckRegister alongside TurnInOpsRegister. */
error_t AlarmOpsRegister(void);

/* A strand's alarm node has come out of the touch queue, in the tick: pass the
 * strand's own baton to a K-Core, which delivers the Touch. Takes no lock and
 * allocates nothing. */
void AlarmFire(struct process_t *target);

#endif /* ALARM_OPS_H */
