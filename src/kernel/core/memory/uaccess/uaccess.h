/*
 * uaccess — kernel ↔ user-space safe memory access under CR4.SMAP=1.
 *
 * Intel SDM Vol 2A documents STAC (0F 01 CB) / CLAC (0F 01 CA) — CPL=0-only
 * instructions that set/clear RFLAGS.AC. Under CR4.SMAP=1, the CPU blocks
 * supervisor-mode access to pages whose PTE has U/S=1 (user-mapped)
 * UNLESS RFLAGS.AC=1. The combination protects ring-0 from accidentally
 * dereferencing a user pointer.
 *
 * Threat model this closes
 * ------------------------
 * Without SMAP, a syscall handler that takes a user pointer and dereferences
 * it directly can be tricked into reading arbitrary memory or writing into
 * the wrong page (privilege confusion). SMAP turns that mistake into a
 * clean #PF instead of silent data corruption.
 *
 * Design — paired-bracket primitives
 * ----------------------------------
 *   1. Bare instructions: stac() / clac(). Use ONLY in the helper layer.
 *   2. Bracketed accessors:
 *        copy_to_user(dst, src, n)        — bulk copy out
 *        copy_from_user(dst, src, n)      — bulk copy in
 *        put_user_u{32,64}(val, ptr)      — single-word write
 *        get_user_u{32,64}(out, ptr)      — single-word read
 *      Each runs STAC → access → CLAC, with page-fault fixup so a fault
 *      inside the access region returns a "bytes-not-copied" count
 *      instead of panicking the kernel.
 *   3. Manual bracket: user_access_begin() / user_access_end() for cases
 *      where the access pattern is too irregular for the bulk helpers
 *      (e.g. building a CabinInfo struct field-by-field with mixed types).
 *      Caller is responsible for NOT taking a long path between them —
 *      AC=1 leaves the kernel running with SMAP disabled until CLAC.
 *
 * Fault recovery
 * --------------
 * Each access primitive's inline asm emits a (fault_rip, recovery_rip)
 * pair into a dedicated linker section (.uaccess_fixup). When the VMM
 * page-fault handler sees a kernel-mode #PF, it consults the table:
 *   - Match → set frame->rip = recovery_rip; CLAC is in the recovery
 *     epilogue so AC is correctly cleared on return.
 *   - No match → genuine kernel bug, kill or panic per current policy.
 *
 * Why not pluck STAC/CLAC at syscall entry/exit?
 * ----------------------------------------------
 * Linux's history shows that pattern doesn't scale: any function the
 * syscall calls into can race with an interrupt that runs with AC=1,
 * defeating SMAP for the duration. The narrow bracket around the actual
 * access keeps the SMAP-off window measured in cycles, not microseconds.
 *
 * Why not use rep-movsb directly?
 * -------------------------------
 * On Intel + AMD, "rep movsb" is the architecturally-recommended copy
 * primitive (ERMSB micro-op) and IS SMAP-aware: with CR4.SMAP=1 + AC=0,
 * it triggers #PF on the user-mapped target. We use it inside the
 * STAC/CLAC bracket — fast on the common path, recoverable on fault.
 *
 * Compile-time invariants
 * -----------------------
 *   - access_ok(addr, size) rejects non-canonical / kernel-half / size-
 *     overflow ranges BEFORE the STAC. A faulty access_ok would let a
 *     malicious caller trick the kernel into accessing kernel memory
 *     via the STAC bracket, which would succeed regardless of SMAP.
 *   - All helpers in this header are inline / fold to a few ops on the
 *     fast path. The page-fault fixup is the only out-of-line cost,
 *     and only on the slow path.
 */
#ifndef UACCESS_H
#define UACCESS_H

#include "ktypes.h"

/* User-virtual address space upper bound (exclusive). Mirrors
 * VMM_USER_STACK_TOP — the highest legitimate user page top — plus the
 * one-page guard. Any access at or above this VA targets the kernel
 * half of the canonical address space and is structurally invalid for
 * a user pointer. Hard-coded rather than #include'ing vmm.h to keep
 * uaccess as a leaf header. */
#define UACCESS_USER_VA_MAX     0x0000800000000000ULL

/* Bare STAC / CLAC primitives. Both are CPL=0-only — calling from
 * userspace would #UD. Both are NOPs when CR4.SMAP=0 (architectural,
 * per Intel SDM Vol 2A). The "cc" clobber on STAC/CLAC is required
 * because they modify RFLAGS.AC. */
static inline void stac(void) {
    __asm__ volatile("stac" ::: "cc", "memory");
}
static inline void clac(void) {
    __asm__ volatile("clac" ::: "cc", "memory");
}

/* Manual bracket. Use ONLY when copy_to/from_user / put_user / get_user
 * don't fit the access pattern. The window is non-recoverable — a #PF
 * inside it WILL panic the kernel (no fixup entry emitted) — so keep
 * the bracket short and around accesses you're certain are mapped. */
static inline void user_access_begin(void) { stac(); }
static inline void user_access_end(void)   { clac(); }

/* access_ok — bounds-check a user-virtual range.
 *
 * Returns true iff [addr, addr+size) lies entirely within the user
 * portion of the canonical address space. Rejects:
 *   - NULL                       (sentinel for "not a pointer")
 *   - addr >= UACCESS_USER_VA_MAX (kernel half / non-canonical)
 *   - addr + size overflow       (size attacks)
 *   - addr + size > USER_VA_MAX  (range crosses canonical boundary)
 *
 * Does NOT verify page presence — that's intentionally the fixup's job.
 * A caller that wants pre-flight verification can `vmm_get_leaf_pte` the
 * range, but the common case is to skip it and rely on the fixup. */
static inline bool access_ok(const void *addr, size_t size) {
    uintptr_t a = (uintptr_t)addr;
    if (a == 0) return false;
    if (a >= UACCESS_USER_VA_MAX) return false;
    if (size == 0) return true;
    /* overflow-safe: size cannot make a + size wrap below a in u64. */
    if (a > UACCESS_USER_VA_MAX - size) return false;
    return true;
}

/* copy_to_user — copy `n` bytes from kernel `src` to user-mapped `dst`.
 *
 * Returns the number of bytes NOT copied:
 *   0     — full copy succeeded
 *   n     — access_ok failed (no SMAP window opened)
 *   k>0   — page fault at byte (n-k); k bytes remained
 *
 * The fault path is engineered so a fault in the FIRST page of the
 * destination returns the full `n` — the caller can treat any nonzero
 * return as "this user pointer was bad, fail the syscall". */
size_t copy_to_user(void *dst, const void *src, size_t n);

/* copy_from_user — copy `n` bytes from user-mapped `src` to kernel `dst`.
 * Same return-value contract as copy_to_user. */
size_t copy_from_user(void *dst, const void *src, size_t n);

/* put_user_uN — write a single N-bit word to a user-mapped address.
 *
 * Returns 0 on success, -1 on access_ok failure or page fault. The
 * single-word path is cheaper than copy_to_user because it skips the
 * rep-movsb setup and the byte count threading. */
int put_user_u32(uint32_t val, uint32_t *ptr);
int put_user_u64(uint64_t val, uint64_t *ptr);

/* get_user_uN — read a single N-bit word from a user-mapped address. */
int get_user_u32(uint32_t *out, const uint32_t *ptr);
int get_user_u64(uint64_t *out, const uint64_t *ptr);

/* Fixup-table accessor. Called by the VMM page-fault handler with the
 * faulting RIP. Returns the recovery RIP to jump to, or 0 if no match
 * (genuine kernel bug). O(log n) after uaccess_init() has run; falls
 * back to O(n) linear scan if init hasn't completed (early-boot #PF). */
uintptr_t uaccess_lookup_fixup(uintptr_t fault_rip);

/* One-time initialisation — snapshots the linker-emitted .uaccess_fixup
 * entries into a sorted array so uaccess_lookup_fixup can binary-search.
 *
 * Must be called from BSP boot context (kernel main) BEFORE any process
 * spawns and BEFORE interrupts are unmasked at the LAPIC. Idempotent.
 *
 * Why not lazy-init inside uaccess_lookup_fixup: that path runs from the
 * #PF handler — taking the init mutex or doing the sort under IST risks
 * deadlock against any code that itself holds the page-fault path.
 * Explicit boot-order init keeps the fast path lock-free. */
void uaccess_init(void);

#endif /* UACCESS_H */
