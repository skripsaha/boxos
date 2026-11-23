#ifndef BOOT_INFO_H
#define BOOT_INFO_H

/*
 * boot_info — what a loader hands the kernel about the MACHINE: where memory
 * is, where the kernel and its boot tables sit, where the framebuffer and the
 * firmware's tables are. What it says about the JOURNEY is the Boarding Pass's
 * (boarding_pass.h).
 *
 * ONE STRUCTURE, recognised by its magic. Both loaders write all of it: stage2
 * (BIOS) zeroes the whole block before it writes the fields it has, so every
 * field a BIOS boot has nothing to say in — the framebuffer, the firmware's
 * tables — reads as zero; Gangway (UEFI) fills it. `boot_method` says which of
 * them wrote it, and a reader asks that, never how long the block is.
 *
 * Integer types come from the includer, as volume_deed.h says: the kernel
 * (ktypes.h) and Gangway (uefi.h) both read this text, so the loader writes
 * the structure the kernel reads rather than a copy of it. stage2 writes its
 * first fields by offsets it spells for itself, and kernel_entry.asm reads the
 * stack by one; the assertions below hold every one of those offsets.
 */

#define BOOT_INFO_MAGIC   0x544F4F42u   /* 'B','O','O','T' */

/* How the machine was booted: which loader wrote the block. */
#define BOOT_METHOD_BIOS  0u            /* stage2 */
#define BOOT_METHOD_UEFI  1u            /* Gangway */

/*
 * Physical address of the handoff block. Moved from 0x9000 to 0xA000 on
 * 2026-08-23: stage1 loads sixteen sectors into 0x8000 unconditionally, so
 * 0x8000..0x9FFF belongs to stage2 in full, and by then the BIOS loader had
 * grown to 0x904F — it was writing this structure into its own message
 * strings. Nothing broke, because those strings are never printed again after
 * the handoff; it was a collision that had not gone off yet. stage2.asm
 * asserts the whole map at assembly time now, so this address and that one
 * cannot drift apart silently.
 */
#define BOOT_INFO_ADDR    0xA000

#ifdef BOOT_INFO_ADDR_FROM_BUILD
_Static_assert(BOOT_INFO_ADDR == BOOT_INFO_ADDR_FROM_BUILD,
               "boot_info address disagrees with the image build: the loaders "
               "write the block where the Makefile says, the kernel reads it "
               "where this header says, and a boot only finds out at its first "
               "push");
#endif

/*
 * fb_format:   0 = RGB (PixelRedGreenBlueReserved8BitPerColor)
 *              1 = BGR (PixelBlueGreenRedReserved8BitPerColor)
 *              The boot contract carries only these two 32-bit layouts.
 *
 * The EFI fields, zero on a BIOS boot:
 *
 * efi_rt_services_phys      EFI_RUNTIME_SERVICES*, captured before
 *                           ExitBootServices from system_table->
 *                           RuntimeServices. Per UEFI 2.10 §4.4.1 the system
 *                           table and the RT services table live in
 *                           EfiRuntimeServicesData and survive EBS.
 *
 * efi_mmap_*                A verbatim copy of the *final* UEFI memory map
 *                           (the one whose key ExitBootServices accepted), in
 *                           EfiACPIMemoryNVS so PMM never reclaims it. The
 *                           kernel walks it for the RT regions it hands
 *                           SetVirtualAddressMap (UEFI §8.4).
 *
 * efi_system_table_phys     EFI_SYSTEM_TABLE*, the pre-EBS physical, so the
 *                           kernel need not trust SVAM-rewritten pointers to
 *                           find it.
 *
 * efi_cfg_table_phys/count  Pre-EBS physical address and length of
 *                           system_table->ConfigurationTable[], which lives
 *                           in EfiRuntimeServicesData (UEFI 2.10 §4.6) —
 *                           ESRT and SMBIOS lookups never depend on SVAM.
 *
 * efi_esrt_copy_phys/size   A Gangway copy of the ESRT (UEFI 2.10 §23.4) in
 *                           EfiACPIMemoryNVS — the firmware's own lives in
 *                           EfiBootServicesData, which PMM reclaims after
 *                           EBS. Zero when the firmware published none.
 *
 * smbios_copy_phys/size     A Gangway copy of the SMBIOS structure table
 *                           (DMTF DSP0134), in EfiACPIMemoryNVS for the same
 *                           reason — UEFI 2.10 §2.3 lets the firmware keep it
 *                           in EfiBootServicesData — and the version and the
 *                           entry point it came by (boxos_smbios.h). Zero on a
 *                           BIOS start, where the kernel finds the entry point
 *                           in the F-segment itself, and when the firmware
 *                           published none.
 */
typedef struct __attribute__((packed))
{
    uint32_t magic;                 /* +0:   BOOT_INFO_MAGIC */
    uint32_t boot_method;           /* +4:   BOOT_METHOD_* */
    uint32_t e820_map_addr;         /* +8:   physical address of E820 entries */
    uint16_t e820_count;            /* +12:  number of E820 entries */
    uint16_t reserved1;             /* +14:  padding */
    uint32_t kernel_start;          /* +16:  kernel load address */
    uint32_t kernel_end;            /* +20:  kernel end (page-aligned) */
    uint8_t  boot_drive;            /* +24:  BIOS drive number (0xFF for UEFI) */
    uint8_t  reserved2;             /* +25:  padding */
    uint16_t reserved3;             /* +26:  padding */
    uint32_t page_table_base;       /* +28:  boot page table physical address */
    uint32_t stack_base;            /* +32:  kernel stack base */
    uint64_t fb_addr;               /* +36:  GOP framebuffer physical address (0 if none) */
    uint32_t fb_width;              /* +44:  horizontal resolution in pixels */
    uint32_t fb_height;             /* +48:  vertical resolution in pixels */
    uint32_t fb_stride;             /* +52:  bytes per scan line */
    uint32_t fb_format;             /* +56:  0=RGB, 1=BGR */
    uint64_t rsdp_addr;             /* +60:  ACPI RSDP physical address (0 if not found) */
    uint64_t efi_rt_services_phys;  /* +68:  EFI_RUNTIME_SERVICES* */
    uint64_t efi_mmap_phys;         /* +76:  copy of the UEFI memory map */
    uint32_t efi_mmap_size;         /* +84:  total bytes of the map */
    uint32_t efi_mmap_desc_size;    /* +88:  bytes per descriptor */
    uint32_t efi_mmap_desc_ver;     /* +92:  descriptor version (the firmware's) */
    uint32_t efi_fw_revision;       /* +96:  system_table->FirmwareRevision */
    uint64_t efi_system_table_phys; /* +100: EFI_SYSTEM_TABLE* */
    uint64_t efi_cfg_table_phys;    /* +108: EFI_CONFIGURATION_TABLE[] */
    uint32_t efi_cfg_table_count;   /* +116: entries in it */
    uint64_t efi_esrt_copy_phys;    /* +120: NVS-preserved ESRT copy */
    uint32_t efi_esrt_copy_size;    /* +128: bytes of the ESRT copy */
    uint64_t smbios_copy_phys;      /* +132: NVS-preserved SMBIOS structure table */
    uint32_t smbios_copy_size;      /* +140: bytes of it */
    uint8_t  smbios_major;          /* +144: its version */
    uint8_t  smbios_minor;          /* +145 */
    uint8_t  smbios_from;           /* +146: SMBIOS_FROM_* (boxos_smbios.h), 0 for none */
    uint8_t  smbios_reserved;       /* +147 */
} boot_info_t;

/*
 * The other half of an agreement with two assemblers: stage2.asm writes the
 * fields up to stack_base by offset, after zeroing BOOT_INFO_BYTES of the
 * block, and kernel_entry.asm takes its stack pointer from +32 before any C
 * runs. Move a field and the build stops, instead of a boot that reads its
 * stack out of the framebuffer's address.
 */
_Static_assert(__builtin_offsetof(boot_info_t, magic)           == 0,  "stage2 writes magic at +0");
_Static_assert(__builtin_offsetof(boot_info_t, boot_method)     == 4,  "stage2 writes boot_method at +4");
_Static_assert(__builtin_offsetof(boot_info_t, e820_map_addr)   == 8,  "stage2 writes e820_map_addr at +8");
_Static_assert(__builtin_offsetof(boot_info_t, e820_count)      == 12, "stage2 writes e820_count at +12");
_Static_assert(__builtin_offsetof(boot_info_t, kernel_start)    == 16, "stage2 writes kernel_start at +16");
_Static_assert(__builtin_offsetof(boot_info_t, kernel_end)      == 20, "stage2 writes kernel_end at +20");
_Static_assert(__builtin_offsetof(boot_info_t, boot_drive)      == 24, "stage2 writes boot_drive at +24");
_Static_assert(__builtin_offsetof(boot_info_t, page_table_base) == 28, "stage2 writes page_table_base at +28");
_Static_assert(__builtin_offsetof(boot_info_t, stack_base)      == 32,
               "stage2 writes stack_base at +32 and kernel_entry.asm reads it there");
_Static_assert(sizeof(boot_info_t) == 148, "stage2 BOOT_INFO_BYTES: the block it zeroes before it writes");

/* A block a loader of this tree wrote: its magic, and a method it knows. */
static inline int boot_info_valid(const boot_info_t *bi)
{
    return bi && bi->magic == BOOT_INFO_MAGIC &&
           (bi->boot_method == BOOT_METHOD_BIOS || bi->boot_method == BOOT_METHOD_UEFI);
}

/* The kernel's way to the block: through the Pull Map once it is up, at its
 * identity address before. A macro, because vmm_phys_to_virt is the kernel's
 * (vmm.h) and this header is shared with a loader that has no such thing. */
#define boot_info_get() ((boot_info_t *)vmm_phys_to_virt(BOOT_INFO_ADDR))

#endif /* BOOT_INFO_H */
