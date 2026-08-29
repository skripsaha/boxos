#ifndef PMM_H
#define PMM_H

#include "klib.h"
#include "error.h"
#include "buddy.h"
#include "kernel_config.h"  // CONFIG_PHYS_ZONE_DMA32_END, CONFIG_PHYS_ZONE_USER_END

#define PMM_PAGE_SIZE       4096
#define PMM_BITMAP_ALIGN    8

/* ─── Zone bias hints ─────────────────────────────────────────────────
 * Pass to pmm_alloc(pages, PHYS_TAG_*) as a hint for which physical zone
 * the buddy should allocate from. Direct buddy_alloc_range; no bitmap
 * involved. Distinct single-bit constants so callers may OR them.
 *
 *   PHYS_TAG_DMA32  — [0,   DMA32_END)  ISA/PCI 32-bit DMA safe
 *   PHYS_TAG_USER   — [DMA32_END, USER_END)  general purpose <4GB
 *   PHYS_TAG_HIGH   — [USER_END, mem_end)    above 4GB, post Pull Map
 *
 * Semantic ("purpose:*", "audio:*", ...) tagging is MemTag's job — see
 * MemTagPmmAlloc / MemTagApply in memtag.h. */
#define PHYS_TAG_DMA32   (1ULL << 0)
#define PHYS_TAG_USER    (1ULL << 1)
#define PHYS_TAG_HIGH    (1ULL << 2)

typedef enum {
    PMM_FRAME_FREE = 0,
    PMM_FRAME_USED,
    PMM_FRAME_RESERVED,
    PMM_FRAME_KERNEL,
    PMM_FRAME_BAD
} pmm_frame_state_t;

error_t pmm_init(void);
void    pmm_free(void* addr, size_t pages);

/*
 * The memory UEFI used to boot us, held back until firmware has finished
 * moving house.
 *
 * UEFI 2.10 §7.4 says EfiBootServicesCode and EfiBootServicesData become the
 * OS's to use once ExitBootServices has returned, and TagBoot reports them to
 * the kernel as ordinary usable RAM on that authority. The trouble is that
 * SetVirtualAddressMap runs LATER — it is the one firmware call the OS makes
 * while firmware is still relocating itself — and a long line of shipping
 * firmwares reach into their boot-services memory while doing it. Linux
 * carries the same workaround under the same names (efi_reserve_boot_services
 * / efi_free_boot_services) and maps those regions for the duration.
 *
 * So: Hold before the buddy allocator can hand them out (from pmm_init, which
 * is the only moment early enough), Release once SetVirtualAddressMap has
 * returned. Between those two points the machine is short exactly the memory
 * the firmware might still be standing on, which on a typical board is a few
 * tens of megabytes for a few dozen milliseconds.
 *
 * Both are no-ops on a BIOS boot, where there is no EFI memory map to read,
 * and Release is idempotent — it is called once from the SetVirtualAddressMap
 * path and once as a backstop from kernel_main, because a boot that never
 * reaches SVAM must still get its memory back.
 */
void    PmmHoldBootServicesMemory(void);
void    PmmReleaseBootServicesMemory(void);

size_t   pmm_total_pages(void);
/* Largest page count a SINGLE pmm_alloc can serve (the buddy's max block).
 * A caller that wants one contiguous span should clamp to this instead of
 * discovering the limit through a failed allocation — that path logs
 * PMM_FAIL, which then reads as a shortage in every boot log. */
size_t   pmm_max_alloc_pages(void);
size_t   pmm_free_pages(void);
size_t   pmm_used_pages(void);
uint64_t pmm_get_total_memory(void);     /* phys-top of buddy zone (one-past-last byte) */
uint64_t pmm_get_total_ram_bytes(void);  /* total_pages * PMM_PAGE_SIZE                 */
void     pmm_dump_stats(void);

void pmm_print_memory_map(void);

error_t pmm_set_maxphyaddr(uint8_t maxphyaddr);
uint8_t pmm_get_maxphyaddr(void);

uint64_t pmm_get_mem_end(void);

bool pmm_is_usable_ram(uintptr_t phys_addr, size_t size);

/* NUMA proximity domain of `phys` based on ACPI SRAT. Returns
 * 0xFFFFFFFF when ACPI / SRAT did not classify the address (UMA hardware
 * or address outside every enabled memory range). Wraps
 * `acpi_numa_domain_for_phys()` so PMM can call without including ACPI
 * headers — keeps the existing memory subsystem free of ACPI deps. */
uint32_t pmm_phys_domain(uintptr_t phys);

/* Walk every page-aligned chunk inside [base, base+len) and report the
 * NUMA breakdown to the boot log. Coalesces consecutive same-domain
 * ranges so the output stays compact. Pure observation — no
 * side-effect. Used at pmm_init() tail when SRAT is present. */
void pmm_log_numa_topology(void);

/* Try to allocate `pages` contiguous physical pages whose backing memory
 * is reported by SRAT to belong to `domain`. Falls back to any-domain
 * allocation when:
 *   - ACPI / SRAT did not classify memory (UMA hardware), OR
 *   - no domain-local range satisfies the request.
 *
 * This is a *hint*, not a guarantee: the current allocator does not
 * partition the buddy by domain, so we sample buddy_alloc results and
 * retry up to `attempts` times if the returned page falls outside the
 * desired domain. Production NUMA allocator (deferred) will partition
 * the buddy and remove the sampling overhead. */
void* pmm_alloc_in_domain(size_t pages, uint32_t domain);

/* Total pages SRAT declares as enabled-memory in `domain`. Includes
 * pages currently allocated — the future NUMA buddy partition will
 * track free pages per domain; until then, this is the static upper
 * bound and is sufficient for scheduler placement heuristics.
 * Returns 0 when SRAT is absent or the domain is unknown. */
size_t pmm_pages_in_domain(uint32_t domain);

void pmm_activate_pull_map(void);
void pmm_test_high_memory(void);

/* ─── Phase 2F — MCE poison-page bitmap ──────────────────────────────
 *
 * The MCE handler (src/kernel/core/mce/mce.c) calls pmm_set_poisoned()
 * for every phys page reported by IA32_MC<i>_ADDR after a UCR or UC
 * error. The buddy allocator post-checks every returned chunk against
 * the bitmap; chunks containing poisoned pages are freed and the alloc
 * retries (up to PMM_MCE_RETRY_MAX times).
 *
 * Bitmap is allocated at pmm_init tail and sized to pmm_get_mem_end() /
 * PMM_PAGE_SIZE bits (~128 KiB per 4 GB RAM). Atomic byte-OR / byte-LOAD
 * for thread-safe SET/IS without a lock.
 *
 * Safe to call from IST-context #MC handler — no kmalloc, no locks. */
void pmm_set_poisoned(uintptr_t phys);
bool pmm_is_poisoned(uintptr_t phys);
bool pmm_is_range_poisoned(uintptr_t phys, size_t pages);
size_t pmm_poisoned_page_count(void);

// Expose the internal BuddyZone for use by the Friend Allocator layer.
BuddyZone* pmm_get_buddy_zone(void);

// ─── Physical allocation — variadic interface ─────────────────────────────────
//
//   pmm_alloc(pages)                — any zone, first-fit
//   pmm_alloc(pages, PHYS_TAG_ZONE) — constrained to the named zone
//   pmm_alloc_zero(...)             — same, plus zero-fill via Pull Map
//
// PHYS_TAG_DMA32 / PHYS_TAG_USER / PHYS_TAG_HIGH are zone HINTS, not
// semantic tags — they bias the buddy to a phys range. For semantic tags
// (string "key:value" form) use MemTagPmmAlloc(pages, "tag") from
// memtag.h, which routes through the same buddy and additionally
// registers a MemRegion under the tag.
//
// pmm_free() is always tag-agnostic — the buddy owns allocation state;
// MemTagPmmFreed (called from pmm_free) destroys any region that was
// registered against the freed range.
// ─────────────────────────────────────────────────────────────────────────────

void* _pmm_alloc_impl(size_t pages, uint64_t zone_hint);
void* _pmm_alloc_zero_impl(size_t pages, uint64_t zone_hint);

#define _PMM_NARG(...)              _PMM_NARG_I(__VA_ARGS__, 2, 1)
#define _PMM_NARG_I(_1, _2, N, ...) N
#define _PMM_CAT(a, b)              _PMM_CAT_(a, b)
#define _PMM_CAT_(a, b)             a##b

#define _pmm_alloc_1(p)             _pmm_alloc_impl((p), 0ULL)
#define _pmm_alloc_2(p, hint)       _pmm_alloc_impl((p), (uint64_t)(hint))

#define pmm_alloc(...)              _PMM_CAT(_pmm_alloc_, _PMM_NARG(__VA_ARGS__))(__VA_ARGS__)

#define _pmm_alloc_zero_1(p)        _pmm_alloc_zero_impl((p), 0ULL)
#define _pmm_alloc_zero_2(p, hint)  _pmm_alloc_zero_impl((p), (uint64_t)(hint))

#define pmm_alloc_zero(...)         _PMM_CAT(_pmm_alloc_zero_, _PMM_NARG(__VA_ARGS__))(__VA_ARGS__)

/* ─── TME / TME-MK: KeyID-tagged allocations ─────────────────────────
 *
 * Returns a kernel virtual pointer to `pages` contiguous PMM-backed
 * pages. The kernel's identity mapping still uses raw phys (= KeyID 0,
 * platform-default encryption) — these allocations are intended to be
 * RE-MAPPED into another address space (typically a user cabin via
 * vmm_map_user_with_keyid) using the supplied KeyID. The kernel side
 * normally does NOT touch these pages directly after allocation; the
 * consumer (e.g. bay_open with BAY_FLAG_ENCRYPTED) owns the lifecycle.
 *
 *   pages : number of 4 KiB pages
 *   keyid : the KeyID reserved via tme_keyid_alloc() that any user-side
 *           mapping must use. KeyID 0 (platform default) is permitted
 *           but degenerates to plain pmm_alloc_zero behavior.
 *
 * The function tags the allocated phys range with `tme:keyid:N` via
 * MemTag so the Touch subscription model + diagnostic dumps see the
 * KeyID association. Returns NULL on out-of-memory.
 *
 * Behavior when TME-MK is inactive (g_tme.mk_active == false): falls
 * back to plain pmm_alloc_zero. Caller's caller (Bay etc.) is expected
 * to gate the encrypted path on tme_keyid_alloc returning OK, so this
 * fallback should only fire under a programmer error.
 */
void *pmm_alloc_with_keyid(size_t pages, uint16_t keyid);

/* Inverse: untag and return pages to the buddy. Does NOT free the
 * KeyID itself (that's tme_keyid_free); the caller manages KeyID
 * lifecycle independently because one KeyID may govern multiple
 * non-contiguous allocations. */
void pmm_free_with_keyid(void *va, size_t pages, uint16_t keyid);

#endif
