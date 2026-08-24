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
 * Two tags are cached because the xHCI port-change handler publishes to
 * one of two distinct events depending on whether the device connected
 * or disconnected. Both are bare keys (no value); the registry resolves
 * `bare_id` populated and `full_id` invalid. TouchPublishIrqPair handles
 * that case correctly by publishing to the non-invalid id only. */
static volatile uint16_t g_xhci_touch_connect_full    = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_connect_bare    = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_disconnect_full = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_disconnect_bare = TOUCH_TAG_INVALID;

void xhci_interrupt_touch_init(void)
{
    TouchTag full, bare;

    TouchTagResolve("usb:connect", &full, &bare);
    __atomic_store_n(&g_xhci_touch_connect_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xhci_touch_connect_bare, bare, __ATOMIC_RELEASE);

    TouchTagResolve("usb:disconnect", &full, &bare);
    __atomic_store_n(&g_xhci_touch_disconnect_full, full, __ATOMIC_RELEASE);
    __atomic_store_n(&g_xhci_touch_disconnect_bare, bare, __ATOMIC_RELEASE);

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

            xhci_device_slot_t* slot = xhci_get_device_slot_by_port(port);
            if (slot) {
                ev.vendor_id  = slot->device_desc.idVendor;
                ev.product_id = slot->device_desc.idProduct;
                ev.speed      = slot->speed;
                kprintf("[xHCI] port %u: %04x:%04x unplugged\n",
                        port, slot->device_desc.idVendor,
                        slot->device_desc.idProduct);
                xhci_post_disable_slot_cmd(ctrl, slot->slot_id);
                xhci_device_slot_cleanup(ctrl, slot);
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

void xhci_process_events(void) {
    xhci_controller_t* ctrl = xhci_get_controller();
    if (!ctrl || !ctrl->running) {
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
                debug_printf("[xHCI] Unknown event TRB type: %u\n", trb_type);
                break;
        }
    }

    /* Update ERDP and clear EHB (Event Handler Busy) by writing 1 to bit 3. */
    uint64_t new_erdp = event_ring->trbs_phys +
                        (event_ring->dequeue_idx * sizeof(xhci_trb_t));
    new_erdp |= XHCI_ERDP_EHB;
    intr0->erdp = new_erdp;
}

void xhci_poll_events(void) {
    xhci_controller_t* ctrl = xhci_get_controller();
    if (!ctrl || !ctrl->use_polling) {
        return;
    }
    xhci_process_events();
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
void xhci_tick(void) {
    xhci_controller_t* ctrl = xhci_get_controller();
    if (!ctrl || !ctrl->running) {
        return;
    }

    if (ctrl->use_polling) {
        xhci_process_events();
    }

    xhci_check_command_timeouts(ctrl);
    xhci_enum_watchdog(ctrl);
}

void xhci_irq_handler(void) {
    xhci_controller_t* ctrl = xhci_get_controller();
    if (!ctrl || !ctrl->running) {
        return;
    }

    uint32_t usbsts = ctrl->op_regs->usbsts;

    /* Clear W1C status bits immediately before processing. */
    ctrl->op_regs->usbsts = usbsts & (XHCI_STS_HSE | XHCI_STS_EINT |
                                      XHCI_STS_PCD);

    if (usbsts & XHCI_STS_HSE) {
        kprintf("[xHCI] host system error — the controller has stopped "
                "(USBSTS=0x%08x)\n", usbsts);
        ctrl->error_state = true;
    }

    if (usbsts & XHCI_STS_HCE) {
        kprintf("[xHCI] internal controller error — reset required "
                "(USBSTS=0x%08x)\n", usbsts);
        ctrl->error_state = true;
    }

    /* Everything the controller has to say arrives on the event ring, port
     * changes included. Port Change Detect above is acknowledged, not acted
     * on: acting on it here as well would mean two paths clearing the same
     * write-one-to-clear bits, with whichever ran first deciding what the
     * other one got to see. */
    xhci_process_events();

    /* Clear IMAN IP (Interrupt Pending) bit — write 1 to clear. */
    ctrl->runtime_regs->interrupters[0].iman |= XHCI_IMAN_IP;
}
