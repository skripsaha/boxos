#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include "ktypes.h"

#define BOOT_INFO_MAGIC   0x42583031  /* "BX01" */
#define BOOT_INFO_VERSION 1
#define BOOT_INFO_ADDR    0x9000

typedef struct {
    uint32_t magic;           /* +0:  BOOT_INFO_MAGIC */
    uint32_t version;         /* +4:  BOOT_INFO_VERSION */
    uint32_t e820_map_addr;   /* +8:  physical address of E820 entries */
    uint16_t e820_count;      /* +12: number of E820 entries */
    uint16_t reserved1;       /* +14: padding */
    uint32_t kernel_start;    /* +16: kernel load address */
    uint32_t kernel_end;      /* +20: kernel end (page-aligned) */
    uint8_t  boot_drive;      /* +24: BIOS boot drive number */
    uint8_t  reserved2;       /* +25: padding */
    uint16_t reserved3;       /* +26: padding */
    uint32_t page_table_base; /* +28: boot page table physical address (dynamic) */
    uint32_t stack_base;      /* +32: kernel stack base (dynamic, after page tables) */
    uint32_t total_size;      /* +36: size of this structure */
} __attribute__((packed)) boot_info_t;

_Static_assert(sizeof(boot_info_t) == 40, "boot_info_t must be 40 bytes");

static inline boot_info_t* boot_info_get(void) {
    return (boot_info_t*)(uintptr_t)BOOT_INFO_ADDR;
}

static inline bool boot_info_valid(const boot_info_t* bi) {
    return bi->magic == BOOT_INFO_MAGIC && bi->version == BOOT_INFO_VERSION;
}

#endif /* BOOT_INFO_H */
