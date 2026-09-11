#ifndef BOX_CORE_CRATE_H
#define BOX_CORE_CRATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "boxos_crate.h"


INLINE void CrateInit(Crate *c)
{
    c->magic    = CRATE_MAGIC;
    c->flags    = 0;
    c->addr     = 0;
    c->size     = 0;
    c->capacity = 0;
    c->kind     = 0;
    c->_pad0    = 0;
    c->_pad1    = 0;
}

INLINE void CrateSetInput(Crate *c, void *buf, uint64_t size)
{
    c->magic    = CRATE_MAGIC;
    c->flags    = 0;
    c->addr     = (uint64_t)(uintptr_t)buf;
    c->size     = size;
    c->capacity = size;
    c->kind     = CRATE_KIND_INPUT;
    c->_pad0    = 0;
    c->_pad1    = 0;
}

INLINE void CrateSetOutput(Crate *c, void *buf, uint64_t capacity)
{
    c->magic    = CRATE_MAGIC;
    c->flags    = 0;
    c->addr     = (uint64_t)(uintptr_t)buf;
    c->size     = 0;
    c->capacity = capacity;
    c->kind     = CRATE_KIND_OUTPUT;
    c->_pad0    = 0;
    c->_pad1    = 0;
}

INLINE void CrateSetInOut(Crate *c, void *buf, uint64_t size, uint64_t capacity)
{
    c->magic    = CRATE_MAGIC;
    c->flags    = 0;
    c->addr     = (uint64_t)(uintptr_t)buf;
    c->size     = size;
    c->capacity = capacity;
    c->kind     = CRATE_KIND_INOUT;
    c->_pad0    = 0;
    c->_pad1    = 0;
}

#ifdef __cplusplus
}
#endif

#endif