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

/* How many slots a producer ring actually has: the last one is the Link TRB
 * and belongs to the hardware, not to whoever is queueing work. */
static uint32_t ring_slots(const xhci_ring_t* ring)
{
    return ring->num_trbs - 1;
}

/* How far `to` is ahead of `from`, going the way the ring goes. */
static uint32_t ring_distance(uint32_t from, uint32_t to, uint32_t slots)
{
    return (to + slots - from) % slots;
}

/* Free slots, with one always kept empty so that full and empty are different
 * pairs of indices. Called with the ring lock held. */
static uint32_t ring_space_locked(const xhci_ring_t* ring)
{
    uint32_t slots = ring_slots(ring);
    return slots - 1u - ring_distance(ring->dequeue_idx, ring->enqueue_idx, slots);
}

uint32_t xhci_ring_space(xhci_ring_t* ring)
{
    if (!ring || !ring->trbs || !ring->producer) {
        return 0;
    }
    spin_lock(&ring->ring_lock);
    uint32_t space = ring_space_locked(ring);
    spin_unlock(&ring->ring_lock);
    return space;
}

void xhci_ring_reclaim_to(xhci_ring_t* ring, uint64_t trb_phys)
{
    if (!ring || !ring->trbs || !ring->producer || trb_phys < ring->trbs_phys) {
        return;
    }

    uint64_t offset = trb_phys - ring->trbs_phys;
    if (offset % sizeof(xhci_trb_t)) {
        return;
    }
    uint32_t idx = (uint32_t)(offset / sizeof(xhci_trb_t));

    spin_lock(&ring->ring_lock);

    uint32_t slots = ring_slots(ring);
    if (idx < slots) {
        /* Only forward, and only as far as software has actually queued: an
         * answer from before the ring was repositioned names a TRB that is
         * behind the dequeue index now, and honouring it would free slots that
         * are in use. */
        uint32_t reach = ring_distance(ring->dequeue_idx, idx, slots);
        uint32_t queued = ring_distance(ring->dequeue_idx, ring->enqueue_idx, slots);
        if (reach < queued) {
            ring->dequeue_idx = (idx + 1u) % slots;
        }
    }

    spin_unlock(&ring->ring_lock);
}

void xhci_ring_abandon(xhci_ring_t* ring)
{
    if (!ring || !ring->trbs || !ring->producer) {
        return;
    }
    spin_lock(&ring->ring_lock);
    ring->dequeue_idx = ring->enqueue_idx;
    spin_unlock(&ring->ring_lock);
}

uint64_t xhci_ring_enqueue(xhci_ring_t* ring, xhci_trb_t* trb)
{
    if (!ring || !trb || !ring->trbs) {
        return 0;
    }

    spin_lock(&ring->ring_lock);

    /* No room means no room. Writing anyway is writing over a TRB the
     * controller has not executed yet. */
    if (ring->producer && ring_space_locked(ring) == 0) {
        spin_unlock(&ring->ring_lock);
        return 0;
    }

    uint32_t idx = ring->enqueue_idx;

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

/* -------------------------------------------------------------------------
 * The ring arithmetic, checked at boot.
 *
 * A transfer ring only ever overflows on a machine doing something this driver
 * does not do yet — one transfer per endpoint is in flight at a time, so the
 * accounting above is right by an invariant nothing states. That is exactly the
 * kind of code that is wrong the first time somebody queues two things: the
 * check it replaced was unreachable for years and nobody noticed, because
 * nothing ever reached it either.
 *
 * So the arithmetic is exercised directly, with the numbers written down rather
 * than computed by the code under test. Eight TRBs: seven slots, the eighth is
 * the Link TRB, and one slot is always left empty — six usable.
 * ------------------------------------------------------------------------- */
void XhciRingSelfTest(void)
{
    kprintf("[xHCI RING TEST] begin\n");
    int pass = 0, fail = 0;

    #define RING_CHECK(cond, label) do {                                       \
        if (cond) { pass++; kprintf("[xHCI RING TEST] PASS: " label "\n"); }    \
        else      { fail++; kprintf("[xHCI RING TEST] FAIL: " label "\n"); }    \
    } while (0)

    xhci_ring_t ring;
    if (xhci_ring_init(&ring, 8, true) != 0) {
        kprintf("[xHCI RING TEST] FAILED: no memory for an eight-TRB ring\n");
        return;
    }

    RING_CHECK(xhci_ring_space(&ring) == 6, "a fresh ring of eight offers six slots");

    /* Six fit, the seventh does not — and the seventh must be REFUSED rather
     * than written over the first. */
    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_NORMAL);
    int accepted = 0;
    for (int i = 0; i < 7; i++) {
        if (xhci_ring_enqueue(&ring, &trb) != 0) {
            accepted++;
        }
    }
    RING_CHECK(accepted == 6 && xhci_ring_space(&ring) == 0,
               "six are taken and the seventh is refused");

    /* An answer for the third TRB frees it and the two queued ahead of it. */
    xhci_ring_reclaim_to(&ring, ring.trbs_phys + 2 * sizeof(xhci_trb_t));
    RING_CHECK(xhci_ring_space(&ring) == 3,
               "an answer for the third frees three");

    /* An answer from before that — for work already accounted for — must move
     * nothing. Honouring it would hand the same slots out twice. */
    xhci_ring_reclaim_to(&ring, ring.trbs_phys);
    RING_CHECK(xhci_ring_space(&ring) == 3,
               "a stale answer moves the dequeue index nowhere");

    /* Repositioning the ring gives every queued slot back at once. */
    xhci_ring_abandon(&ring);
    RING_CHECK(xhci_ring_space(&ring) == 6,
               "abandoning what was queued returns all six");

    /* And the wrap: the enqueue index steps onto the Link TRB, which is armed
     * with the cycle bit the controller is still looking for, and the ring's
     * own cycle flips behind it. */
    uint8_t before = ring.cycle_state;
    xhci_ring_enqueue(&ring, &trb);
    uint32_t link_control = ring.trbs[7].control;
    RING_CHECK(ring.enqueue_idx == 0 && ring.cycle_state != before &&
               (link_control & TRB_TC) &&
               ((link_control & TRB_C) ? 1 : 0) == (before ? 1 : 0) &&
               xhci_ring_space(&ring) == 5,
               "crossing the Link TRB arms it and flips the cycle");

    #undef RING_CHECK

    xhci_ring_destroy(&ring);

    if (fail == 0) {
        kprintf("[xHCI RING TEST] %[S]PASSED%[D]: all %d checks OK\n", pass);
    } else {
        kprintf("[xHCI RING TEST] %[R]FAILED%[D]: %d pass, %d fail\n", pass, fail);
    }
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
