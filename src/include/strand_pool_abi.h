#ifndef BOXOS_STRAND_POOL_ABI_H
#define BOXOS_STRAND_POOL_ABI_H


#define STRAND_POOL_CLASS_COUNT   11u

#define STRAND_POOL_MAX_CLASS     8192u

#define STRAND_POOL_SLAB_MAX      256u

#define STRANDPOOL_FREE      0u
#define STRANDPOOL_LIVE      1u
#define STRANDPOOL_ORPHANED  2u

#define STRANDPOOL_PACK(gen, state)  ((((uint32_t)(gen) & 0xFFFFFFu) << 8) | ((uint32_t)(state) & 0xFFu))
#define STRANDPOOL_STATE(word)       ((uint32_t)((word) & 0xFFu))
#define STRANDPOOL_GEN(word)         ((uint32_t)((word) >> 8))

typedef struct StrandPool {
    void*             Heads[STRAND_POOL_CLASS_COUNT];
    uint16_t          Counts[STRAND_POOL_CLASS_COUNT];
    uint16_t          _pad;
    uint32_t          OwnerPid;
    volatile uint32_t GenState;
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

#endif