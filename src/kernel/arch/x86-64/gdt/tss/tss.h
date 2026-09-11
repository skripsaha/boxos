#ifndef TSS_H
#define TSS_H

#include "ktypes.h"

#define IST_DOUBLE_FAULT    1
#define IST_NMI            2
#define IST_MACHINE_CHECK  3
#define IST_DEBUG          4
#define IST_STACK_FAULT    5

#define IST_COUNT          5
#define IST_STACK_PAGES    4
#define IST_GUARD_PAGES    1
#define IST_STACK_SIZE     (IST_STACK_PAGES * 4096)

typedef struct {
    uint32_t reserved1;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved2;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved3;
    uint16_t reserved4;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) tss_descriptor_t;

void tss_init(void);
void tss_setup_dynamic_stacks(void);
void tss_set_rsp0(uint64_t rsp0);
void tss_load(void);
uint64_t tss_get_ist_stack(int ist_num);

tss_t* tss_get_ptr(void);

#endif