/*
 * Gangway — memory: the pages the handoff is written into, asked for before
 * anything is written there; the firmware's memory map as the E820 table the
 * kernel divides memory by, built before ExitBootServices and again from the
 * map whose key it accepted; and the leaving of boot services itself.
 */

#include "gangway.h"
#include "boot_info.h"

#define E820_COUNT_ADDR       0x500ULL
#define E820_SIZE_ADDR        0x502ULL
/* Must stay equal to E820_MAX_ENTRIES in src/kernel/core/memory/e820/e820.h
 * and in stage2.asm — all three describe the same bytes at the same address,
 * and the ceiling is stage1's scratch at 0x1200 (asserted in stage2.asm).
 * EmitE820 merges adjacent runs of the same type, which is what keeps a
 * two-hundred-descriptor UEFI map an order of magnitude below this. */
#define E820_MAX_ENTRIES      128U

/* =========================================================================
 * E820 entry type codes
 * ========================================================================= */

#define E820_USABLE    1U
#define E820_RESERVED  2U
#define E820_ACPI_RECL 3U
#define E820_ACPI_NVS  4U

/* =========================================================================
 * e820_entry_t (matches kernel's e820.h)
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} E820Entry;

_Static_assert(sizeof(E820Entry) == 24, "E820Entry must be 24 bytes");

/*
 * Raw physical address write for 16-bit values.
 * noinline prevents GCC from tracing the pointer back to its origin and
 * incorrectly firing -Warray-bounds on known-good physical memory addresses.
 */
static __attribute__((noinline)) void PhysWrite16(uint64_t addr, uint16_t val)
{
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)addr;
    *p = val;
}

/* =========================================================================
 * Memory map: UEFI → E820
 * ========================================================================= */

/* Convert UEFI memory type to E820 type. */
static uint32_t UefiTypeToE820(uint32_t uefi_type)
{
    switch (uefi_type) {
        case EfiConventionalMemory:    return E820_USABLE;
        case EfiLoaderCode:            return E820_USABLE;
        case EfiLoaderData:            return E820_USABLE;
        case EfiBootServicesCode:      return E820_USABLE;
        case EfiBootServicesData:      return E820_USABLE;
        case EfiACPIReclaimMemory:     return E820_ACPI_RECL;
        case EfiACPIMemoryNVS:         return E820_ACPI_NVS;
        case EfiReservedMemoryType:    return E820_RESERVED;
        case EfiRuntimeServicesCode:   return E820_RESERVED;
        case EfiRuntimeServicesData:   return E820_RESERVED;
        case EfiUnusableMemory:        return E820_RESERVED;
        case EfiMemoryMappedIO:        return E820_RESERVED;
        case EfiMemoryMappedIOPortSpace: return E820_RESERVED;
        default:                       return E820_RESERVED;
    }
}
/* Set by EmitE820 when a map did not fit the table at 0x500. Read after BOTH
 * calls, and each call answers only for its own map: the pre-EBS one over the
 * firmware console, the post-EBS one over COM1, whose ports this loader drives
 * itself and which is the one voice that outlives ExitBootServices. */
static int g_e820_truncated = 0;

/*
 * EmitE820 — write one UEFI memory map out as the E820 table at 0x500.
 *
 * Called twice, and the second call is the point of the whole thing.
 *
 * ‼ The first call happens while boot services are still up, and the map it
 * describes is ALREADY STALE by the time the kernel reads it: between that
 * call and ExitBootServices this loader asks the firmware's allocator for
 * the ESRT copy and for the buffer the final memory map is staged in, and
 * both go through AllocateAnyPages — which lets the firmware pick, and EDK2
 * picks top-down out of high conventional memory. In the E820 the kernel
 * received, those pages were still CONVENTIONAL, therefore USABLE, therefore
 * handed to the buddy allocator (pmm.c) and available to be given away and
 * written over. The kernel then walks that same EFI map at runtime to call
 * SetVirtualAddressMap. Nothing would say a word: a count is still a count.
 *
 * So it is called again from PostEbsPatchBootInfo, on the map whose key
 * ExitBootServices ACCEPTED. That map is the only description of memory that
 * includes this loader's own last allocations, and it is by definition
 * current — the firmware would have refused the key otherwise.
 *
 * Adjacent runs of the same E820 type are merged. A UEFI map is far more
 * granular than E820 needs (Linux merges for the same reason —
 * efi_merge_regions), and merging is what keeps a 200-descriptor server map
 * from meeting the ceiling at all. Nothing is lost: the kernel's EFI runtime
 * driver reads the FULL descriptor list from boot_info->efi_mmap_phys, not
 * from here, so per-descriptor attributes still reach the code that needs
 * them.
 *
 * Returns the number of entries written.
 */
static uint32_t EmitE820(const uint8_t *map, uint64_t map_size, uint64_t desc_size)
{
    /* Volatile byte pointer with manual offsets: casting a raw physical
     * address to a typed array fires -Warray-bounds on known-good memory. */
    volatile uint8_t *e820_raw = (volatile uint8_t *)(uintptr_t)E820_MAP_ADDR;
    uint32_t count = 0;

    /* The run currently open, held back one entry so the next descriptor can
     * still extend it. count is the number already WRITTEN. */
    int      have_run   = 0;
    uint64_t run_base   = 0;
    uint64_t run_length = 0;
    uint32_t run_type   = 0;

    const uint8_t *p   = map;
    const uint8_t *end = map + map_size;
    int truncated = 0;

    while (p + desc_size <= end) {
        const EFI_MEMORY_DESCRIPTOR *desc = (const EFI_MEMORY_DESCRIPTOR *)p;
        p += desc_size;

        uint64_t base   = desc->physical_start;
        uint64_t length = desc->number_of_pages * PAGE_4KB;
        uint32_t type   = UefiTypeToE820(desc->type);

        if (length == 0) continue;   /* a zero-page descriptor describes nothing */

        if (have_run && run_type == type && run_base + run_length == base) {
            run_length += length;
            continue;
        }

        if (have_run) {
            if (count >= E820_MAX_ENTRIES) { truncated = 1; break; }
            volatile uint8_t *slot = e820_raw + count * sizeof(E820Entry);
            /* ACPI 3.0 extended attributes (E820 entry +20):
             *   bit 0 = "valid" (1 = use this entry, 0 = ignore)
             *   bit 1 = "non-volatile"
             * Every UEFI descriptor we copy is valid by definition — the
             * firmware just told us about it — so bit 0 is set. Kernels that
             * ignore ACPI 3.0 ignore the field; kernels that use it to filter
             * stale BIOS-style entries need to see it. */
            uint32_t acpi = 1;
            MemCopy((void *)slot,       &run_base,   8);
            MemCopy((void *)(slot + 8), &run_length, 8);
            MemCopy((void *)(slot + 16), &run_type,  4);
            MemCopy((void *)(slot + 20), &acpi,      4);
            count++;
        }

        have_run   = 1;
        run_base   = base;
        run_length = length;
        run_type   = type;
    }

    if (have_run && !truncated && count < E820_MAX_ENTRIES) {
        volatile uint8_t *slot = e820_raw + count * sizeof(E820Entry);
        uint32_t acpi = 1;
        MemCopy((void *)slot,       &run_base,   8);
        MemCopy((void *)(slot + 8), &run_length, 8);
        MemCopy((void *)(slot + 16), &run_type,  4);
        MemCopy((void *)(slot + 20), &acpi,      4);
        count++;
    } else if (have_run) {
        truncated = 1;
    }

    PhysWrite16(E820_COUNT_ADDR, (uint16_t)count);
    PhysWrite16(E820_SIZE_ADDR,  (uint16_t)(count * sizeof(E820Entry)));

    /* A truncated memory map is not a smaller memory map — it is a map that
     * stops describing memory partway through, and whatever it stopped before
     * is unclaimed as far as the kernel is concerned. If it ever happens it
     * has to be said, and it can only be said before ExitBootServices. */
    g_e820_truncated = truncated;
    return count;
}

/* The memory map ExitBootServices accepted, staged in EfiACPIMemoryNVS and
 * forwarded to the kernel via boot_info, which walks its RUNTIME descriptors
 * for SetVirtualAddressMap (UEFI §8.4). */
static uint64_t g_efi_mmap_copy_phys   = 0;
static uint32_t g_efi_mmap_copy_size   = 0;
static uint32_t g_efi_mmap_desc_size   = 0;
static uint32_t g_efi_mmap_desc_ver    = 0;

/*
 * ── the pages the kernel will be told to read, asked for before they are written
 *
 * Three fixed addresses below 64 KB carry everything this loader tells the
 * kernel: the E820 map at 0x500, boot_info at 0xA000, the boarding pass at
 * 0xA600. All three were written into memory this loader had never asked the
 * firmware for.
 *
 * ‼ AND THE WINDOW IS NOT THEORETICAL. Between BuildMemoryMap writing E820 at
 * 0x504 and ExitBootServices, this loader itself calls the firmware's
 * allocator at least three more times — page tables, the ESRT copy, the memory
 * map buffer — and two of those are AllocateAnyPages, which lets the firmware
 * choose. A firmware that satisfies them from low conventional memory hands
 * out the very pages the E820 map is sitting in, and the kernel then reads a
 * memory map with somebody else's data in it. Nothing anywhere would say so:
 * the count at 0x500 would still look like a count.
 *
 * So they are asked for, once, before anything is written into them — which is
 * what AllocateAddress is for (UEFI 2.10 §7.2). Page granularity means three
 * pages cover all three structures:
 *
 *   0x0000  E820 count (0x500), entry size (0x502), start of the map (0x504)
 *   0x1000  the tail of that map — 128 entries of 24 bytes end at 0x1104
 *   0xA000  boot_info (132 bytes) and the boarding pass (512 at 0xA600)
 *
 * ‼ WHAT THIS DOES NOT DO IS free_pages FIRST — and neither does any other
 * AllocateAddress site in this loader. There are three of them (the kernel
 * image, gangway_attest.c; the page tables + stack, gangway_paging.c; the BSS
 * tail, gangway.c) and all three used to free the range
 * the firmware had just refused them, two of them behind a guard on
 * EFI_NOT_FOUND. That guard read FreePages' EFI_NOT_FOUND ("The requested
 * memory pages were not allocated with AllocatePages()", UEFI 2.10 §7.2.2)
 * into a status that came back from AllocatePages, where §7.2.1 says only
 * "The requested pages could not be found" — and where EDK2 returns it
 * precisely BECAUSE somebody holds the range ("the pages to allocate have
 * been allocated", CoreConvertPagesEx). The guard admitted exactly the
 * dangerous case. Handing firmware memory back is not a retry; it is a
 * corruption with a spec section against it, and no status code tells this
 * loader's own pages apart from the firmware's.
 *
 * A refusal is not fatal either. The addresses are a contract with the kernel
 * and with stage2 — they cannot move — so the loader writes there regardless
 * and the machine goes on booting exactly as well as it did before this
 * function existed. What changes is that it SAYS which page it could not get,
 * on a board where that is the difference between a mystery and a sentence.
 */
void ClaimHandoffPages(void)
{
    static const struct { uint64_t at; const char *what; } wanted[] = {
        { 0x0000ULL, "the E820 map"          },
        { 0x1000ULL, "the rest of the E820 map" },
        { 0xA000ULL, "boot_info and the boarding pass" },
    };

    unsigned held = 0;
    for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
        EFI_PHYSICAL_ADDRESS pa = (EFI_PHYSICAL_ADDRESS)wanted[i].at;
        EFI_STATUS s = g_bs->allocate_pages(AllocateAddress, EfiLoaderData,
                                            1, &pa);
        if (EFI_ERROR(s)) {
            Print("Gangway: firmware would not set aside ");
            PrintHex64(wanted[i].at);
            Print(" for ");
            Print(wanted[i].what);
            Print(" — writing there anyway (status=");
            PrintHex64((uint64_t)s);
            Print(")\r\n");
            continue;
        }
        held++;
    }

    Print("Gangway: set aside ");
    PrintDec(held);
    Print(" of 3 handoff page(s)\r\n");
}
/* Allocate and retrieve UEFI memory map, convert to E820 at 0x500.
 * The map_key is needed for ExitBootServices. */
EFI_STATUS BuildMemoryMap(uint32_t *e820_count)
{
    UINTN     map_size     = 0;
    UINTN     map_key      = 0;
    UINTN     desc_size    = 0;
    uint32_t  desc_version = 0;
    uint8_t  *map_buf      = NULL;

    /* First call to get required size. */
    EFI_STATUS status = g_bs->get_memory_map(&map_size, NULL,
                                              &map_key, &desc_size,
                                              &desc_version);
    /* Expected: EFI_BUFFER_TOO_SMALL */
    map_size += desc_size * 8;   /* allocating extra for safety */

    status = g_bs->allocate_pool(EfiLoaderData, map_size, (void **)&map_buf);
    if (EFI_ERROR(status)) return status;

    status = g_bs->get_memory_map(&map_size, (EFI_MEMORY_DESCRIPTOR *)map_buf,
                                   &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        g_bs->free_pool(map_buf);
        return status;
    }

    uint32_t count = EmitE820(map_buf, map_size, desc_size);

    /* The pool is ours and its only purpose was this conversion. Freeing it
     * here rather than leaking it to ExitBootServices removes one descriptor
     * from every map the firmware builds after this point — and one more
     * reason for the map key to move under the EBS retry loop. */
    g_bs->free_pool(map_buf);
    map_buf = NULL;

    *e820_count = count;

    /* The ceiling goes on the line too. It is what turns "126" into a fact an
     * operator can act on, and it is the only headroom figure anyone gets:
     * the map rebuilt after ExitBootServices is a different map, and this one
     * does not bound it. */
    Print("Gangway: memory map ");
    PrintDec(count);
    Print(" of ");
    PrintDec(E820_MAX_ENTRIES);
    Print(" entries\r\n");

    /* A memory map that did not fit is a map that stops describing memory
     * partway through — and everything past the cut is unclaimed as far as
     * the kernel is concerned, including regions that were RESERVED. Worse
     * than unclaimed: pmm_buddy_free_carved carves the USABLE runs against
     * the non-USABLE entries in this same table, so a RESERVED or ACPI_NVS
     * entry dropped past the cut stops being carved out of a USABLE run
     * below it, and the buddy hands out firmware-owned pages.
     *
     * ‼ This speaks for THIS map and no other. It is not a prediction about
     * the map rebuilt after ExitBootServices: between here and there this
     * loader asks the firmware for the ESRT copy and for the buffer the final
     * map is staged in, and an NVS island carved out of a conventional run
     * splits one merged USABLE run into as many as three — while the pool
     * freed here can just as well make the second map shorter. Two different
     * maps, neither bounding the other, which is why Linux's stub budgets
     * EFI_MMAP_NR_SLACK_SLOTS between its own two. The second pass says its
     * own piece, over COM1, in PostEbsPatchBootInfo. */
    if (g_e820_truncated) {
        Print("Gangway: WARNING — the memory map does not fit ");
        PrintDec(E820_MAX_ENTRIES);
        Print(" entries and was cut short\r\n");
    }

    return EFI_SUCCESS;
}

/*
 * DoExitBootServices — proper retry loop per UEFI 2.10 §7.4.6.
 *
 * Strategy per attempt:
 *   1. Allocate a sized scratch buffer for the upcoming GetMemoryMap.
 *      We use EfiACPIMemoryNVS so the buffer is treated as ACPI NVS by
 *      the kernel's PMM (never reclaimed), letting it survive EBS and
 *      still be referenced via boot_info → kernel-side EFI RT driver.
 *   2. GetMemoryMap with that buffer to obtain a *fresh* key that
 *      matches the post-allocation memory layout.
 *   3. ExitBootServices(image_handle, key).
 *
 * If step 3 returns EFI_INVALID_PARAMETER the key is stale (some firmware
 * event allocated memory after step 2). Free the scratch and retry.
 *
 * On success we forward the staged map pointer + sizes to the kernel via
 * boot_info so the kernel can walk RUNTIME descriptors for
 * SetVirtualAddressMap (UEFI 2.10 §8.4).
 *
 * 10 retries — spec only requires 1, but slow enterprise boards (HP Z,
 * Lenovo ThinkPad certain BIOS revs) fire 4-5 memory-allocating events
 * between GetMemoryMap and ExitBootServices while RT virtualisation
 * tables get rearranged; 3 has been observed insufficient there.
 */
EFI_STATUS DoExitBootServices(EFI_HANDLE image_handle)
{
    EFI_STATUS last_status = EFI_ABORTED;

    for (UINTN attempt = 0; attempt < 10; attempt++) {
        UINTN                map_size = 0;
        UINTN                key      = 0;
        UINTN                desc_sz  = 0;
        uint32_t             desc_ver = 0;
        EFI_PHYSICAL_ADDRESS buf_pa   = 0;

        /* First call: get required buffer size (EFI_BUFFER_TOO_SMALL expected). */
        g_bs->get_memory_map(&map_size, NULL, &key, &desc_sz, &desc_ver);
        /* Slack for allocations we (and firmware) may do between this sizing
         * call and ExitBootServices:
         *   - the AllocatePages below splits the conventional-memory region
         *     containing our buffer into two descriptors;
         *   - many enterprise firmwares (HP Z, Lenovo, Insyde-based servers)
         *     fire 4-8 memory-allocating events per attempt while TPL
         *     timers and RT-virtualisation tables get rearranged;
         *   - QEMU OVMF can fire CPU-add / hot-plug events on multi-AP boot
         *     before ExitBootServices completes.
         * Linux uses EFI_MMAP_SLACK_DESCS for the same reason. 32 was
         * observed insufficient on real HP Z440 firmware (audit 2026-05-31). */
        #define EFI_MMAP_SLACK_DESCS  256U
        map_size += desc_sz * EFI_MMAP_SLACK_DESCS;

        /* Round up to whole pages — AllocatePages is page-granular. */
        UINTN pages = (map_size + PAGE_4KB - 1) / PAGE_4KB;

        /* Allocate as EfiACPIMemoryNVS: the kernel sees this region as
         * E820 ACPI_NVS and PMM never reclaims it (real-HW carve in PMM
         * follows the E820 type strictly). This is critical: the memory
         * must outlive ExitBootServices because the kernel-side EFI RT
         * driver walks the staged map at runtime. EfiLoaderData would
         * survive EBS too, but PMM treats it as USABLE and frees it. */
        /* Below 4 GB — same reason as the ESRT copy above: the kernel walks
         * this map through the loader's identity window, which ends at 4 GB,
         * and PmmReserveFirmwareMemory reads it before vmm_init has run. */
        buf_pa = 0xFFFFF000ULL;
        EFI_STATUS s = g_bs->allocate_pages(AllocateMaxAddress,
                                            EfiACPIMemoryNVS,
                                            pages,
                                            &buf_pa);
        if (EFI_ERROR(s)) {
            last_status = s;
            Print("Gangway: EBS attempt ");
            PrintDec(attempt);
            Print(" — allocate_pages(NVS) failed\r\n");
            continue;
        }

        /* Second call: get the fresh key + map into our NVS buffer. */
        s = g_bs->get_memory_map(&map_size,
                                  (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)buf_pa,
                                  &key, &desc_sz, &desc_ver);
        if (EFI_ERROR(s)) {
            last_status = s;
            Print("Gangway: EBS attempt ");
            PrintDec(attempt);
            Print(" — get_memory_map failed\r\n");
            g_bs->free_pages(buf_pa, pages);
            continue;
        }

        /* Publish the map pointer + parameters BEFORE the call so even
         * a misbehaving firmware that "succeeds" but rewrites parts of
         * memory in flight still leaves the kernel a usable description.
         * On retry these get overwritten by the next attempt. */
        g_efi_mmap_copy_phys = (uint64_t)buf_pa;
        g_efi_mmap_copy_size = (uint32_t)map_size;
        g_efi_mmap_desc_size = (uint32_t)desc_sz;
        g_efi_mmap_desc_ver  = desc_ver;

        /* Exit — do NOT free buf; freeing would change the key. */
        s = g_bs->exit_boot_services(image_handle, key);
        if (!EFI_ERROR(s)) return EFI_SUCCESS;

        /* ExitBootServices failed — typically EFI_INVALID_PARAMETER (0x80000002)
         * because something (TPL change, async event) bumped the map key
         * between GetMemoryMap and our call. Other codes worth seeing on
         * console for triage on real boards: EFI_OUT_OF_RESOURCES (0x80000009)
         * indicates firmware is starved and may need a power cycle. Logging
         * the status hex lets us tell them apart without a debugger. */
        last_status = s;
        Print("Gangway: EBS attempt ");
        PrintDec(attempt);
        Print(" — exit_boot_services failed status=");
        PrintHex64((uint64_t)s);
        Print(", retrying\r\n");
        g_bs->free_pages(buf_pa, pages);
        g_efi_mmap_copy_phys = 0;
        g_efi_mmap_copy_size = 0;
    }
    return last_status;
}

/* After ExitBootServices succeeds we know the FINAL memory map staged in
 * EfiACPIMemoryNVS; patch the still-mapped boot_info at BOOT_INFO_ADDR.
 * No UEFI services are valid here — pure memory writes. */
void PostEbsPatchBootInfo(void)
{
    boot_info_t *bi = (boot_info_t *)(uintptr_t)BOOT_INFO_ADDR;
    bi->efi_mmap_phys      = g_efi_mmap_copy_phys;
    bi->efi_mmap_size      = g_efi_mmap_copy_size;
    bi->efi_mmap_desc_size = g_efi_mmap_desc_size;
    bi->efi_mmap_desc_ver  = g_efi_mmap_desc_ver;

    /* And the E820 the kernel will actually divide memory by is rewritten
     * from the accepted map — see the note on EmitE820. The map built before
     * ExitBootServices did not know about this loader's own last two
     * allocations (the ESRT copy and the buffer this very map is staged in),
     * both of which the firmware placed wherever it liked; the kernel's
     * allocator would have found them free and given them away. */
    if (g_efi_mmap_copy_phys && g_efi_mmap_copy_size && g_efi_mmap_desc_size) {
        uint32_t n = EmitE820((const uint8_t *)(uintptr_t)g_efi_mmap_copy_phys,
                              g_efi_mmap_copy_size,
                              g_efi_mmap_desc_size);
        bi->e820_count = (uint16_t)n;

        /*
         * ‼ And if THIS map did not fit, it has to be said here — not left to
         * the warning printed before ExitBootServices, which answered for a
         * different map and could not have known about this one.
         *
         * Boot services are gone and Print() is gone with them. COM1 is not:
         * those are port writes this loader does itself, the same ones that
         * announced its entry before g_st was even read. The UART is
         * re-programmed first because the firmware's own console driver owned
         * it in between and may have left it at another divisor.
         *
         * The count is not printed because there is only one value it can
         * have: EmitE820 stops at E820_MAX_ENTRIES, so a cut map is a full
         * table by construction. What is not known — and is the whole of the
         * damage — is how much memory lay past the cut.
         */
        if (g_e820_truncated) {
            Com1Init();
            Com1Str("\r\n[GANGWAY] the final memory map did not fit the E820 "
                    "table: the entries past the cut are gone, and memory they "
                    "described is neither usable to the kernel nor carved out "
                    "of what it will hand away\r\n");
        }
    }
}
