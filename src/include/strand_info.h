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

/* Per-strand result stash capacity (entries). The main strand keeps a large
 * static stash in boxlib (result.c STASH_CAP); a spawned strand carries this
 * smaller inline stash so concurrent strands never share stash storage. 256
 * comfortably absorbs any single-strand reply burst — a strand's own
 * syscalls are serial, so the stash only buffers IPC payloads interleaved
 * with its kernel replies. */
#define STRAND_STASH_CAP       256u
/* One stash entry == one Result == 24 bytes. Asserted against sizeof(Result)
 * on both sides where Result is in scope (kernel kring path / boxlib result.c). */
#define STRAND_STASH_ENTRY_SZ  24u
/* Bytes reserved per stash: the 256-entry ring plus a 16-byte
 * head/tail/count/pad control block that boxlib overlays via StrandStashRing. */
#define STRAND_STASH_BYTES     (STRAND_STASH_CAP * STRAND_STASH_ENTRY_SZ + 16u)

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
    /* @56: two stash regions. Kernel zero-inits; boxlib overlays
     * StrandStashRing and uses them for ipc / non-ipc result buffering. */
    uint8_t  ipc_stash[STRAND_STASH_BYTES];
    uint8_t  non_ipc_stash[STRAND_STASH_BYTES];
    /* Ф20e — this strand's claimed StrandPool slab slot (boxlib memory.c).
     * Kernel zero-inits the whole block (strand_rings.c pmm_alloc_zero), so 0
     * means "not yet claimed"; boxlib lazy-claims a slab slot on first malloc
     * and caches the pointer here for the lock-free fast path. */
    uint64_t strand_pool_ptr;
    /* Ф21 — per-strand Touch tag-filter stash (boxlib touch.c); kernel
     * zero-inits → 0 = not yet allocated; lazy-malloc'd on first tag-consume.
     * Mirrors strand_pool_ptr. */
    uint64_t touch_stash_ptr;
    /* Ф26e — per-strand async file-I/O (box::ferry) completion stash
     * (boxlib result.c); kernel zero-inits → 0 = not yet allocated;
     * lazy-malloc'd on first KCTX_STORAGE record routed on this strand.
     * A lazy pointer (not an inline reservation like ipc_stash) because
     * ferry I/O is opt-in — a strand that never issues a ferry op pays
     * nothing, and a third inline STRAND_STASH_BYTES block would overflow
     * the 4-page StrandInfo reservation. Mirrors touch_stash_ptr. */
    uint64_t ferry_stash_ptr;
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
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, ipc_stash)      == 56, "StrandInfo.ipc_stash @56");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, strand_pool_ptr) ==
                     56 + 2u * STRAND_STASH_BYTES, "StrandInfo.strand_pool_ptr @ 56+2*STRAND_STASH_BYTES");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, touch_stash_ptr) ==
                     56 + 2u * STRAND_STASH_BYTES + 8u, "StrandInfo.touch_stash_ptr");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, ferry_stash_ptr) ==
                     56 + 2u * STRAND_STASH_BYTES + 16u, "StrandInfo.ferry_stash_ptr last");
STRAND_STATIC_ASSERT(__builtin_offsetof(StrandInfo, print_state) ==
                     56 + 2u * STRAND_STASH_BYTES + 24u, "StrandInfo.print_state @ 56+2*STRAND_STASH_BYTES+24");
/* The whole block must fit the Hammock StrandInfo reservation (4 pages =
 * 16 KiB — see HAMMOCK_STRANDINFO_PAGES in strand_rings.c). */
STRAND_STATIC_ASSERT(sizeof(StrandInfo) <= 4u * 4096u, "StrandInfo must fit 4 pages");

#undef STRAND_STATIC_ASSERT

#endif /* BOXOS_STRAND_INFO_H */
