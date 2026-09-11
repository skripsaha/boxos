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
    bool producer;
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

int xhci_ring_init(xhci_ring_t* ring, uint32_t num_trbs, bool producer);
void xhci_ring_destroy(xhci_ring_t* ring);
uint64_t xhci_ring_get_phys_addr(xhci_ring_t* ring);

uint64_t xhci_ring_enqueue(xhci_ring_t* ring, xhci_trb_t* trb);

uint32_t xhci_ring_space(xhci_ring_t* ring);

void xhci_ring_reclaim_to(xhci_ring_t* ring, uint64_t trb_phys);

void xhci_ring_abandon(xhci_ring_t* ring);

void XhciRingSelfTest(void);

int xhci_erst_init(xhci_erst_t* erst, xhci_ring_t* event_ring);
void xhci_erst_destroy(xhci_erst_t* erst);
uint64_t xhci_erst_get_phys_addr(xhci_erst_t* erst);

#endif