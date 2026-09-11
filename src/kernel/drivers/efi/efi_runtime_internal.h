#ifndef EFI_RUNTIME_INTERNAL_H
#define EFI_RUNTIME_INTERNAL_H


#include "efi.h"

EfiRuntimeServices *efi_rt_get(void);

void efi_rt_lock(uint64_t *saved_rflags);
void efi_rt_unlock(uint64_t saved_rflags);

void efi_rt_watch_start(uint64_t *t0);
void efi_rt_watch_end(const char *name, uint64_t t0);

#define EFI_RT_CALL_WARN_MS  100u

size_t efi_ascii_to_ucs2(const char *ascii, uint16_t *dest, size_t dest_chars);

size_t efi_ucs2_strlen(const uint16_t *s);

#endif