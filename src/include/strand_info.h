#ifndef BOXOS_STRAND_INFO_H
#define BOXOS_STRAND_INFO_H


#define STRAND_INFO_MAGIC      0x5354524E44494E46ULL

#define STRAND_PRINT_BYTES     168u

typedef struct StrandInfo {
    uint64_t tcb_self;
    uint64_t tcb_reserved;
    uint64_t magic;
    uint32_t strand_pid;
    uint32_t generation;
    uint64_t pocket_ring_va;
    uint64_t result_ring_va;
    uint64_t touch_ring_va;
    uint64_t ipc_stash_ptr;
    uint64_t non_ipc_stash_ptr;
    uint64_t ferry_stash_ptr;
    uint64_t touch_stash_ptr;
    uint64_t strand_pool_ptr;
    uint8_t print_state[STRAND_PRINT_BYTES];
} StrandInfo;

#ifdef __cplusplus
#define STRAND_STATIC_ASSERT static_assert
#else
#define STRAND_STATIC_ASSERT _Static_assert
#endif

STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, tcb_self)       == 0,  "StrandInfo.tcb_self @0");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, magic)          == 16, "StrandInfo.magic @16 (fs-base probe offset)");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, strand_pid)     == 24, "StrandInfo.strand_pid @24");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, generation)     == 28, "StrandInfo.generation @28");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, pocket_ring_va) == 32, "StrandInfo.pocket_ring_va @32");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, result_ring_va) == 40, "StrandInfo.result_ring_va @40");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, touch_ring_va)  == 48, "StrandInfo.touch_ring_va @48");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, ipc_stash_ptr)     == 56, "StrandInfo.ipc_stash_ptr @56");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, non_ipc_stash_ptr) == 64, "StrandInfo.non_ipc_stash_ptr @64");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, ferry_stash_ptr)   == 72, "StrandInfo.ferry_stash_ptr @72");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, touch_stash_ptr)   == 80, "StrandInfo.touch_stash_ptr @80");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, strand_pool_ptr)   == 88, "StrandInfo.strand_pool_ptr @88");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, print_state)       == 96, "StrandInfo.print_state @96");
STRAND_STATIC_ASSERT(sizeof(StrandInfo) <= 4u * 4096u, "StrandInfo must fit 4 pages");

#undef STRAND_STATIC_ASSERT

#endif