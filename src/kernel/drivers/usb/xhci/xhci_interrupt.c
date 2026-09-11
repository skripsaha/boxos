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

static volatile uint16_t g_xhci_touch_connect_full    = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_connect_bare    = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_disconnect_full = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_disconnect_bare = TOUCH_TAG_INVALID;

static volatile uint16_t g_xhci_touch_arrived_full = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_arrived_bare = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_left_full    = TOUCH_TAG_INVALID;
static volatile uint16_t g_xhci_touch_left_bare    = TOUCH_TAG_INVALID;

typedef struct __attribute__((packed)) {
    uint8_t  port;
    uint8_t  slot_id;
    uint8_t  speed;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t usb_version;
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

static void xhci_scan_ports(xhci_controller_t* ctrl)
{
    for (unsigned pn = 1; pn <= ctrl->max_ports; pn++) {
        uint8_t  port   = (uint8_t)pn;
        uint32_t portsc = xhci_get_port_status(ctrl, port);
        uint32_t change_bits = portsc & XHCI_PORTSC_W1C_MASK;

        if (change_bits == 0) {
            continue;
        }

        xhci_port_clear_change_bits(ctrl, port, change_bits);

        if ((change_bits & XHCI_PORTSC_OCC) && (portsc & XHCI_PORTSC_OCA)) {
            kprintf("[xHCI] port %u: over-current — power removed\n", port);
        }

        if ((change_bits & XHCI_PORTSC_CSC) && !(portsc & XHCI_PORTSC_CCS)) {

            struct __attribute__((packed)) {
                uint8_t  port; uint8_t speed;
                uint16_t vendor_id; uint16_t product_id;
                uint16_t _reserved;
            } ev = { port, 0, 0, 0, 0 };

            bool socket_empty = xhci_port_socket_is_empty(ctrl, port);
            uint8_t other     = xhci_port_other_half(ctrl, port);

            if (ctrl->enum_attempts[port] > XHCI_ENUM_ATTEMPTS) {
                kprintf("[xHCI %s] port %u: it had been given up on, and what "
                        "was in it has gone — it will be tried again\n",
                        ctrl->name, port);
            }
            ctrl->enum_attempts[port] = 0;

            xhci_device_slot_t* slot = xhci_get_device_slot_by_port(ctrl, port);
            if (slot) {
                ev.vendor_id  = slot->device_desc.idVendor;
                ev.product_id = slot->device_desc.idProduct;
                ev.speed      = slot->speed;

                bool named = slot->device_desc.idVendor != 0 ||
                             slot->device_desc.idProduct != 0;

                if (!named) {
                    kprintf("[xHCI] port %u: whatever was there has gone "
                            "before it said who it is — %s\n", port,
                            socket_empty ? "the socket is empty"
                                         : "the other half of the socket has "
                                           "something in it");
                } else if (socket_empty) {
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

                if (slot->state == ENUM_STATE_CONFIGURED) {
                    xhci_touch_device_left(slot);
                }

                xhci_slot_retire(ctrl, slot);
            }

            TouchTag full = __atomic_load_n(&g_xhci_touch_disconnect_full,
                                           __ATOMIC_ACQUIRE);
            TouchTag bare = __atomic_load_n(&g_xhci_touch_disconnect_bare,
                                           __ATOMIC_ACQUIRE);
            TouchPublishIrqPair(full, bare, &ev, sizeof(ev),
                                0, TOUCH_FLAG_KERNEL);
            continue;
        }

        if (change_bits & (XHCI_PORTSC_PRC | XHCI_PORTSC_WRC)) {
            xhci_enum_port_reset_done(ctrl, port);
        }

        if ((change_bits & XHCI_PORTSC_CSC) && (portsc & XHCI_PORTSC_CCS)) {

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

    ctrl->op_regs->usbsts = usbsts & XHCI_STS_HSE;
    ctrl->error_state = true;
    ctrl->running     = false;
    return true;
}

static void xhci_process_events_on(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->running) {
        return;
    }

    uint32_t me = (uint32_t)amp_get_core_index() + 1u;
    for (;;) {
        if (spin_trylock(&ctrl->event_lock)) {
            break;
        }
        if (__atomic_load_n(&ctrl->drain_owner, __ATOMIC_ACQUIRE) == me) {
            __atomic_fetch_add(&ctrl->drain_skips, 1, __ATOMIC_RELAXED);
            return;
        }
        cpu_pause();
    }
    __atomic_store_n(&ctrl->drain_owner, me, __ATOMIC_RELEASE);

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

        xhci_trb_t event = *trb;
        uint8_t trb_type = TRB_GET_TYPE(event.control);

        event_ring->dequeue_idx++;
        if (event_ring->dequeue_idx >= event_ring->num_trbs) {
            event_ring->dequeue_idx = 0;
            event_ring->cycle_state ^= 1;
        }

        if (((event.status >> 24) & 0xFF) == TRB_COMPLETION_EVENT_RING_FULL) {
            kprintf("[xHCI %s] the event ring was full — the controller had "
                    "something to say and nowhere to write it, and that "
                    "answer is lost (ring of %u, dequeue at %u)\n",
                    ctrl->name, event_ring->num_trbs, event_ring->dequeue_idx);
        }

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
                kprintf("[xHCI %s] event type %u on the ring is one this "
                        "driver does not handle\n", ctrl->name, trb_type);
                break;
        }
    }

    uint64_t new_erdp = event_ring->trbs_phys +
                        (event_ring->dequeue_idx * sizeof(xhci_trb_t));
    new_erdp |= XHCI_ERDP_EHB;
    intr0->erdp = new_erdp;

    if ((event_ring->trbs[event_ring->dequeue_idx].control & TRB_C) ==
        event_ring->cycle_state) {
        goto drain_again;
    }

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
        xhci_process_events_on(ctrl);
        xhci_check_command_timeouts(ctrl);
        xhci_enum_watchdog(ctrl);
        xhci_enum_pump(ctrl);
    }
}

static void xhci_irq_handler_on(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->running) {
        return;
    }
    __atomic_fetch_add(&ctrl->irq_count, 1, __ATOMIC_RELAXED);

    uint32_t usbsts = ctrl->op_regs->usbsts;

    ctrl->op_regs->usbsts = usbsts & (XHCI_STS_HSE | XHCI_STS_EINT |
                                      XHCI_STS_PCD);

    ctrl->runtime_regs->interrupters[0].iman |= XHCI_IMAN_IP;

    xhci_controller_stopped(ctrl);

    xhci_process_events_on(ctrl);
}

void xhci_irq_handler_vector(uint8_t vector) {
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_controller_t* ctrl = xhci_controller_at(i);
        if (ctrl && ctrl->irq_vector == vector) {
            xhci_irq_handler_on(ctrl);
            return;
        }
    }
}

void xhci_irq_handler(void) {
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_irq_handler_on(xhci_controller_at(i));
    }
}