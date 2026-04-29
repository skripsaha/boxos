#ifndef BOXOS_SIZES_H
#define BOXOS_SIZES_H

#define PAGE_SIZE              4096

/* Phase 12: every per-syscall buffer-size constant moved out.
 * - PocketRing/ResultRing capacities live in cabin_layout.h
 *   (POCKET_RING_SLOT_MAX / RESULT_RING_SLOT_MAX), derived from a 1 MiB
 *   reserved slot range and the 128/32-byte slot stride. They are
 *   instance-time, not compile-time.
 * - Prefix-chain limits (POCKET_MAX_PREFIXES, POCKET_ROUTE_TAG_SIZE) are
 *   gone with the prefix-chain dispatcher itself. */

#endif /* BOXOS_SIZES_H */
