#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include "ktypes.h"
#include "kernel/core/memory/vmm/vmm.h" // for vmm_phys_to_virt()

#define BOOT_INFO_MAGIC    0x42583031 /* "BX01" */
#define BOOT_INFO_VERSION  1          /* MBR/BIOS boot */
#define BOOT_INFO_VERSION2 2          /* UEFI boot (TagBoot) — RSDP forwarded */
#define BOOT_INFO_VERSION3 3          /* UEFI boot (TagBoot) — full EFI RT handoff */
#define BOOT_INFO_VERSION4 4          /* UEFI boot (TagBoot) — + system_table + config_table + ESRT */
#define BOOT_INFO_ADDR     0x9000

/*
 * v1: filled by stage2.asm (MBR/BIOS boot) — 40 bytes total.
 * v2: filled by tagboot.c  (UEFI boot)     — 76 bytes total.
 * v3: filled by tagboot.c  (UEFI boot)     — 108 bytes total. Adds EFI runtime
 *     services pointer and a preserved copy of the UEFI memory map so the
 *     kernel can call SetVirtualAddressMap (UEFI §8.4) and invoke RT
 *     services such as ResetSystem (§8.5.1) and GetTime (§8.3.1).
 * v4: filled by tagboot.c  (UEFI boot)     — 140 bytes total. Adds the EFI
 *     System Table pointer (so the kernel can walk the Configuration Table
 *     for SMBIOS / RT Properties / debug-image-info GUIDs), the pre-EBS
 *     physical address + count of the Configuration Table array (decoupling
 *     kernel access from SVAM success), and an ACPI-NVS-preserved copy of
 *     the EFI System Resource Table (UEFI 2.10 §23) which by spec lives in
 *     EfiBootServicesData and therefore does NOT survive ExitBootServices
 *     unless the bootloader copies it before EBS.
 *
 * Kernel code must check version before accessing v2/v3/v4 fields.
 * Fields below total_size on v1 are undefined; fields beyond +76 are
 * undefined unless version >= BOOT_INFO_VERSION3; fields beyond +108 are
 * undefined unless version == BOOT_INFO_VERSION4.
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
    uint32_t version;         /* +4:  1 = BIOS, 2/3/4 = UEFI */
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
    uint32_t total_size;      /* +36: 40 for v1, 76 for v2, 108 for v3, 140 for v4 */

    /* v2 additions — only valid when version >= 2 (offset +40) */
    uint64_t fb_addr;         /* +40: GOP framebuffer physical address (0 if none) */
    uint32_t fb_width;        /* +48: horizontal resolution in pixels */
    uint32_t fb_height;       /* +52: vertical resolution in pixels */
    uint32_t fb_stride;       /* +56: bytes per scan line */
    uint32_t fb_format;       /* +60: 0=RGB, 1=BGR, 2=BGRX */
    uint8_t  boot_method;     /* +64: 0=BIOS/MBR, 1=UEFI */
    uint8_t  reserved_v2[3];  /* +65: padding to align */
    uint64_t rsdp_addr;       /* +68: ACPI RSDP physical address (0 if not found) */

    /* v3 additions — only valid when version >= BOOT_INFO_VERSION3.
     *
     * The EFI runtime services pointer is captured before ExitBootServices
     * from system_table->RuntimeServices. Per UEFI 2.10 §4.4.1 the system
     * table and the RT services table itself live in EfiRuntimeServicesData
     * memory and survive ExitBootServices.
     *
     * The EFI memory map below is a verbatim copy of the *final* map (the
     * one whose key was accepted by ExitBootServices). It is allocated in
     * EfiACPIMemoryNVS so PMM treats the region as ACPI_NVS (never
     * reclaimed). The kernel walks this copy to enumerate RT regions for
     * SetVirtualAddressMap (UEFI §8.4). */
    uint64_t efi_rt_services_phys; /* +76: EFI_RUNTIME_SERVICES* phys addr */
    uint64_t efi_mmap_phys;        /* +84: copy of UEFI memory map */
    uint32_t efi_mmap_size;        /* +92: total bytes of the map */
    uint32_t efi_mmap_desc_size;   /* +96: bytes per descriptor */
    uint32_t efi_mmap_desc_ver;    /* +100: descriptor version */
    uint32_t efi_fw_revision;      /* +104: firmware revision (system_table->FirmwareRevision) */

    /* v4 additions — only valid when version == BOOT_INFO_VERSION4.
     *
     * efi_system_table_phys     EFI_SYSTEM_TABLE* physical address. The system
     *                           table itself lives in EfiRuntimeServicesData
     *                           and survives EBS — we capture the pre-EBS
     *                           physical so the kernel doesn't have to depend
     *                           on SVAM-rewritten pointers to find it.
     *
     * efi_cfg_table_phys        Pre-EBS physical address of system_table->
     *                           ConfigurationTable[]. Array lives in
     *                           EfiRuntimeServicesData per UEFI 2.10 §4.6 and
     *                           survives EBS. Captured here so kernel ESRT /
     *                           SMBIOS lookups never have to trust SVAM
     *                           pointer-rewrite.
     *
     * efi_cfg_table_count       Number of entries in that array.
     *
     * efi_esrt_copy_phys        Physical address of a TagBoot-allocated copy
     *                           of the ESRT (UEFI 2.10 §23.4) in
     *                           EfiACPIMemoryNVS. Zero if the firmware did
     *                           not publish an ESRT. We copy because ESRT
     *                           lives in EfiBootServicesData by spec, which
     *                           PMM reclaims as USABLE after EBS.
     *
     * efi_esrt_copy_size        Total bytes of the ESRT copy (header + all
     *                           EFI_SYSTEM_RESOURCE_ENTRY records). Zero
     *                           when efi_esrt_copy_phys is zero. */
    uint64_t efi_system_table_phys; /* +108: EFI_SYSTEM_TABLE* phys */
    uint64_t efi_cfg_table_phys;    /* +116: EFI_CONFIGURATION_TABLE[] phys */
    uint32_t efi_cfg_table_count;   /* +124: number of cfg table entries */
    uint64_t efi_esrt_copy_phys;    /* +128: NVS-preserved ESRT copy */
    uint32_t efi_esrt_copy_size;    /* +136: bytes of ESRT copy */
} __attribute__((packed)) boot_info_t;

_Static_assert(sizeof(boot_info_t) == 140, "boot_info_t must be 140 bytes (v4)");

/* Backwards-compat accessor: treat the struct as v1 (first 40 bytes only). */
static inline bool boot_info_is_v1(const boot_info_t *bi)
{
    return bi && bi->version == BOOT_INFO_VERSION;
}

static inline bool boot_info_is_v2(const boot_info_t *bi)
{
    return bi && bi->version == BOOT_INFO_VERSION2;
}

static inline bool boot_info_is_v3(const boot_info_t *bi)
{
    return bi && bi->version == BOOT_INFO_VERSION3;
}

static inline bool boot_info_is_v4(const boot_info_t *bi)
{
    return bi && bi->version == BOOT_INFO_VERSION4;
}

/* True iff bi->version indicates a UEFI boot (v2, v3, or v4). v4 strictly
 * supersedes v3/v2 (adds EFI Config Table + ESRT handoff) but all v2/v3
 * consumers stay correct because the v1+v2+v3 layout is preserved for the
 * first 108 bytes. */
static inline bool boot_info_is_uefi(const boot_info_t *bi)
{
    return bi && (bi->version == BOOT_INFO_VERSION2 ||
                  bi->version == BOOT_INFO_VERSION3 ||
                  bi->version == BOOT_INFO_VERSION4);
}

/* True iff the kernel may consume the EFI runtime services + EFI memory map
 * handoff fields introduced in v3 (and still present in v4). */
static inline bool boot_info_has_efi_rt(const boot_info_t *bi)
{
    return bi && (bi->version == BOOT_INFO_VERSION3 ||
                  bi->version == BOOT_INFO_VERSION4);
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
           (bi->version == BOOT_INFO_VERSION  ||
            bi->version == BOOT_INFO_VERSION2 ||
            bi->version == BOOT_INFO_VERSION3 ||
            bi->version == BOOT_INFO_VERSION4);
}

#endif /* BOOT_INFO_H */
