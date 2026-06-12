#ifndef BOX_CORE_NOTIFY_H
#define BOX_CORE_NOTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/core/pocket.h"

/*
 * BoxOS notify — signal the kernel that PocketRing has work for the Guide.
 * Wraps the x86-64 syscall fast entry. INT 0x80 remains as kernel fallback.
 */
#define __notify() __asm__ volatile("syscall" ::: "memory", "rcx", "r11")

/* Prepare a fresh Pocket for a new syscall (zero-init). */
void pocket_prepare(Pocket* p);

/*
 * Submit a Pocket to the PocketRing and notify the kernel. Used internally
 * by ManifestSubmit. Application code should call MfCall1 or the higher
 * level wrappers, not build Pockets directly.
 */
int pocket_submit(Pocket* p);

/* Yield: cooperative scheduler hint via a YIELD-flagged Pocket. */
void yield(void);

#ifdef __cplusplus
}
#endif

#endif /* BOX_CORE_NOTIFY_H */
