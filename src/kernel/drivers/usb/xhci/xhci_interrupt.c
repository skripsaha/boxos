#include "xhci_interrupt.h"
#include "xhci_msd.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "xhci_trb.h"
#include "xhci_port.h"
#include "xhci_command.h"
#include "xhci_transfer.h"
#include "xhci_enumeration.h"
#include "klib.h"
#include "touch.h"
#include "logbook.h"
#include "amp.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/* ─── Touch tag handle cache (resolved once via xhci_interrupt_touch_init)
 *
 * xhci_irq_handler runs in MSI/MSI-X / legacy-INTx context. Resolving a
 * tag string at IRQ time would take the TagFS registry lock and possibly
 * kmalloc a fresh intern entry — both of which violate IRQ-context lock
 * ordering on this kernel (see project memory `irq_defer_done_2026_05_17`).
 *
 * Four tags are cached: a connect/disconnect pair about the socket, and an
 * arrived/left pair about the device in it.
 *
 * Each is a key:value tag, and the registry interns BOTH halves — the full
 * (usb, arrived) id and the bare (usb) id — so a subscriber naming the full
 * tag gets only that event, while one naming the bare key gets everything USB.
 * A comment here used to claim the full id came back invalid for these; it
 * does not, and a subscriber measured on all four distinct ids. Publishing
 * carries both, and TouchPublishIrqPair skips whichever side is invalid. */
static volatile uint16_t g_xhci_touch_connect_full    = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_connect_bare    = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_disconnect_full = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_disconnect_bare = TOUCH_TAG_INVALID;

/* And two more for the device, as opposed to the socket it is in. */
static volatile uint16_t g_xhci_touch_arrived_full = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_arrived_bare = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_left_full    = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_left_bare    = TOUCH_TAG_INVALID;

/* What an arrival or a departure carries. Sixteen bytes, well inside the
 * bounded payload an IRQ-side publish is allowed, and enough that nothing
 * subscribing to it has to go and ask a second question. */
typedef struct __attribute__((packed)) {
    uint8_t  port;
    uint8_t  slot_id;
    uint8_t  speed;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t usb_version;       /* bcdUSB, so 0x0300 says SuperSpeed */
    uint32_t reserved;
} xhci_touch_device_t;

_Static_assert(sizeof(xhci_touch_device_t) == 16, "device event is 16 bytes");

void xhci_interrupt_touch_init(void)
{
    TouchTag full, bare;

    TouchLogbookResolve("usb:connect", &full, &bare);
    __atomic_store_n(&g_xhci_touch_connect_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xhci_touch_connect_bare, bare, __ATOMIC_RELEASE);

    TouchLogbookResolve("usb:disconnect", &full, &bare);
    __atomic_store_n(&g_xhci_touch_disconnect_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xhci_touch_disconnect_bare, bare, __ATOMIC_RELEASE);

    TouchLogbookResolve("usb:arrived", &full, &bare);
    __atomic_store_n(&g_xhci_touch_arrived_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xhci_touch_arrived_bare, bare, __ATOMIC_RELEASE);

    TouchLogbookResolve("usb:left", &full, &bare);
    __atomic_store_n(&g_xhci_touch_left_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xhci_touch_left_bare, bare, __ATOMIC_RELEASE);

    debug_printf("[xHCI] Touch tag handles cached: connect=%u/%u, disconnect=%u/%u\n",
                 (unsigned)__atomic_load_n(&g_xhci_touch_connect_full,    __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&g_xhci_touch_connect_bare,    __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&g_xhci_touch_disconnect_full, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&g_xhci_touch_disconnect_bare, __ATOMIC_RELAXED));
}

/*
 * A port said something. Find out what, for every port, and act on it.
 *
 * There is exactly one place that reads and clears the port change bits. The
 * driver used to have two — a scan driven by the Port Change Detect bit in
 * USBSTS, reachable only from the interrupt handler, and a Port Status Change
 * event on the event ring that was decoded and thrown away. That meant a
 * controller with no usable interrupt line, running on the polled fallback,
 * never noticed a port change at all: the events arrived and were discarded,
 * and the scan that would have caught them was never reached.
 *
 * The event ring is the path that works in both worlds, so it is the only one.
 */
static void xhci_scan_ports(xhci_controller_t* ctrl)
{
    /*
     * ‼ `port` is wider than the eight-bit field it is counting over, and has to
     * be. MaxPorts and MaxSlots are both eight bits, so both may legitimately
     * be 255 — and `for (uint8_t i = 1; i <= 255; i++)` never ends: the
     * counter wraps to zero before the test can fail. Every board this has run
     * on reports twenty-four ports and sixty-four slots, which is exactly the
     * kind of number that makes a loop look correct for years.
     */
    for (unsigned pn = 1; pn <= ctrl->max_ports; pn++) {
        uint8_t  port   = (uint8_t)pn;
        uint32_t portsc = xhci_get_port_status(ctrl, port);
        uint32_t change_bits = portsc & XHCI_PORTSC_W1C_MASK;

        if (change_bits == 0) {
            continue;
        }

        xhci_port_clear_change_bits(ctrl, port, change_bits);

        if ((change_bits & XHCI_PORTSC_OCC) && (portsc & XHCI_PORTSC_OCA)) {
            /* Over-current is the port protecting itself, and the device on it
             * is gone until a human unplugs whatever caused it. Saying so is
             * the whole of what software can do. */
            kprintf("[xHCI] port %u: over-current — power removed\n", port);
        }

        if ((change_bits & XHCI_PORTSC_CSC) && !(portsc & XHCI_PORTSC_CCS)) {

            /* Publish before tearing down, so the event can still carry what
             * was on the port rather than what is left of it. */
            struct __attribute__((packed)) {
                uint8_t  port; uint8_t speed;
                uint16_t vendor_id; uint16_t product_id;
                uint16_t _reserved;
            } ev = { port, 0, 0, 0, 0 };

            /*
             * ‼ AND WHETHER THE SOCKET IS EMPTY, WHICH IS A DIFFERENT QUESTION.
             *
             * A USB 3 socket is two root ports. A device whose SuperSpeed link
             * does not train disappears from one of them and turns up on the
             * other — measured on a live laptop by another developer: connect
             * on port 6, disconnect on port 6, connect on port 2, one hole in
             * the case. On the port it left, that is bit-for-bit what a hand
             * pulling it out looks like, and telling the two apart from a log
             * was not possible: both are CSC set with CCS clear.
             *
             * Asking the OTHER half separates them, with a register read
             * rather than a clock. An empty socket is a hand. An occupied one
             * is the machine dropping to a slower half of the same connector,
             * and the device is still there.
             */
            bool socket_empty = xhci_port_socket_is_empty(ctrl, port);
            uint8_t other     = xhci_port_other_half(ctrl, port);

            xhci_device_slot_t* slot = xhci_get_device_slot_by_port(ctrl, port);
            if (slot) {
                ev.vendor_id  = slot->device_desc.idVendor;
                ev.product_id = slot->device_desc.idProduct;
                ev.speed      = slot->speed;
                if (socket_empty) {
                    kprintf("[xHCI] port %u: %04x:%04x unplugged — the socket "
                            "is empty\n",
                            port, slot->device_desc.idVendor,
                            slot->device_desc.idProduct);
                } else {
                    kprintf("[xHCI] port %u: %04x:%04x left this port, and "
                            "port %u — the other half of the same socket — "
                            "has something in it\n",
                            port, slot->device_desc.idVendor,
                            slot->device_desc.idProduct, other);
                }

                /* Only a device that was announced gets a departure. */
                if (slot->state == ENUM_STATE_CONFIGURED) {
                    xhci_touch_device_left(slot);
                }

                /* Marked as gone; what is left of it is taken down where
                 * waiting is allowed. This handler used to post the Disable
                 * Slot and free the device context in the next statement,
                 * without waiting for the controller to say it had finished
                 * reading it. */
                xhci_slot_retire(ctrl, slot);
            }
            /* No slot means nothing was lost — an empty port clearing a stale
             * connect-change from power-on is not an unplug, and saying so for
             * each of a couple of dozen root ports at boot would be noise
             * standing exactly where a real fault has to be readable. */

            /* IRQ context: hand off to K-Core via the static-ring +
             * irq_defer path. See touch.c TouchPublishIrqPair block. */
            TouchTag full = __atomic_load_n(&g_xhci_touch_disconnect_full,
                                           __ATOMIC_ACQUIRE);
            TouchTag bare = __atomic_load_n(&g_xhci_touch_disconnect_bare,
                                           __ATOMIC_ACQUIRE);
            TouchPublishIrqPair(full, bare, &ev, sizeof(ev),
                                0, TOUCH_FLAG_KERNEL);
            continue;
        }

        /* A reset that finished. The enumeration this belongs to has been
         * waiting for exactly this rather than spinning on the register. */
        if (change_bits & (XHCI_PORTSC_PRC | XHCI_PORTSC_WRC)) {
            xhci_enum_port_reset_done(ctrl, port);
        }

        if ((change_bits & XHCI_PORTSC_CSC) && (portsc & XHCI_PORTSC_CCS)) {

            /*
             * Connect-change with something connected, on a port that already
             * has a device on it, means the connection CHANGED — a hand pulled
             * one thing out and pushed another in between two reads of this
             * register. There is one connect-change bit and it does not count,
             * so a swap and a re-seat look the same, and both leave a slot
             * addressed to a device that is not there.
             *
             * The old code called xhci_enumerate_device, which found the live
             * slot and returned "already enumerating" — so the newcomer was
             * never spoken to, and every transfer aimed at the departed device
             * went to whatever now answers on that socket. That is worse than
             * losing the device: it is a disk driver writing to a stranger.
             *
             * Only for a device that had finished coming up. During
             * enumeration the connect-change is this driver's own port reset,
             * and tearing the slot down for it would mean no device ever
             * finished.
             */
            xhci_device_slot_t* live = xhci_get_device_slot_by_port(ctrl, port);
            if (live && __atomic_load_n(&live->state, __ATOMIC_ACQUIRE) ==
                            ENUM_STATE_CONFIGURED) {
                kprintf("[xHCI %s] port %u: the connection changed under a "
                        "device that was already there (%04x:%04x) — letting "
                        "it go and starting again\n",
                        ctrl->name, port, live->device_desc.idVendor,
                        live->device_desc.idProduct);
                xhci_touch_device_left(live);
                xhci_slot_retire(ctrl, live);
            }

            debug_printf("[xHCI] Device connected on port %u\n", port);
            xhci_enumerate_device(ctrl, port);

            /* Hot-plug Touch event — port + speed. Enumeration runs
             * asynchronously and fills vendor/product later; subscribers that
             * need device IDs query the slot APIs once a descriptor is in. */
            uint8_t speed = (uint8_t)((portsc >> 10) & 0xF);
            struct __attribute__((packed)) {
                uint8_t  port; uint8_t speed;
                uint16_t vendor_id; uint16_t product_id;
                uint16_t _reserved;
            } ev = { port, speed, 0, 0, 0 };
            TouchTag full = __atomic_load_n(&g_xhci_touch_connect_full,
                                           __ATOMIC_ACQUIRE);
            TouchTag bare = __atomic_load_n(&g_xhci_touch_connect_bare,
                                           __ATOMIC_ACQUIRE);
            TouchPublishIrqPair(full, bare, &ev, sizeof(ev),
                                0, TOUCH_FLAG_KERNEL);
        }
    }
}

static void xhci_publish_device(const xhci_device_slot_t* slot,
                                volatile uint16_t* full_cache,
                                volatile uint16_t* bare_cache)
{
    if (!slot) {
        return;
    }

    xhci_touch_device_t ev = {
        .port          = slot->port_num,
        .slot_id       = slot->slot_id,
        .speed         = slot->speed,
        .dev_class     = slot->interface_class,
        .dev_subclass  = slot->interface_subclass,
        .dev_protocol  = slot->interface_protocol,
        .vendor_id     = slot->device_desc.idVendor,
        .product_id    = slot->device_desc.idProduct,
        .usb_version   = slot->device_desc.bcdUSB,
        .reserved      = 0,
    };

    TouchTag full = __atomic_load_n(full_cache, __ATOMIC_ACQUIRE);
    TouchTag bare = __atomic_load_n(bare_cache, __ATOMIC_ACQUIRE);
    TouchPublishIrqPair(full, bare, &ev, sizeof(ev), 0, TOUCH_FLAG_KERNEL);
}

void xhci_touch_device_arrived(const xhci_device_slot_t* slot)
{
    xhci_publish_device(slot, &g_xhci_touch_arrived_full,
                              &g_xhci_touch_arrived_bare);
}

void xhci_touch_device_left(const xhci_device_slot_t* slot)
{
    xhci_publish_device(slot, &g_xhci_touch_left_full,
                              &g_xhci_touch_left_bare);
}

/*
 * Has the controller stopped itself?
 *
 * Host System Error and Host Controller Error both mean it has hit something
 * it cannot continue past and halted: it stops executing the command ring,
 * stops producing events, and answers nothing. From the outside that is
 * indistinguishable from a command that is merely slow — which is exactly how
 * it was read, for weeks, on a live board.
 *
 * ‼ This used to be asked ONLY from the interrupt handler, and on this kernel
 * `sti` is the last thing main() does — long after xhci_init, BoardroomInit
 * and storage_deck_init. So through the whole of USB bring-up no interrupt can
 * fire, and a controller that failed on its first Address Device was silent by
 * construction. The single fact that would have explained the failure was
 * being read from the one place that cannot run when it happens.
 *
 * So it is asked here instead, in the drain, which runs continuously while
 * devices are being enumerated. One register read per pass.
 */
static bool xhci_controller_stopped(xhci_controller_t* ctrl)
{
    uint32_t usbsts = ctrl->op_regs->usbsts;

    if (usbsts == 0xFFFFFFFFu) {
        if (!ctrl->error_state) {
            kprintf("[xHCI %s] the controller has stopped answering its own "
                    "registers — it is gone from the bus\n", ctrl->name);
            ctrl->error_state = true;
            ctrl->running     = false;
        }
        return true;
    }

    if (!(usbsts & (XHCI_STS_HSE | XHCI_STS_HCE))) {
        return false;
    }

    if (!ctrl->error_state) {
        kprintf("[xHCI %s] %s — the controller has stopped and needs a reset "
                "(USBSTS=0x%08x USBCMD=0x%08x CRCR=0x%08x DCBAAP=0x%08x "
                "CONFIG=0x%08x)\n",
                ctrl->name,
                (usbsts & XHCI_STS_HCE) ? "internal controller error"
                                        : "host system error",
                usbsts,
                ctrl->op_regs->usbcmd,
                (uint32_t)ctrl->op_regs->crcr,
                (uint32_t)ctrl->op_regs->dcbaap,
                ctrl->op_regs->config);
    }

    /* HSE is write-one-to-clear and HCE is not: a controller in HCE stays in
     * it until it is reset, which is what the recovery pass is for. */
    ctrl->op_regs->usbsts = usbsts & XHCI_STS_HSE;
    ctrl->error_state = true;
    ctrl->running     = false;
    return true;
}

/* One controller's event ring. Draining is per-controller because the ring,
 * the lock and the interrupter all are. */
static void xhci_process_events_on(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->running) {
        return;
    }

    /* One drainer at a time. This used to be reached only from the interrupt
     * handler, where that was true by construction; it is now also reached by
     * whoever is waiting on a transfer, and two drainers advancing the same
     * dequeue pointer would each consume events the other was looking for.
     *
     * The lock disables interrupts while held, so the handler cannot preempt a
     * drain on this core, and on another core it waits out a drain measured in
     * microseconds. */
    /*
     * One drainer at a time — but "give up" and "wait a moment" are different
     * answers, and this used to give the first to both questions.
     *
     * A nested call on the SAME core has to leave: it would be waiting for a
     * lock its own stack frame is holding. A call from another core has only
     * to wait, because the holder is about to finish and is reading the very
     * ring this caller wants read. Abandoning that one means a core walks away
     * from a ring nobody else is going to look at again soon — and during boot
     * there is no timer tick to come back for it.
     */
    uint32_t me = (uint32_t)amp_get_core_index() + 1u;
    for (;;) {
        if (spin_trylock(&ctrl->event_lock)) {
            break;
        }
        if (__atomic_load_n(&ctrl->drain_owner, __ATOMIC_ACQUIRE) == me) {
            /* Ourselves, further up this stack. Theirs will reach every event
             * on the ring, including the one this caller is waiting for. */
            __atomic_fetch_add(&ctrl->drain_skips, 1, __ATOMIC_RELAXED);
            return;
        }
        cpu_pause();
    }
    __atomic_store_n(&ctrl->drain_owner, me, __ATOMIC_RELEASE);

    /* Before anything else: is it still running at all? A halted controller
     * produces no events, so a drain that does not ask this simply finds an
     * empty ring, every time, for ever — which is what "the device stopped
     * being answered" looked like from the outside. */
    if (xhci_controller_stopped(ctrl)) {
        __atomic_store_n(&ctrl->drain_owner, 0, __ATOMIC_RELEASE);
        spin_unlock(&ctrl->event_lock);
        return;
    }

    xhci_ring_t* event_ring = &ctrl->event_ring;
    xhci_interrupter_regs_t* intr0 = &ctrl->runtime_regs->interrupters[0];
    uint32_t drained = 0;
    uint64_t drain_began = rdtsc();

drain_again:
    while (1) {
        xhci_trb_t* trb = &event_ring->trbs[event_ring->dequeue_idx];

        uint8_t trb_cycle = trb->control & TRB_C;
        if (trb_cycle != event_ring->cycle_state) {
            break;
        }

        /* Read the TRB out before the dequeue pointer moves past it: the
         * controller may reuse the slot the moment it sees ERDP advance. */
        xhci_trb_t event = *trb;
        uint8_t trb_type = TRB_GET_TYPE(event.control);

        event_ring->dequeue_idx++;
        if (event_ring->dequeue_idx >= event_ring->num_trbs) {
            event_ring->dequeue_idx = 0;
            event_ring->cycle_state ^= 1;
        }

        /*
         * The one completion code that is not about a transfer at all.
         *
         * Event Ring Full says the controller had something to tell software
         * and no room to write it: whatever that was is gone, and whoever was
         * waiting for it will wait until a watchdog gives up. It is the only
         * failure in this driver that is invisible by construction — the event
         * that would have reported it is the event that could not be posted —
         * so it is said out loud the moment the controller manages to mention
         * it, and never folded into the per-transfer handling below.
         */
        if (((event.status >> 24) & 0xFF) == TRB_COMPLETION_EVENT_RING_FULL) {
            kprintf("[xHCI %s] the event ring was full — the controller had "
                    "something to say and nowhere to write it, and that "
                    "answer is lost (ring of %u, dequeue at %u)\n",
                    ctrl->name, event_ring->num_trbs, event_ring->dequeue_idx);
        }

        /*
         * Tell the controller how far software has got, DURING the drain and
         * not only at the end of it.
         *
         * xHCI 1.2 Section 4.9.4: the ring is full when the controller's
         * enqueue position runs into the dequeue pointer software published,
         * and software publishes that pointer here. Doing it once, after the
         * loop, means a burst longer than the ring meets a controller that
         * still believes software has read nothing — the ring fills, and the
         * events at the end of the burst are the ones that never get written.
         * Costs one register write per batch of events actually handled.
         */
        if (++drained % XHCI_ERDP_BATCH == 0) {
            intr0->erdp = (event_ring->trbs_phys +
                           event_ring->dequeue_idx * sizeof(xhci_trb_t)) |
                          XHCI_ERDP_EHB;
        }

        switch (trb_type) {
            case TRB_TYPE_PORT_STATUS_CHANGE:
                xhci_scan_ports(ctrl);
                break;

            case TRB_TYPE_COMMAND_COMPLETION:
                xhci_handle_command_completion(ctrl, &event);
                break;

            case TRB_TYPE_TRANSFER_EVENT:
                xhci_handle_transfer_event(ctrl, &event);
                break;

            default:
                /* Visible: an event this driver does not understand is an
                 * event somebody is waiting for and will not get. */
                kprintf("[xHCI %s] event type %u on the ring is one this "
                        "driver does not handle\n", ctrl->name, trb_type);
                break;
        }
    }

    /* Update ERDP and clear EHB (Event Handler Busy) by writing 1 to bit 3. */
    uint64_t new_erdp = event_ring->trbs_phys +
                        (event_ring->dequeue_idx * sizeof(xhci_trb_t));
    new_erdp |= XHCI_ERDP_EHB;
    intr0->erdp = new_erdp;

    /*
     * And look once more, because the ring is not empty just because it was.
     *
     * The controller may post an event between the read that found the ring
     * empty and the write that advances ERDP past it — xHCI 1.2 §4.9.4 asks
     * software to re-examine the ring after advancing the pointer for exactly
     * this reason. Leaving without looking parks that event until the next
     * drain, and during boot there is no timer tick to be the next drain: it
     * waits for whoever happens along, which on a live board meant a
     * controller that had executed a command sitting there with the
     * completion nobody collected.
     */
    if ((event_ring->trbs[event_ring->dequeue_idx].control & TRB_C) ==
        event_ring->cycle_state) {
        goto drain_again;
    }

    /*
     * How far behind the controller software was allowed to fall, said once.
     *
     * A ring that has been half full has been one burst of the same size away
     * from losing events, and losing an event is the one failure here that
     * reports itself as silence. Whether 256 entries is enough for a given
     * board is not something to decide by argument — this is the number that
     * settles it, and it costs a comparison per drain.
     */
    /* How long this drain held the lock, which is how long interrupts were off
     * on this core. Kept as a maximum because the worst one is the only one
     * that matters. */
    {
        uint32_t took_us = (uint32_t)cpu_tsc_to_us(rdtsc() - drain_began);
        if (took_us > ctrl->drain_longest_us) {
            ctrl->drain_longest_us = took_us;
        }
    }

    if (drained > ctrl->event_high_water) {
        ctrl->event_high_water = drained;
        if (!ctrl->event_pressure_said && drained * 2 > event_ring->num_trbs) {
            ctrl->event_pressure_said = true;
            kprintf("[xHCI %s] one drain handled %u events of a %u-entry ring "
                    "— the ring is closer to full than it should ever be\n",
                    ctrl->name, drained, event_ring->num_trbs);
        }
    }

    __atomic_store_n(&ctrl->drain_owner, 0, __ATOMIC_RELEASE);
    spin_unlock(&ctrl->event_lock);
}

bool xhci_drain_is_mine(const xhci_controller_t* ctrl)
{
    if (!ctrl) {
        return false;
    }
    uint32_t me = (uint32_t)amp_get_core_index() + 1u;
    return __atomic_load_n(&ctrl->drain_owner, __ATOMIC_ACQUIRE) == me;
}

void xhci_poll_events(void) {
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_controller_t* ctrl = xhci_controller_at(i);
        if (ctrl && ctrl->use_polling) {
            xhci_process_events_on(ctrl);
        }
    }
}

/*
 * Once per timer tick: drain events if this controller has no interrupt to
 * drain them with, and check that nothing has quietly stopped answering.
 *
 * The two watchdogs are the reason this exists. Command timeouts were already
 * written and reachable from nowhere at all — a facility that had never once
 * run. Enumeration had no equivalent. Between them they cover every wait this
 * driver makes: a controller that swallows a command, and a device that stops
 * partway through being asked who it is. Neither is supposed to happen, and
 * both are supposed to be said out loud when they do.
 */
/*
 * Every controller, every time.
 *
 * Interrupts are shared and a vector says which line fired, not which
 * controller is holding an event — and the drain of a ring with nothing on it
 * is one register read. Asking all of them is both simpler and the only answer
 * that stays right when a machine has more than one.
 */
void xhci_process_events(void) {
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_process_events_on(xhci_controller_at(i));
    }
}

void xhci_tick(void) {
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_controller_t* ctrl = xhci_controller_at(i);
        if (!ctrl || !ctrl->running) {
            continue;
        }
        /*
         * Drained every tick, whether or not this controller has an interrupt.
         *
         * It used to be drained here only in the polled fallback, on the
         * assumption that a controller with MSI would deliver one. A machine
         * is not obliged to honour that assumption: an interrupt that never
         * arrives — a message routed nowhere, a chipset erratum, firmware that
         * left something half-configured — turned into a bus where four
         * devices had been found, addressed, and then left mid-conversation
         * until the watchdog gave up on them, with nothing in the log to say
         * why. Nothing else in this driver polls, and a ring with no events on
         * it costs one read of one register per hundredth of a second.
         *
         * The interrupt is still what makes it quick. This is what makes it
         * work at all.
         */
        xhci_process_events_on(ctrl);
        xhci_check_command_timeouts(ctrl);
        xhci_enum_watchdog(ctrl);
        xhci_enum_pump(ctrl);
    }
    /*
     * ‼ The reads nobody is standing over are NOT looked at from here.
     *
     * Giving up on one means resetting the transport, and a Bulk-Only
     * Transport reset is three control transfers with a budget of a second
     * each — three seconds inside IRQ0, which does not send its
     * end-of-interrupt until it returns. Measured against the rest of this
     * file: that is the same fault as aborting the command ring from the tick,
     * and it has the same answer. xhci_msd_watchdog is called from the guide
     * loop and the idle loop instead, where waiting is allowed; it compares a
     * TSC deadline, so the cadence it runs at is nobody's business but its own.
     */
}

/*
 * One interrupt, asked of every controller.
 *
 * A vector says which line fired, not which controller is holding an event,
 * and on a machine with two of them the line may well be shared. Reading a
 * status register and finding nothing costs one access; guessing costs a
 * device that never gets serviced.
 */
static void xhci_irq_handler_on(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->running) {
        return;
    }
    __atomic_fetch_add(&ctrl->irq_count, 1, __ATOMIC_RELAXED);

    uint32_t usbsts = ctrl->op_regs->usbsts;

    /* Clear W1C status bits immediately before processing. */
    ctrl->op_regs->usbsts = usbsts & (XHCI_STS_HSE | XHCI_STS_EINT |
                                      XHCI_STS_PCD);

    /*
     * Acknowledge the interrupt BEFORE reading the ring, not after.
     *
     * xHCI 1.2 §4.17.5 puts it in this order for a reason: the controller
     * raises Interrupt Pending again the moment it adds an event, and an
     * acknowledgement written after the ring has been read clears a flag that
     * was raised by an event arriving during the read. That event is still on
     * the ring, but nothing will interrupt to say so. Clearing it first costs
     * a spurious interrupt at worst — the drain finds nothing and says so with
     * a register write — and loses none.
     */
    ctrl->runtime_regs->interrupters[0].iman |= XHCI_IMAN_IP;

    xhci_controller_stopped(ctrl);

    /* Everything the controller has to say arrives on the event ring, port
     * changes included. Port Change Detect above is acknowledged, not acted
     * on: acting on it here as well would mean two paths clearing the same
     * write-one-to-clear bits, with whichever ran first deciding what the
     * other one got to see. */
    xhci_process_events_on(ctrl);
}

/*
 * The controller that raised this vector, and only that one.
 *
 * Each controller signals on a vector of its own, so an interrupt names its
 * source instead of being offered to every controller in turn on the chance
 * that it was the one that spoke.
 */
void xhci_irq_handler_vector(uint8_t vector) {
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_controller_t* ctrl = xhci_controller_at(i);
        if (ctrl && ctrl->irq_vector == vector) {
            xhci_irq_handler_on(ctrl);
            return;
        }
    }
}

/* Every controller, for the polled fallback and for anything that has no
 * vector to go on. */
void xhci_irq_handler(void) {
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_irq_handler_on(xhci_controller_at(i));
    }
}
