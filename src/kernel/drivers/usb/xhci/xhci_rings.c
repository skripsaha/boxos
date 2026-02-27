#include "xhci_rings.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"

int xhci_ring_init(xhci_ring_t* ring, uint32_t num_trbs) {
    if (!ring || num_trbs == 0) {
        return -1;
    }

    size_t pages_needed = vmm_size_to_pages(num_trbs * sizeof(xhci_trb_t));
    void* trbs_phys = pmm_alloc_zero(pages_needed);
    if (!trbs_phys) {
        return -1;
    }

    ring->trbs = (xhci_trb_t*)vmm_phys_to_virt((uintptr_t)trbs_phys);
    ring->trbs_phys = (uint64_t)trbs_phys;
    ring->num_trbs = num_trbs;
    ring->enqueue_idx = 0;
    ring->dequeue_idx = 0;
    ring->cycle_state = 1;
    spinlock_init(&ring->ring_lock);

    return 0;
}

void xhci_ring_destroy(xhci_ring_t* ring) {
    if (!ring || !ring->trbs) {
        return;
    }

    size_t pages = vmm_size_to_pages(ring->num_trbs * sizeof(xhci_trb_t));
    pmm_free((void*)ring->trbs_phys, pages);

    ring->trbs = NULL;
    ring->trbs_phys = 0;
    ring->num_trbs = 0;
}

uint64_t xhci_ring_get_phys_addr(xhci_ring_t* ring) {
    if (!ring) {
        return 0;
    }
    return ring->trbs_phys;
}

int xhci_erst_init(xhci_erst_t* erst, xhci_ring_t* event_ring) {
    if (!erst || !event_ring || !event_ring->trbs) {
        return -1;
    }

    void* entries_phys = pmm_alloc_zero(1);
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

void xhci_erst_destroy(xhci_erst_t* erst) {
    if (!erst || !erst->entries) {
        return;
    }

    pmm_free((void*)erst->entries_phys, 1);
    erst->entries = NULL;
    erst->entries_phys = 0;
}

uint64_t xhci_erst_get_phys_addr(xhci_erst_t* erst) {
    if (!erst) {
        return 0;
    }
    return erst->entries_phys;
}
