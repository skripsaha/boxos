#ifndef TSS_H
#define TSS_H

#include "ktypes.h"

#define IST_DOUBLE_FAULT    1
#define IST_NMI            2
#define IST_MACHINE_CHECK  3
#define IST_DEBUG          4
#define IST_STACK_FAULT    5

#define IST_COUNT          5      // IST entries used (1-5)
#define IST_STACK_PAGES    4      // 4 pages = 16KB per IST stack
#define IST_GUARD_PAGES    1      // 1 guard page per IST stack
#define IST_STACK_SIZE     (IST_STACK_PAGES * 4096)  // 16KB per IST stack

typedef struct {
    uint32_t reserved1;
    uint64_t rsp0;      // Stack pointer for ring 0
    uint64_t rsp1;      // Stack pointer for ring 1 (unused in 64-bit)
    uint64_t rsp2;      // Stack pointer for ring 2 (unused in 64-bit)
    uint64_t reserved2;
    uint64_t ist1;      // IST #1 - Double Fault
    uint64_t ist2;      // IST #2 - NMI
    uint64_t ist3;      // IST #3 - Machine Check
    uint64_t ist4;      // IST #4 - Debug
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved3;
    uint16_t reserved4;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

// TSS descriptor in GDT (occupies 2 entries in x86-64)
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;      // 0x89 for available TSS
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;  // Upper 32 bits of base (x86-64 only)
    uint32_t reserved;
} __attribute__((packed)) tss_descriptor_t;

void tss_init(void);
void tss_setup_dynamic_stacks(void);  // Phase 2: after PMM+VMM ready
void tss_set_rsp0(uint64_t rsp0);
void tss_load(void);
uint64_t tss_get_ist_stack(int ist_num);

// Get pointer to the static BSP TSS (for per-core copying).
tss_t* tss_get_ptr(void);

#endif // TSS_H
