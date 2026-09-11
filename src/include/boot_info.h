#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include "ktypes.h"
#include "kernel/core/memory/vmm/vmm.h"

#define BOOT_INFO_MAGIC    0x42583031
#define BOOT_INFO_VERSION  1
#define BOOT_INFO_VERSION2 2
#define BOOT_INFO_VERSION3 3
#define BOOT_INFO_VERSION4 4
#define BOOT_INFO_ADDR     0xA000

#ifdef BOOT_INFO_ADDR_FROM_BUILD
_Static_assert(BOOT_INFO_ADDR == BOOT_INFO_ADDR_FROM_BUILD,
               "boot_info address disagrees with the image build: the loaders "
               "write the block where the Makefile says, the kernel reads it "
               "where this header says, and a boot only finds out at its first "
               "push");
#endif

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t e820_map_addr;
    uint16_t e820_count;
    uint16_t reserved1;
    uint32_t kernel_start;
    uint32_t kernel_end;
    uint8_t  boot_drive;
    uint8_t  reserved2;
    uint16_t reserved3;
    uint32_t page_table_base;
    uint32_t stack_base;
    uint32_t total_size;

    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_stride;
    uint32_t fb_format;
    uint8_t  boot_method;
    uint8_t  reserved_v2[3];
    uint64_t rsdp_addr;

    uint64_t efi_rt_services_phys;
    uint64_t efi_mmap_phys;
    uint32_t efi_mmap_size;
    uint32_t efi_mmap_desc_size;
    uint32_t efi_mmap_desc_ver;
    uint32_t efi_fw_revision;

    uint64_t efi_system_table_phys;
    uint64_t efi_cfg_table_phys;
    uint32_t efi_cfg_table_count;
    uint64_t efi_esrt_copy_phys;
    uint32_t efi_esrt_copy_size;
} __attribute__((packed)) boot_info_t;

_Static_assert(sizeof(boot_info_t) == 140, "boot_info_t must be 140 bytes (v4)");

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

static inline bool boot_info_is_uefi(const boot_info_t *bi)
{
    return bi && (bi->version == BOOT_INFO_VERSION2 ||
                  bi->version == BOOT_INFO_VERSION3 ||
                  bi->version == BOOT_INFO_VERSION4);
}

static inline bool boot_info_has_efi_rt(const boot_info_t *bi)
{
    return bi && (bi->version == BOOT_INFO_VERSION3 ||
                  bi->version == BOOT_INFO_VERSION4);
}

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

#endif