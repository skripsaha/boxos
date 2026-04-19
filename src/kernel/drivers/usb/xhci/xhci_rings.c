#include "xhci_rings.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"

int xhci_ring_init(xhci_ring_t* ring, uint32_t num_trbs, bool producer)
{
    if (!ring || num_trbs < 4) {
        return -1;
    }

    // TRB ring pages are DMA targets — xHCI controller reads them directly.
    // Even if AC64=1, allocate below 4GB to stay safe with all QEMU configs.
    size_t pages_needed = vmm_size_to_pages(num_trbs * sizeof(xhci_trb_t));
    void* trbs_phys = pmm_alloc_zero(pages_needed, PHYS_TAG_DMA32);
    if (!trbs_phys) {
        return -1;
    }

    ring->trbs = (xhci_trb_t*)vmm_phys_to_virt((uintptr_t)trbs_phys);
    ring->trbs_phys = (uint64_t)trbs_phys;
    ring->num_trbs = num_trbs;
    ring->enqueue_idx = 0;
    ring->dequeue_idx = 0;
    ring->cycle_state = 1;
    ring->producer = producer;
    spinlock_init(&ring->ring_lock);

    if (producer) {
        /* Place Link TRB at the last slot pointing back to ring start.
         * TC (Toggle Cycle) bit is set so HC toggles cycle on wrap.
         * Cycle bit starts as 0 — software sets it when enqueue wraps. */
        xhci_trb_t* link = &ring->trbs[num_trbs - 1];
        link->parameter = ring->trbs_phys;
        link->status = 0;
        link->control = TRB_SET_TYPE(TRB_TYPE_LINK) | TRB_TC;
    }

    return 0;
}

void xhci_ring_destroy(xhci_ring_t* ring)
{
    if (!ring || !ring->trbs) {
        return;
    }

    size_t pages = vmm_size_to_pages(ring->num_trbs * sizeof(xhci_trb_t));
    pmm_free((void*)ring->trbs_phys, pages);

    ring->trbs = NULL;
    ring->trbs_phys = 0;
    ring->num_trbs = 0;
}

uint64_t xhci_ring_get_phys_addr(xhci_ring_t* ring)
{
    if (!ring) {
        return 0;
    }
    return ring->trbs_phys;
}

uint64_t xhci_ring_enqueue(xhci_ring_t* ring, xhci_trb_t* trb)
{
    if (!ring || !trb || !ring->trbs) {
        return 0;
    }

    spin_lock(&ring->ring_lock);

    uint32_t idx = ring->enqueue_idx;

    /* For producer rings, last slot is the Link TRB — can't write there */
    if (ring->producer && idx >= ring->num_trbs - 1) {
        spin_unlock(&ring->ring_lock);
        return 0;
    }

    /* Set cycle bit to match current producer cycle state */
    trb->control = (trb->control & ~TRB_C) | (ring->cycle_state ? TRB_C : 0);

    /* Write TRB to ring */
    ring->trbs[idx] = *trb;
    __sync_synchronize();

    uint64_t trb_phys = ring->trbs_phys + (idx * sizeof(xhci_trb_t));

    /* Advance index */
    ring->enqueue_idx = idx + 1;

    if (ring->producer && ring->enqueue_idx >= ring->num_trbs - 1) {
        /* Reached Link TRB slot — activate it and wrap */
        xhci_trb_t* link = &ring->trbs[ring->num_trbs - 1];
        link->control = TRB_SET_TYPE(TRB_TYPE_LINK) | TRB_TC |
                        (ring->cycle_state ? TRB_C : 0);
        __sync_synchronize();
        ring->cycle_state ^= 1;
        ring->enqueue_idx = 0;
    } else if (!ring->producer && ring->enqueue_idx >= ring->num_trbs) {
        ring->enqueue_idx = 0;
        ring->cycle_state ^= 1;
    }

    spin_unlock(&ring->ring_lock);
    return trb_phys;
}

int xhci_erst_init(xhci_erst_t* erst, xhci_ring_t* event_ring)
{
    if (!erst || !event_ring || !event_ring->trbs) {
        return -1;
    }

    void* entries_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    if (!entries_phys) {
        return -1;
    }

    erst->entries = (xhci_erst_entry_t*)vmm_phys_to_virt((uintptr_t)entries_phys);
    erst->entries_phys = (uint64_t)entries_phys;
    erst->num_entries = 1;

    erst->entries[0].ring_segment_base = event_ring->trbs_phys;
    erst->entries[0].ring_segment_size = event_ring->num_trbs;
    erst->entries[0].reserved = 0;

    return 0;
}

void xhci_erst_destroy(xhci_erst_t* erst)
{
    if (!erst || !erst->entries) {
        return;
    }

    pmm_free((void*)erst->entries_phys, 1);
    erst->entries = NULL;
    erst->entries_phys = 0;
}

uint64_t xhci_erst_get_phys_addr(xhci_erst_t* erst)
{
    if (!erst) {
        return 0;
    }
    return erst->entries_phys;
}
