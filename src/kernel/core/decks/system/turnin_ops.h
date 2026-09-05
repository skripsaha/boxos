#ifndef TURNIN_OPS_H
#define TURNIN_OPS_H

#include "error.h"

/* Register SYSTEM_OP_TURN_IN with the op registry. Called from
 * SystemDeckRegister alongside TouchOpsRegister / SyncOpsRegister. */
error_t TurnInOpsRegister(void);

#endif /* TURNIN_OPS_H */
