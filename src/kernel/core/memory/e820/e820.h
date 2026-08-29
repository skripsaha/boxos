#ifndef E820_H
#define E820_H

#include "ktypes.h"

/* The map lives at a fixed low address (0x504) written by BOTH loaders, so
 * this number is a three-way agreement: it must stay equal to
 * E820_MAX_ENTRIES in src/boot/stage2/stage2.asm and in src/boot/uefi/
 * tagboot.c, and it is bounded by stage1's scratch at 0x1200 (see the
 * assertion at the bottom of stage2.asm). TagBoot merges adjacent runs of
 * the same E820 type before writing here, which is what keeps a UEFI map of
 * two hundred descriptors from ever approaching this. */
#define E820_MAX_ENTRIES  128

#define E820_USABLE      1
#define E820_RESERVED    2
#define E820_ACPI_RECL   3
#define E820_ACPI_NVS    4
#define E820_BAD         5

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed)) e820_entry_t;

void e820_set_entries(e820_entry_t* entries, size_t count);
e820_entry_t* memory_map_get_entries(void);
size_t memory_map_get_entry_count(void);
void e820_activate_pull_map(void);

#endif // E820_H
