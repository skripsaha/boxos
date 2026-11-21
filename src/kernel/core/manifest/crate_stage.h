#ifndef CRATE_STAGE_H
#define CRATE_STAGE_H

#include "ktypes.h"
#include "error.h"
#include "boxos_crate.h"

#include "vmm.h"

/*
 * CrateStage — async-safe Crate[] descriptor staging.
 *
 *   Why this exists
 *   ───────────────
 *   The dispatch's Manifest stream is staged into per-K-Core scratch
 *   (ManifestStage) and released at dispatch exit. Crate descriptors
 *   CANNOT use the same scratch arena: storage_ops async (ObjRead,
 *   ObjWrite via write_job) captures a pointer into the Crate array and
 *   dereferences it later from AHCI IRQ context, well after the
 *   dispatch frame returns. A scratch-backed Crate pointer would be
 *   reused by the next dispatch on the same K-Core → corruption.
 *
 *   A Pull-Map pointer onto the user's page was the earlier workaround — it
 *   returned a kernel pointer that aliased user memory directly, so
 *   pinning the pointer was safe (user pages outlive dispatch). But
 *   Pull-Map silently clips at the first 4 KiB page boundary, capping
 *   crate_count at MAX_CRATES_PER_POCKET = 100. CrateStage lifts that
 *   cap by routing crates through a kmalloc'd buffer with explicit
 *   handler-driven ownership.
 *
 *   Ownership protocol
 *   ──────────────────
 *   1. Dispatcher calls crate_stage_in() — an array (the shelf's, or the
 *      heap's) + page-walked copy
 *      from user PT. Returns Crate*.
 *   2. Dispatcher hands the buffer to ManifestExecuteOnce → handler.
 *   3. SYNC handlers: write crates[i].size etc. directly into the kbuf,
 *      return OK. Dispatcher calls crate_stage_commit_and_release →
 *      vmm_user_buf_commit_out + release.
 *   4. ASYNC handlers: BEFORE returning ERR_WOULD_BLOCK + setting
 *      PROC_WAITING, stash (kbuf, count, cabin, uaddr) into their
 *      async_ctx. Dispatcher detects async-park and skips both commit
 *      AND release — handler now owns the buffer. On async completion,
 *      handler writes crates[idx].size, then calls
 *      crate_stage_commit_and_release.
 *
 *   The dispatcher's signal for "handler took ownership" is
 *   (rc == ERR_WOULD_BLOCK && process_state == PROC_WAITING) — the
 *   same condition that skips the synchronous Result push.
 *
 *   Allocation strategy
 *   ───────────────────
 *   A block from this core's shelf for a call of up to eight crates, the
 *   heap beyond (crate_stage.c, "The shelf"). Lifetime is bounded:
 *   ≥ one dispatch (sync) or ≤ one async I/O round-trip (async).
 */
error_t crate_stage_in(vmm_context_t        *cabin,
                        uint64_t              user_uaddr,
                        uint16_t              count,
                        Crate               **out_crates);

/*
 * Write staged Crate[] descriptors back to user memory via page-walked
 * commit_out, then kfree the staged buffer. Idempotent on a NULL crates
 * pointer (no-op). The commit_out is logged-on-failure but never aborts
 * the kfree — the buffer must always be released to avoid leak.
 *
 * Used by:
 *   - guide.c sync-path after ManifestExecuteOnce
 *   - storage_ops obj_read_finish (async I/O completion)
 *   - write_job wjob_finalize (async I/O completion)
 */
error_t crate_stage_commit_and_release(Crate                *crates,
                                        uint16_t              count,
                                        vmm_context_t        *cabin,
                                        uint64_t              user_uaddr);

/*
 * Release without commit. Used by the dispatcher's pre-execution error
 * paths (validation failure, ManifestStage acquire failure) where the
 * handler never ran, so no descriptor mutations need write-back.
 */
void crate_stage_release_no_commit(Crate *crates);

/*
 * An array for `count` crates, taken as crate_stage_in takes one — from this
 * core's shelf when it holds that many, else the heap — and released only
 * through crate_stage_commit_and_release or crate_stage_release_no_commit.
 * For a caller that builds its crates in the kernel (touch.c).
 */
Crate *crate_stage_alloc(uint16_t count);

/* The shelf of crate arrays (crate_stage.c), laid out once at boot, after the
 * cores are counted. Without it every array comes from the heap. */
error_t CrateShelfInit(void);

#endif /* CRATE_STAGE_H */
