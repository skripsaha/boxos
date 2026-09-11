
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


#define HAMMOCK_SSP_PAGES           4u
#define HAMMOCK_POCKET_SLOT_PAGES   4u
#define HAMMOCK_RESULT_SLOT_PAGES   4u
#define HAMMOCK_TOUCH_SLOT_PAGES    2u
#define HAMMOCK_STRANDINFO_PAGES    4u

#define HAMMOCK_POCKET_HDR_PAGE \
    (CABIN_HAMMOCK_STACK_PAGE_OFF + CONFIG_USER_STACK_PAGES + 1u + HAMMOCK_SSP_PAGES + 1u)
#define HAMMOCK_RESULT_HDR_PAGE     (HAMMOCK_POCKET_HDR_PAGE + 1u)
#define HAMMOCK_TOUCH_HDR_PAGE      (HAMMOCK_RESULT_HDR_PAGE + 1u)
#define HAMMOCK_POCKET_SLOTS_PAGE   (HAMMOCK_TOUCH_HDR_PAGE + 2u)
#define HAMMOCK_RESULT_SLOTS_PAGE   (HAMMOCK_POCKET_SLOTS_PAGE + HAMMOCK_POCKET_SLOT_PAGES)
#define HAMMOCK_TOUCH_SLOTS_PAGE    (HAMMOCK_RESULT_SLOTS_PAGE + HAMMOCK_RESULT_SLOT_PAGES)
#define HAMMOCK_STRANDINFO_PAGE     (HAMMOCK_TOUCH_SLOTS_PAGE + HAMMOCK_TOUCH_SLOT_PAGES + 2u)
#define HAMMOCK_NEGTLS_PAGE         (HAMMOCK_STRANDINFO_PAGE - 1u)
#define HAMMOCK_END_PAGE            (HAMMOCK_STRANDINFO_PAGE + HAMMOCK_STRANDINFO_PAGES + 1u)

_Static_assert(HAMMOCK_POCKET_HDR_PAGE == 23,
               "Hammock ring base must be page 23 — preserves the P4 SSP page math");
_Static_assert(HAMMOCK_NEGTLS_PAGE == 38,
               "C++ neg-TLS page must be 38 (directly below the StrandInfo TCB)");
_Static_assert(HAMMOCK_END_PAGE <= CABIN_HAMMOCK_SLOT_PAGES,
               "Hammock slot too small for stack+SSP+rings+StrandInfo");

#define HAMMOCK_POCKET_CAP   (HAMMOCK_POCKET_SLOT_PAGES * VMM_PAGE_SIZE / POCKET_SLOT_SIZE)
#define HAMMOCK_RESULT_CAP   (HAMMOCK_RESULT_SLOT_PAGES * VMM_PAGE_SIZE / RESULT_SLOT_SIZE)
#define HAMMOCK_TOUCH_CAP    (HAMMOCK_TOUCH_SLOT_PAGES  * VMM_PAGE_SIZE / TOUCH_SLOT_SIZE)

#define HAMMOCK_TEARDOWN_FIRST_PAGE  HAMMOCK_POCKET_HDR_PAGE
#define HAMMOCK_TEARDOWN_PAGES \
    (HAMMOCK_STRANDINFO_PAGE + HAMMOCK_STRANDINFO_PAGES - HAMMOCK_POCKET_HDR_PAGE)


static inline uint64_t hammock_page_va(const process_t *proc, uint32_t page)
{
    return proc->hammock_base + (uint64_t)page * VMM_PAGE_SIZE;
}

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

    uintptr_t pocket_phys = hammock_map_pages(vmm, pocket_hdr_va, 1);
    uintptr_t result_phys = pocket_phys ? hammock_map_pages(vmm, result_hdr_va, 1) : 0;
    uintptr_t touch_phys  = result_phys ? hammock_map_pages(vmm, touch_hdr_va, 1) : 0;
    bool ok = (touch_phys != 0);

    ok = ok && (hammock_map_pages(vmm, pocket_slots_va, HAMMOCK_POCKET_SLOT_PAGES) != 0);
    ok = ok && (hammock_map_pages(vmm, result_slots_va, HAMMOCK_RESULT_SLOT_PAGES) != 0);
    ok = ok && (hammock_map_pages(vmm, touch_slots_va,  HAMMOCK_TOUCH_SLOT_PAGES)  != 0);

    ok = ok && (hammock_map_pages(vmm, negtls_va, 1) != 0);

    uintptr_t si_phys = ok ? hammock_map_pages(vmm, strandinfo_va, HAMMOCK_STRANDINFO_PAGES) : 0;

    if (!si_phys)
    {
        hammock_unmap_pages(vmm, hammock_page_va(proc, HAMMOCK_TEARDOWN_FIRST_PAGE),
                            HAMMOCK_TEARDOWN_PAGES);
        debug_printf("[STRAND] ERROR: ring/StrandInfo alloc failed for strand pid %u\n", proc->pid);
        return false;
    }

    KRingPocketInitAt((PocketRing *)vmm_phys_to_virt(pocket_phys), pocket_slots_va, HAMMOCK_POCKET_CAP);
    KRingResultInitAt((ResultRing *)vmm_phys_to_virt(result_phys), result_slots_va, HAMMOCK_RESULT_CAP);
    KTouchRingInitAt ((TouchRing  *)vmm_phys_to_virt(touch_phys),  touch_slots_va,  HAMMOCK_TOUCH_CAP);

    StrandInfo *si      = (StrandInfo *)vmm_phys_to_virt(si_phys);
    si->tcb_self        = strandinfo_va;
    si->tcb_reserved    = 0;
    si->magic           = STRAND_INFO_MAGIC;
    si->strand_pid      = proc->pid;
    si->generation      = proc->generation;
    si->pocket_ring_va  = pocket_hdr_va;
    si->result_ring_va  = result_hdr_va;
    si->touch_ring_va   = touch_hdr_va;

    proc->pocket_ring_phys    = pocket_phys;
    proc->result_ring_phys    = result_phys;
    proc->touch_ring_phys     = touch_phys;
    proc->strandinfo_phys     = si_phys;
    proc->context.user_fsbase = strandinfo_va;

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
        return;

    vmm_context_t *vmm = proc->cabin->vmm;

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

    hammock_unmap_pages(vmm, hammock_page_va(proc, HAMMOCK_TEARDOWN_FIRST_PAGE),
                        HAMMOCK_TEARDOWN_PAGES);

    proc->pocket_ring_phys = 0;
    proc->result_ring_phys = 0;
    proc->touch_ring_phys  = 0;
    proc->strandinfo_phys  = 0;
}