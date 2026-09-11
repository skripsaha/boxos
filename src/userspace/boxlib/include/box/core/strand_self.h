#ifndef BOX_CORE_STRAND_SELF_H
#define BOX_CORE_STRAND_SELF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "strand_info.h"


typedef struct {
    uint64_t pocket_va;
    uint64_t result_va;
    uint64_t touch_va;
} strand_rings_t;

StrandInfo *strand_info_or_null(void);

strand_rings_t strand_rings(void);

uint32_t strand_self(void);

uint32_t strand_self_generation(void);

#ifdef __cplusplus
}
#endif

#endif