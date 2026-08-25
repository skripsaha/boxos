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
    bool producer;          /* true = command/transfer ring (has Link TRB at end) */
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

/* Initialize a ring. producer=true reserves last TRB as Link TRB. */
int xhci_ring_init(xhci_ring_t* ring, uint32_t num_trbs, bool producer);
void xhci_ring_destroy(xhci_ring_t* ring);
uint64_t xhci_ring_get_phys_addr(xhci_ring_t* ring);

/* Enqueue a TRB onto a producer ring. Sets cycle bit, handles Link TRB wrap.
 * Returns physical address of the enqueued TRB, or 0 when the ring is full. */
uint64_t xhci_ring_enqueue(xhci_ring_t* ring, xhci_trb_t* trb);

/*
 * How many TRBs may still be written before the ring would run into work the
 * controller has not finished with.
 *
 * ‼ This is the number the driver used to have no way of knowing. A producer
 * ring's dequeue index was never advanced by anything, so the "is it full"
 * test could not fire — it asked whether the enqueue index had reached the Link
 * TRB, and the enqueue index is wrapped past the Link TRB inside the very call
 * that would have set it. What looked like an overflow check was a branch no
 * execution could take, and the ring simply wrote over TRBs the controller was
 * still reading whenever anything queued deeper than the endpoint's one
 * in-flight transfer.
 *
 * One slot is always left empty so that a full ring and an empty one are not
 * the same pair of indices.
 */
uint32_t xhci_ring_space(xhci_ring_t* ring);

/*
 * The controller has finished with the TRB at this address, and therefore with
 * everything queued before it — a transfer ring is executed strictly in order.
 * Named by address because that is what every completion event carries.
 *
 * Ignores an address outside the ring, and an address that is not between the
 * dequeue and enqueue positions: that is a stale answer for work already
 * abandoned, and letting it move the dequeue index backwards would hand out
 * slots twice.
 */
void xhci_ring_reclaim_to(xhci_ring_t* ring, uint64_t trb_phys);

/*
 * Everything queued so far is abandoned, and the slots go back.
 *
 * This is the software half of a Set TR Dequeue Pointer: the controller is told
 * to resume where software stands, so the TRBs it was going to execute will
 * never be executed and will never be answered. Without this they would be
 * accounted as in flight for ever and the ring would run down to nothing, a
 * stalled control pipe at a time.
 */
void xhci_ring_abandon(xhci_ring_t* ring);

/* The ring arithmetic above, checked at boot with the numbers written down
 * rather than computed by the code under test. */
void XhciRingSelfTest(void);

int xhci_erst_init(xhci_erst_t* erst, xhci_ring_t* event_ring);
void xhci_erst_destroy(xhci_erst_t* erst);
uint64_t xhci_erst_get_phys_addr(xhci_erst_t* erst);

#endif
