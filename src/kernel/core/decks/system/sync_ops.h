#ifndef SYNC_OPS_H
#define SYNC_OPS_H

#include "error.h"

/* Register SYSTEM_OP_ADDR_PARK / SYSTEM_OP_ADDR_WAKE with the op registry
 * and initialise the AddrWaitTable. Called from SystemDeckRegister. */
error_t SyncOpsRegister(void);

void AddrWaitSelfTest(void);

#endif /* SYNC_OPS_H */
