#ifndef SYSTEM_DECK_H
#define SYSTEM_DECK_H

#include "ktypes.h"
#include "error.h"
#include "boxos_decks.h"

typedef struct process_t process_t;

/* All SYSTEM_OP_* opcodes live in src/include/boxos_decks.h (included
 * above) — single source of truth for kernel handler tables and boxlib
 * syscall wrappers. NEVER add a SYSTEM_OP_* define here. */

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

/* The second half of the same: allocate target->buf_heap pages and copy
 * `length` bytes from a KERNEL buffer into them. Returns the target user
 * vaddr or 0 on failure. What system.proc.exec uses to hand a cabin Luggage
 * too long for its CabinInfo page. */
uint64_t cabin_heap_deposit(process_t *target, const void *kbuf, uint32_t length);

/* Register Manifest-native System Deck ops. Defined in system_ops.c. */
error_t SystemDeckRegister(void);

/* Boot self-test for the proc-mutation authority predicate (kill/tag gate).
 * Defined in system_ops.c next to the static proc_has_authority_over it drives. */
error_t ProcAuthSelfTest(void);

/* Register Touch ops. Defined in touch_ops.c. Called from SystemDeckRegister. */
error_t TouchOpsRegister(void);

/* Register Bay ops + heap-prefault. Defined in bay_ops.c. Called from
 * SystemDeckRegister alongside TouchOpsRegister. */
error_t BayOpsRegister(void);

/* Register Brook ops. Defined in brook_ops.c. Called from
 * SystemDeckRegister alongside Bay/Touch registration. */
error_t BrookOpsRegister(void);

/* MemTag — RAM-region tagging, TagFS-shaped.
 * Opcodes (SYSTEM_OP_MEMTAG_QUERY..APPLY_PKEY, 0xA0–0xAA) live in the
 * shared header `src/include/boxos_decks.h` so kernel + boxlib see the
 * same numeric values via a single source of truth. */

error_t MemTagOpsRegister(void);

/* HW per-process real-HW state ops (LAM, future CET/TME context).
 * Defined in hw_ops.c. */
error_t HwOpsRegister(void);

/* Strand sync: addr_park / addr_wake. Defined in sync_ops.c. */
error_t SyncOpsRegister(void);

/* Register SYSTEM_OP_TURN_IN (turnin_ops.c) — the sleep a strand takes when
 * nothing it waits on has arrived yet. Called from SystemDeckRegister. */
error_t TurnInOpsRegister(void);

#endif /* SYSTEM_DECK_H */
