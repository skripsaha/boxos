#ifndef GDT_H
#define GDT_H

#include "ktypes.h"

// Selector format: [Index:13][TI:1][RPL:2]
// RPL: 0=Ring0 (kernel), 3=Ring3 (user)
#define GDT_KERNEL_CODE   0x08              // Index 1, RPL=0
#define GDT_KERNEL_DATA   0x10              // Index 2, RPL=0
#define GDT_USER_CODE     (0x18 | 3)        // Index 3, RPL=3 (0x1B)
#define GDT_USER_DATA     (0x20 | 3)        // Index 4, RPL=3 (0x23)
#define GDT_TSS           0x28              // Index 5

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_descriptor_t;

void gdt_init(void);
void gdt_set_entry(int index, uint64_t base, uint64_t limit, uint8_t access, uint8_t flags);
void gdt_set_tss_entry(int index, uint64_t base, uint64_t limit);
void gdt_load(void);

#endif // GDT_H
