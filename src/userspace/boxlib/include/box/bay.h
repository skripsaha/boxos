#ifndef BOX_BAY_H
#define BOX_BAY_H

#include "box/types.h"
#include "box/error.h"

/*
 * Bay — cross-cabin shared memory, tag-driven, refcounted.
 *
 * One call, one pointer: bay_open returns a user pointer directly. The
 * kernel resolves the tag string into the shared physical pages and
 * maps them into THIS cabin's address space. Every other cabin that
 * opens the same tag sees the SAME memory.
 *
 *   Cabin A:  void *fb = bay_open("video:frame:0", 3UL<<30, BAY_CREATE);
 *             ... write into fb ...
 *
 *   Cabin B:  void *fb = bay_open("video:frame:0", 0, BAY_OPEN);
 *             ... read from fb (same physical pages) ...
 *
 * Cleanup is automatic on process_destroy — leaked claims do not
 * starve PMM. Explicit bay_release(ptr) drops the per-cabin claim;
 * when the last claim across all cabins drops, the backing pages
 * return to PMM.
 *
 * Allocations ≥ 2 MB are silently backed by 2 MB pages (one PDE per
 * 512 × 4 KB unit) for TLB efficiency. The userspace API has no knob
 * for this — the kernel decides.
 *
 * Names are TagFS-rooted, "key:value" or bare "key". Use the same
 * tag conventions as Touch ("video:frame:0", "ml:weights:resnet50",
 * "audio:mix:left", etc.) so the surface stays uniform.
 */

#define BAY_OPEN     0x00u   /* default — fail if tag missing */
#define BAY_CREATE   0x01u   /* create if missing (size must be > 0) */
#define BAY_RO       0x02u   /* read-only mapping */

/* Open or create a Bay by tag. Returns a user-VA pointer mapped to the
 * shared physical pages, or NULL on failure (errno-style return value
 * via box_last_error()). For BAY_CREATE size MUST be non-zero; for
 * BAY_OPEN size is informational (the existing Bay's size wins). */
void   *bay_open(const char *tag, uint64_t size, uint32_t flags);

/* Release this cabin's claim on the Bay. Drops one ref; if the last
 * cabin releases, the backing pages return to PMM. Returns 0 on
 * success, -ERR_NOT_FOUND if ptr does not name an open Bay claim. */
int     bay_release(void *ptr);

/* Query the Bay's user-visible size (in bytes). Returns 0 if ptr is
 * not a live claim. */
uint64_t bay_size(void *ptr);

#endif /* BOX_BAY_H */
