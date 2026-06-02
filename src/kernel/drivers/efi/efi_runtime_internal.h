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

#endif /* EFI_RUNTIME_INTERNAL_H */
