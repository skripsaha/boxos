#ifndef NOTIFY_H
#define NOTIFY_H

#include "ktypes.h"


typedef struct {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
    uint64_t self;
    uint32_t core_index;
    uint32_t _pad;
} __attribute__((aligned(16))) PerCpuData;

void notify_init(void);

void notify_set_kernel_rsp(uint64_t rsp);

extern void notify_entry(void);

#endif