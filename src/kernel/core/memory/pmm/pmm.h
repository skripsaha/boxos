#ifndef PMM_H
#define PMM_H

#include "klib.h"
#include "error.h"
#include "buddy.h"
#include "pmtag.h"          // PHYS_TAG_* visible at every pmm_alloc call site
#include "kernel_config.h"  // CONFIG_PHYS_ZONE_DMA32_END, CONFIG_PHYS_ZONE_USER_END

#define PMM_PAGE_SIZE       4096
#define PMM_BITMAP_ALIGN    8

typedef enum {
    PMM_FRAME_FREE = 0,
    PMM_FRAME_USED,
    PMM_FRAME_RESERVED,
    PMM_FRAME_KERNEL,
    PMM_FRAME_BAD
} pmm_frame_state_t;

error_t pmm_init(void);
void    pmm_free(void* addr, size_t pages);

size_t   pmm_total_pages(void);
size_t   pmm_free_pages(void);
size_t   pmm_used_pages(void);
uint64_t pmm_get_total_memory(void);
void     pmm_dump_stats(void);

void pmm_print_memory_map(void);
bool pmm_check_integrity(void);

error_t pmm_set_maxphyaddr(uint8_t maxphyaddr);
uint8_t pmm_get_maxphyaddr(void);

uint64_t pmm_get_mem_end(void);

bool pmm_is_usable_ram(uintptr_t phys_addr, size_t size);

void pmm_activate_pull_map(void);
void pmm_test_high_memory(void);

// Expose the internal BuddyZone for use by the Friend Allocator layer.
BuddyZone* pmm_get_buddy_zone(void);

// ─── Physical allocation — variadic interface ─────────────────────────────────
//
// pmm_alloc(pages)               — any zone, first-fit (O(log max_order))
// pmm_alloc(pages, PHYS_TAG_*)   — constrained to zone, direct range alloc
//                                   O(log max_order) for well-known zone tags
//                                   (DMA32, USER, HIGH bypass the PMTAG scan)
// pmm_alloc_zero(pages [, tag])  — same, plus memset to 0 via Pull Map
//
// Tags understood for fast-path (no PMTAG scan):
//   PHYS_TAG_DMA32  — [0,  1GB)   ISA/PCI 32-bit DMA safe
//   PHYS_TAG_USER   — [1GB, 4GB)  general purpose below identity-map limit
//   PHYS_TAG_HIGH   — [4GB, end)  post Phase-2, requires Pull Map
//
// Any other PHYS_TAG_* (SHARED, KERNEL, MMIO, user-defined) falls through to
// PhysAllocTagged() which scans the PMTAG band index.
//
// pmm_free() is always tag-agnostic — the buddy owns allocation state.
// ─────────────────────────────────────────────────────────────────────────────

void* _pmm_alloc_impl(size_t pages, uint64_t tags);
void* _pmm_alloc_zero_impl(size_t pages, uint64_t tags);

// Forward declaration — defined in memtag.c. Not including memtag.h here avoids
// circular dependency (memtag.h includes pmm.h).
void* _pmm_alloc_memtag(size_t pages, const char *tag);

// Argument-count dispatcher (1 or 2 args)
#define _PMM_NARG(...)              _PMM_NARG_I(__VA_ARGS__, 2, 1)
#define _PMM_NARG_I(_1, _2, N, ...) N
#define _PMM_CAT(a, b)              _PMM_CAT_(a, b)
#define _PMM_CAT_(a, b)             a##b

#define _pmm_alloc_1(p)             _pmm_alloc_impl((p), 0ULL)

// Type dispatch: integer second arg → zone-constrained allocation (fast path).
//               string/pointer second arg → MemTag registration via _pmm_alloc_memtag.
// The unary + forces array decay (char[N] → char*) while leaving integer types
// unchanged, giving __builtin_types_compatible_p a stable type to inspect.
#define _pmm_alloc_2(p, arg)                                                              \
    __builtin_choose_expr(                                                                 \
        __builtin_types_compatible_p(__typeof__(+(arg)), unsigned long long)              \
     || __builtin_types_compatible_p(__typeof__(+(arg)), unsigned long)                   \
     || __builtin_types_compatible_p(__typeof__(+(arg)), unsigned int)                    \
     || __builtin_types_compatible_p(__typeof__(+(arg)), unsigned short)                  \
     || __builtin_types_compatible_p(__typeof__(+(arg)), unsigned char),                  \
        _pmm_alloc_impl((p), (uint64_t)(arg)),                                            \
        _pmm_alloc_memtag((p), (const char *)(arg))                                       \
    )

#define pmm_alloc(...)              _PMM_CAT(_pmm_alloc_, _PMM_NARG(__VA_ARGS__))(__VA_ARGS__)

#define _pmm_alloc_zero_1(p)        _pmm_alloc_zero_impl((p), 0ULL)

#define _pmm_alloc_zero_2(p, arg)                                                         \
    __builtin_choose_expr(                                                                 \
        __builtin_types_compatible_p(__typeof__(+(arg)), unsigned long long)              \
     || __builtin_types_compatible_p(__typeof__(+(arg)), unsigned long)                   \
     || __builtin_types_compatible_p(__typeof__(+(arg)), unsigned int)                    \
     || __builtin_types_compatible_p(__typeof__(+(arg)), unsigned short)                  \
     || __builtin_types_compatible_p(__typeof__(+(arg)), unsigned char),                  \
        _pmm_alloc_zero_impl((p), (uint64_t)(arg)),                                       \
        _pmm_alloc_memtag((p), (const char *)(arg))                                       \
    )

#define pmm_alloc_zero(...)         _PMM_CAT(_pmm_alloc_zero_, _PMM_NARG(__VA_ARGS__))(__VA_ARGS__)

#endif
