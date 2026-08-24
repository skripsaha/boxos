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

/* ── the hub class, on the wire ─────────────────────────────────────────────
 *
 * There are two dialects of it, and a hub states which one it speaks in its own
 * device descriptor rather than by how fast it is: USB 2.0 §11.23.1 and USB 3.2
 * Table 10-7 share that one field. Asking the port how fast it trained would be
 * the wrong question — a SuperSpeed hub and the USB 2.0 hub inside the same
 * plastic box are two separate devices with separate ports, and each of them
 * answers for itself.
 */
#define HUB_PROTOCOL_FULL_SPEED  0
#define HUB_PROTOCOL_SINGLE_TT   1
#define HUB_PROTOCOL_MULTI_TT    2
#define HUB_PROTOCOL_SUPERSPEED  3

#define USB_DESC_HUB             0x29
#define USB_DESC_HUB_SS          0x2A
#define HUB_DESC_LEN             9
#define HUB_DESC_LEN_SS          12

#define HUB_REQ_GET_STATUS       0x00
#define HUB_REQ_SET_FEATURE      0x03
#define HUB_REQ_SET_DEPTH        0x0C
#define USB_REQ_SET_INTERFACE    0x0B

/* Features of a downstream port, as SET_FEATURE and CLEAR_FEATURE name them.
 * The numbering is shared; which of them exist is not. */
#define PORT_FEAT_ENABLE         1
#define PORT_FEAT_RESET          4
#define PORT_FEAT_POWER          8
#define PORT_FEAT_C_CONNECTION   16
#define PORT_FEAT_C_ENABLE       17     /* USB 2.0 only */
#define PORT_FEAT_C_OVERCURRENT  19
#define PORT_FEAT_C_RESET        20
#define PORT_FEAT_C_LINK_STATE   25     /* SuperSpeed only */
#define PORT_FEAT_C_CONFIG_ERROR 26     /* SuperSpeed only */
#define PORT_FEAT_BH_RESET       28     /* SuperSpeed only */
#define PORT_FEAT_C_BH_RESET     29     /* SuperSpeed only */

/* wPortStatus. The first four bits mean the same thing in both dialects; above
 * them the layouts part company, because a SuperSpeed port has a link state to
 * report and had to find sixteen values' worth of room for it. */
#define PORT_STAT_CONNECTION     (1u << 0)
#define PORT_STAT_ENABLE         (1u << 1)
#define PORT_STAT_OVERCURRENT    (1u << 3)
#define PORT_STAT_RESET          (1u << 4)

#define PORT_STAT_POWER          (1u << 8)      /* USB 2.0 */
#define PORT_STAT_LOW_SPEED      (1u << 9)
#define PORT_STAT_HIGH_SPEED     (1u << 10)

#define SS_PORT_STAT_LINK_STATE  (0xFu << 5)    /* SuperSpeed */
#define SS_PORT_STAT_POWER       (1u << 9)
#define SS_PORT_STAT_SPEED       (0x7u << 10)

/* wPortChange. Bit 5 is C_PORT_L1 to a USB 2.0 hub and C_BH_PORT_RESET to a
 * SuperSpeed one — the same bit, two meanings, and the wrong CLEAR_FEATURE for
 * it is a request the hub is entitled to refuse. */
#define PORT_CHG_CONNECTION      (1u << 0)
#define PORT_CHG_ENABLE          (1u << 1)      /* USB 2.0 only */
#define PORT_CHG_OVERCURRENT     (1u << 3)
#define PORT_CHG_RESET           (1u << 4)
#define SS_PORT_CHG_BH_RESET     (1u << 5)      /* SuperSpeed only */
#define SS_PORT_CHG_LINK_STATE   (1u << 6)
#define SS_PORT_CHG_CONFIG_ERR   (1u << 7)

/* Link states worth naming. A link that came out of a reset in one of the last
 * two did not fail to reset — it failed to train, and that is a different
 * failure with a different answer. */
#define SS_LINK_U0               0
#define SS_LINK_SS_INACTIVE      6
#define SS_LINK_COMPLIANCE       10

/*
 * Fifteen ports, and the number is not this driver's choice.
 *
 * A device below a hub is reached by a route string that spends four bits on
 * each tier naming the port taken there, so port 16 has no way of being named.
 * The old code clamped a larger port number to 15 — which does not address port
 * 16, it addresses port 15, and quietly hands whatever is plugged into that one
 * the transfers meant for its neighbour.
 */
#define HUB_MAX_ROUTABLE_PORTS   15

#define HUB_CTRL_TIMEOUT_MS      1000

/* USB 2.0 §7.1.7.5: a port reset is driven for at least 10 ms and the device
 * is allowed 10 ms more to recover. The hub reports when it is done, and this
 * is only the outside edge of waiting for that. */
#define HUB_RESET_TIMEOUT_MS     800
#define HUB_RESET_POLL_MS        10
#define HUB_RESET_RECOVERY_MS    20

/* A newly connected device must be given time to settle before it is reset —
 * §7.1.7.3 calls it debounce and asks for 100 ms. */
#define HUB_DEBOUNCE_MS          100

typedef struct XhciHub {
    struct XhciHub* next;
    xhci_controller_t*  ctrl;
    xhci_device_slot_t* slot;

    uint8_t  ports;
    bool     superspeed;
    bool     attached;

    /* bPwrOn2PwrGood counts two-millisecond units in a byte, so a hub is
     * entitled to ask for half a second. Keeping the answer in a byte turned
     * the 300 ms a slow hub asks for into 44. */
    uint16_t power_on_delay_ms;

    /* Raised by the status-change endpoint from interrupt context, lowered by
     * the service pass that acts on it. */
    volatile bool change_pending;

    /*
     * Two facts about a port, and they are not the same fact.
     *
     * `present` is a connection this driver has already answered — whether the
     * answer was a working device or a failure. `occupied` is a device that is
     * actually there. Keeping only the second one meant a port that would not
     * come up was never marked as dealt with, so the next status report found
     * it connected and unknown all over again; the hub goes on reporting until
     * the change is resolved, and the change is resolved by bringing the port
     * up. Measured nowhere, because nothing in QEMU fails to reset — on a desk
     * with a bad cable it is a loop with no way out.
     */
    uint32_t present;
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

static const char* ss_link_name(uint16_t status)
{
    switch ((status & SS_PORT_STAT_LINK_STATE) >> 5) {
        case 0:  return "U0";
        case 1:  return "U1";
        case 2:  return "U2";
        case 3:  return "U3 (suspended)";
        case 4:  return "SS.Disabled";
        case 5:  return "Rx.Detect";
        case 6:  return "SS.Inactive";
        case 7:  return "Polling";
        case 8:  return "Recovery";
        case 9:  return "Hot Reset";
        case 10: return "Compliance Mode";
        case 11: return "Loopback";
        default: return "a state the specification has not given a name";
    }
}

/* A SuperSpeed link that is inactive or stuck in compliance mode is one the
 * ordinary reset cannot rescue: it has to be taken all the way down and
 * retrained, which is what the hub calls a warm reset. */
static bool ss_link_usable(uint16_t status)
{
    uint16_t state = (status & SS_PORT_STAT_LINK_STATE) >> 5;
    return state != SS_LINK_SS_INACTIVE && state != SS_LINK_COMPLIANCE;
}

/* ── class requests ─────────────────────────────────────────────────────── */

/* Asked more than once on purpose. A hub that has this instant been given its
 * configuration is entitled not to answer yet, and the first attempt failing is
 * not the same as the hub being broken — measured: QEMU's own hub refuses the
 * first request and answers the second. */
#define HUB_DESCRIBE_ATTEMPTS 4

static int hub_get_descriptor_once(XhciHub* h)
{
    uint8_t  type = h->superspeed ? USB_DESC_HUB_SS : USB_DESC_HUB;
    uint16_t want = h->superspeed ? HUB_DESC_LEN_SS : HUB_DESC_LEN;

    usb_setup_packet_t setup = {
        .bmRequestType = 0xA0,          /* device to host, class, device */
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)(type << 8),
        .wIndex = 0,
        .wLength = want
    };

    memset(h->buf_virt, 0, want);
    int code = xhci_control_transfer_sync(h->ctrl, h->slot, &setup,
                                          h->buf_phys, want, true,
                                          HUB_CTRL_TIMEOUT_MS);
    if (code != TRB_COMPLETION_SUCCESS && code != TRB_COMPLETION_SHORT_PKT) {
        return -1;
    }

    const uint8_t* d = (const uint8_t*)h->buf_virt;
    if (d[1] != type || d[2] == 0) {
        return -1;
    }

    h->ports = d[2];
    if (h->ports > HUB_MAX_ROUTABLE_PORTS) {
        kprintf("[USB hub slot %u] says it has %u ports; the route string has "
                "room to name %u, so the rest cannot be reached and are left "
                "alone\n",
                h->slot->slot_id, h->ports, HUB_MAX_ROUTABLE_PORTS);
        h->ports = HUB_MAX_ROUTABLE_PORTS;
    }

    /* wHubCharacteristics: bits 6:5 are the think time in units of eight full
     * speed bit times, and the slot context wants exactly those two bits. They
     * are reserved in a SuperSpeed hub's characteristics — a transaction
     * translator is a thing only a high-speed hub has, because only it has
     * something slower on the other side to translate for. */
    uint16_t characteristics = (uint16_t)(d[3] | ((uint16_t)d[4] << 8));
    h->slot->tt_think_time = h->superspeed
                           ? 0
                           : (uint8_t)((characteristics >> 5) & 0x3);

    /* bPwrOn2PwrGood is in two-millisecond units, and is the hub telling us how
     * long after switching a port on its power is worth believing. */
    h->power_on_delay_ms = (uint16_t)((uint16_t)d[5] * 2u);
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

/*
 * Tell a SuperSpeed hub how deep it is.
 *
 * The route string names a port per tier, and a hub reading one has to know
 * which tier is its own before it can tell which four bits are addressed to it.
 * A USB 2.0 hub never asks, because it does not route: it repeats everything
 * downstream and lets the devices sort it out. A SuperSpeed hub does route, and
 * one that has not been told its depth routes by the wrong nibble — which is
 * not a failure it reports, it is transfers arriving at the wrong port.
 */
static int hub_set_depth(XhciHub* h)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0x20,          /* host to device, class, device */
        .bRequest = HUB_REQ_SET_DEPTH,
        .wValue = h->slot->depth,
        .wIndex = 0,
        .wLength = 0
    };
    int code = xhci_control_transfer_sync(h->ctrl, h->slot, &setup, 0, 0, false,
                                          HUB_CTRL_TIMEOUT_MS);
    if (code != TRB_COMPLETION_SUCCESS) {
        kprintf("[USB hub slot %u] would not accept a depth of %u — it cannot "
                "route, so nothing below it would be reachable\n",
                h->slot->slot_id, h->slot->depth);
        return -1;
    }
    return 0;
}

/*
 * Put a high-speed hub on its second alternate setting, which is where its
 * extra transaction translators are.
 *
 * A hub capable of one translator per port starts with one for all of them, and
 * says which it is doing by which alternate setting it is on — not by what its
 * descriptor is capable of. The old code read the capability out of the
 * interface descriptor and told the controller about it, which is the worse of
 * the two mistakes available here: the controller would then schedule split
 * transactions per port to hardware with a single queue behind all of them.
 */
static void hub_select_multi_tt(XhciHub* h)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0x01,          /* host to device, standard, interface */
        .bRequest = USB_REQ_SET_INTERFACE,
        .wValue = 1,                    /* alternate setting 1 = multiple TTs */
        .wIndex = h->slot->interface_num,
        .wLength = 0
    };
    int code = xhci_control_transfer_sync(h->ctrl, h->slot, &setup, 0, 0, false,
                                          HUB_CTRL_TIMEOUT_MS);

    h->slot->multi_tt = (code == TRB_COMPLETION_SUCCESS);
    if (!h->slot->multi_tt) {
        kprintf("[USB hub slot %u] would not switch to one translator per port "
                "— using the single one it starts with\n", h->slot->slot_id);
    }
}

static int hub_port_feature(XhciHub* h, uint8_t port, uint8_t feature, bool set)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0x23,          /* host to device, class, other */
        .bRequest = set ? HUB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE,
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
        .bRequest = HUB_REQ_GET_STATUS,
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

/*
 * Acknowledge everything the port has to say.
 *
 * A change bit nobody clears is a hub that never stops talking: the status
 * change endpoint reports for as long as anything is outstanding, and the only
 * thing that ends it is clearing the bit that caused it. Over-current was not
 * being cleared at all, and neither was the reset change on any of the paths
 * that give up on a port — both of which are conditions QEMU never produces
 * and a desk with a bad cable produces immediately.
 */
static void hub_clear_changes(XhciHub* h, uint8_t port, uint16_t change)
{
    if (change & PORT_CHG_CONNECTION) {
        hub_port_feature(h, port, PORT_FEAT_C_CONNECTION, false);
    }
    if (change & PORT_CHG_OVERCURRENT) {
        hub_port_feature(h, port, PORT_FEAT_C_OVERCURRENT, false);
    }
    if (change & PORT_CHG_RESET) {
        hub_port_feature(h, port, PORT_FEAT_C_RESET, false);
    }

    if (h->superspeed) {
        if (change & SS_PORT_CHG_BH_RESET) {
            hub_port_feature(h, port, PORT_FEAT_C_BH_RESET, false);
        }
        if (change & SS_PORT_CHG_LINK_STATE) {
            hub_port_feature(h, port, PORT_FEAT_C_LINK_STATE, false);
        }
        if (change & SS_PORT_CHG_CONFIG_ERR) {
            hub_port_feature(h, port, PORT_FEAT_C_CONFIG_ERROR, false);
        }
    } else if (change & PORT_CHG_ENABLE) {
        hub_port_feature(h, port, PORT_FEAT_C_ENABLE, false);
    }
}

/* The speed the port reports, in the numbering the controller uses. */
static uint8_t hub_port_speed(const XhciHub* h, uint16_t status)
{
    if (h->superspeed) {
        /* Everything below a SuperSpeed hub is SuperSpeed — the USB 2.0 half of
         * the same plastic box is a separate device with its own ports, and a
         * slower device plugs into that one. Which SuperSpeed is the remaining
         * question, and it is not answered here: a Gen 1 hub leaves the speed
         * field zero because in its world there is one answer, and a Gen 2 hub
         * states the lane speeds in an extended port status this driver does
         * not read. Saying SuperSpeed is right for the first and understates
         * the second, which costs scheduling headroom and never correctness. */
        return XHCI_PORT_SPEED_SUPER;
    }
    if (status & PORT_STAT_LOW_SPEED)  return XHCI_PORT_SPEED_LOW;
    if (status & PORT_STAT_HIGH_SPEED) return XHCI_PORT_SPEED_HIGH;
    return XHCI_PORT_SPEED_FULL;       /* neither flag set means full speed */
}

/* ── one port ───────────────────────────────────────────────────────────── */

/*
 * Drive one reset and wait for the hub to say it finished.
 *
 * The reset is driven by the hub, not by the controller, so this is where the
 * waiting happens — a real wait on a real device, bounded and reported rather
 * than assumed. Both reset-change bits are cleared on the way out whatever
 * happened, including on the paths that give up: leaving one standing is
 * leaving the hub with something to report that nothing will ever answer.
 */
static int hub_reset_port(XhciHub* h, uint8_t port, bool warm,
                          uint16_t* out_status)
{
    uint8_t  feature = warm ? PORT_FEAT_BH_RESET : PORT_FEAT_RESET;
    uint16_t finished = warm ? SS_PORT_CHG_BH_RESET : PORT_CHG_RESET;

    /* A SuperSpeed hub may answer either way round: it is entitled to escalate
     * a reset it was asked for into the deeper one on its own. */
    if (h->superspeed) {
        finished = PORT_CHG_RESET | SS_PORT_CHG_BH_RESET;
    }

    if (hub_port_feature(h, port, feature, true) != 0) {
        return -1;
    }

    uint16_t status = 0, change = 0;
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(HUB_RESET_TIMEOUT_MS);

    for (;;) {
        hub_pause_ms(HUB_RESET_POLL_MS);

        if (hub_port_status(h, port, &status, &change) != 0) {
            return -1;
        }
        if (change & finished) {
            break;
        }
        if ((int64_t)(rdtsc() - deadline) >= 0) {
            kprintf("[USB hub slot %u] port %u did not finish %s resetting\n",
                    h->slot->slot_id, port, warm ? "warm" : "");
            hub_clear_changes(h, port, change);
            return -1;
        }
    }

    hub_clear_changes(h, port, change);
    hub_pause_ms(HUB_RESET_RECOVERY_MS);

    if (hub_port_status(h, port, &status, &change) != 0) {
        return -1;
    }
    hub_clear_changes(h, port, change);

    if (out_status) {
        *out_status = status;
    }
    return 0;
}

/*
 * Reset a port and enumerate what answers.
 *
 * A port that resets and does not enable has something attached that did not
 * answer, and enumerating into that produces a slot the controller will refuse
 * to address.
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

    if (hub_reset_port(h, port, false, &status) != 0) {
        return -1;
    }

    /* A link that came out of the reset inactive or stuck in compliance mode
     * has not failed to reset — it has failed to train, and the answer to that
     * is the other reset: the one that takes the link down and brings it back
     * up from nothing. */
    if (h->superspeed && !ss_link_usable(status)) {
        kprintf("[USB hub slot %u] port %u came out of the reset in %s — "
                "warm resetting it\n",
                h->slot->slot_id, port, ss_link_name(status));
        if (hub_reset_port(h, port, true, &status) != 0) {
            return -1;
        }
    }

    if (!(status & PORT_STAT_ENABLE)) {
        kprintf("[USB hub slot %u] port %u reset but did not enable\n",
                h->slot->slot_id, port);
        return -1;
    }

    uint8_t speed = hub_port_speed(h, status);
    kprintf("[USB hub slot %u] port %u: %s-speed device attached\n",
            h->slot->slot_id, port,
            speed == XHCI_PORT_SPEED_LOW   ? "low"   :
            speed == XHCI_PORT_SPEED_HIGH  ? "high"  :
            speed == XHCI_PORT_SPEED_SUPER ? "super" : "full");

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

/* Forget whatever this port was holding. */
static void hub_port_forget(XhciHub* h, uint8_t port)
{
    if (h->occupied & (1u << port)) {
        hub_port_gone(h, port);
    }
    h->present  &= ~(1u << port);
    h->occupied &= ~(1u << port);
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

        hub_clear_changes(h, port, change);

        if (status & PORT_STAT_OVERCURRENT) {
            /* The hub has already cut power to it, and will not restore it
             * while the condition lasts. There is nothing to bring up. */
            kprintf("[USB hub slot %u] port %u draws more current than the hub "
                    "will supply — it has switched the port off\n",
                    h->slot->slot_id, port);
            hub_port_forget(h, port);
            acted++;
            continue;
        }

        bool connected = (status & PORT_STAT_CONNECTION) != 0;
        bool answered  = (h->present & (1u << port)) != 0;

        /* A connection that changed and is present again need not be the same
         * connection. Whatever was being held for this port belongs to a device
         * that has already gone, whether or not anything noticed it leave. */
        if ((change & PORT_CHG_CONNECTION) && answered) {
            hub_port_forget(h, port);
            answered = false;
            acted++;
        }

        if (announce_only_changes && connected == answered &&
            !(change & PORT_CHG_CONNECTION)) {
            continue;
        }

        if (connected && !answered) {
            /* Marked as answered before the attempt, not after it: a port that
             * will not come up must not be tried again until the connection
             * itself changes. */
            h->present |= (1u << port);
            if (hub_bring_up_port(h, port) == 0) {
                h->occupied |= (1u << port);
            }
            acted++;
        } else if (!connected && answered) {
            hub_port_forget(h, port);
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

    /* Which dialect, asked of the hub rather than of the port it is on. */
    uint8_t protocol = slot->device_desc.bDeviceProtocol;
    h->superspeed = (protocol == HUB_PROTOCOL_SUPERSPEED);

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

    /* Depth before anything below it is addressed: a SuperSpeed hub routes by
     * the four bits of the route string that belong to its own tier, and does
     * not know which four those are until it is told. */
    if (h->superspeed && hub_set_depth(h) != 0) {
        xhci_hub_release(ctrl, slot);
        return -1;
    }

    /* One translator per port is a mode to be entered, not a capability to be
     * reported. A hub that is not asked stays on the one it starts with. */
    slot->multi_tt = false;
    if (protocol == HUB_PROTOCOL_MULTI_TT) {
        hub_select_multi_tt(h);
    }

    /* The controller has to be told this device is a hub before it can address
     * anything below it: a slot context without the Hub bit is scheduled as an
     * endpoint device, and the ports underneath do not exist as far as it is
     * concerned. The context is rewritten and re-evaluated. */
    slot->hub_ports = h->ports;

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

    kprintf("[USB hub slot %u] %s: %u port(s), %u ms to power%s\n",
            slot->slot_id,
            h->superspeed        ? "SuperSpeed hub" :
            protocol == HUB_PROTOCOL_FULL_SPEED ? "full-speed hub"
                                                : "high-speed hub",
            h->ports, h->power_on_delay_ms,
            slot->multi_tt ? ", one translator per port" : "");

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
