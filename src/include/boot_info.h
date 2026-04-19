#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include "ktypes.h"
#include "kernel/core/memory/vmm/vmm.h" // for vmm_phys_to_virt()

#define BOOT_INFO_MAGIC    0x42583031 /* "BX01" */
#define BOOT_INFO_VERSION  1          /* MBR/BIOS boot */
#define BOOT_INFO_VERSION2 2          /* UEFI boot (TagBoot) */
#define BOOT_INFO_ADDR     0x9000

/*
 * v1: filled by stage2.asm (MBR/BIOS boot) — 40 bytes total.
 * v2: filled by tagboot.c  (UEFI boot)     — 68 bytes total.
 *
 * Kernel code must check version before accessing v2 fields.
 * If version == 1, fields after offset 40 are undefined.
 *
 * boot_method: 0 = BIOS/MBR, 1 = UEFI
 *
 * fb_format:   0 = RGB (PixelRedGreenBlueReserved8BitPerColor)
 *              1 = BGR (PixelBlueGreenRedReserved8BitPerColor)
 *              2 = BGRX / other
 */
typedef struct
{
    /* v1 fields — present for both BIOS and UEFI boots */
    uint32_t magic;           /* +0:  BOOT_INFO_MAGIC */
    uint32_t version;         /* +4:  1 = BIOS, 2 = UEFI */
    uint32_t e820_map_addr;   /* +8:  physical address of E820 entries */
    uint16_t e820_count;      /* +12: number of E820 entries */
    uint16_t reserved1;       /* +14: padding */
    uint32_t kernel_start;    /* +16: kernel load address */
    uint32_t kernel_end;      /* +20: kernel end (page-aligned) */
    uint8_t  boot_drive;      /* +24: BIOS drive number (0xFF for UEFI) */
    uint8_t  reserved2;       /* +25: padding */
    uint16_t reserved3;       /* +26: padding */
    uint32_t page_table_base; /* +28: boot page table physical address */
    uint32_t stack_base;      /* +32: kernel stack base */
    uint32_t total_size;      /* +36: 40 for v1, 68 for v2 */

    /* v2 additions — only valid when version >= 2 (offset +40) */
    uint64_t fb_addr;         /* +40: GOP framebuffer physical address (0 if none) */
    uint32_t fb_width;        /* +48: horizontal resolution in pixels */
    uint32_t fb_height;       /* +52: vertical resolution in pixels */
    uint32_t fb_stride;       /* +56: bytes per scan line */
    uint32_t fb_format;       /* +60: 0=RGB, 1=BGR, 2=BGRX */
    uint8_t  boot_method;     /* +64: 0=BIOS/MBR, 1=UEFI */
    uint8_t  reserved_v2[3];  /* +65: padding to align */
} __attribute__((packed)) boot_info_t;

_Static_assert(sizeof(boot_info_t) == 68, "boot_info_t must be 68 bytes");

/* Backwards-compat accessor: treat the struct as v1 (first 40 bytes only). */
static inline bool boot_info_is_v1(const boot_info_t *bi)
{
    return bi && bi->version == BOOT_INFO_VERSION;
}

static inline bool boot_info_is_v2(const boot_info_t *bi)
{
    return bi && bi->version == BOOT_INFO_VERSION2;
}

// Returns boot_info pointer via Pull Map (if active) or identity address (early boot).
static inline boot_info_t *boot_info_get(void)
{
    return (boot_info_t *)vmm_phys_to_virt(BOOT_INFO_ADDR);
}

static inline bool boot_info_valid(const boot_info_t *bi)
{
    return bi &&
           bi->magic == BOOT_INFO_MAGIC &&
           (bi->version == BOOT_INFO_VERSION || bi->version == BOOT_INFO_VERSION2);
}

#endif /* BOOT_INFO_H */
