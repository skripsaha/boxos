#ifndef BOX_NAMEPLATE_H
#define BOX_NAMEPLATE_H


#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"

typedef struct NameplateSite {
    const char *Name;
    uintptr_t   Start;
    uint64_t    Offset;
} NameplateSite;

int nameplate_lookup(uintptr_t address, NameplateSite *site);

uint32_t nameplate_count(void);

#ifdef __cplusplus
}
#endif

#endif