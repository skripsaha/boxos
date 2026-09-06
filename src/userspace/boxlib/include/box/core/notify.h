#ifndef BOX_CORE_NOTIFY_H
#define BOX_CORE_NOTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/core/pocket.h"

/*
 * BoxOS notify — signal the kernel that PocketRing has work for the Guide.
 * Wraps the x86-64 syscall fast entry. INT 0x80 remains as kernel fallback.
 * RDI is the gate's one word from the caller: 0 says "look at my ring",
 * GATE_YIELD says "take my core" (boxos_pocket.h). The kernel returns
 * through the frame, so RAX comes back zeroed and is named as clobbered.
 */
#define __notify() \
    __asm__ volatile("syscall" : : "D"(0) : "memory", "rax", "rcx", "r11")

/* Prepare a fresh Pocket for a new syscall (zero-init). */
void pocket_prepare(Pocket* p);

/*
 * Submit a Pocket to the PocketRing and notify the kernel. Used internally
 * by ManifestSubmit. Application code should call MfCall1 or the higher
 * level wrappers, not build Pockets directly.
 */
int pocket_submit(Pocket* p);

/* Yield: give the core away. Not a pocket — see GATE_YIELD. */
void yield(void);

#ifdef __cplusplus
}
#endif

#endif /* BOX_CORE_NOTIFY_H */
