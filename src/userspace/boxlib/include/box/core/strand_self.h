#ifndef BOX_CORE_STRAND_SELF_H
#define BOX_CORE_STRAND_SELF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "strand_info.h"   /* shared kernel/userspace ABI: StrandInfo, magic */

/*
 * strand_self — per-strand identity + IPC ring routing (P5a userspace side).
 *
 * A spawned strand carries its own StrandInfo TLS block (kernel-populated in
 * its Hammock slot) and the kernel sets the strand's FS base to it. boxlib
 * reads the FS base via RDFSBASE to find "which rings are mine". The main
 * strand has no StrandInfo (FS base is 0 for a C program, or the C++ TCB for a
 * boxcxx program) and falls back to the fixed cabin ring VAs.
 *
 * This header deliberately does NOT include the ring headers (pocket.h /
 * result.h / touch_ring.h) — those include this one. Ring VAs are returned as
 * plain uint64_t to break the cycle.
 */

typedef struct {
    uint64_t pocket_va;   /* PocketRing header VA */
    uint64_t result_va;   /* ResultRing header VA */
    uint64_t touch_va;    /* TouchRing header VA */
} strand_rings_t;

/* The calling strand's StrandInfo, or NULL if this is the main strand.
 *
 * Safe on any CPU and against any FS-base value: returns NULL when FSGSBASE is
 * absent (spawned strands require it), and identifies the main strand purely
 * by the FS base NOT lying inside the Hammock window — so it never dereferences
 * an unknown base (a C++ TCB on the heap is never read). */
StrandInfo *strand_info_or_null(void);

/* Per-strand ring header VAs (main strand → the fixed cabin ring VAs). */
strand_rings_t strand_rings(void);

/* The calling strand's pid (main strand → cabin pid via CabinInfo). */
uint32_t strand_self(void);

#ifdef __cplusplus
}
#endif

#endif /* BOX_CORE_STRAND_SELF_H */
