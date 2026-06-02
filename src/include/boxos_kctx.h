#ifndef BOXOS_KCTX_H
#define BOXOS_KCTX_H

/*
 * boxos_kctx.h — shared kernel/userspace ABI for the KResult `context` tag.
 *
 * Every Result the kernel pushes into a Cabin's ResultRing carries a 32-bit
 * `context` field telling the consumer which subsystem produced it. boxlib
 * uses this to fan out kernel-broadcast Touches (KCTX_TOUCH) from manifest
 * replies and IPC payloads without an extra round-trip.
 *
 * Keep the enum byte-identical with the kernel-internal KResultContext in
 * src/include/kresult.h (it now includes this header). Adding a new context
 * means: append here, rebuild kernel+boxlib together, update consumers.
 */

typedef enum {
    KCTX_NONE   = 0,
    KCTX_PMM    = 1,
    KCTX_VMM    = 2,
    KCTX_TAGFS  = 3,
    KCTX_AHCI   = 4,
    KCTX_SCHED  = 5,
    KCTX_IPC    = 6,
    KCTX_GUIDE  = 7,
    KCTX_FRIEND = 8,
    KCTX_TOUCH  = 9,
} KResultContext;

#endif /* BOXOS_KCTX_H */
