/*
 * BoxOS — EFI Runtime Services driver (kernel side).
 *
 * Responsibilities (see efi.h for the public surface):
 *   1. Parse the EFI memory map preserved in boot_info v3/v4 (EfiACPIMemoryNVS
 *      region, kept alive across ExitBootServices).
 *   2. Map every EFI_MEMORY_RUNTIME descriptor at EFI_RT_VA_BASE +
 *      physical_start, and identically at its physical address, since
 *      firmware is still running physical when SVAM is called. Cacheability
 *      comes from the descriptor's Attribute field — what the firmware said
 *      the region supports — not from its type; execute permission is given
 *      only to EFI_RUNTIME_SERVICES_CODE, and only when the firmware has not
 *      marked it EFI_MEMORY_XP. EFI_MEMORY_MAPPED_IO_PORT_SPACE is never
 *      paged; it gets a virtual_start for the firmware's bookkeeping and
 *      nothing else, per UEFI 2.10 §8.4.
 *   3. Call SetVirtualAddressMap (UEFI 2.10 §8.4) exactly once, handing it
 *      an array of ONLY the runtime descriptors, at that array's physical
 *      address. Boot-services regions are mapped for the duration and their
 *      memory is held out of the allocator across the call, because shipping
 *      firmware reaches into them while relocating. From this point on,
 *      firmware uses the virtual addresses we supplied.
 *   4. Rebase the runtime_services pointer from physical → virtual so
 *      subsequent ResetSystem / GetTime / GetVariable calls land at the
 *      new VA where the firmware now expects to be entered.
 *   5. Provide shared serialisation primitives (efi_rt_lock /
 *      efi_rt_unlock / efi_rt_watch_*) consumed by the per-service
 *      wrappers in efi_variable.c / efi_wakeup.c / efi_capsule.c so
 *      every RT call obeys UEFI 2.10 §8.1 "Runtime services are not
 *      reentrant."
 *
 * Error handling: every failure path leaves efi_runtime_available()
 * false; callers must have a non-EFI fallback (acpi_reboot already
 * chains 0xCF9 / 8042 / triple-fault after the EFI attempt).
 *
 * Concurrency: SVAM is called exactly once from the BSP before AP boot.
 * Wrappers serialise with a spinlock + CLI/STI window per UEFI 2.10
 * §8.1: "Runtime services are not reentrant."
 */

#include "efi.h"
#include "efi_runtime_internal.h"
#include "boot_info.h"
#include "e820.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"
#include "io.h"
#include "cpu_calibrate.h"
#include "atomics.h"
#include "crypto.h"   /* KCrc32 — shared CRC32(IEEE 802.3) implementation */

#define EFI_PAGE_SIZE      4096ULL
#define EFI_PAGE_SHIFT     12

/* =========================================================================
 * Driver state
 * ========================================================================= */

static EfiRuntimeServices *g_rt           = NULL;   /* live (virtual) pointer */
static bool                g_rt_available = false;
static spinlock_t          g_rt_lock;
static bool                g_rt_lock_init = false;

/* For diagnostics. */
static uint32_t g_rt_runtime_descriptors = 0;
static uint64_t g_rt_runtime_pages_total = 0;
static uint32_t g_rt_fw_revision         = 0;

/* =========================================================================
 * Helpers
 * ========================================================================= */

static inline uint64_t efi_pages_to_bytes(uint64_t pages)
{
    return pages << EFI_PAGE_SHIFT;
}

/* Cacheability the firmware ASKED FOR, not the cacheability its type
 * suggests. UEFI 2.10 §7.2 Table 7.10 gives every descriptor an Attribute
 * field naming the memory types the region supports, and the OS is meant to
 * pick from that set rather than infer one. Inferring is what this used to
 * do, and it is wrong in both directions on real boards: a runtime-data
 * region a firmware declares UC-only got write-back, and an MMIO aperture
 * declared write-combining got strong uncacheable and ran at a crawl.
 *
 * Preference order matches Linux's efi_memory_desc → page-attribute mapping:
 * write-back if offered, then write-through, then write-combining, then
 * uncacheable. Anything that offers none of them is treated as device
 * memory, which is the safe assumption for something that did not claim to
 * be RAM.
 *
 * WC uses the PAT slot vmm_pat_init programs to write-combining (entry 6 =
 * PAT_bit | PCD); the plain PCD+PWT pair below is PAT index 3, which is
 * strong UC under the default IA32_PAT. */
static uint64_t efi_rt_cache_flags(uint64_t attribute)
{
    if (attribute & EFI_MEMORY_WB) return 0;
    if (attribute & EFI_MEMORY_WT) return VMM_FLAG_WRITE_THROUGH;
    if (attribute & EFI_MEMORY_WC) return VMM_FLAG_PAT_BIT | VMM_FLAG_CACHE_DISABLE;
    return VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH;   /* UC */
}

/* Map flags for a runtime descriptor. Returns 0 only for IO port space,
 * which is given a virtual_start for the firmware's bookkeeping and no
 * page-table entry at all.
 *
 * NX policy (defense-in-depth, Intel SDM Vol 3 §4.6):
 *   RT code is executable; everything else carries the NX bit so a firmware
 *   bug that branches into RT data / MMIO regions faults instead of running
 *   whatever bytes are there. Bit 63 only reaches the entry once the kernel
 *   has taken no-execute up — vmm_make_pte drops it otherwise, and on this
 *   path that mattered: mapping firmware data with bit 63 while EFER.NXE
 *   was still 0 is what killed the UEFI boot on two real machines.
 *
 * ‼ The default arm is a MAPPING, not a refusal. It used to return 0 for
 * every type this switch did not name, and efi_map_rt_descriptor turned
 * that into "abort SVAM", which turned into "no EFI runtime services at
 * all" — announced through debug_printf, which compiles to nothing in a
 * production build. Real firmware does set EFI_MEMORY_RUNTIME on
 * EfiReservedMemoryType and on EfiPersistentMemory. A descriptor the
 * firmware declared as runtime is one the firmware intends to reach after
 * SetVirtualAddressMap, and refusing to map it is refusing the whole
 * service over a type number. */
static uint64_t efi_rt_map_flags(const EfiMemoryDescriptor *d)
{
    if (d->type == EFI_MEMORY_MAPPED_IO_PORT_SPACE) return 0;

    uint64_t flags = VMM_FLAGS_KERNEL_RW | efi_rt_cache_flags(d->attribute);

    /* Executable only where firmware keeps code. EFI_MEMORY_XP is the
     * firmware saying "no execution here" explicitly; honour it even for a
     * code-typed region, since it can only ever remove a permission. */
    if (d->type != EFI_RUNTIME_SERVICES_CODE || (d->attribute & EFI_MEMORY_XP))
        flags |= VMM_FLAG_NO_EXECUTE;

    return flags;
}

/* Validate the CRC32 field in a table header (UEFI 2.10 §4.2).
 *
 * Per spec: "the 32-bit CRC for the EFI Runtime Services Table and the
 * EFI System Table must be recomputed" after SetVirtualAddressMap. A
 * mismatch here indicates a buggy firmware whose patched table we
 * should not trust.
 *
 * Algorithm matches u-boot's `efi_update_table_header_crc32`: zero the
 * CRC field, then compute IEEE-802.3 CRC over `header_size` bytes.
 * Reuses the project's shared `KCrc32` from src/lib/kernel/crypto.c
 * (same polynomial 0xEDB88320 + ~crc finalisation).
 *
 * Returns true if the header passes validation. */
static bool efi_table_header_valid(const EfiRuntimeServices *rt)
{
    if (!rt) return false;
    const EfiTableHeader *h = &rt->hdr;
    if (h->header_size < sizeof(EfiTableHeader)) return false;
    if (h->header_size > 4096) return false;   /* sanity */

    /* Compute CRC over the table with the CRC32 field zeroed. We do this
     * on a temporary copy in the BSS so we never write to the firmware's
     * table. BSS scratch is fine here — efi_runtime_init is single-
     * threaded BSP-only and the only call site after init is in the
     * deferred wrappers which don't validate. */
    static uint8_t scratch[4096];
    uint32_t len = h->header_size;
    memcpy(scratch, rt, len);
    /* Zero the crc32 field at its known offset (signature=8, revision=4,
     * header_size=4 → crc32 at offset 16). */
    *(uint32_t *)(scratch + 16) = 0;

    uint32_t computed = KCrc32(scratch, len);
    return computed == h->crc32;
}

/* Map a runtime descriptor at EFI_RT_VA_BASE + physical_start.
 *
 * Critical real-HW detail: the firmware's runtime functions are stored
 * as PHYSICAL addresses in the RT services table until SetVirtualAddressMap
 * is called. The kernel-side caller must therefore be able to execute
 * code at those physical addresses for the duration of the SVAM call
 * itself — *after* SVAM returns, firmware uses the VAs we supplied.
 *
 * The loader installs a 4 GB identity map at boot but vmm_init() drops
 * it. So before calling SVAM we MUST install identity mappings (VA==PA)
 * for every RT region as well as the canonical EFI_RT_VA_BASE+phys VA.
 * Both mappings reference the same physical pages; identity is needed
 * only during the SVAM transition but kept in the kernel context
 * afterwards as a safety net (cost: a handful of PTEs).
 *
 * Returns true on success or on intentional skip (IO ports). */
static bool efi_map_rt_descriptor(EfiMemoryDescriptor *d, vmm_context_t *kctx)
{
    if (!(d->attribute & EFI_MEMORY_RUNTIME)) return true;

    uint64_t flags = efi_rt_map_flags(d);

    uintptr_t phys = (uintptr_t)d->physical_start;
    uintptr_t virt = EFI_RT_VA_BASE + phys;
    size_t    pages = (size_t)d->number_of_pages;

    /* IO port range: assign a fake VA so firmware's bookkeeping has a
     * stable virtual_start, but do not actually create a paging mapping
     * — accessing it via the VA would fault, but firmware never does
     * (it uses the OUT/IN ops at the PHYS port number which is in the
     * descriptor's physical_start). */
    d->virtual_start = virt;
    if (flags == 0) return true;

    /* Identity mapping (VA == PA) — required by the firmware's internal
     * code which still references physical addresses until SVAM. */
    vmm_map_result_t ri = vmm_map_pages(kctx, phys, phys, pages, flags);
    if (!ri.success) {
        /* kprintf, not debug_printf: failing here costs the machine its
         * entire EFI runtime surface — ResetSystem, GetTime, variables,
         * Secure Boot state — and debug_printf compiles to nothing in a
         * production build, so this used to be lost in exactly the builds
         * that ship. */
        kprintf("[EFI] identity mapping failed for RT desc type=%u "
                "phys=0x%lx pages=%lu: %s\n",
                d->type, (unsigned long)phys, (unsigned long)pages,
                ri.error_msg ? ri.error_msg : "(no msg)");
        return false;
    }

    /* Canonical kernel VA for the same range — what firmware will use
     * after SVAM. */
    vmm_map_result_t rv = vmm_map_pages(kctx, virt, phys, pages, flags);
    if (!rv.success) {
        kprintf("[EFI] runtime-VA mapping failed for RT desc type=%u "
                "phys=0x%lx virt=0x%lx pages=%lu: %s\n",
                d->type, (unsigned long)phys, (unsigned long)virt,
                (unsigned long)pages,
                rv.error_msg ? rv.error_msg : "(no msg)");
        return false;
    }

    g_rt_runtime_descriptors++;
    g_rt_runtime_pages_total += pages;
    return true;
}

/*
 * Boot-services regions, mapped where firmware still thinks they are.
 *
 * UEFI 2.10 §7.4 hands EfiBootServicesCode / EfiBootServicesData to the OS
 * the moment ExitBootServices returns, and both loaders report them to the
 * kernel as usable RAM on that authority. But SetVirtualAddressMap runs
 * later, and it is the one call made while firmware is still moving itself —
 * a long line of shipping firmwares touch their boot-services memory while
 * doing it. Linux maps those regions for exactly this reason
 * (should_map_region in arch/x86/platform/efi/efi.c, "a workaround for buggy
 * firmware that accesses them even when they shouldn't") and unmaps them
 * again immediately after the call.
 *
 * The pages themselves are already held out of the allocator by
 * PmmHoldBootServicesMemory, so this alias cannot collide with anything the
 * kernel owns. Failure to map one is not fatal: it costs the workaround, not
 * the boot, and the descriptor is named so a board that needs it can be
 * recognised from its log.
 *
 * `unmap` runs the same walk in reverse, so the two are one function with a
 * direction rather than two that could drift apart.
 */
static void efi_boot_services_alias(uint8_t *map, uint32_t map_size,
                                    uint32_t desc_size, vmm_context_t *kctx,
                                    bool install)
{
    uint32_t regions = 0;
    for (uint8_t *p = map; p + desc_size <= map + map_size; p += desc_size) {
        EfiMemoryDescriptor *d = (EfiMemoryDescriptor *)p;
        if (d->type != EFI_BOOT_SERVICES_CODE &&
            d->type != EFI_BOOT_SERVICES_DATA) continue;
        if (d->number_of_pages == 0) continue;

        uintptr_t phys  = (uintptr_t)d->physical_start;
        size_t    pages = (size_t)d->number_of_pages;

        if (install) {
            /* Writable and never executable: the firmware may read and write
             * its own scratch here, but nothing gives it a reason to branch
             * into memory the OS already owns. */
            vmm_map_result_t r = vmm_map_pages(kctx, phys, phys, pages,
                                               VMM_FLAGS_KERNEL_RW |
                                               VMM_FLAG_NO_EXECUTE);
            if (!r.success) {
                kprintf("[EFI] could not alias boot-services region "
                        "0x%lx (%lu page(s)): %s\n",
                        (unsigned long)phys, (unsigned long)pages,
                        r.error_msg ? r.error_msg : "(no msg)");
                continue;
            }
        } else {
            vmm_unmap_pages(kctx, phys, pages);
        }
        regions++;
    }

    debug_printf("[EFI] %s %u boot-services region(s) for the SVAM call\n",
                 install ? "aliased" : "released", regions);
}

/* Locate the descriptor that covers a given physical address. The map
 * is a flat array of EfiMemoryDescriptor records but the spec-defined
 * `descriptor_size` may exceed sizeof(EfiMemoryDescriptor) when the
 * firmware extends the struct in a future revision. Always step by
 * desc_size, never sizeof.
 *
 * Returns the descriptor, or NULL if `phys` is not in the map. */
static EfiMemoryDescriptor *efi_find_descriptor(uint8_t *map, uint32_t map_size,
                                                uint32_t desc_size,
                                                uint64_t phys)
{
    uint8_t *p   = map;
    uint8_t *end = map + map_size;
    while (p + desc_size <= end) {
        EfiMemoryDescriptor *d = (EfiMemoryDescriptor *)p;
        uint64_t span = efi_pages_to_bytes(d->number_of_pages);
        if (phys >= d->physical_start && phys < d->physical_start + span)
            return d;
        p += desc_size;
    }
    return NULL;
}

/* =========================================================================
 * EFI_MEMORY_ATTRIBUTES_TABLE (UEFI 2.10 §4.6.4)
 *
 * Firmware that publishes this table is saying how tightly its own runtime
 * regions may be mapped: EFI_MEMORY_RO on the pages it keeps code in,
 * EFI_MEMORY_XP on the pages it keeps data in, split to page granularity.
 * Without it there is nothing to go on, and a runtime code region has to stay
 * both writable and executable for the life of the machine — the one
 * combination nobody wants for resident code.
 *
 * Applied only AFTER SetVirtualAddressMap, and that timing is the point: the
 * firmware patches its own code during that call, so the write permission it
 * needs there is exactly the write permission it must not keep afterwards.
 * Linux applies it at the same moment and for the same reason
 * (efi_memattr_apply_permissions, from efi_runtime_update_mappings).
 *
 * ‼ Cacheability does NOT come from these entries. Their Attribute field
 * carries RO/XP and typically no memory type at all, so deriving one from it
 * would quietly turn write-back firmware data into uncacheable. The type
 * keeps coming from the descriptor in the real memory map covering the same
 * address — which is also the descriptor that has to agree with this entry
 * before it is honoured.
 *
 * An entry that does not check out is SKIPPED, not fatal, and the region it
 * describes simply keeps the looser permissions it already had — exactly the
 * state a machine whose firmware publishes no table lives in. Skipping is
 * counted and said out loud.
 * ========================================================================= */

const EfiGuid EFI_MEMORY_ATTRIBUTES_TABLE_GUID = {
    0xdcfa911d, 0x26eb, 0x469f,
    {0xa2, 0x20, 0x38, 0xb7, 0xdc, 0x46, 0x12, 0x20}
};

/* Version 1 is the original; version 2 adds only a flags bit describing
 * forward-edge control-flow guard in runtime code, which changes nothing
 * about how the descriptors are read. Anything higher is a table this kernel
 * has not been taught. */
#define EFI_MEMORY_ATTRIBUTES_TABLE_MAX_VERSION 2u
/* Ludicrously higher than any real board publishes; the bound exists so a
 * corrupted count cannot walk this kernel across all of memory. Linux picks
 * the same number for the same reason. */
#define EFI_MEMORY_ATTRIBUTES_MAX_ENTRIES       65536u

typedef struct {
    uint32_t version;
    uint32_t number_of_entries;
    uint32_t descriptor_size;
    uint32_t flags;
} EfiMemoryAttributesTable;

static void efi_apply_memory_attributes(vmm_context_t *kctx, uint8_t *map,
                                        uint32_t map_size, uint32_t desc_size)
{
    void *raw = efi_find_configuration_table(&EFI_MEMORY_ATTRIBUTES_TABLE_GUID);
    if (!raw) {
        kprintf("[EFI] firmware publishes no memory-attributes table — "
                "runtime code keeps write permission\n");
        return;
    }

    EfiMemoryAttributesTable *t =
        (EfiMemoryAttributesTable *)vmm_phys_to_virt((uintptr_t)raw);
    if (!t) return;

    /* A descriptor SMALLER than EfiMemoryDescriptor cannot be read at all; a
     * descriptor LARGER than the memory map's makes no sense and is the shape
     * a corrupted table takes. Both are Linux's checks. */
    if (t->version == 0 || t->version > EFI_MEMORY_ATTRIBUTES_TABLE_MAX_VERSION ||
        t->descriptor_size < sizeof(EfiMemoryDescriptor) ||
        t->descriptor_size > desc_size ||
        t->number_of_entries == 0 ||
        t->number_of_entries > EFI_MEMORY_ATTRIBUTES_MAX_ENTRIES) {
        kprintf("[EFI] memory-attributes table not understood "
                "(version=%u desc=%u entries=%u) — ignored\n",
                t->version, t->descriptor_size, t->number_of_entries);
        return;
    }

    uint8_t *first = (uint8_t *)t + sizeof(EfiMemoryAttributesTable);
    uint32_t tightened = 0, skipped = 0;

    for (uint32_t i = 0; i < t->number_of_entries; i++) {
        EfiMemoryDescriptor *d =
            (EfiMemoryDescriptor *)(first + (size_t)i * t->descriptor_size);

        uint64_t phys = d->physical_start;
        uint64_t span = d->number_of_pages * EFI_PAGE_SIZE;

        if ((d->type != EFI_RUNTIME_SERVICES_CODE &&
             d->type != EFI_RUNTIME_SERVICES_DATA) ||
            d->number_of_pages == 0 ||
            (phys & (EFI_PAGE_SIZE - 1)) != 0) {
            skipped++;
            continue;
        }

        /* The entry has to name memory this kernel actually mapped, of the
         * same type, and one memory-map descriptor has to cover it WHOLE —
         * a permission applied across a boundary would land on a region
         * nobody described. */
        EfiMemoryDescriptor *cover =
            efi_find_descriptor(map, map_size, desc_size, phys);
        if (!cover ||
            !(cover->attribute & EFI_MEMORY_RUNTIME) ||
            cover->type != d->type ||
            cover->physical_start + efi_pages_to_bytes(cover->number_of_pages)
                < phys + span) {
            skipped++;
            continue;
        }

        uint64_t flags = VMM_FLAG_PRESENT | efi_rt_cache_flags(cover->attribute);
        if (!(d->attribute & EFI_MEMORY_RO)) flags |= VMM_FLAG_WRITABLE;
        if (d->attribute & EFI_MEMORY_XP)    flags |= VMM_FLAG_NO_EXECUTE;

        /* Both windows onto the same pages: the canonical runtime VA the
         * firmware uses from here on, and the identity alias kept as a safety
         * net. Leaving either one writable makes the other's read-only an
         * ornament. */
        bool a = vmm_protect(kctx, EFI_RT_VA_BASE + (uintptr_t)phys,
                             (size_t)span, flags);
        bool b = vmm_protect(kctx, (uintptr_t)phys, (size_t)span, flags);
        if (a && b) {
            tightened++;
        } else {
            skipped++;
            kprintf("[EFI] could not tighten runtime region 0x%lx "
                    "(%lu page(s))\n",
                    (unsigned long)phys, (unsigned long)d->number_of_pages);
        }
    }

    kprintf("[EFI] memory attributes: %u runtime region(s) tightened to what "
            "the firmware declared, %u left as they were\n",
            tightened, skipped);
}

/* =========================================================================
 * Initialisation
 * ========================================================================= */

bool efi_runtime_init(void)
{
    if (g_rt_available) return true;   /* idempotent */

    boot_info_t *bi = boot_info_get();
    if (!bi || !boot_info_valid(bi) || !boot_info_has_efi_rt(bi)) {
        debug_printf("[EFI] boot_info lacks EFI RT handoff (boot_method=%u version=%u); "
                     "EFI runtime services unavailable\n",
                     bi ? bi->boot_method : 0,
                     bi ? bi->version    : 0);
        return false;
    }

    if (bi->boot_method != 1) {
        debug_printf("[EFI] BIOS boot — no EFI runtime services\n");
        return false;
    }

    if (!bi->efi_rt_services_phys || !bi->efi_mmap_phys ||
        !bi->efi_mmap_size || !bi->efi_mmap_desc_size) {
        debug_printf("[EFI] boot_info missing EFI handoff fields\n");
        return false;
    }

    g_rt_fw_revision = bi->efi_fw_revision;

    /* Resolve the map copy via Pull Map — the firmware's EfiACPIMemoryNVS
     * region is identity-physical RAM, fully covered. */
    uint8_t *map = (uint8_t *)vmm_phys_to_virt((uintptr_t)bi->efi_mmap_phys);
    if (!map) {
        debug_printf("[EFI] vmm_phys_to_virt failed for mmap 0x%lx\n",
                     (unsigned long)bi->efi_mmap_phys);
        return false;
    }

    /*
     * Is the map the kernel is about to walk memory the allocator believes is
     * free? It used to be, on every UEFI boot, and nothing said so.
     *
     * TagBoot asks the firmware for this buffer AFTER it has already written
     * the E820 table, and the firmware answers out of ordinary conventional
     * memory. In the E820 the kernel received, those pages were still
     * CONVENTIONAL — so the buddy allocator owned them, could hand them out,
     * and whatever landed there would be read here as memory descriptors. The
     * loader now rebuilds E820 from the map ExitBootServices accepted, which
     * describes the buffer as ACPI NVS and puts it out of reach.
     *
     * ‼ MEASURED, and the measurement is worth writing down: on OVMF this
     * check cannot go red. With the rebuild disabled, the E820 the kernel
     * receives is BYTE-IDENTICAL to the rebuilt one — OVMF satisfies the
     * EfiACPIMemoryNVS request out of a region that was already ACPI NVS in
     * the earlier snapshot, and the page-table allocation reads as USABLE
     * either way. So the emulator cannot exercise this hazard, exactly as it
     * could not exercise the NX one. The check stays because a firmware whose
     * NVS pool has to grow into conventional memory WILL put the staged map
     * in pages the allocator calls free, and this line is the only thing that
     * would turn that into a sentence instead of a mystery.
     */
    {
        e820_entry_t *e = memory_map_get_entries();
        size_t        n = memory_map_get_entry_count();
        uint64_t      a = bi->efi_mmap_phys;
        bool          loose = false;
        for (size_t i = 0; i < n && e; i++) {
            if (e[i].type != E820_USABLE || e[i].length == 0) continue;
            if (a >= e[i].base && a < e[i].base + e[i].length) { loose = true; break; }
        }
        if (loose) {
            kprintf("[EFI] the staged memory map at 0x%lx sits in memory the "
                    "allocator calls free — the loader's E820 is older than "
                    "its own allocations\n", (unsigned long)a);
        } else {
            debug_printf("[EFI] staged memory map at 0x%lx is out of the "
                         "allocator's reach\n", (unsigned long)a);
        }
    }

    debug_printf("[EFI] runtime init: mmap=0x%lx size=%u desc_size=%u "
                 "desc_ver=%u rt_services=0x%lx fw_rev=0x%x\n",
                 (unsigned long)bi->efi_mmap_phys,
                 bi->efi_mmap_size,
                 bi->efi_mmap_desc_size,
                 bi->efi_mmap_desc_ver,
                 (unsigned long)bi->efi_rt_services_phys,
                 bi->efi_fw_revision);

    vmm_context_t *kctx = vmm_get_kernel_context();
    if (!kctx) {
        debug_printf("[EFI] no kernel VMM context\n");
        return false;
    }

    /* Stage 1: map every EFI_MEMORY_RUNTIME descriptor. The same map is
     * used as input to SetVirtualAddressMap below, so the patch to
     * virtual_start happens inside efi_map_rt_descriptor. */
    uint8_t *p   = map;
    uint8_t *end = map + bi->efi_mmap_size;
    uint32_t desc_size = bi->efi_mmap_desc_size;

    g_rt_runtime_descriptors = 0;
    g_rt_runtime_pages_total = 0;

    while (p + desc_size <= end) {
        EfiMemoryDescriptor *d = (EfiMemoryDescriptor *)p;
        if (d->attribute & EFI_MEMORY_RUNTIME) {
            if (!efi_map_rt_descriptor(d, kctx)) {
                kprintf("[EFI] a runtime region could not be mapped — "
                        "no EFI runtime services on this boot\n");
                return false;
            }
        }
        p += desc_size;
    }

    if (g_rt_runtime_descriptors == 0) {
        debug_printf("[EFI] no EFI_MEMORY_RUNTIME descriptors found; "
                     "calling RT in physical mode\n");
        /* No SVAM needed — RT services are usable at their physical
         * addresses since the Pull Map keeps RAM identity-accessible. */
        g_rt = (EfiRuntimeServices *)vmm_phys_to_virt(
            (uintptr_t)bi->efi_rt_services_phys);
        if (!g_rt) return false;
        if (!g_rt_lock_init) { spinlock_init(&g_rt_lock); g_rt_lock_init = true; }
        g_rt_available = true;
        return true;
    }

    /* Stage 2: find the descriptor that contains rt_services_phys so we
     * can compute the new VA for the RT services pointer. */
    EfiMemoryDescriptor *rt_desc = efi_find_descriptor(
        map, bi->efi_mmap_size, desc_size, bi->efi_rt_services_phys);
    if (!rt_desc) {
        debug_printf("[EFI] cannot locate descriptor for rt_services 0x%lx\n",
                     (unsigned long)bi->efi_rt_services_phys);
        return false;
    }
    if (!(rt_desc->attribute & EFI_MEMORY_RUNTIME)) {
        debug_printf("[EFI] descriptor for rt_services 0x%lx lacks RUNTIME bit\n",
                     (unsigned long)bi->efi_rt_services_phys);
        return false;
    }
    uint64_t rt_new_va = rt_desc->virtual_start +
                         (bi->efi_rt_services_phys - rt_desc->physical_start);

    /* Stage 3: invoke SetVirtualAddressMap via the still-physical RT
     * pointer (firmware is currently in flat-physical mode; the call
     * itself is the transition point). After the call returns, firmware
     * uses virtual addresses. */
    EfiRuntimeServices *rt_phys = (EfiRuntimeServices *)vmm_phys_to_virt(
        (uintptr_t)bi->efi_rt_services_phys);
    if (!rt_phys || !rt_phys->set_virtual_address_map) {
        kprintf("[EFI] RT services table at 0x%lx has no SVAM entry\n",
                (unsigned long)bi->efi_rt_services_phys);
        return false;
    }

    /* Stage 3a: build the array the firmware is actually given — the runtime
     * descriptors, and nothing else, at an address that is its own physical
     * address.
     *
     * Two changes from handing over the whole map by its Pull Map pointer,
     * and both are about not relying on a courtesy:
     *
     *   - ONLY RUNTIME. UEFI 2.10 §8.4 describes VirtualMap as the new
     *     addresses "for all runtime ranges"; EDK2's RuntimeDriverSetVirtual-
     *     AddressMap tolerates the rest by skipping every descriptor without
     *     EFI_MEMORY_RUNTIME, and Linux never sends them at all. Sending
     *     hundreds of descriptors a firmware is only specified to ignore is
     *     leaning on one implementation's forgiveness.
     *
     *   - BY PHYSICAL ADDRESS. The firmware has not switched to virtual
     *     addressing yet — that is what this call does — so a pointer it
     *     receives is one it is entitled to read as physical. We were passing
     *     0xFFFF8800_xxxxxxxx, a Pull Map address, which works only because
     *     our CR3 happens to map it; the register dump from the laptop that
     *     panicked here shows those very pointers inside firmware code. Linux
     *     passes __pa(new_memmap) and identity-maps it first. So do we. */
    size_t   rt_bytes  = (size_t)g_rt_runtime_descriptors * desc_size;
    size_t   rt_pages  = (rt_bytes + 0xFFFu) / 0x1000u;
    uintptr_t svam_phys = (uintptr_t)pmm_alloc(rt_pages);
    if (!svam_phys) {
        kprintf("[EFI] cannot allocate %lu page(s) for the SVAM map\n",
                (unsigned long)rt_pages);
        return false;
    }
    {
        vmm_map_result_t rs = vmm_map_pages(kctx, svam_phys, svam_phys,
                                            rt_pages, VMM_FLAGS_KERNEL_RW);
        if (!rs.success) {
            kprintf("[EFI] cannot identity-map the SVAM map at 0x%lx: %s\n",
                    (unsigned long)svam_phys,
                    rs.error_msg ? rs.error_msg : "(no msg)");
            pmm_free((void *)svam_phys, rt_pages);
            return false;
        }
    }

    uint8_t *svam_map = (uint8_t *)vmm_phys_to_virt(svam_phys);
    uint32_t svam_count = 0;
    for (uint8_t *q = map; q + desc_size <= end; q += desc_size) {
        EfiMemoryDescriptor *d = (EfiMemoryDescriptor *)q;
        if (!(d->attribute & EFI_MEMORY_RUNTIME)) continue;
        if (svam_count >= g_rt_runtime_descriptors) break;
        memcpy(svam_map + (size_t)svam_count * desc_size, d, desc_size);
        svam_count++;
    }

    /* Memory the firmware may still walk into while it relocates itself is
     * held out of the allocator (PmmHoldBootServicesMemory, at pmm_init) and
     * mapped where the firmware last saw it, for the duration of this call
     * and no longer. */
    efi_boot_services_alias(map, bi->efi_mmap_size, desc_size, kctx, true);

    kprintf("[EFI] SetVirtualAddressMap: %u runtime region(s), %lu page(s) "
            "at base 0x%lx; rt_services VA=0x%lx\n",
            svam_count,
            (unsigned long)g_rt_runtime_pages_total,
            (unsigned long)EFI_RT_VA_BASE,
            (unsigned long)rt_new_va);

    EfiStatus s = rt_phys->set_virtual_address_map(
        (EfiUintn)((EfiUintn)svam_count * desc_size),
        (EfiUintn)desc_size,
        bi->efi_mmap_desc_ver,
        (EfiMemoryDescriptor *)svam_phys);

    /* The array was only ever an argument. Firmware keeps no pointer to it
     * (EDK2 clears mVirtualMap on the way out of the same call), so the
     * pages and their identity mapping go back now. */
    vmm_unmap_pages(kctx, svam_phys, rt_pages);
    pmm_free((void *)svam_phys, rt_pages);

    /* Whatever the firmware answered, it is done relocating: the alias comes
     * down and the memory goes back to the machine. */
    efi_boot_services_alias(map, bi->efi_mmap_size, desc_size, kctx, false);
    PmmReleaseBootServicesMemory();

    /* And now — only now — the runtime regions can be tightened to what the
     * firmware itself declared. Before this call it was patching its own code
     * and needed the write permission it is about to lose. */
    if (!EFI_IS_ERROR(s))
        efi_apply_memory_attributes(kctx, map, bi->efi_mmap_size, desc_size);

    if (EFI_IS_ERROR(s)) {
        debug_printf("[EFI] SetVirtualAddressMap failed: 0x%lx\n",
                     (unsigned long)s);
        /* SVAM failure leaves firmware in physical mode per UEFI 2.10
         * §8.4 "If SetVirtualAddressMap() returns an error, EFI runtime
         * services remain available in physical mode." So we keep using
         * the physical pointer. */
        g_rt = rt_phys;
        if (!g_rt_lock_init) { spinlock_init(&g_rt_lock); g_rt_lock_init = true; }
        g_rt_available = true;
        return true;
    }

    /* Stage 4: SVAM succeeded — every internal RT pointer in the table
     * has been re-based to the VAs we supplied. Use the new VA. */
    EfiRuntimeServices *rt_new = (EfiRuntimeServices *)(uintptr_t)rt_new_va;

    /* Defensive: refuse a rebased pointer that lies outside the runtime
     * VA window (firmware bug). Better to fall back to physical-mode RT
     * than dispatch to a wild address.
     *
     * Bounds:
     *   - lower: must be >= EFI_RT_VA_BASE (otherwise virtual_start
     *     was never set, or set wrong).
     *   - upper: skipped — EFI_RT_VA_BASE has bits 40..63 set, so
     *     `EFI_RT_VA_BASE + N` for any N >= 2^40 overflows uint64_t.
     *     In practice RT services live in low-phys memory (typically
     *     < 4 GiB) so the lower-bound check + canonical-form check
     *     below is sufficient. The canonical-form check ensures bits
     *     47..63 are still set (kernel canonical half), which catches
     *     any wild carry-out that wrapped the VA into user space. */
    const uint64_t CANONICAL_KERNEL_MASK = 0xFFFF800000000000ULL;
    if (rt_new_va < EFI_RT_VA_BASE ||
        (rt_new_va & CANONICAL_KERNEL_MASK) != CANONICAL_KERNEL_MASK) {
        debug_printf("[EFI] rebased rt_services VA 0x%lx outside RT window — "
                     "keeping physical mode\n", (unsigned long)rt_new_va);
        g_rt = rt_phys;
    } else {
        /* Validate the CRC32 of the RT services table per UEFI 2.10 §4.2.
         * Firmware re-computes this when it patches function pointers
         * during SVAM; a mismatch indicates a buggy firmware whose table
         * we should not trust. Downgrade to physical mode on failure. */
        if (efi_table_header_valid(rt_new)) {
            g_rt = rt_new;
            debug_printf("[EFI] runtime services online at VA 0x%lx "
                         "(post-SVAM, CRC OK)\n",
                         (unsigned long)rt_new_va);
        } else {
            debug_printf("[EFI] rt_services CRC32 mismatch at VA 0x%lx — "
                         "downgrading to physical mode\n",
                         (unsigned long)rt_new_va);
            g_rt = rt_phys;
        }
    }

    if (!g_rt_lock_init) { spinlock_init(&g_rt_lock); g_rt_lock_init = true; }
    g_rt_available = true;

    /*
     * And then it USES the thing, once, before anything depends on it.
     *
     * Everything above this line is arrangement: pages mapped, a pointer
     * rebased, permissions tightened to what the firmware asked for. None of
     * it is evidence. GetTime is the cheapest runtime service there is — it
     * reads the RTC and returns — and dispatching it here puts the whole new
     * arrangement under load at the one moment the machine can still say
     * something useful about it: through the rebased pointer, into the
     * relocated code, off the read-only pages.
     *
     * If SVAM rebased us wrong, or the attributes table tightened something
     * the firmware still writes, this is where it shows — with a year on the
     * screen, rather than three subsystems later at shutdown with no clue
     * which arrangement was to blame.
     *
     * A refusal is not a failure. UEFI 2.10 §8.1: a platform may publish an
     * EFI_RT_PROPERTIES_TABLE saying a service is unsupported at runtime, and
     * must still provide a callable implementation that returns
     * EFI_UNSUPPORTED. So the interesting outcome is not the status — it is
     * that the call RETURNED.
     */
    {
        EfiTime t;
        memset(&t, 0, sizeof(t));
        EfiStatus gs = efi_get_time(&t, NULL);
        if (!EFI_IS_ERROR(gs)) {
            kprintf("[EFI] runtime services answer: firmware clock reads "
                    "%04u-%02u-%02u %02u:%02u:%02u\n",
                    t.year, t.month, t.day, t.hour, t.minute, t.second);
        } else {
            kprintf("[EFI] runtime services answer: GetTime returned 0x%lx "
                    "(the call came back, which is the part that was in "
                    "question)\n", (unsigned long)gs);
        }
    }

    return true;
}

bool efi_runtime_available(void)
{
    return g_rt_available && g_rt != NULL;
}

/* =========================================================================
 * Internal helpers (consumed by efi_variable.c / efi_wakeup.c /
 * efi_capsule.c / efi_esrt.c / efi_secureboot.c). Defined here because
 * they touch g_rt + g_rt_lock + the diagnostics globals.
 * ========================================================================= */

EfiRuntimeServices *efi_rt_get(void)
{
    return g_rt_available ? g_rt : NULL;
}

bool efi_guid_equal(const EfiGuid *a, const EfiGuid *b)
{
    if (!a || !b) return false;
    return memcmp(a, b, sizeof(EfiGuid)) == 0;
}

void efi_rt_lock(uint64_t *saved_rflags)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    spin_lock(&g_rt_lock);
    *saved_rflags = rflags;
}

void efi_rt_unlock(uint64_t saved_rflags)
{
    spin_unlock(&g_rt_lock);
    if (saved_rflags & (1ULL << 9)) __asm__ volatile("sti");
}

void efi_rt_watch_start(uint64_t *t0)
{
    *t0 = rdtsc();
}

void efi_rt_watch_end(const char *name, uint64_t t0)
{
    uint64_t khz = cpu_get_tsc_freq_khz();
    if (!khz) return;
    uint64_t elapsed_ms = (rdtsc() - t0) / khz;
    if (elapsed_ms > EFI_RT_CALL_WARN_MS) {
        debug_printf("[EFI] WARNING: %s took %lu ms (firmware slow)\n",
                     name, (unsigned long)elapsed_ms);
    }
}

/* =========================================================================
 * Public wrappers — Reset / Time
 *
 * UEFI 2.10 §8.1 "Runtime services are not reentrant; the caller must
 * serialise multiple invocations." We hold a spinlock and disable IRQs
 * around each call — RT must not preempt itself.
 * ========================================================================= */

void efi_reset_system(EfiResetType type, EfiStatus status,
                      uint64_t data_size, void *data)
{
    if (!efi_runtime_available() || !g_rt->reset_system) {
        debug_printf("[EFI] reset_system unavailable\n");
        return;
    }

    /* CLI then take the lock: this call may not return. We disable IRQs
     * before the lock so the wakeup we'd miss is genuinely irrelevant. */
    __asm__ volatile("cli");
    spin_lock(&g_rt_lock);

    debug_printf("[EFI] ResetSystem(type=%u, status=0x%lx)\n",
                 type, (unsigned long)status);

    g_rt->reset_system(type, status, (EfiUintn)data_size, data);

    /* If we return, the firmware refused the reset — release the lock
     * and let the caller fall through to alternate methods. */
    spin_unlock(&g_rt_lock);
    debug_printf("[EFI] ResetSystem returned (firmware refused)\n");
}

EfiStatus efi_get_time(EfiTime *time, EfiTimeCapabilities *cap)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->get_time) return EFI_STATUS_UNSUPPORTED;

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);
    EfiStatus s = rt->get_time(time, cap);
    efi_rt_watch_end("GetTime", t0);
    efi_rt_unlock(rflags);
    return s;
}

EfiStatus efi_set_time(EfiTime *time)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->set_time) return EFI_STATUS_UNSUPPORTED;

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);
    EfiStatus s = rt->set_time(time);
    efi_rt_watch_end("SetTime", t0);
    efi_rt_unlock(rflags);

    /* SetTime mutates persistent RTC state — publish a Touch event so
     * subscribers (clockboard, audit log) can resync without polling. */
    if (!EFI_IS_ERROR(s)) {
        debug_printf("[EFI] SetTime succeeded\n");
    }
    return s;
}

/* =========================================================================
 * Configuration Table access (UEFI 2.10 §4.6)
 * ========================================================================= */

EfiConfigurationTable *efi_get_configuration_table(uint32_t *out_count)
{
    if (out_count) *out_count = 0;

    boot_info_t *bi = boot_info_get();
    if (!bi || !boot_info_valid(bi) || bi->version != BOOT_INFO_VERSION4) {
        return NULL;
    }
    if (!bi->efi_cfg_table_phys || bi->efi_cfg_table_count == 0) {
        return NULL;
    }

    EfiConfigurationTable *ct = (EfiConfigurationTable *)
        vmm_phys_to_virt((uintptr_t)bi->efi_cfg_table_phys);
    if (!ct) return NULL;

    if (out_count) *out_count = bi->efi_cfg_table_count;
    return ct;
}

void *efi_find_configuration_table(const EfiGuid *target)
{
    if (!target) return NULL;

    uint32_t count = 0;
    EfiConfigurationTable *ct = efi_get_configuration_table(&count);
    if (!ct) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        if (efi_guid_equal(&ct[i].vendor_guid, target)) {
            return ct[i].vendor_table;
        }
    }
    return NULL;
}

void efi_runtime_print_info(void)
{
    if (!efi_runtime_available()) {
        debug_printf("[EFI] runtime services unavailable\n");
        return;
    }

    uint32_t cfg_count = 0;
    (void)efi_get_configuration_table(&cfg_count);

    debug_printf("[EFI] runtime: fw_rev=0x%x rt=%p desc_count=%u pages=%lu "
                 "cfg_entries=%u\n",
                 g_rt_fw_revision, (void *)g_rt,
                 g_rt_runtime_descriptors,
                 (unsigned long)g_rt_runtime_pages_total,
                 cfg_count);
}
