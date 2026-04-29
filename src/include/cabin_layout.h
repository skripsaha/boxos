#ifndef CABIN_LAYOUT_H
#define CABIN_LAYOUT_H

#include "boxos_limits.h"

/*
 * Fixed virtual address layout for every Cabin (per-process address space).
 *
 * Phase 11 redesign:
 *   - PocketRing/ResultRing each split into two regions: a 1-page header at the
 *     traditional low address (kept for ABI continuity) and a separately-mapped
 *     slot region at a high virtual address.
 *   - The slot region is *reserved* virtual but *lazily mapped* phys: the page
 *     fault handler allocates and maps slot pages on first touch (PocketRing
 *     producer is userspace; vmm_ensure_user_page handles ResultRing).
 *   - Indices in the header are monotonic 64-bit. Slot lookup uses modulo
 *     against slot_count_max — once a virtual slot page is mapped, it is reused
 *     forever via wrap-around.
 *   - Maximum physical use per ring is bounded by (slot_count_max * slot_size)
 *     rounded up to whole pages — i.e. the reservation. Idle processes only
 *     pay for the header pages.
 */

#define CABIN_NULL_TRAP_START          0x0000000000000000ULL
#define CABIN_NULL_TRAP_END            0x0000000000000FFFULL
#define CABIN_NULL_TRAP_SIZE           4096

/* CabinInfo: read-only metadata */
#define CABIN_INFO_ADDR                0x0000000000001000ULL
#define CABIN_INFO_SIZE                0x1000
#define CABIN_INFO_PAGES               1

/* PocketRing header (head/tail/slots_base/slot_size/slot_count_max). */
#define CABIN_POCKET_RING_ADDR         0x0000000000002000ULL
#define CABIN_POCKET_RING_SIZE         0x1000
#define CABIN_POCKET_RING_PAGES        1

/* ResultRing header (same layout as PocketRing header). */
#define CABIN_RESULT_RING_ADDR         0x0000000000003000ULL
#define CABIN_RESULT_RING_SIZE         0x1000
#define CABIN_RESULT_RING_PAGES        1

/* Code starts after the ring headers. The 0x4000-0xC000 gap (formerly part of
 * the legacy fixed-size ResultRing) is left unmapped — it is recoverable in a
 * later phase but for now we keep CABIN_CODE_START_ADDR at the same value so
 * existing user ELFs (linked at 0xC000) continue to load unchanged. */
#define CABIN_CODE_START_ADDR          0x000000000000C000ULL

/* High-VA slot regions. 64 TiB into user space — well clear of the heap and
 * buffer-heap regions below, AND well below the 128 TiB canonical-address
 * boundary (bit 47). Crossing 0x0000_8000_0000_0000 produces non-canonical
 * addresses, which fault with #GP regardless of the page tables. Reserved
 * virtually; physical pages are allocated lazily on first touch. */
#define CABIN_POCKET_SLOTS_BASE        0x0000400000000000ULL
#define CABIN_POCKET_SLOTS_PAGES       256                                /* 1 MiB */
#define CABIN_POCKET_SLOTS_SIZE        (CABIN_POCKET_SLOTS_PAGES * 0x1000ULL)
#define CABIN_POCKET_SLOTS_END         (CABIN_POCKET_SLOTS_BASE + CABIN_POCKET_SLOTS_SIZE)

#define CABIN_RESULT_SLOTS_BASE        0x0000400000100000ULL
#define CABIN_RESULT_SLOTS_PAGES       256                                /* 1 MiB */
#define CABIN_RESULT_SLOTS_SIZE        (CABIN_RESULT_SLOTS_PAGES * 0x1000ULL)
#define CABIN_RESULT_SLOTS_END         (CABIN_RESULT_SLOTS_BASE + CABIN_RESULT_SLOTS_SIZE)

/* Slot strides. POCKET_SLOT_SIZE matches sizeof(Pocket); RESULT_SLOT_SIZE is
 * rounded up from sizeof(Result)=24 to the next power of two so a slot never
 * straddles a page boundary. */
#define POCKET_SLOT_SIZE               128
#define RESULT_SLOT_SIZE               32

/* Capacity computed from reservation / stride. */
#define POCKET_RING_SLOT_MAX           (CABIN_POCKET_SLOTS_SIZE / POCKET_SLOT_SIZE)  /* 8192 */
#define RESULT_RING_SLOT_MAX           (CABIN_RESULT_SLOTS_SIZE / RESULT_SLOT_SIZE)  /* 32768 */

/* Ring header magic comes from boxos_magic.h — POCKET_RING_MAGIC / RESULT_RING_MAGIC. */

/* Nominal base addresses (actual per-process addresses are ASLR randomized). */
#define CABIN_HEAP_BASE                0x0000000010000000ULL
#define CABIN_BUF_HEAP_START           0x0000000040000000ULL

#define CABIN_HEAP_MAX_SIZE            BOXOS_USER_HEAP_MAX_SIZE

#define USER_CODE_ENTRY_POINT          CABIN_CODE_START_ADDR

#endif /* CABIN_LAYOUT_H */
