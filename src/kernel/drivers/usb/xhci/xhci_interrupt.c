#include "xhci_interrupt.h"
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

    TouchTagResolve("usb:connect", &full, &bare);
    __atomic_store_n(&g_xhci_touch_connect_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xhci_touch_connect_bare, bare, __ATOMIC_RELEASE);

    TouchTagResolve("usb:disconnect", &full, &bare);
    __atomic_store_n(&g_xhci_touch_disconnect_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xhci_touch_disconnect_bare, bare, __ATOMIC_RELEASE);

    TouchTagResolve("usb:arrived", &full, &bare);
    __atomic_store_n(&g_xhci_touch_arrived_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xhci_touch_arrived_bare, bare, __ATOMIC_RELEASE);

    TouchTagResolve("usb:left", &full, &bare);
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
    for (uint8_t port = 1; port <= ctrl->max_ports; port++) {
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

            xhci_device_slot_t* slot = xhci_get_device_slot_by_port(ctrl, port);
            if (slot) {
                ev.vendor_id  = slot->device_desc.idVendor;
                ev.product_id = slot->device_desc.idProduct;
                ev.speed      = slot->speed;
                kprintf("[xHCI] port %u: %04x:%04x unplugged\n",
                        port, slot->device_desc.idVendor,
                        slot->device_desc.idProduct);

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
    if (!spin_trylock(&ctrl->event_lock)) {
        /* Somebody is already draining. Theirs will reach every event on the
         * ring including the one this caller is waiting for, so there is
         * nothing to add by queueing behind them — and a great deal to lose:
         * a drain reached from inside another drain, on the same core, would
         * be waiting for a lock its own caller holds. */
        return;
    }

    xhci_ring_t* event_ring = &ctrl->event_ring;
    xhci_interrupter_regs_t* intr0 = &ctrl->runtime_regs->interrupters[0];

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

    spin_unlock(&ctrl->event_lock);
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
    }
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

    if (usbsts & (XHCI_STS_HSE | XHCI_STS_HCE)) {
        /*
         * The controller has stopped and will not start again by itself.
         *
         * Said once, with the state that explains it and the name of the
         * controller it happened to — on a machine with two of them, "the
         * controller has stopped" identifies neither. Everything on it is now
         * unreachable: every device mid-enumeration will run out its two
         * seconds and be released, and there is no point pretending otherwise.
         */
        if (!ctrl->error_state) {
            kprintf("[xHCI %s] %s — the controller has stopped and needs a "
                    "reset (USBSTS=0x%08x USBCMD=0x%08x CRCR=0x%08x "
                    "DCBAAP=0x%08x CONFIG=0x%08x)\n",
                    ctrl->name,
                    (usbsts & XHCI_STS_HCE) ? "internal controller error"
                                            : "host system error",
                    usbsts,
                    ctrl->op_regs->usbcmd,
                    (uint32_t)ctrl->op_regs->crcr,
                    (uint32_t)ctrl->op_regs->dcbaap,
                    ctrl->op_regs->config);
        }
        ctrl->error_state = true;
        ctrl->running     = false;
    }

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
