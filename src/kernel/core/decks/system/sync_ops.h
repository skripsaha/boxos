#ifndef SYNC_OPS_H
#define SYNC_OPS_H

#include "error.h"
#include "ktypes.h"

struct process_t;

error_t SyncOpsRegister(void);

void SyncTimeoutDeliver(void *ctx);

void ProcessGoneDeliver(struct process_t *proc, int32_t exit_code);

void AddrWaitSelfTest(void);

#endif