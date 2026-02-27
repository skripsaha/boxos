#ifndef XHCI_RINGS_H
#define XHCI_RINGS_H

#include "ktypes.h"
#include "xhci_trb.h"
#include "klib.h"

typedef struct {
    xhci_trb_t* trbs;
    uint64_t trbs_phys;
    uint32_t num_trbs;
    uint32_t enqueue_idx;
    uint32_t dequeue_idx;
    uint8_t cycle_state;
    spinlock_t ring_lock;
} xhci_ring_t;

typedef struct {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size;
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

typedef struct {
    xhci_erst_entry_t* entries;
    uint64_t entries_phys;
    uint32_t num_entries;
} xhci_erst_t;

int xhci_ring_init(xhci_ring_t* ring, uint32_t num_trbs);
void xhci_ring_destroy(xhci_ring_t* ring);
uint64_t xhci_ring_get_phys_addr(xhci_ring_t* ring);

int xhci_erst_init(xhci_erst_t* erst, xhci_ring_t* event_ring);
void xhci_erst_destroy(xhci_erst_t* erst);
uint64_t xhci_erst_get_phys_addr(xhci_erst_t* erst);

#endif
