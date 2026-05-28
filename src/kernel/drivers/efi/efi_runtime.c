/*
 * BoxOS — EFI Runtime Services driver (kernel side).
 *
 * Responsibilities (see efi.h for the public surface):
 *   1. Parse the EFI memory map preserved in boot_info v3 (EfiACPIMemoryNVS
 *      region, kept alive across ExitBootServices).
 *   2. Map every EFI_MEMORY_RUNTIME descriptor at EFI_RT_VA_BASE +
 *      physical_start. Cacheability is selected per descriptor type:
 *        - EFI_RUNTIME_SERVICES_CODE  → WB executable (no NX)
 *        - EFI_RUNTIME_SERVICES_DATA  → WB NX
 *        - EFI_MEMORY_MAPPED_IO       → UC (cache disable + write through)
 *        - EFI_MEMORY_MAPPED_IO_PORT  → never mapped; assigned VA for
 *          firmware's bookkeeping per UEFI 2.10 §8.4
 *   3. Call SetVirtualAddressMap (UEFI 2.10 §8.4) once with the patched
 *      descriptors. From this point on, firmware uses the virtual
 *      addresses we supplied.
 *   4. Rebase the runtime_services pointer from physical → virtual so
 *      subsequent ResetSystem / GetTime / GetVariable calls land at the
 *      new VA where the firmware now expects to be entered.
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
#include "boot_info.h"
#include "vmm.h"
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

/* Map flags for a runtime descriptor. Returns 0 if the descriptor must
 * be skipped (e.g. IO port space).
 *
 * NX policy (defense-in-depth, Intel SDM Vol 3 §4.6):
 *   RT code is executable; everything else carries the NX bit so a
 *   firmware bug that branches into RT data / MMIO regions triple-faults
 *   instead of running attacker-controlled bytes. */
static uint64_t efi_rt_map_flags(uint32_t type)
{
    switch (type) {
        case EFI_RUNTIME_SERVICES_CODE:
            /* Executable, writable for relocations during SVAM, WB. */
            return VMM_FLAGS_KERNEL_RW;
        case EFI_RUNTIME_SERVICES_DATA:
        case EFI_ACPI_MEMORY_NVS:
            /* Data — WB, NX. */
            return VMM_FLAGS_KERNEL_RW | VMM_FLAG_NO_EXECUTE;
        case EFI_MEMORY_MAPPED_IO:
            /* MMIO — UC + WT (PCD=1, PWT=1 → UC- effective), NX. */
            return VMM_FLAGS_KERNEL_RW
                 | VMM_FLAG_CACHE_DISABLE
                 | VMM_FLAG_WRITE_THROUGH
                 | VMM_FLAG_NO_EXECUTE;
        case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
            /* IO port space — assigned a VA in the map but not paged. */
            return 0;
        default:
            return 0;
    }
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

    uint64_t flags = efi_rt_map_flags(d->type);
    if (flags == 0 && d->type != EFI_MEMORY_MAPPED_IO_PORT_SPACE) {
        debug_printf("[EFI] RT desc type=%u unmappable, skipping (phys=0x%lx)\n",
                     d->type, (unsigned long)d->physical_start);
        return false;
    }

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
        debug_printf("[EFI] identity vmm_map_pages failed for RT desc type=%u "
                     "phys=0x%lx pages=%lu: %s\n",
                     d->type, (unsigned long)phys, (unsigned long)pages,
                     ri.error_msg ? ri.error_msg : "(no msg)");
        return false;
    }

    /* Canonical kernel VA for the same range — what firmware will use
     * after SVAM. */
    vmm_map_result_t rv = vmm_map_pages(kctx, virt, phys, pages, flags);
    if (!rv.success) {
        debug_printf("[EFI] virtual vmm_map_pages failed for RT desc type=%u "
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
 * Initialisation
 * ========================================================================= */

bool efi_runtime_init(void)
{
    if (g_rt_available) return true;   /* idempotent */

    boot_info_t *bi = boot_info_get();
    if (!bi || !boot_info_valid(bi) || bi->version != BOOT_INFO_VERSION3) {
        debug_printf("[EFI] boot_info is not v3 (boot_method=%u version=%u); "
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
        debug_printf("[EFI] boot_info v3 missing EFI handoff fields\n");
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
                debug_printf("[EFI] failed to map RT desc — aborting SVAM\n");
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
        debug_printf("[EFI] RT services table at 0x%lx has no SVAM thunk\n",
                     (unsigned long)bi->efi_rt_services_phys);
        return false;
    }

    debug_printf("[EFI] calling SetVirtualAddressMap: %u runtime desc, "
                 "%lu pages mapped at base 0x%lx; rt_services VA=0x%lx\n",
                 g_rt_runtime_descriptors,
                 (unsigned long)g_rt_runtime_pages_total,
                 (unsigned long)EFI_RT_VA_BASE,
                 (unsigned long)rt_new_va);

    EfiStatus s = rt_phys->set_virtual_address_map(
        (EfiUintn)bi->efi_mmap_size,
        (EfiUintn)bi->efi_mmap_desc_size,
        bi->efi_mmap_desc_ver,
        (EfiMemoryDescriptor *)map);

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
    return true;
}

bool efi_runtime_available(void)
{
    return g_rt_available && g_rt != NULL;
}

/* =========================================================================
 * Public wrappers
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

/* Soft watchdog: warn if any RT call exceeds this many milliseconds.
 * We cannot preempt firmware mid-call (a stuck RT function hangs the
 * CPU forever), but logging an abnormally long call gives operators a
 * visible signal that the firmware on this board misbehaves. 100 ms is
 * very generous — every well-behaved RT call returns in microseconds. */
#define EFI_RT_CALL_WARN_MS  100u

static inline uint64_t efi_rt_tsc_now(void) { return rdtsc(); }

static void efi_rt_check_elapsed(const char *name, uint64_t t0)
{
    uint64_t khz = cpu_get_tsc_freq_khz();
    if (!khz) return;
    uint64_t elapsed_ms = (rdtsc() - t0) / khz;
    if (elapsed_ms > EFI_RT_CALL_WARN_MS) {
        debug_printf("[EFI] WARNING: %s took %lu ms (firmware slow)\n",
                     name, (unsigned long)elapsed_ms);
    }
}

EfiStatus efi_get_time(EfiTime *time, EfiTimeCapabilities *cap)
{
    if (!efi_runtime_available() || !g_rt->get_time) {
        return EFI_STATUS_ERROR_BIT | 3ULL;   /* EFI_UNSUPPORTED */
    }

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    spin_lock(&g_rt_lock);

    uint64_t t0 = efi_rt_tsc_now();
    EfiStatus s = g_rt->get_time(time, cap);
    efi_rt_check_elapsed("GetTime", t0);

    spin_unlock(&g_rt_lock);
    if (rflags & (1ULL << 9)) __asm__ volatile("sti");
    return s;
}

EfiStatus efi_get_variable(uint16_t *variable_name,
                           const EfiGuid *vendor,
                           uint32_t *attributes,
                           uint64_t *data_size,
                           void *data)
{
    if (!efi_runtime_available() || !g_rt->get_variable) {
        return EFI_STATUS_ERROR_BIT | 3ULL;
    }

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    spin_lock(&g_rt_lock);

    EfiUintn sz = data_size ? (EfiUintn)*data_size : 0;
    uint64_t t0 = efi_rt_tsc_now();
    EfiStatus s = g_rt->get_variable(variable_name, vendor, attributes,
                                      &sz, data);
    efi_rt_check_elapsed("GetVariable", t0);
    if (data_size) *data_size = sz;

    spin_unlock(&g_rt_lock);
    if (rflags & (1ULL << 9)) __asm__ volatile("sti");
    return s;
}

void efi_runtime_print_info(void)
{
    if (!efi_runtime_available()) {
        debug_printf("[EFI] runtime services unavailable\n");
        return;
    }
    debug_printf("[EFI] runtime: fw_rev=0x%x rt=%p desc_count=%u pages=%lu\n",
                 g_rt_fw_revision, (void *)g_rt,
                 g_rt_runtime_descriptors,
                 (unsigned long)g_rt_runtime_pages_total);
}
