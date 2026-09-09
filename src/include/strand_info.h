#ifndef BOXOS_STRAND_INFO_H
#define BOXOS_STRAND_INFO_H

/*
 * Shared kernel/userspace ABI header — the per-strand TLS control block.
 *
 * A strand spawned via strand_spawn carries its OWN IPC rings (Pocket /
 * Result / Touch) and its own result stashes so concurrent multi-strand
 * syscalls never share ring storage (the P4 → P5 data-race fix). The kernel
 * populates one StrandInfo per spawned strand inside that strand's Hammock slot
 * and points the strand's FS base at it. Userspace (boxlib strand_self.c)
 * reads FS base via RDFSBASE, validates `magic`, and routes ring access to
 * the per-strand VAs carried here.
 *
 * The MAIN strand has NO StrandInfo: its FS base is either 0 or points at the
 * boxcxx C++ TCB. boxlib distinguishes the two by reading `magic` at its
 * fixed offset — a C++ TCB will not carry STRAND_INFO_MAGIC, so the main
 * strand falls back to the fixed cabin ring VAs and the large static stashes.
 *
 * This is the SINGLE source of truth for the layout: the kernel producer
 * (strand_rings.c) and the userspace consumer (strand_self.c / result.c)
 * both include this header, and the static asserts below pin every offset so
 * a stray field can never silently desynchronise the two sides.
 *
 * Includer must provide uint*_t BEFORE including this header.
 *   Kernel:    #include "ktypes.h"
 *   Userspace: #include "box/types.h"
 */

/* "STRNDINF" packed big-endian — distinctive 8-byte tag at StrandInfo.magic
 * (offset 16). The fs-base probe reads this offset off a base that may be a
 * C++ TCB; the value is specific enough that a false positive is a 1-in-2^64
 * accident. */
#define STRAND_INFO_MAGIC      0x5354524E44494E46ULL

/* Per-strand print state (boxlib print.c StrandPrintState). Fixed size,
 * always present (unlike the lazy opt-in stashes) since every strand that
 * ever calls print/printf needs one. 280 → 160 when the io_buf telegram
 * buffer gave way to the console-lane frame: the block now carries a lane
 * handle, one ConsoleRun under construction and the colour state (S2+S3 of
 * the console-stream epic). */
#define STRAND_PRINT_BYTES     160u

typedef struct StrandInfo {
    uint64_t tcb_self;        /* @0  — System V variant-2 TCB self-pointer (fs:0) */
    uint64_t tcb_reserved;    /* @8  — TCB DTV slot (reserved, unused in P5a) */
    uint64_t magic;           /* @16 — STRAND_INFO_MAGIC; discriminates from a C++ TCB */
    uint32_t strand_pid;      /* @24 — this strand's pid (strand_self) */
    uint32_t generation;      /* @28 — pid_generation at spawn: (pid, generation) is who this strand is */
    uint64_t pocket_ring_va;  /* @32 — per-strand PocketRing header VA */
    uint64_t result_ring_va;  /* @40 — per-strand ResultRing header VA */
    uint64_t touch_ring_va;   /* @48 — per-strand TouchRing header VA */
    /* @56..@80: this strand's four stashes (boxlib box/core/stash.h) — what
     * its rings handed it that was not for the caller at hand: IPC messages,
     * plain kernel replies, box::ferry completions, Touches of another tag.
     * Each is a heap Stash that grows by the chunk, made on first use; the
     * kernel zero-inits the block (strand_rings.c pmm_alloc_zero), so 0 is
     * "not yet". What stood here were two inline rings of 256 entries that
     * dropped the oldest when full. */
    uint64_t ipc_stash_ptr;      /* @56 */
    uint64_t non_ipc_stash_ptr;  /* @64 */
    uint64_t ferry_stash_ptr;    /* @72 */
    uint64_t touch_stash_ptr;    /* @80 */
    /* Ф20e — this strand's claimed StrandPool slab slot (boxlib memory.c);
     * 0 = not yet claimed; boxlib lazy-claims a slab slot on first malloc
     * and caches the pointer here for the lock-free fast path. */
    uint64_t strand_pool_ptr;    /* @88 */
    /* per-strand print/IPC-output buffer (boxlib print.c); kernel zero-inits via
     * pmm_alloc_zero -> color_fg/bg=0, corrected to defaults by print.c's
     * initialized flag on first touch. Inline (small+universal), unlike the
     * large/opt-in stashes. */
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
/* The whole block must fit the Hammock StrandInfo reservation (4 pages =
 * 16 KiB — see HAMMOCK_STRANDINFO_PAGES in strand_rings.c). */
STRAND_STATIC_ASSERT(sizeof(StrandInfo) <= 4u * 4096u, "StrandInfo must fit 4 pages");

#undef STRAND_STATIC_ASSERT

#endif /* BOXOS_STRAND_INFO_H */
