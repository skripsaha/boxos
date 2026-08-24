#include "xhci_hub.h"
#include "xhci_endpoint.h"
#include "xhci_enumeration.h"
#include "xhci_transfer.h"
#include "xhci_command.h"
#include "xhci_device.h"
#include "xhci_port.h"
#include "xhci_trb.h"
#include "usb_common.h"
#include "usb_descriptors.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/* ── the hub class, on the wire (USB 2.0 chapter 11) ────────────────────── */

#define USB_DESC_HUB            0x29

/* Features of a downstream port, as SET_FEATURE and CLEAR_FEATURE name them. */
#define PORT_FEAT_ENABLE        1
#define PORT_FEAT_RESET         4
#define PORT_FEAT_POWER         8
#define PORT_FEAT_C_CONNECTION  16
#define PORT_FEAT_C_ENABLE      17
#define PORT_FEAT_C_RESET       20

/* wPortStatus */
#define PORT_STAT_CONNECTION    (1u << 0)
#define PORT_STAT_ENABLE        (1u << 1)
#define PORT_STAT_RESET         (1u << 4)
#define PORT_STAT_POWER         (1u << 8)
#define PORT_STAT_LOW_SPEED     (1u << 9)
#define PORT_STAT_HIGH_SPEED    (1u << 10)

/* wPortChange */
#define PORT_CHG_CONNECTION     (1u << 0)
#define PORT_CHG_ENABLE         (1u << 1)
#define PORT_CHG_RESET          (1u << 4)

#define HUB_CTRL_TIMEOUT_MS     1000

/* USB 2.0 §7.1.7.5: a port reset is driven for at least 10 ms and the device
 * is allowed 10 ms more to recover. The hub reports when it is done, and this
 * is only the outside edge of waiting for that. */
#define HUB_RESET_TIMEOUT_MS    800
#define HUB_RESET_POLL_MS       10
#define HUB_RESET_RECOVERY_MS   20

/* A newly connected device must be given time to settle before it is reset —
 * §7.1.7.3 calls it debounce and asks for 100 ms. */
#define HUB_DEBOUNCE_MS         100

typedef struct XhciHub {
    struct XhciHub* next;
    xhci_controller_t*  ctrl;
    xhci_device_slot_t* slot;

    uint8_t  ports;
    uint8_t  power_on_delay_ms;
    bool     attached;

    /* Raised by the status-change endpoint from interrupt context, lowered by
     * the service pass that acts on it. */
    volatile bool change_pending;

    /* Which of our ports currently has a device, so a change report can be
     * turned into "this one arrived" or "this one left". */
    uint32_t occupied;

    void*    buf_virt;
    uint64_t buf_phys;
} XhciHub;

static XhciHub* g_hubs = NULL;
static spinlock_t g_hubs_lock;
static bool g_hubs_lock_ready = false;
static volatile uint32_t g_hub_work = 0;

/*
 * One core services the hubs at a time.
 *
 * This is reached from the idle loop, and on a multi-core machine EVERY idle
 * core reaches it at once — they all see the same flag raised, all walk the
 * same list of slots, all find the same unattached hub, and all attach it.
 * Measured: one hub, three XhciHub structures, three port scans, and a
 * keyboard enumerated by whichever of them got there last.
 *
 * A lock would serialise them into doing the work three times in a row
 * instead of three times at once. What is wanted is for the other two to go
 * away: the work is idempotent and whoever is already inside it will do it.
 */
static volatile uint32_t g_hub_busy = 0;

static void hubs_lock_init(void)
{
    if (!g_hubs_lock_ready) {
        spinlock_init(&g_hubs_lock);
        g_hubs_lock_ready = true;
    }
}

static void hub_pause_ms(uint32_t ms)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(ms);
    while ((int64_t)(rdtsc() - deadline) < 0) {
        cpu_pause();
    }
}

static XhciHub* hub_for_slot(xhci_device_slot_t* slot)
{
    for (XhciHub* h = g_hubs; h; h = h->next) {
        if (h->slot == slot) {
            return h;
        }
    }
    return NULL;
}

/* ── class requests ─────────────────────────────────────────────────────── */

/* Asked more than once on purpose. A hub that has this instant been given its
 * configuration is entitled not to answer yet, and the first attempt failing is
 * not the same as the hub being broken — measured: QEMU's own hub refuses the
 * first request and answers the second. */
#define HUB_DESCRIBE_ATTEMPTS 4

static int hub_get_descriptor_once(XhciHub* h)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0xA0,          /* device to host, class, device */
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)(USB_DESC_HUB << 8),
        .wIndex = 0,
        .wLength = 9
    };

    memset(h->buf_virt, 0, 9);
    int code = xhci_control_transfer_sync(h->ctrl, h->slot, &setup,
                                          h->buf_phys, 9, true,
                                          HUB_CTRL_TIMEOUT_MS);
    if (code != TRB_COMPLETION_SUCCESS && code != TRB_COMPLETION_SHORT_PKT) {
        return -1;
    }

    const uint8_t* d = (const uint8_t*)h->buf_virt;
    if (d[1] != USB_DESC_HUB || d[2] == 0) {
        return -1;
    }

    h->ports = d[2];

    /* wHubCharacteristics: bits 6:5 are the think time in units of eight full
     * speed bit times, and the slot context wants exactly those two bits. */
    uint16_t characteristics = (uint16_t)(d[3] | ((uint16_t)d[4] << 8));
    h->slot->tt_think_time = (uint8_t)((characteristics >> 5) & 0x3);

    /* bPwrOn2PwrGood is in two-millisecond units, and is the hub telling us how
     * long after switching a port on its power is worth believing. */
    h->power_on_delay_ms = (uint8_t)(d[5] * 2);
    if (h->power_on_delay_ms < 20) {
        h->power_on_delay_ms = 20;
    }
    return 0;
}

static int hub_get_descriptor(XhciHub* h)
{
    for (unsigned attempt = 0; attempt < HUB_DESCRIBE_ATTEMPTS; attempt++) {
        if (hub_get_descriptor_once(h) == 0) {
            return 0;
        }
        hub_pause_ms(20);
    }
    return -1;
}

static int hub_port_feature(XhciHub* h, uint8_t port, uint8_t feature, bool set)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0x23,          /* host to device, class, other */
        .bRequest = set ? 0x03 /* SET_FEATURE */ : USB_REQ_CLEAR_FEATURE,
        .wValue = feature,
        .wIndex = port,
        .wLength = 0
    };
    int code = xhci_control_transfer_sync(h->ctrl, h->slot, &setup, 0, 0, false,
                                          HUB_CTRL_TIMEOUT_MS);
    return (code == TRB_COMPLETION_SUCCESS) ? 0 : -1;
}

static int hub_port_status(XhciHub* h, uint8_t port,
                           uint16_t* out_status, uint16_t* out_change)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0xA3,          /* device to host, class, other */
        .bRequest = 0x00,               /* GET_STATUS */
        .wValue = 0,
        .wIndex = port,
        .wLength = 4
    };

    memset(h->buf_virt, 0, 4);
    int code = xhci_control_transfer_sync(h->ctrl, h->slot, &setup,
                                          h->buf_phys, 4, true,
                                          HUB_CTRL_TIMEOUT_MS);
    if (code != TRB_COMPLETION_SUCCESS && code != TRB_COMPLETION_SHORT_PKT) {
        return -1;
    }

    const uint8_t* d = (const uint8_t*)h->buf_virt;
    if (out_status) *out_status = (uint16_t)(d[0] | ((uint16_t)d[1] << 8));
    if (out_change) *out_change = (uint16_t)(d[2] | ((uint16_t)d[3] << 8));
    return 0;
}

/* The speed the port reports, in the numbering the controller uses. A hub
 * states it as two flags, and neither of them set means full speed. */
static uint8_t hub_port_speed(uint16_t status)
{
    if (status & PORT_STAT_LOW_SPEED)  return XHCI_PORT_SPEED_LOW;
    if (status & PORT_STAT_HIGH_SPEED) return XHCI_PORT_SPEED_HIGH;
    return XHCI_PORT_SPEED_FULL;
}

/* ── one port ───────────────────────────────────────────────────────────── */

/*
 * Reset a port and enumerate what answers.
 *
 * The reset is driven by the hub, not by the controller, so this is where the
 * waiting happens — and it is a real wait on a real device, bounded and
 * reported rather than assumed. A port that resets and does not enable has
 * something attached that did not answer, and enumerating into that produces a
 * slot the controller will refuse to address.
 */
static int hub_bring_up_port(XhciHub* h, uint8_t port)
{
    uint16_t status = 0, change = 0;

    /* Let the connection settle before touching it. */
    hub_pause_ms(HUB_DEBOUNCE_MS);

    if (hub_port_status(h, port, &status, &change) != 0) {
        return -1;
    }
    if (!(status & PORT_STAT_CONNECTION)) {
        return -1;                      /* gone again already */
    }

    if (hub_port_feature(h, port, PORT_FEAT_RESET, true) != 0) {
        return -1;
    }

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(HUB_RESET_TIMEOUT_MS);
    for (;;) {
        hub_pause_ms(HUB_RESET_POLL_MS);

        if (hub_port_status(h, port, &status, &change) != 0) {
            return -1;
        }
        if (change & PORT_CHG_RESET) {
            break;
        }
        if ((int64_t)(rdtsc() - deadline) >= 0) {
            kprintf("[USB hub slot %u] port %u did not finish resetting\n",
                    h->slot->slot_id, port);
            return -1;
        }
    }

    hub_port_feature(h, port, PORT_FEAT_C_RESET, false);
    hub_pause_ms(HUB_RESET_RECOVERY_MS);

    if (hub_port_status(h, port, &status, &change) != 0) {
        return -1;
    }
    if (!(status & PORT_STAT_ENABLE)) {
        kprintf("[USB hub slot %u] port %u reset but did not enable\n",
                h->slot->slot_id, port);
        return -1;
    }

    uint8_t speed = hub_port_speed(status);
    kprintf("[USB hub slot %u] port %u: %s-speed device attached\n",
            h->slot->slot_id, port,
            speed == XHCI_PORT_SPEED_LOW  ? "low"  :
            speed == XHCI_PORT_SPEED_HIGH ? "high" : "full");

    return xhci_enumerate_behind_hub(h->ctrl, h->slot, port, speed);
}

/* Everything below a departed port goes with it. */
static void hub_port_gone(XhciHub* h, uint8_t port)
{
    kprintf("[USB hub slot %u] port %u: device removed\n",
            h->slot->slot_id, port);

    for (int guard = 0; guard < XHCI_MAX_DEVICE_SLOTS; guard++) {
        xhci_device_slot_t* child = NULL;
        /* Find a device whose parent is this hub port. Repeated rather than
         * recursive: releasing a child that is itself a hub releases its own
         * children, and the list is re-walked because it changed underneath. */
        for (uint8_t id = 1; id < 255; id++) {
            xhci_device_slot_t* s = xhci_get_device_slot_by_id(id);
            if (s && s->parent_slot_id == h->slot->slot_id &&
                s->parent_port == port) {
                child = s;
                break;
            }
        }
        if (!child) {
            return;
        }
        xhci_device_slot_cleanup(h->ctrl, child);
    }
}

/*
 * Listen for the next thing the hub has to say.
 *
 * Armed here and nowhere else, and in particular never from the interrupt
 * handler: the hub reports for as long as a change is outstanding, and the
 * change is cleared by the scan below. Arming before the scan is asking it to
 * repeat itself, at interrupt rate, into a core that will then never get far
 * enough to answer.
 */
static void hub_arm_status(XhciHub* h)
{
    if (!h->slot->ep_interrupt_in || !h->slot->endpoints) {
        return;
    }
    xhci_endpoint_t* ep = &h->slot->endpoints[h->slot->ep_interrupt_in];
    if (ep->xfer_state == XHCI_XFER_IN_FLIGHT) {
        return;
    }
    ep->xfer_state = XHCI_XFER_IDLE;
    xhci_ep_submit(h->ctrl, h->slot, h->slot->ep_interrupt_in,
                   ep->buffer_phys, ep->max_packet);
}

static int hub_scan_ports(XhciHub* h, bool announce_only_changes)
{
    int acted = 0;

    for (uint8_t port = 1; port <= h->ports; port++) {
        uint16_t status = 0, change = 0;
        if (hub_port_status(h, port, &status, &change) != 0) {
            continue;
        }

        if (change & PORT_CHG_CONNECTION) {
            hub_port_feature(h, port, PORT_FEAT_C_CONNECTION, false);
        }
        if (change & PORT_CHG_ENABLE) {
            hub_port_feature(h, port, PORT_FEAT_C_ENABLE, false);
        }

        bool connected = (status & PORT_STAT_CONNECTION) != 0;
        bool known     = (h->occupied & (1u << port)) != 0;

        if (announce_only_changes && connected == known &&
            !(change & PORT_CHG_CONNECTION)) {
            continue;
        }

        if (connected && !known) {
            if (hub_bring_up_port(h, port) == 0) {
                h->occupied |= (1u << port);
            }
            acted++;
        } else if (!connected && known) {
            hub_port_gone(h, port);
            h->occupied &= ~(1u << port);
            acted++;
        }
    }

    return acted;
}

/* ── attach and release ─────────────────────────────────────────────────── */

bool xhci_hub_slot_attached(xhci_device_slot_t* slot)
{
    return hub_for_slot(slot) != NULL;
}

int xhci_hub_attach(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    if (!ctrl || !slot || !slot->endpoints) {
        return -1;
    }

    hubs_lock_init();

    XhciHub* h = (XhciHub*)kmalloc(sizeof(XhciHub));
    if (!h) {
        return -1;
    }
    memset(h, 0, sizeof(*h));
    h->ctrl = ctrl;
    h->slot = slot;

    void* page = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    if (!page) {
        kfree(h);
        return -1;
    }
    h->buf_phys = (uint64_t)page;
    h->buf_virt = vmm_phys_to_virt((uintptr_t)page);

    spin_lock(&g_hubs_lock);
    h->next = g_hubs;
    g_hubs  = h;
    spin_unlock(&g_hubs_lock);

    if (hub_get_descriptor(h) != 0) {
        kprintf("[USB hub slot %u] would not describe itself\n", slot->slot_id);
        xhci_hub_release(ctrl, slot);
        return -1;
    }

    /* The controller has to be told this device is a hub before it can address
     * anything below it: a slot context without the Hub bit is scheduled as an
     * endpoint device, and the ports underneath do not exist as far as it is
     * concerned. The context is rewritten and re-evaluated. */
    slot->hub_ports = h->ports;
    slot->multi_tt  = (slot->interface_protocol == 2);

    uint32_t pages = xhci_input_ctx_pages(ctrl);
    void* input_phys = pmm_alloc_zero(pages, PHYS_TAG_DMA32);
    if (!input_phys) {
        xhci_hub_release(ctrl, slot);
        return -1;
    }
    uint8_t* base = (uint8_t*)vmm_phys_to_virt((uintptr_t)input_phys);
    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)base;
    icc->add_context_flags = (1u << 0);          /* the slot context alone */

    xhci_slot_context_t* sctx =
        (xhci_slot_context_t*)(base + ctrl->context_size);
    xhci_fill_slot_context(sctx, slot);
    sctx->dwords[0] = (sctx->dwords[0] & ~(0x1Fu << 27)) |
                      ((uint32_t)slot->max_dci << 27);

    /* Configure Endpoint, not Evaluate Context.
     *
     * Evaluate Context updates exactly two fields of a slot context — the max
     * exit latency and the interrupter target — and silently ignores the rest.
     * The Hub bit, the port count and the think time are not among them, so a
     * hub configured that way is one the controller still believes is an
     * ordinary device: measured, the slot context came back with Hub clear and
     * Number of Ports zero. Configure Endpoint applies the whole input slot
     * context, and adding no endpoints with it is legal and is exactly what is
     * wanted here. */
    if (xhci_post_configure_endpoint_cmd(ctrl, slot->slot_id,
                                         (uint64_t)input_phys) < 0 ||
        xhci_command_wait_idle(ctrl, HUB_CTRL_TIMEOUT_MS) != 0) {
        kprintf("[USB hub slot %u] the controller would not accept it as a "
                "hub\n", slot->slot_id);
        pmm_free(input_phys, pages);
        xhci_hub_release(ctrl, slot);
        return -1;
    }
    pmm_free(input_phys, pages);

    kprintf("[USB hub slot %u] %u port(s), %u ms to power\n",
            slot->slot_id, h->ports, h->power_on_delay_ms);

    /* Power every port, then wait once for all of them. */
    for (uint8_t port = 1; port <= h->ports; port++) {
        hub_port_feature(h, port, PORT_FEAT_POWER, true);
    }
    hub_pause_ms(h->power_on_delay_ms);

    h->attached = true;

    /* Whatever is already plugged into it. */
    hub_scan_ports(h, false);

    /* And from here on, the hub itself will say when something changes. */
    hub_arm_status(h);

    return 0;
}

void xhci_hub_release(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    if (!slot || !g_hubs_lock_ready) {
        return;
    }

    spin_lock(&g_hubs_lock);
    XhciHub** link = &g_hubs;
    XhciHub*  h    = NULL;
    while (*link) {
        if ((*link)->slot == slot) {
            h = *link;
            *link = h->next;
            break;
        }
        link = &(*link)->next;
    }
    spin_unlock(&g_hubs_lock);

    if (!h) {
        return;
    }

    /* Everything that was reached through this hub is now unreachable, whether
     * or not it is still physically there. */
    for (uint8_t port = 1; port <= h->ports; port++) {
        if (h->occupied & (1u << port)) {
            hub_port_gone(h, port);
        }
    }

    if (h->buf_phys) {
        pmm_free((void*)h->buf_phys, 1);
    }
    kfree(h);
    (void)ctrl;
}

/* ── the deferred half ──────────────────────────────────────────────────── */

/*
 * One flag, not a count.
 *
 * A count has to be decremented exactly as many times as it was incremented,
 * and the two happen in different places for different reasons — a hub bound
 * but unattached, a hub reporting a change. Get that wrong in the direction
 * that leaves it standing and the idle loop calls the service on every
 * iteration for the rest of the boot, finding nothing, forever.
 *
 * The flag is lowered before the work is looked for, never after: anything
 * raised while the pass is running raises it again, and a spurious extra pass
 * costs one walk of a short list.
 */
void xhci_hub_note_change(xhci_device_slot_t* slot)
{
    XhciHub* h = hub_for_slot(slot);
    if (!h) {
        return;
    }
    h->change_pending = true;
    __atomic_store_n(&g_hub_work, 1, __ATOMIC_RELEASE);
}

void xhci_hub_note_work(void)
{
    __atomic_store_n(&g_hub_work, 1, __ATOMIC_RELEASE);
}

bool xhci_hub_work_pending(void)
{
    return __atomic_load_n(&g_hub_work, __ATOMIC_ACQUIRE) != 0;
}

int xhci_hub_service(xhci_controller_t* ctrl)
{
    if (!ctrl) {
        return 0;
    }

    if (__atomic_exchange_n(&g_hub_busy, 1, __ATOMIC_ACQUIRE) != 0) {
        return 0;                       /* somebody is already doing this */
    }

    __atomic_store_n(&g_hub_work, 0, __ATOMIC_RELEASE);

    int acted = 0;

    /* Hubs the enumerator bound but nobody has spoken to yet. Attaching one
     * cannot happen where it was bound: that was the event handler, and every
     * step of this is a control transfer somebody has to wait for. */
    for (;;) {
        xhci_device_slot_t* pending = NULL;
        for (uint8_t id = 1; id < 255; id++) {
            xhci_device_slot_t* s = xhci_get_device_slot_by_id(id);
            if (s && s->driver == XHCI_DRIVER_HUB && !xhci_hub_slot_attached(s)) {
                pending = s;
                break;
            }
        }
        if (!pending) {
            break;
        }

        if (xhci_hub_attach(ctrl, pending) != 0) {
            /* A hub that will not come up is not a hub to keep trying: this
             * loop looks for the same slot every time round, and a failure
             * that leaves the slot claiming to be an unattached hub is a loop
             * with no way out of it. It stays enumerated and addressed, and it
             * stops being a hub as far as anything here is concerned. */
            kprintf("[xHCI] slot %u will not act as a hub — leaving it alone\n",
                    pending->slot_id);
            pending->driver = XHCI_DRIVER_NONE;
        }
        acted++;
    }

    for (XhciHub* h = g_hubs; h; h = h->next) {
        if (!h->attached || !h->change_pending) {
            continue;
        }
        h->change_pending = false;
        acted += hub_scan_ports(h, true);
        hub_arm_status(h);
    }

    __atomic_store_n(&g_hub_busy, 0, __ATOMIC_RELEASE);
    return acted;
}

void xhci_hub_service_if_pending(void)
{
    if (!xhci_hub_work_pending()) {
        return;
    }
    xhci_controller_t* ctrl = xhci_get_controller();
    if (ctrl) {
        xhci_hub_service(ctrl);
    }
}
