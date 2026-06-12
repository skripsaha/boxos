#ifndef BOX_BAY_H
#define BOX_BAY_H

#ifdef __cplusplus
extern "C" {
#endif

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

#define BAY_OPEN        0x00u   /* default — fail if tag missing */
#define BAY_CREATE      0x01u   /* create if missing (size must be > 0) */
#define BAY_RO          0x02u   /* read-only mapping */

/* TME-MK Per-Bay encryption (Intel SDM Vol 3D Chap 16).
 *
 * Pass BAY_CREATE | BAY_ENCRYPTED to request that the kernel reserve
 * a unique TME-MK KeyID, allocate backing pages, and map them with
 * KeyID-bearing PTEs. The CPU transparently encrypts every store and
 * decrypts every load using the key associated with that KeyID — DRAM
 * contents become opaque without the key, defending against physical
 * memory attacks (DMA dumps, cold boot, off-chip probes).
 *
 * Every other cabin opening the same tag with bay_open() receives a
 * mapping with the SAME KeyID so cross-cabin reads return the expected
 * plaintext. Opening an encrypted Bay without BAY_ENCRYPTED (or vice
 * versa) returns NULL with ERR_INVALID_ARGUMENT.
 *
 * On the last bay_release across all cabins, the kernel re-keys the
 * slot before returning it to the pool — a future Bay reusing the
 * KeyID slot cannot decrypt this Bay's freed ciphertext.
 *
 * Returns NULL with ERR_UNSUPPORTED on hosts without TME-MK active
 * (QEMU TCG, BIOS without TME-MK locked). Userspace should check the
 * `hw tme` shell command or attempt the create and fall back to a
 * non-encrypted Bay if needed.
 *
 * Encrypted Bays are 4 KiB-page-backed only (huge-page TME-MK is not
 * yet supported by the kernel mapper); this is performance-equivalent
 * to non-encrypted Bays below the 2 MiB threshold and a small penalty
 * above it.
 */
#define BAY_ENCRYPTED   0x04u

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

#ifdef __cplusplus
}
#endif

#endif /* BOX_BAY_H */
