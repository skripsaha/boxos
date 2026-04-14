#ifndef KRESULT_H
#define KRESULT_H

#include "ktypes.h"
#include "error.h"

typedef enum {
    KCTX_NONE   = 0,
    KCTX_PMM    = 1,
    KCTX_VMM    = 2,
    KCTX_TAGFS  = 3,
    KCTX_AHCI   = 4,
    KCTX_SCHED  = 5,
    KCTX_IPC    = 6,
    KCTX_GUIDE  = 7,
    KCTX_FRIEND = 8,
} KResultContext;

typedef struct {
    uint32_t status;
    uint32_t context;
    uint64_t value;
} KResult;

#define KRESULT_OK(val)       ((KResult){ .status = 0,               .context = KCTX_NONE,         .value = (uint64_t)(uintptr_t)(val) })
#define KRESULT_ERR(ctx, err) ((KResult){ .status = (uint32_t)(err), .context = (uint32_t)(ctx),   .value = 0 })
#define KRESULT_IS_OK(r)      ((r).status == 0)
#define KRESULT_IS_ERR(r)     ((r).status != 0)

#endif // KRESULT_H
