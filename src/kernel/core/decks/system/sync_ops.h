#ifndef SYNC_OPS_H
#define SYNC_OPS_H

#include "error.h"
#include "ktypes.h"

struct process_t;

/* Register SYSTEM_OP_ADDR_PARK / SYSTEM_OP_ADDR_WAKE with the op registry
 * and initialise the AddrWaitTable. Called from SystemDeckRegister. */
error_t SyncOpsRegister(void);

/* The deadline baton's continuation: deliver an expired park's ERR_TIMEOUT
 * Result from a K-Core. ctx is the process (the pass holds a ref, released
 * here). Claims the waiter's addr_wait_entry by its state — linked, timed,
 * deadline passed — and on a win KResultPushes ERR_TIMEOUT (the PROC_WAITING
 * reschedule already happened in the IRQ). On a lost claim it does nothing.
 * See sync_ops.c. */
void SyncTimeoutDeliver(void *ctx);

/* Answer every waiter parked on this process being gone, carrying the exit
 * disposition (proc_exit.h). Called from TouchCleanupProcess — the single
 * commit point of a death — AFTER its exactly-once claim, which is what makes
 * "sleep past your own answer" impossible: a waiter reads that same claim
 * under gone_lock and either sees the death or is already on the list this
 * drains. Unlike the process:died multicast alongside it, this debt is owed
 * to a named waiter and is never dropped. */
void ProcessGoneDeliver(struct process_t *proc, int32_t exit_code);

void AddrWaitSelfTest(void);

#endif /* SYNC_OPS_H */
