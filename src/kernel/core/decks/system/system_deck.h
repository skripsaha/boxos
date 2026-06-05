#ifndef SYSTEM_DECK_H
#define SYSTEM_DECK_H

#include "ktypes.h"
#include "error.h"
#include "boxos_decks.h"

typedef struct process_t process_t;

/* Manifest opcodes (used by system_ops.c handlers + boxlib wrappers). */
#define SYSTEM_OP_PROC_SPAWN    0x01
#define SYSTEM_OP_PROC_KILL     0x02
#define SYSTEM_OP_PROC_INFO     0x03
#define SYSTEM_OP_CTX_USE       0x04
#define SYSTEM_OP_PROC_EXEC     0x06
#define SYSTEM_OP_INFO          0x07
#define SYSTEM_OP_BUF_ALLOC     0x10
#define SYSTEM_OP_BUF_FREE      0x11
#define SYSTEM_OP_BUF_RESIZE    0x12
#define SYSTEM_OP_DEFRAG_FILE   0x18
#define SYSTEM_OP_FRAG_SCORE    0x19
#define SYSTEM_OP_TAG_ADD       0x20
#define SYSTEM_OP_TAG_REMOVE    0x21
#define SYSTEM_OP_TAG_CHECK     0x22
#define SYSTEM_OP_ROUTE         0x40
#define SYSTEM_OP_ROUTE_TAG     0x41
#define SYSTEM_OP_PERF_DUMP     0x50

/*
 * Manifest compile-and-reuse — "prepared statement" pattern for hot syscall
 * paths. Userspace builds a Manifest once, pre-compiles it via COMPILE
 * (in_crate = raw Manifest bytes, out_crate receives 8-byte handle), then
 * submits the handle many times via POCKET_FLAG_MANIFEST_HANDLE. The submit
 * path skips full validation, op-bounds walk, and per-op OpRegistryLookup —
 * roughly the per-syscall overhead the dispatcher would otherwise repeat on
 * identical-shape requests.
 *
 *   COMPILE  in_crate = const Manifest bytes; out_crate ≥ 8 B receives handle.
 *   RELEASE  in_crate = 8-byte handle; decrements refcount, frees on 0.
 *
 * Ownership is per-process: only the cabin that compiled a handle may
 * release it. Handles outstanding at process_destroy are auto-released via
 * ManifestReleaseAllForOwner (no leak).
 */
#define SYSTEM_OP_MANIFEST_COMPILE 0x80
#define SYSTEM_OP_MANIFEST_RELEASE 0x81
/* Touch handle-based ABI (real-HW audit 2026-05-30).
 *
 * Userspace resolves a tag string ONCE via SYSTEM_OP_TOUCH_INTERN, caches
 * the uint16_t TouchTag locally, and passes it to all subsequent ops. The
 * hot syscall path (CLAIM/RELEASE/SEND/AWAIT/ACK) takes the tag_id as a
 * u16 in params — no string parsing, no registry lookup. */
#define SYSTEM_OP_TOUCH_INTERN     0x58  /* resolve tag string -> (full,bare) ids */
#define SYSTEM_OP_TOUCH_CLAIM      0x51
#define SYSTEM_OP_TOUCH_RELEASE    0x52
#define SYSTEM_OP_TOUCH_SEND       0x53
#define SYSTEM_OP_TOUCH_AWAIT      0x54
#define SYSTEM_OP_TOUCH_IRQ_RETURN 0x55
#define SYSTEM_OP_TOUCH_REGISTER   0x56
#define SYSTEM_OP_TOUCH_ACK        0x57

/* EFI runtime / Secure Boot / ESRT introspection (read-only). */
#define SYSTEM_OP_EFI_INFO         0x60
#define SYSTEM_OP_EFI_ESRT_GET     0x61
#define SYSTEM_OP_EFI_VERIFY_PE    0x62

/* Bay — cross-cabin shared memory, tag-driven, refcounted. The full
 * surface is just three ops: open creates-or-attaches and returns a
 * user-VA pointer, release drops the per-cabin claim, size queries the
 * region size. See src/kernel/core/bay/bay.h for semantics. */
#define SYSTEM_OP_BAY_OPEN         0x70
#define SYSTEM_OP_BAY_RELEASE      0x71
#define SYSTEM_OP_BAY_SIZE         0x72

/* Brook — SPSC ordered streaming, tag-driven, refcounted.
 *
 *   OPEN    — attach (or create) the Brook in this cabin with a role
 *             (BROOK_WRITER xor BROOK_READER) and map header + slots.
 *   RELEASE — drop this cabin's claim. Last release destroys backing.
 *   INFO    — snapshot live stats (diagnostic).
 *
 * Wait/wake is entirely userspace — push/pop hot path runs lock-free
 * on the shared BrookHeader, and block-on-full / block-on-empty loop
 * via pause / UMWAIT / yield on header.head / header.tail directly.
 * Peer death is detected by reading the kernel-managed `*_alive` flag
 * in the header.
 *
 * See src/kernel/core/brook/brook.h for the full protocol. */
#define SYSTEM_OP_BROOK_OPEN       0x75
#define SYSTEM_OP_BROOK_RELEASE    0x76
#define SYSTEM_OP_BROOK_INFO       0x7A

/* Implicit huge-page prefault: user-heap allocator (boxlib memory.c)
 * calls this when growing the heap by ≥ 2 MB so the kernel pre-backs
 * the new VA range with 2 MB pages instead of waiting for 4 KB demand
 * paging. The pre-fault path falls back to 4 KB transparently if the
 * PMM cannot deliver a 2 MB chunk.
 *
 * Params: [u64 va_base][u64 size] (size MUST be 2 MB-aligned, va MUST
 * be 2 MB-aligned). The whole range becomes resident immediately. */
#define SYSTEM_OP_HEAP_PREFAULT    0x14

/* Layout of SYSTEM_OP_EFI_INFO out_crate (128 bytes, packed). Versioned
 * via the leading u32 so userspace can grow the surface forwards-
 * compatibly. Returned in one blob so a single op call gives userspace
 * the full picture without round-trips. */
#define EFI_INFO_BLOB_SIZE        128u
#define EFI_INFO_VERSION          1u

/* Free every buffer owned by the given pid. Called during process teardown.
 * Equivalent to BufferRegistryCleanupProcess(pid) — the wrapper exists for
 * call-site stability across the Phase 12 cleanup. */
void system_deck_cleanup_process_buffers(uint32_t pid);

/* Allocate target->buf_heap pages, copy `length` bytes from sender's heap
 * to target's heap. Returns target user vaddr or 0 on failure. Used by the
 * Manifest-native system.route / system.broadcast ops. */
uint64_t ipc_copy_to_heap(process_t *sender, process_t *target,
                          uint64_t src_addr, uint32_t length);

/* Register Manifest-native System Deck ops. Defined in system_ops.c. */
error_t SystemDeckRegister(void);

/* Register Touch ops. Defined in touch_ops.c. Called from SystemDeckRegister. */
error_t TouchOpsRegister(void);

/* Register Bay ops + heap-prefault. Defined in bay_ops.c. Called from
 * SystemDeckRegister alongside TouchOpsRegister. */
error_t BayOpsRegister(void);

/* Register Brook ops. Defined in brook_ops.c. Called from
 * SystemDeckRegister alongside Bay/Touch registration. */
error_t BrookOpsRegister(void);

#endif /* SYSTEM_DECK_H */
