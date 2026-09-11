#ifndef KERNEL_NAMEPLATE_H
#define KERNEL_NAMEPLATE_H


#include "ktypes.h"

#define NAMEPLATE_NAME_MAX 96

typedef struct NameplateName {
    char     Text[NAMEPLATE_NAME_MAX];
    uint64_t Offset;
} NameplateName;

int nameplate_locate(const void *image, uint64_t bytes, uintptr_t load_base,
                     uintptr_t *out_va, uint64_t *out_bytes);

int nameplate_name_at_kernel(uintptr_t table_va, uint64_t table_bytes,
                             uintptr_t addr, NameplateName *out);

int nameplate_name_at(uintptr_t table_va, uint64_t table_bytes, uintptr_t addr,
                      NameplateName *out);

#endif