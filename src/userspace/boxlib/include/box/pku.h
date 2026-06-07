#ifndef BOX_PKU_H
#define BOX_PKU_H

#include "box/types.h"
#include "box/error.h"

/*
 * PKU — Intel Memory Protection Keys (userspace surface).
 *
 * Hardware (Intel SDM Vol 3A §4.6.2):
 *   - 16 keys (0..15), 2 control bits per key in the IA32_PKRU register:
 *       bit 2*N     — AD (Access Disable) — any access denied
 *       bit 2*N + 1 — WD (Write Disable)  — writes denied, reads OK
 *   - Each leaf PTE carries a 4-bit PKEY in bits 62:59. The PKRU pair
 *     at that key gates access.
 *   - PKRU is per-thread, written via WRPKRU (0F 01 EF), read via
 *     RDPKRU (0F 01 EE). Both unprivileged (no syscall needed).
 *   - The kernel saves/restores PKRU via XSAVE component 9 on context
 *     switch (wired up by Phase 2H + audit-2 fpu_xsave_register_extension).
 *
 * BoxOS integration:
 *   - Apply a pku:N tag to a region with `pku_apply_region(rid, N)`. The
 *     kernel auto-stamps PTE bits 62:59 across every attach + cross-core
 *     TLB shootdown (MemTagApplyPkey + MemTagSweepPkey).
 *   - Userspace then controls per-key access for the current thread via
 *     `pku_set_rights(N, ad, wd)` / `pku_get_rights(N, ...)`.
 *
 * Use cases:
 *   - W^X JIT: stamp code page with pku:exec, data page with pku:rw;
 *     flip PKRU.WD on the code key around emit.
 *   - Secrets isolation: stamp secret heap with pku:secrets, unlock only
 *     inside a narrow code window.
 *   - Post-init RO: stamp once, flip WD=1, never undo. Cheaper than
 *     mprotect because there's no PTE walk on the flip.
 *
 * Compatibility:
 *   - PKRU=0 (default) means "all keys allow all access" — programs that
 *     don't use PKU see no behavior change.
 *   - On hardware without PKU (CPUID.07H.0:ECX[3]=0), all functions
 *     return -ERR_NOT_SUPPORTED.
 */

#define PKU_MAX_KEYS  16u

/* Read current thread's PKRU. Returns 0 on CPUs without PKU support. */
uint32_t pku_read_pkru(void);

/* Write current thread's PKRU. No-op on CPUs without PKU. */
void     pku_write_pkru(uint32_t value);

/* Set the (AD, WD) pair for `pkey`. `ad` non-zero disables ALL access;
 * `wd` non-zero disables writes only. Returns 0 on success or -ERR_*. */
int      pku_set_rights(uint8_t pkey, int ad, int wd);

/* Read current (AD, WD) for `pkey`. Either pointer may be NULL. */
int      pku_get_rights(uint8_t pkey, int *out_ad, int *out_wd);

/* Apply pku:<pkey> to a MemTag region. Kernel clears any prior pku:* tag,
 * applies pku:<pkey>, sweeps every attach's PTEs to stamp bits 62:59,
 * and runs a cross-core TLB shootdown per attach. pkey == 0 clears any
 * existing pku:* tag (returns the region to "any-key" semantics).
 * Returns 0 on success or -ERR_*. */
int      pku_apply_region(uint32_t region_id, uint8_t pkey);

#endif /* BOX_PKU_H */
