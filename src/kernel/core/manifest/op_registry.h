#ifndef OP_REGISTRY_H
#define OP_REGISTRY_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"


struct process_t;

typedef struct OpContext {
    struct process_t *proc;
    uint32_t          target_pid;
    uint32_t          flags;
    uint32_t          submit_cookie;
    uint16_t          pier_id;
    uint16_t          crate_count;
    uint64_t          crates_uaddr;
    bool             *async_owns_crates;
} OpContext;

typedef int (*OpHandler)(const ManifestOp *op,
                         Crate            *crates,
                         uint16_t          crate_count,
                         const OpContext  *ctx);

typedef struct OpRegistration {
    uint32_t       op_kind;
    uint32_t       security_mask;
    OpHandler      handler;
    const char    *name;
} OpRegistration;

error_t OpRegistryInit(void);
void    OpRegistryShutdown(void);

error_t OpRegistryRegister(uint32_t    op_kind,
                           OpHandler   handler,
                           uint32_t    security_mask,
                           const char *name);

const OpRegistration *OpRegistryLookup(uint32_t op_kind);

uint32_t OpRegistryCount(void);
void     OpRegistryDump(void);

#endif