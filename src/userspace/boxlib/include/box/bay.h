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
 * THE CLAIM IS THE CABIN'S, NOT THE STRAND'S. A pointer from bay_open
 * stays mapped for as long as the cabin lives, and exactly two things
 * end it: an explicit bay_release(ptr) — which any strand of the cabin
 * may call, on any window any strand of the cabin opened — and the
 * cabin's own teardown, when its last strand is gone. Nothing else does,
 * and in particular a strand exiting unmaps nothing. So one strand may
 * open a Bay and hand the pointer to its siblings, and it may finish
 * first; they keep reading.
 *
 * It is the cabin's because the mapping is: a Bay window is a range of
 * the cabin's address space, which every strand in the cabin runs in.
 * Unmapping it because one occupant moved out is a fault in the next
 * sibling to touch it — code that did nothing wrong, in a place with no
 * clue why. The console is where this was found: one shared print order,
 * opened by whichever strand happened to print first, disappeared from
 * under all of them the moment that strand finished.
 *
 * The counterpart of the rule is that a release is a decision for the
 * whole cabin. Call bay_release only when no sibling is still reading
 * through the pointer; otherwise just let the cabin's exit take it, and
 * nothing leaks — when the last claim across all cabins drops, the
 * backing pages return to PMM either way.
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
 * shared physical pages, or NULL on failure. (BoxOS has no errno register;
 * the typed cause is surfaced through the C++ box::result face,
 * box/cxx/error.h — this raw C entry only signals NULL vs non-NULL.) For
 * BAY_CREATE size MUST be non-zero; for BAY_OPEN size is informational
 * (the existing Bay's size wins). */
void   *bay_open(const char *tag, uint64_t size, uint32_t flags);

/* A Bay made for one program: created, never an existing one opened
 * (-ERR_ALREADY_EXISTS when the tag names a Bay already), and it opens for
 * (program_pid, program_generation) alone — the program as AskFrom names it
 * (boxos_kctx.h); any other opener is refused -ERR_ACCESS_DENIED. For a server
 * that makes memory for a program and hands it the name: a name is no secret,
 * and this is what keeps the memory that program's. OK and *out set, or
 * -ERR_*. */
int     bay_create_for(const char *tag, uint64_t size, uint32_t program_pid,
                       uint32_t program_generation, void **out);

/* Release this cabin's claim on the Bay, unmapping the window for every
 * strand in the cabin — not just the caller. Drops one ref; if the last
 * cabin releases, the backing pages return to PMM. Returns 0 on
 * success, -ERR_NOT_FOUND if ptr does not name an open Bay claim of
 * this cabin. */
int     bay_release(void *ptr);

/* Query the Bay's user-visible size (in bytes). Returns 0 if ptr is
 * not a live claim of this cabin. */
uint64_t bay_size(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* BOX_BAY_H */
