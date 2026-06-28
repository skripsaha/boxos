#ifndef CRATE_IO_SELFTEST_H
#define CRATE_IO_SELFTEST_H

#include "ktypes.h"
#include "error.h"
#include "vmm.h"       /* vmm_context_t */
#include "process.h"   /* process_t, cabin_t */

/*
 * In-kernel proof that the crate_io page-walk (crate_read / crate_write)
 * copies a Crate whose payload straddles a page boundary across BOTH real
 * backing frames, where the legacy single-frame translation corrupts a
 * foreign frame.
 *
 * The proof is only meaningful when the two virtually-adjacent user pages are
 * backed by physically NON-adjacent frames: if page B's frame happened to sit
 * right after page A's, an overrun past A would land in B by luck and hide the
 * bug. NonContigPages builds exactly that pathological mapping.
 */

/*
 * Two virtually-adjacent, physically NON-adjacent user pages plus a poison
 * guard page, mapped USER-RW in a private (never-activated) context that is
 * walked through the Pull Map.
 *
 * Physical layout of `block` — ONE contiguous 3-frame allocation:
 *
 *     frame A          |  spacer (unmapped)  |  frame B
 *     base_va          |     (no VA)         |  base_va + VMM_PAGE_SIZE
 *
 * Frame B is two physical frames above frame A, so the frame physically after
 * A is the spacer, never B. A single-frame translation of a range that starts
 * in A and overruns the A|B virtual seam writes the overflow into the spacer
 * (test-owned, harmless) and leaves B untouched — the straddle bug. A correct
 * page-walk delivers the overflow to B and never touches the spacer.
 *
 * Reuse: point a Crate at base_va + VMM_PAGE_SIZE - k (0 < k < VMM_PAGE_SIZE)
 * and it straddles the seam over non-contiguous frames. page_a / spacer /
 * page_b / guard are Pull-Map kernel pointers to the raw frames, for planting
 * and checking bytes directly. `proc` is a fake initiator whose only valid
 * field chain is proc->cabin->vmm == this context, so it can drive
 * crate_read / crate_write through an OpContext.
 */
typedef struct {
    vmm_context_t *vmm;          /* private context, walked via Pull Map     */
    process_t     *proc;         /* fake initiator: only ->cabin->vmm is set */
    cabin_t       *cabin;        /* fake cabin: only ->vmm is set            */
    uintptr_t      base_va;      /* frame A VA; B = +PAGE, guard = +2*PAGE   */
    void          *block;        /* pmm_alloc(3): frames A | spacer | B      */
    void          *guard_frame;  /* pmm_alloc(1): poison guard backing       */
    uint8_t       *page_a;       /* Pull-Map kptr to frame A                 */
    uint8_t       *spacer;       /* Pull-Map kptr to the unmapped spacer     */
    uint8_t       *page_b;       /* Pull-Map kptr to frame B                 */
    uint8_t       *guard;        /* Pull-Map kptr to the guard frame         */
    uintptr_t      phys_a;       /* frame A physical                         */
    uintptr_t      phys_b;       /* frame B physical (asserted non-adjacent) */
} NonContigPages;

/* Build the mapping at `base_va` (page-aligned, 3 unused user pages). Returns
 * true with `m` fully populated; false (and `m` torn back down) on any
 * allocation, mapping, or non-adjacency failure. */
bool NonContigPagesSetup(NonContigPages *m, uintptr_t base_va);

/* Unmap the test VAs, free the frames, destroy the private context, and free
 * the fake proc/cabin. Safe on a partially-built `m`. */
void NonContigPagesTeardown(NonContigPages *m);

/* Boot self-test. Prints exactly "[CRATE-IO] SELFTEST PASS" on success or
 * "[CRATE-IO] SELFTEST FAIL: <reason>" on the first failing assertion.
 * Returns OK / ERR_INTERNAL. */
error_t CrateIoSelfTest(void);

#endif /* CRATE_IO_SELFTEST_H */
