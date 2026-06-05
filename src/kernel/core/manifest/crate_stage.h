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
 *   Pull-Map (vmm_translate_user_addr) was the v1 workaround — it
 *   returned a kernel pointer that aliased user memory directly, so
 *   pinning the pointer was safe (user pages outlive dispatch). But
 *   Pull-Map silently clips at the first 4 KiB page boundary, capping
 *   crate_count at MAX_CRATES_PER_POCKET = 100. CrateStage lifts that
 *   cap by routing crates through a kmalloc'd buffer with explicit
 *   handler-driven ownership.
 *
 *   Ownership protocol
 *   ──────────────────
 *   1. Dispatcher calls crate_stage_in() — kmalloc + page-walked copy
 *      from user PT. Returns Crate*.
 *   2. Dispatcher hands the buffer to ManifestExecuteOnce → handler.
 *   3. SYNC handlers: write crates[i].size etc. directly into the kbuf,
 *      return OK. Dispatcher calls crate_stage_commit_and_release →
 *      vmm_user_buf_commit_out + kfree.
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
 *   Pure kmalloc. No scratch tier — crates are small (40 B × N), the
 *   sub-page allocations satisfy quickly from the slab without touching
 *   the buddy directly. Lifetime is bounded: ≥ one dispatch (sync) or
 *   ≤ one async I/O round-trip (async). No NUMA-direct path needed at
 *   this size class.
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

#endif /* CRATE_STAGE_H */
