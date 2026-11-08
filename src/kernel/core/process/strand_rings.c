/*
 * strand_rings.c — per-strand IPC rings + StrandInfo TLS block (P5a).
 *
 * Layout of a Hammock slot (page indices; see cabin_layout.h for the picture),
 * P5a "balanced" geometry:
 *
 *   [gap 0..255][user stack 256..2303][guard 2304][CET SSP 2305..2308][guard 2309]
 *   [Pocket hdr 2310][Result hdr 2311][Touch hdr 2312][guard 2313]
 *   [Pocket slots 2314..2317][Result slots 2318..2321][Touch slots 2322..2323][guard 2324]
 *   [thread-local storage page 2325][StrandInfo 2326..2329][guard 2330]
 *   [unused 2331..2559]
 *
 * The stack/SSP page offsets are what process_strand_ssp_base relies on. The
 * ring + StrandInfo offsets are derived here from kernel_config.h and pinned by
 * _Static_assert so they cannot silently drift. Everything above the stack is
 * the P5a tail unchanged, moved up by the stack's growth from 16 pages to 2048
 * and the gap under it. The stack itself is not part of the tail: its pages
 * come in as the strand touches them and are released with it
 * (process_release_strand_slot).
 *
 * Ring slot regions are EAGER-mapped: the userspace PocketRing producer writes
 * its first Pocket to slot[0] on the strand's first syscall, and the #PF
 * demand-map handler only recognises the FIXED cabin slot VAs (0x4000…), not
 * these Hammock VAs — so a lazily-mapped Hammock slot would take a fatal
 * userspace #PF. Eager mapping (10 pages = 40 KiB/strand) closes that whole
 * demand-paging class. The fixed cabin rings (256 pages) stay lazy — untouched.
 *
 * Per-strand ring capacities are a memory↔throughput knob carried in the ring
 * header — NEVER a correctness boundary: a full ring just back-pressures
 * (push retries). Spawned strands make near-serial syscalls; the "balanced"
 * sizing (256 / 512 / 64 slots) covers light workers and moderate strand-
 * servers alike. The main strand keeps its large 1 MiB cabin rings.
 */

#include "strand_rings.h"
#include "process.h"
#include "cabin.h"
#include "cabin_layout.h"
#include "kernel_config.h"
#include "vmm.h"
#include "pmm.h"
#include "kring.h"
#include "touch_ring.h"
#include "memtag.h"
#include "strand_info.h"
#include "klib.h"

/* ── Hammock slot page geometry (derived; asserted) ────────────────────── */

#define HAMMOCK_SSP_PAGES           4u    /* CET user SSP (P4) */
#define HAMMOCK_POCKET_SLOT_PAGES   4u    /* 256 Pocket slots */
#define HAMMOCK_RESULT_SLOT_PAGES   4u    /* 512 Result slots */
#define HAMMOCK_TOUCH_SLOT_PAGES    2u    /* 64  Touch  slots */
#define HAMMOCK_STRANDINFO_PAGES    4u    /* StrandInfo block (16 KiB) */

/* First ring header page = after [gap][stack][guard][SSP][guard]. */
#define HAMMOCK_POCKET_HDR_PAGE \
    (CABIN_HAMMOCK_STACK_PAGE_OFF + CONFIG_USER_STACK_PAGES + 1u + HAMMOCK_SSP_PAGES + 1u)   /* 2310 */
#define HAMMOCK_RESULT_HDR_PAGE     (HAMMOCK_POCKET_HDR_PAGE + 1u)                            /* 2311 */
#define HAMMOCK_TOUCH_HDR_PAGE      (HAMMOCK_RESULT_HDR_PAGE + 1u)                            /* 2312 */
/* guard at HAMMOCK_TOUCH_HDR_PAGE + 1 (2313) */
#define HAMMOCK_POCKET_SLOTS_PAGE   (HAMMOCK_TOUCH_HDR_PAGE + 2u)                             /* 2314 */
#define HAMMOCK_RESULT_SLOTS_PAGE   (HAMMOCK_POCKET_SLOTS_PAGE + HAMMOCK_POCKET_SLOT_PAGES)   /* 2318 */
#define HAMMOCK_TOUCH_SLOTS_PAGE    (HAMMOCK_RESULT_SLOTS_PAGE + HAMMOCK_RESULT_SLOT_PAGES)   /* 2322 */
/* guard at +2 (2324), thread-local storage page at 2325 */
#define HAMMOCK_STRANDINFO_PAGE     (HAMMOCK_TOUCH_SLOTS_PAGE + HAMMOCK_TOUCH_SLOT_PAGES + 2u)/* 2326 */
/* The single page directly below StrandInfo (the fs:0 TCB) holds the strand's
 * variant-II thread-local storage, [fsbase - below, fsbase): the kernel pours
 * the program's template into it here (image_thread.h). A program whose
 * storage does not fit the page has no strand spawned (strand_spawn). */
#define HAMMOCK_NEGTLS_PAGE         (HAMMOCK_STRANDINFO_PAGE - 1u)                            /* 2325 */
/* guard at HAMMOCK_STRANDINFO_PAGE + HAMMOCK_STRANDINFO_PAGES (2330) */
#define HAMMOCK_END_PAGE            (HAMMOCK_STRANDINFO_PAGE + HAMMOCK_STRANDINFO_PAGES + 1u) /* 2331 */

_Static_assert(HAMMOCK_POCKET_HDR_PAGE == 2310,
               "Hammock ring base must be page 2310 — 256 gap + 2048 stack + guard + 4 SSP + guard");
_Static_assert(HAMMOCK_NEGTLS_PAGE == HAMMOCK_POCKET_HDR_PAGE + 15u,
               "the thread-local storage page must be directly below the StrandInfo TCB, 15 pages into the rings");
_Static_assert(HAMMOCK_END_PAGE <= CABIN_HAMMOCK_SLOT_PAGES,
               "Hammock slot too small for stack+SSP+rings+StrandInfo");
_Static_assert((CABIN_HAMMOCK_SLOT_SIZE % 0x200000ULL) == 0 &&
                   CABIN_HAMMOCK_SLOT_PAGES - HAMMOCK_END_PAGE < 512u,
               "Hammock slot is the tail rounded up to a whole 2 MiB, no more");

/* Per-strand ring capacities = slot-region bytes / slot stride. */
#define HAMMOCK_POCKET_CAP   (HAMMOCK_POCKET_SLOT_PAGES * VMM_PAGE_SIZE / POCKET_SLOT_SIZE)   /* 256 */
#define HAMMOCK_RESULT_CAP   (HAMMOCK_RESULT_SLOT_PAGES * VMM_PAGE_SIZE / RESULT_SLOT_SIZE)   /* 512 */
#define HAMMOCK_TOUCH_CAP    (HAMMOCK_TOUCH_SLOT_PAGES  * VMM_PAGE_SIZE / TOUCH_SLOT_SIZE)    /* 64  */

/* Teardown span: ring headers (2310) through the end of the StrandInfo block
 * (2329, inclusive). The thread-local storage page (2325) falls inside and is
 * freed by the present-check; the guards (absent VA) are skipped. Stack
 * (256..2303) and SSP (2305..2308) are NOT here — they are freed by
 * process_release_strand_slot / cet_process_destroy. */
#define HAMMOCK_TEARDOWN_FIRST_PAGE  HAMMOCK_POCKET_HDR_PAGE
#define HAMMOCK_TEARDOWN_PAGES \
    (HAMMOCK_STRANDINFO_PAGE + HAMMOCK_STRANDINFO_PAGES - HAMMOCK_POCKET_HDR_PAGE)            /* 20 */

/* ── Helpers ───────────────────────────────────────────────────────────── */

static inline uint64_t hammock_page_va(const process_t *proc, uint32_t page)
{
    return proc->hammock_base + (uint64_t)page * VMM_PAGE_SIZE;
}

/* Unmap + free every PRESENT 4 KiB leaf in [va_base, va_base + n_pages).
 * Tolerant of absent pages (guards, partial create unwind). */
static void hammock_unmap_pages(vmm_context_t *vmm, uint64_t va_base, uint32_t n_pages)
{
    for (uint32_t i = 0; i < n_pages; i++)
    {
        uint64_t va = va_base + (uint64_t)i * VMM_PAGE_SIZE;
        uint8_t level = 0;
        pte_t *pte = vmm_get_leaf_pte(vmm, va, &level);
        if (pte && (*pte & VMM_FLAG_PRESENT) && level == 1)
        {
            uintptr_t phys = vmm_pte_to_phys(*pte);
            vmm_unmap_page(vmm, va);
            if (phys)
                pmm_free((void *)phys, 1);
        }
    }
}

/* Allocate n fresh zeroed pages and map them user-RW at [va_base, …).
 * Returns the FIRST page's phys (never 0 on success); on any failure unwinds
 * everything it mapped and returns 0. */
static uintptr_t hammock_map_pages(vmm_context_t *vmm, uint64_t va_base, uint32_t n_pages)
{
    uintptr_t first_phys = 0;
    for (uint32_t i = 0; i < n_pages; i++)
    {
        void *phys = pmm_alloc_zero(1);
        if (!phys)
        {
            hammock_unmap_pages(vmm, va_base, i);
            return 0;
        }
        uint64_t va = va_base + (uint64_t)i * VMM_PAGE_SIZE;
        /* W^X: every page in a strand's slot — ring headers, ring slot
         * regions, and the StrandInfo TLS block — is pure DATA, never code.
         * Map NX, matching the fixed cabin slot regions (vmm_ensure_user_page
         * / the #PF demand-map handler both set VMM_FLAG_NO_EXECUTE). Without
         * this the per-strand pages would be writable+executable — a real-HW
         * W^X hole (dormant on TCG, enforced on NX-capable silicon). */
        vmm_map_result_t m = vmm_map_pages(vmm, va, (uintptr_t)phys, 1,
                                           VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                                           VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE);
        if (!m.success)
        {
            pmm_free(phys, 1);
            hammock_unmap_pages(vmm, va_base, i);
            return 0;
        }
        if (i == 0)
            first_phys = (uintptr_t)phys;
    }
    return first_phys;
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool strand_rings_create(process_t *proc)
{
    if (!proc || !proc->cabin || !proc->cabin->vmm || proc->hammock_base == 0)
        return false;

    vmm_context_t *vmm = proc->cabin->vmm;

    const uint64_t pocket_hdr_va   = hammock_page_va(proc, HAMMOCK_POCKET_HDR_PAGE);
    const uint64_t result_hdr_va   = hammock_page_va(proc, HAMMOCK_RESULT_HDR_PAGE);
    const uint64_t touch_hdr_va    = hammock_page_va(proc, HAMMOCK_TOUCH_HDR_PAGE);
    const uint64_t pocket_slots_va = hammock_page_va(proc, HAMMOCK_POCKET_SLOTS_PAGE);
    const uint64_t result_slots_va = hammock_page_va(proc, HAMMOCK_RESULT_SLOTS_PAGE);
    const uint64_t touch_slots_va  = hammock_page_va(proc, HAMMOCK_TOUCH_SLOTS_PAGE);
    const uint64_t negtls_va       = hammock_page_va(proc, HAMMOCK_NEGTLS_PAGE);
    const uint64_t strandinfo_va   = hammock_page_va(proc, HAMMOCK_STRANDINFO_PAGE);

    /* (1) Ring header pages (1 each) — capture phys for kernel routing. */
    uintptr_t pocket_phys = hammock_map_pages(vmm, pocket_hdr_va, 1);
    uintptr_t result_phys = pocket_phys ? hammock_map_pages(vmm, result_hdr_va, 1) : 0;
    uintptr_t touch_phys  = result_phys ? hammock_map_pages(vmm, touch_hdr_va, 1) : 0;
    bool ok = (touch_phys != 0);

    /* (2) EAGER-map the three slot regions. */
    ok = ok && (hammock_map_pages(vmm, pocket_slots_va, HAMMOCK_POCKET_SLOT_PAGES) != 0);
    ok = ok && (hammock_map_pages(vmm, result_slots_va, HAMMOCK_RESULT_SLOT_PAGES) != 0);
    ok = ok && (hammock_map_pages(vmm, touch_slots_va,  HAMMOCK_TOUCH_SLOT_PAGES)  != 0);

    /* (2b) The thread-local storage page, poured below once the strand's
     * StrandInfo is in place. Same eager-map rationale as the slot regions —
     * the #PF demand-map handler only knows the fixed cabin VAs, so this
     * Hammock page must be present before the strand first touches a
     * thread_local. Unwound by the full-range teardown below on any failure. */
    uintptr_t negtls_phys = ok ? hammock_map_pages(vmm, negtls_va, 1) : 0;
    ok = negtls_phys != 0;

    /* (3) StrandInfo block. */
    uintptr_t si_phys = ok ? hammock_map_pages(vmm, strandinfo_va, HAMMOCK_STRANDINFO_PAGES) : 0;

    if (!si_phys)
    {
        /* Unwind everything mapped so far via the full-range teardown. */
        hammock_unmap_pages(vmm, hammock_page_va(proc, HAMMOCK_TEARDOWN_FIRST_PAGE),
                            HAMMOCK_TEARDOWN_PAGES);
        debug_printf("[STRAND] ERROR: ring/StrandInfo alloc failed for strand pid %u\n", proc->pid);
        return false;
    }

    /* (4) Initialise the per-strand ring headers (kernel direct-map view). */
    KRingPocketInitAt((PocketRing *)vmm_phys_to_virt(pocket_phys), pocket_slots_va, HAMMOCK_POCKET_CAP);
    KRingResultInitAt((ResultRing *)vmm_phys_to_virt(result_phys), result_slots_va, HAMMOCK_RESULT_CAP);
    KTouchRingInitAt ((TouchRing  *)vmm_phys_to_virt(touch_phys),  touch_slots_va,  HAMMOCK_TOUCH_CAP);

    /* (5) Populate the StrandInfo TLS block (already zeroed by pmm_alloc_zero). */
    StrandInfo *si      = (StrandInfo *)vmm_phys_to_virt(si_phys);
    si->tcb_self        = strandinfo_va;     /* System V variant-2 self-pointer (fs:0) */
    si->tcb_reserved    = 0;
    si->magic           = STRAND_INFO_MAGIC;
    si->strand_pid      = proc->pid;
    si->generation      = proc->generation;
    si->pocket_ring_va  = pocket_hdr_va;
    si->result_ring_va  = result_hdr_va;
    si->touch_ring_va   = touch_hdr_va;
    /* The stash pointers and strand_pool_ptr are already zero — pmm_alloc_zero
     * cleared the whole 4-page block, so every lazily-made boxlib field starts
     * at 0 ("not yet"). */

    /* (5b) The program's thread-local storage, right below that TCB. */
    ImageThreadPour(&proc->cabin->thread, (uint8_t *)vmm_phys_to_virt(negtls_phys));

    /* (6) Wire the strand: per-strand routing + FS base = StrandInfo VA. The
     * first scheduler dispatch installs user_fsbase via context_restore. */
    proc->pocket_ring_phys    = pocket_phys;
    proc->result_ring_phys    = result_phys;
    proc->result_ring_slots   = HAMMOCK_RESULT_CAP;
    proc->touch_ring_phys     = touch_phys;
    proc->strandinfo_phys     = si_phys;
    proc->context.user_fsbase = strandinfo_va;

    /* (7) MemTag the per-strand ring headers — parity with cabin_create's main
     * rings. FIXED interned purpose tags only (no per-pid tag) so spawning
     * many strands never grows the tag registry. */
    MemTagApplyByPhys(pocket_phys, 1, "purpose:shared");
    MemTagApplyByPhys(pocket_phys, 1, "purpose:pocket-ring");
    MemTagApplyByPhys(result_phys, 1, "purpose:shared");
    MemTagApplyByPhys(result_phys, 1, "purpose:result-ring");
    MemTagApplyByPhys(touch_phys,  1, "purpose:shared");
    MemTagApplyByPhys(touch_phys,  1, "purpose:touch-ring");

    return true;
}

void strand_rings_destroy(process_t *proc)
{
    if (!proc || proc->hammock_base == 0 || !proc->cabin || !proc->cabin->vmm)
        return;   /* main strand (hammock_base == 0) → no-op */

    vmm_context_t *vmm = proc->cabin->vmm;

    /* Clear the per-strand ring-header MemTags (mirror create). */
    if (proc->pocket_ring_phys)
    {
        MemTagClearByPhys(proc->pocket_ring_phys, "purpose:shared");
        MemTagClearByPhys(proc->pocket_ring_phys, "purpose:pocket-ring");
    }
    if (proc->result_ring_phys)
    {
        MemTagClearByPhys(proc->result_ring_phys, "purpose:shared");
        MemTagClearByPhys(proc->result_ring_phys, "purpose:result-ring");
    }
    if (proc->touch_ring_phys)
    {
        MemTagClearByPhys(proc->touch_ring_phys, "purpose:shared");
        MemTagClearByPhys(proc->touch_ring_phys, "purpose:touch-ring");
    }

    /* Unmap + free ring headers, slot regions and StrandInfo (pages 2310..2329). */
    hammock_unmap_pages(vmm, hammock_page_va(proc, HAMMOCK_TEARDOWN_FIRST_PAGE),
                        HAMMOCK_TEARDOWN_PAGES);

    proc->pocket_ring_phys = 0;
    proc->result_ring_phys = 0;
    proc->result_ring_slots = 0;
    proc->touch_ring_phys  = 0;
    proc->strandinfo_phys  = 0;
}
