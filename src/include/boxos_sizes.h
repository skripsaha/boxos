#ifndef BOXOS_SIZES_H
#define BOXOS_SIZES_H

#define PAGE_SIZE              4096

/* 2 MiB huge page — single PDE leaf with VMM_FLAG_LARGE_PAGE. Shared
 * with userspace so the boxlib heap allocator can talk to the kernel's
 * SYSTEM_OP_HEAP_PREFAULT in the same vocabulary. Keep in lock-step
 * with VMM_LARGE_PAGE_2M_SIZE in vmm.h. */
#define LARGE_PAGE_2M_SIZE     0x200000UL
#define LARGE_PAGE_2M_MASK     (LARGE_PAGE_2M_SIZE - 1)
#define LARGE_PAGE_2M_PAGES    (LARGE_PAGE_2M_SIZE / PAGE_SIZE)

/* Phase 12: every per-syscall buffer-size constant moved out.
 * - PocketRing/ResultRing capacities live in cabin_layout.h
 *   (POCKET_RING_SLOT_MAX / RESULT_RING_SLOT_MAX), derived from a 1 MiB
 *   reserved slot range and the 128/32-byte slot stride. They are
 *   instance-time, not compile-time.
 * - Prefix-chain limits (POCKET_MAX_PREFIXES, POCKET_ROUTE_TAG_SIZE) are
 *   gone with the prefix-chain dispatcher itself. */

#endif /* BOXOS_SIZES_H */
