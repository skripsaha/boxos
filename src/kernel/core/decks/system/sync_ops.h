#ifndef SYNC_OPS_H
#define SYNC_OPS_H

#include "error.h"

/* Register SYSTEM_OP_ADDR_PARK / SYSTEM_OP_ADDR_WAKE with the op registry
 * and initialise the AddrWaitTable. Called from SystemDeckRegister. */
error_t SyncOpsRegister(void);

/* irq_defer bottom-half: deliver an expired park timeout's ERR_TIMEOUT Result
 * from a K-Core. ctx packs (wait_seq << 32 | pid). Re-resolves the process,
 * seq-gate-claims the waiter's addr_wait_entry, and on a win KResultPushes
 * ERR_TIMEOUT (the PROC_WAITING reschedule already happened in the IRQ). On a
 * lost/stale claim it does nothing. See sync_ops.c. */
void SyncTimeoutDeliver(void *ctx);

void AddrWaitSelfTest(void);

#endif /* SYNC_OPS_H */
