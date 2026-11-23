/*
 * Gangway — the firmware's tables the kernel is told of: the system table,
 * its runtime services and Configuration Table, captured at entry; the ACPI
 * RSDP found in that table; and the ESRT, copied where the kernel's allocator
 * will not hand it away.
 */

#include "gangway.h"
#include "boxos_smbios.h"

/* Defensive ceiling on EFI Configuration Table entry count. Real firmware
 * publishes 6-20 entries (ACPI 1.0, ACPI 2.0, SMBIOS 2.1/3.0, ESRT, MPS,
 * SAL, debug-image-info, RT-properties, etc.). A count larger than this
 * indicates a corrupted system table; treat as "no entries" rather than
 * loop over uncontrolled memory. */
#define EFI_CFG_TABLE_MAX_ENTRIES  256U

/* =========================================================================
 * EFI runtime handoff state
 *
 * Captured pre-ExitBootServices and forwarded to the kernel via
 * boot_info. The kernel uses this to invoke SetVirtualAddressMap
 * (UEFI §8.4) and call runtime services like ResetSystem, plus walk
 * the surviving Configuration Table for SMBIOS / ESRT / RT properties.
 * ========================================================================= */
uint64_t g_efi_rt_services_phys = 0;
uint32_t g_efi_fw_revision      = 0;
uint64_t g_efi_system_table_phys = 0;
uint64_t g_efi_cfg_table_phys    = 0;
uint32_t g_efi_cfg_table_count   = 0;
uint64_t g_efi_esrt_copy_phys    = 0;
uint32_t g_efi_esrt_copy_size    = 0;
uint64_t g_smbios_copy_phys      = 0;
uint32_t g_smbios_copy_size      = 0;
uint8_t  g_smbios_major, g_smbios_minor, g_smbios_from;

/* The system table's runtime services, Configuration Table and revision,
 * captured before any boot-services call that could reshape them. Per UEFI
 * 2.10 §4.4.1 and §4.6 all three structures live in EfiRuntimeServicesData
 * and survive ExitBootServices, so a pre-EBS physical address remains a valid
 * handle for the kernel. The kernel will dereference these to invoke RT
 * services and walk the Configuration Table for ESRT / SMBIOS /
 * RT-properties / debug-image-info lookups. */
void FirmwareTablesTake(EFI_SYSTEM_TABLE *st)
{
    g_efi_rt_services_phys   = (uint64_t)(uintptr_t)st->runtime_services;
    g_efi_fw_revision        = st->firmware_revision;
    g_efi_system_table_phys  = (uint64_t)(uintptr_t)st;
    g_efi_cfg_table_phys     = (uint64_t)(uintptr_t)st->configuration_table;

    /* Bound the count we forward to the kernel. Real firmware publishes
     * ~6-20 entries; anything bigger here points at memory corruption
     * (or, in adversarial scenarios, a tampered system table). The kernel
     * walker uses this count as its loop bound, so capping at the source
     * prevents a kernel-side OOB read even if downstream code forgets to
     * re-check. */
    uint64_t cfg_count_raw = st->number_of_table_entries;
    if (cfg_count_raw > EFI_CFG_TABLE_MAX_ENTRIES)
        cfg_count_raw = EFI_CFG_TABLE_MAX_ENTRIES;
    g_efi_cfg_table_count    = (uint32_t)cfg_count_raw;
}

/* What was captured, in the journal. The firmware vendor string is a CHAR16*
 * in EfiRuntimeServicesData and survives EBS. Print as ASCII (best-effort
 * transliteration; high bytes are dropped). Lets operator see "American
 * Megatrends", "Insyde", "HP", "Apple", etc. without a debugger. */
void FirmwareTablesSay(EFI_SYSTEM_TABLE *st)
{
    if (st->firmware_vendor) {
        Print("Gangway: firmware vendor=");
        for (CHAR16 *v = st->firmware_vendor; *v; v++) {
            char ch[2] = { (*v < 0x80) ? (char)*v : '?', 0 };
            Print(ch);
        }
        Print(" rev=");
        PrintHex64(g_efi_fw_revision);
        Print("\r\n");
    }

    Print("Gangway: EFI ST=");
    PrintHex64(g_efi_system_table_phys);
    Print(" RT=");
    PrintHex64(g_efi_rt_services_phys);
    Print(" CT=");
    PrintHex64(g_efi_cfg_table_phys);
    Print(" entries=");
    PrintDec(g_efi_cfg_table_count);
    Print("\r\n");
}
/* =========================================================================
 * ACPI RSDP discovery from UEFI Configuration Table
 * ========================================================================= */

/*
 * FindAcpiRsdp — scan the UEFI Configuration Table for the ACPI RSDP.
 * Prefers ACPI 2.0 (XSDP) over ACPI 1.0 (RSDP).
 * Returns the physical address of the RSDP, or 0 if not found.
 */
uint64_t FindAcpiRsdp(void)
{
    if (!g_st || g_st->number_of_table_entries == 0) return 0;

    /* Defensive: a sane firmware whose ConfigurationTable[] pointer is NULL
     * MUST report number_of_table_entries==0 (UEFI 2.10 §4.3). Real boards
     * are not always sane — refuse to deref NULL with non-zero count. */
    EFI_CONFIGURATION_TABLE *ct = g_st->configuration_table;
    if (!ct) return 0;

    /* Bound the iteration. A corrupt system table with a >256 entry count
     * (real firmware ships ~6-20) would otherwise have us scan arbitrary
     * memory. Capped scan returns whatever ACPI/SMBIOS/ESRT GUIDs we find
     * in the legitimate prefix and ignores the rest. */
    UINTN n = g_st->number_of_table_entries;
    if (n > EFI_CFG_TABLE_MAX_ENTRIES) n = EFI_CFG_TABLE_MAX_ENTRIES;

    EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10_guid = EFI_ACPI_TABLE_GUID;

    uint64_t rsdp_v2 = 0;
    uint64_t rsdp_v1 = 0;

    for (UINTN i = 0; i < n; i++) {
        if (GuidEqual(&ct[i].vendor_guid, &acpi20_guid))
            rsdp_v2 = (uint64_t)(uintptr_t)ct[i].vendor_table;
        else if (GuidEqual(&ct[i].vendor_guid, &acpi10_guid))
            rsdp_v1 = (uint64_t)(uintptr_t)ct[i].vendor_table;
    }

    return rsdp_v2 ? rsdp_v2 : rsdp_v1;
}

/* =========================================================================
 * EFI System Resource Table capture (UEFI 2.10 §23.4)
 *
 * ESRT lives in EfiBootServicesData per spec; after EBS the OS PMM treats
 * those pages as USABLE and may overwrite them at any point. To preserve
 * the firmware-published resource list we copy the ESRT (header + every
 * EFI_SYSTEM_RESOURCE_ENTRY) into a fresh EfiACPIMemoryNVS allocation
 * before EBS. PMM never reclaims ACPI_NVS so the kernel-side ESRT driver
 * sees a stable image.
 * ========================================================================= */

#define ESRT_FW_RESOURCE_VERSION  1ULL    /* UEFI 2.10 §23.4 fixed */
#define ESRT_ENTRY_SIZE           40U     /* sizeof(EFI_SYSTEM_RESOURCE_ENTRY) */

#define EFI_SYSTEM_RESOURCE_TABLE_GUID \
    EFI_GUID_INIT(0xb122a263,0x3661,0x4f68, 0x99,0x29,0x78,0xf8,0xb0,0xd6,0x21,0x80)

typedef struct __attribute__((packed)) {
    uint32_t fw_resource_count;
    uint32_t fw_resource_count_max;
    uint64_t fw_resource_version;
} EsrtHeader;

_Static_assert(sizeof(EsrtHeader) == 16, "ESRT header must be 16 bytes");

/* Locate ESRT pointer + total size from the Configuration Table.
 * Returns 0 if not found. The pointer is a physical address; the table
 * is still in EfiBootServicesData here, valid only until EBS. */
static uint64_t FindEsrt(uint32_t *out_size)
{
    *out_size = 0;
    if (!g_st || g_st->number_of_table_entries == 0) return 0;

    EFI_CONFIGURATION_TABLE *ct = g_st->configuration_table;
    if (!ct) return 0;   /* same defense as FindAcpiRsdp */

    UINTN n = g_st->number_of_table_entries;
    if (n > EFI_CFG_TABLE_MAX_ENTRIES) n = EFI_CFG_TABLE_MAX_ENTRIES;

    EFI_GUID esrt_guid = EFI_SYSTEM_RESOURCE_TABLE_GUID;

    for (UINTN i = 0; i < n; i++) {
        if (!GuidEqual(&ct[i].vendor_guid, &esrt_guid)) continue;

        EsrtHeader *hdr = (EsrtHeader *)ct[i].vendor_table;
        if (!hdr) return 0;
        if (hdr->fw_resource_version != ESRT_FW_RESOURCE_VERSION) {
            Print("Gangway: ESRT: unknown FwResourceVersion=");
            PrintDec(hdr->fw_resource_version);
            Print(" — ignoring\r\n");
            return 0;
        }
        /* Defensive cap: 1024 entries (=40 KB) is wildly more than any
         * board publishes; rejects a corrupted table that would otherwise
         * overflow our NVS copy. */
        if (hdr->fw_resource_count > 1024U) {
            Print("Gangway: ESRT: unreasonable FwResourceCount=");
            PrintDec(hdr->fw_resource_count);
            Print(" — ignoring\r\n");
            return 0;
        }
        *out_size = sizeof(EsrtHeader) +
                    hdr->fw_resource_count * ESRT_ENTRY_SIZE;
        return (uint64_t)(uintptr_t)hdr;
    }
    return 0;
}

/* Copy ESRT to fresh EfiACPIMemoryNVS pages. Returns physical address of
 * the copy, or 0 on failure. Idempotent in the sense that a subsequent
 * call would just consume another arena — but EsrtKeep calls this once.
 *
 * The ESRT here still resides in EfiBootServicesData and therefore must be
 * fully read out before EBS. We do this immediately after FindEsrt. */
static uint64_t CopyEsrtToNvs(uint64_t esrt_phys, uint32_t esrt_size)
{
    if (esrt_phys == 0 || esrt_size == 0) return 0;
    if (esrt_size > 65536U) return 0;   /* 64 KB cap — same reason as above */

    UINTN pages = (esrt_size + PAGE_4KB - 1) / PAGE_4KB;
    /* Below 4 GB, deliberately. AllocateAnyPages lets the firmware answer
     * from anywhere it likes, and EDK2 answers top-down — on a machine with
     * memory above 4 GB that is where these pages land. The kernel reads
     * this copy before it has built page tables of its own, through the
     * loader's identity map, and that map covers the low 4 GB. Asking for a
     * ceiling costs nothing and removes the one address range where the
     * answer would be unreachable. */
    EFI_PHYSICAL_ADDRESS buf = 0xFFFFF000ULL;
    EFI_STATUS s = g_bs->allocate_pages(AllocateMaxAddress,
                                         EfiACPIMemoryNVS,
                                         pages,
                                         &buf);
    if (EFI_ERROR(s)) {
        Print("Gangway: ESRT NVS alloc failed\r\n");
        return 0;
    }
    MemCopy((void *)(uintptr_t)buf,
             (const void *)(uintptr_t)esrt_phys,
             esrt_size);
    return (uint64_t)buf;
}

/* The ESRT, captured before EBS — it lives in EfiBootServicesData per UEFI
 * 2.10 §23.4 and would be reclaimed by PMM otherwise. */
void EsrtKeep(void)
{
    uint32_t esrt_size = 0;
    uint64_t esrt_src  = FindEsrt(&esrt_size);
    if (esrt_src != 0 && esrt_size != 0) {
        uint64_t nvs = CopyEsrtToNvs(esrt_src, esrt_size);
        if (nvs != 0) {
            g_efi_esrt_copy_phys = nvs;
            g_efi_esrt_copy_size = esrt_size;
            Print("Gangway: ESRT copied to ACPI_NVS at ");
            PrintHex64(nvs);
            Print(" (");
            PrintDec(esrt_size);
            Print(" bytes)\r\n");
        }
    } else {
        Print("Gangway: ESRT not published by firmware\r\n");
    }
}

/* =========================================================================
 * SMBIOS capture (DMTF DSP0134 3.8.0 §5.2; UEFI 2.10 §4.6)
 *
 * The firmware publishes the entry point in the Configuration Table — the
 * 64-bit one under SMBIOS3_TABLE_GUID, the 32-bit under SMBIOS_TABLE_GUID —
 * and UEFI 2.10 §2.3 lets the structure table lie in EfiBootServicesData,
 * which the kernel's PMM takes back after EBS. So the table is copied whole
 * into EfiACPIMemoryNVS here, as the ESRT is, and the kernel reads what the
 * machine calls itself from the copy (drivers/smbios). The 64-bit entry point
 * first: the 32-bit one's table is a consistent subset of it (§5.2).
 * ========================================================================= */

#define SMBIOS_TABLE_GUID \
    EFI_GUID_INIT(0xeb9d2d31,0x2d88,0x11d3, 0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d)
#define SMBIOS3_TABLE_GUID \
    EFI_GUID_INIT(0xf2fd1544,0x9794,0x4a2c, 0x99,0x2e,0xe5,0xbb,0xcf,0x20,0xe3,0x94)

/* The entry point published under `guid`, read into *e: 1 when it holds. */
static int SmbiosEntryFound(const EFI_GUID* guid, SmbiosEntry* e)
{
    if (!g_st || !g_st->configuration_table)
        return 0;
    UINTN n = g_st->number_of_table_entries;
    if (n > EFI_CFG_TABLE_MAX_ENTRIES) n = EFI_CFG_TABLE_MAX_ENTRIES;
    for (UINTN i = 0; i < n; i++)
        if (GuidEqual(&g_st->configuration_table[i].vendor_guid, guid) && g_st->configuration_table[i].vendor_table)
            return SmbiosEntryRead((const uint8_t*)g_st->configuration_table[i].vendor_table, 32u, e);
    return 0;
}

void SmbiosKeep(void)
{
    EFI_GUID    v3 = SMBIOS3_TABLE_GUID, v2 = SMBIOS_TABLE_GUID;
    SmbiosEntry e;
    if (!SmbiosEntryFound(&v3, &e) && !SmbiosEntryFound(&v2, &e)) {
        Print("Gangway: SMBIOS not published by firmware\r\n");
        return;
    }
    if (e.bytes > SMBIOS_TABLE_MOST) {
        Print("Gangway: SMBIOS: a structure table of ");
        PrintDec(e.bytes);
        Print(" bytes is no machine's — ignoring\r\n");
        return;
    }
    /* Below 4 GB, as the ESRT's copy: the kernel reads it before its own
     * page tables cover more. */
    EFI_PHYSICAL_ADDRESS buf = 0xFFFFF000ULL;
    if (EFI_ERROR(g_bs->allocate_pages(AllocateMaxAddress, EfiACPIMemoryNVS, (e.bytes + PAGE_4KB - 1) / PAGE_4KB,
                                       &buf))) {
        Print("Gangway: SMBIOS NVS alloc failed\r\n");
        return;
    }
    MemCopy((void*)(uintptr_t)buf, (const void*)(uintptr_t)e.table, e.bytes);
    g_smbios_copy_phys = buf;
    g_smbios_copy_size = e.bytes;
    g_smbios_major     = e.major;
    g_smbios_minor     = e.minor;
    g_smbios_from      = e.from;
    Print("Gangway: SMBIOS ");
    PrintDec(e.major);
    Print(".");
    PrintDec(e.minor);
    Print(" copied to ACPI_NVS (");
    PrintDec(e.bytes);
    Print(" bytes)\r\n");
}
