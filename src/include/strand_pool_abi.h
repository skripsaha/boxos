#ifndef BOXOS_STRAND_POOL_ABI_H
#define BOXOS_STRAND_POOL_ABI_H

/*
 * Shared kernel/userspace ABI — the per-strand StrandPool magazine cache.
 *
 * A StrandPool is a per-strand cache of free heap blocks layered OVER the one
 * global-locked boxlib heap (memory.c). Each strand owns exactly one pool, so
 * the malloc/free fast path is single-writer and needs no lock — yet the global
 * heap stays the single source of truth (a cached block is still `free=0`; the
 * pool only holds a private LIFO of checked-out-but-idle blocks).
 *
 * Pools live in a cabin-persistent slab in boxlib BSS so they SURVIVE the unmap
 * of a strand's StrandInfo when that strand crashes. When a strand dies without
 * an orderly flush, the kernel stamps its slab slot ORPHANED (one CAS in
 * process_destroy); a surviving strand later reclaims the orphan's blocks back
 * to the global heap. The kernel needs only the GenState offset and the state
 * enum below — hence this tiny shared header (the same single-source-of-truth
 * discipline as strand_info.h).
 *
 * GenState packs {generation:24, state:8} in ONE atomic word. The generation
 * makes the kernel death-stamp immune to slot-reuse ABA: an orderly flush bumps
 * the generation, so a late kernel CAS (expecting the bound generation) misses
 * and is a harmless no-op — no unbind syscall is ever needed.
 *
 * The GenState word is a plain `volatile uint32_t` operated on exclusively with
 * the __atomic_* builtins — the same convention as the kernel's atomic_u32_t
 * (sync/atomics.h) and the boxlib spinlock, so this header compiles identically
 * in the kernel's -nostdinc UEFI build and in userspace with no <stdatomic.h>.
 *
 * Includer must provide uint*_t BEFORE including this header.
 *   Kernel:    #include "ktypes.h"
 *   Userspace: #include "box/types.h"
 */

/* Eleven tapered size classes. Worst-case retained per strand is bounded by
 * Σ(CapForClass[c] · StrandPoolClassSize[c]) ≈ 30976 bytes — a hard ceiling, no
 * unbounded growth. The map is a FLOOR map (see memory.c StrandPoolSizeToClass):
 * a request of n bytes is served from the smallest class whose size is >= n, so
 * every block handed out is at least as large as asked. */
#define STRAND_POOL_CLASS_COUNT   11u

/* The largest cached class size. Allocations larger than this bypass the cache
 * and go straight to the global locked path. */
#define STRAND_POOL_MAX_CLASS     8192u

/* Slab capacity = max concurrently-cached strands per cabin. A slot is reused
 * once its strand exits (orderly) or is reclaimed (crash). */
#define STRAND_POOL_SLAB_MAX      256u

/* GenState states (low byte of the packed word). */
#define STRANDPOOL_FREE      0u   /* slot idle, claimable */
#define STRANDPOOL_LIVE      1u   /* owned by a running strand */
#define STRANDPOOL_ORPHANED  2u   /* owner crashed; blocks awaiting reclaim */

/* Pack / unpack helpers for the {generation:24, state:8} word. The generation is
 * masked to its 24 bits so it can never bleed into the state byte under future
 * arithmetic (the period stays 2^24, the field boundary is explicit). */
#define STRANDPOOL_PACK(gen, state)  ((((uint32_t)(gen) & 0xFFFFFFu) << 8) | ((uint32_t)(state) & 0xFFu))
#define STRANDPOOL_STATE(word)       ((uint32_t)((word) & 0xFFu))
#define STRANDPOOL_GEN(word)         ((uint32_t)((word) >> 8))

typedef struct StrandPool {
    void*             Heads[STRAND_POOL_CLASS_COUNT];   /* @0   — per-class LIFO head (payload ptr) */
    uint16_t          Counts[STRAND_POOL_CLASS_COUNT];  /* @88  — cached blocks per class */
    uint16_t          _pad;                             /* @110 — align OwnerPid to 4 */
    uint32_t          OwnerPid;                         /* @112 — claiming strand's pid (diagnostic) */
    volatile uint32_t GenState;                         /* @116 — {generation:24, state:8}, __atomic_* only */
} StrandPool;

#ifdef __cplusplus
#define STRAND_POOL_STATIC_ASSERT static_assert
#else
#define STRAND_POOL_STATIC_ASSERT _Static_assert
#endif

STRAND_POOL_STATIC_ASSERT(__builtin_offsetof(StrandPool, Heads)    == 0,
                          "StrandPool.Heads @0");
STRAND_POOL_STATIC_ASSERT(__builtin_offsetof(StrandPool, GenState) == 116,
                          "StrandPool.GenState @116 (kernel death-stamp offset)");
STRAND_POOL_STATIC_ASSERT(sizeof(StrandPool) == 120,
                          "StrandPool layout pinned (kernel + userspace share it)");

#undef STRAND_POOL_STATIC_ASSERT

#endif /* BOXOS_STRAND_POOL_ABI_H */
