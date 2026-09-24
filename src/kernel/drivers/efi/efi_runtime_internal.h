#ifndef EFI_RUNTIME_INTERNAL_H
#define EFI_RUNTIME_INTERNAL_H

/*
 * Internal helpers shared between the EFI driver translation units
 * (efi_runtime.c, efi_variable.c, efi_wakeup.c, efi_capsule.c,
 * efi_esrt.c, efi_secureboot.c, efi_authenticode.c).
 *
 * The contract here matches UEFI 2.10 §8.1: every RT call must be
 * serialised across all CPUs. We hold one global spinlock + IRQ disable
 * window for the duration of a call. Sleeping under this lock is
 * forbidden — RT calls do not return until firmware completes.
 *
 * efi_rt_get() returns either the SVAM-rebased virtual RT pointer or
 * the captured physical pointer (when SVAM failed and we degraded to
 * physical-mode RT). NULL is returned on BIOS boots.
 */

#include "efi.h"

EfiRuntimeServices *efi_rt_get(void);

void efi_rt_lock(uint64_t *saved_rflags);
void efi_rt_unlock(uint64_t saved_rflags);

void efi_rt_watch_start(uint64_t *t0);
void efi_rt_watch_end(const char *name, uint64_t t0);

#define EFI_RT_CALL_WARN_MS  100u

/* Convert a NUL-terminated ASCII C-string into a kernel-scratch UCS-2
 * (UTF-16LE, NUL-terminated) buffer suitable for passing into UEFI
 * Variable Services. Returns the number of CHAR16 units written (excluding
 * NUL) or 0 on overflow. dest_chars is the capacity of `dest` in CHAR16
 * units; the helper writes at most dest_chars-1 characters then a NUL.
 *
 * Implementation lives in efi_variable.c (shared with the other RT
 * modules that take variable names). */
size_t efi_ascii_to_ucs2(const char *ascii, uint16_t *dest, size_t dest_chars);

/* Length of a UCS-2 string in CHAR16 units (excluding NUL). */
size_t efi_ucs2_strlen(const uint16_t *s);

/* ─── The two things EVERY firmware entry in this driver says ─────────────
 *
 * Six runtime services live in efi_variable.c, efi_wakeup.c and
 * efi_capsule.c, and the rest in efi_runtime.c. All of them enter the
 * firmware the same way — on the kernel's map (Footing) and under a Lifeline
 * — so all of them refuse an unreachable argument in the same words and
 * report a fall in the same words. ONE definition, in efi_runtime.c, because
 * four copies of a diagnostic are four chances for the machine to describe
 * the same event four different ways.
 *
 * EfiFootingRefuse: true when `p` is NOT reachable from the kernel map, after
 * saying so by name. With Footing loaded, a pointer that lives only in a
 * caller's address space becomes unreachable mid-call, and a wrong answer
 * from the firmware is worse than a refusal. A NULL `p` is reachable by
 * definition (the firmware is being handed nothing).
 * ‼ CALL IT BEFORE efi_rt_lock, NEVER UNDER IT: vmm_virt_to_phys takes the
 * kernel context's lock, and g_rt_lock → kctx->lock is an order this driver
 * establishes nowhere else.
 *
 * EfiSayFall: what Lifeline caught on the way back out, plus the memory
 * map's own answer for CR2 — which is the whole difference between "a bare
 * physical address nobody recognises" and "runtime data descriptor 41".
 * Safe after a throwback: it takes no lock and allocates nothing. */
bool EfiFootingRefuse(const char *service, const char *argument,
                      const void *p, uint64_t len);
void EfiSayFall(const char *service, const char *consequence);

#endif /* EFI_RUNTIME_INTERNAL_H */
