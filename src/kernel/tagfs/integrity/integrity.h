#ifndef TAGFS_INTEGRITY_H
#define TAGFS_INTEGRITY_H

#include "../../lib/kernel/ktypes.h"
#include "../../core/error/error.h"


error_t  IntegrityInit(void);
void     IntegrityShutdown(void);

void     IntegrityUpdate(uint32_t block, const void *data);

bool     IntegrityVerify(uint32_t block, const void *data);

error_t  IntegrityFlush(void);

void     IntegrityDrainReports(void);

bool     IntegrityIsInitialized(void);
uint32_t IntegrityErrorCount(void);

void     IntegrityMarkMapBlocks(uint8_t *computed_bm, uint32_t total_blocks);

#endif