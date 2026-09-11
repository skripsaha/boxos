#ifndef SYSTEM_DECK_H
#define SYSTEM_DECK_H

#include "ktypes.h"
#include "error.h"
#include "boxos_decks.h"

typedef struct process_t process_t;


#define EFI_INFO_BLOB_SIZE        128u
#define EFI_INFO_VERSION          1u

uint64_t ipc_copy_to_heap(process_t *sender, process_t *target,
                          uint64_t src_addr, uint32_t length);

uint64_t cabin_heap_deposit(process_t *target, const void *kbuf, uint32_t length);

error_t SystemDeckRegister(void);

error_t ProcAuthSelfTest(void);

error_t TouchOpsRegister(void);

error_t BayOpsRegister(void);

error_t BrookOpsRegister(void);


error_t MemTagOpsRegister(void);

error_t HwOpsRegister(void);

error_t SyncOpsRegister(void);

error_t TurnInOpsRegister(void);

#endif