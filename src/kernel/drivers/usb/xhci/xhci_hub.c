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

/* Features of the HUB itself, which are a different list from a port's and are
 * cleared with a different request. USB 2.0 §11.24.2.1: ClearHubFeature "is
 * used to clear either the C_HUB_LOCAL_POWER or C_HUB_OVER_CURRENT features",
 * and those two are all there are. */
#define HUB_FEAT_C_LOCAL_POWER   0
#define HUB_FEAT_C_OVER_CURRENT  1

/* wHubStatus and wHubChange, USB 2.0 Tables 11-19 and 11-20. Bit 0 of the
 * status is the local supply — 1 means LOST, which is the way round it reads
 * on the wire — and bit 1 is an over-current the hub reports on its own
 * behalf rather than on any port's. */
#define HUB_STAT_LOCAL_POWER_LOST (1u << 0)
#define HUB_STAT_OVERCURRENT      (1u << 1)
#define HUB_CHG_LOCAL_POWER       (1u << 0)
#define HUB_CHG_OVERCURRENT       (1u << 1)

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
 * Thirty-one ports, and the number is this driver's own arrays rather than
 * anything the bus says.
 *
 * ‼ THE BLOCK THAT USED TO STAND HERE SAID FIFTEEN, AND GAVE A REASON THAT IS
 * IN NO SPECIFICATION: "a device below a hub is reached by a route string that
 * spends four bits on each tier naming the port taken there, so port 16 has no
 * way of being named". xHCI 1.2 Table 6-4, Route String, footnote 106 says the
 * opposite for the hubs this concerns: "If HS or FS hub in the path supports
 * more than 14 ports the associated Route String Port field shall be set to
 * 15." A USB 2.0 hub does not route by the route string at all — it repeats
 * downstream and the device address picks the device out — so the nibble is a
 * placeholder for such a tier, and the port above it is perfectly reachable.
 * bNbrPorts is an eight-bit field (USB 2.0 Table 11-13) with no fifteen-port
 * limit on it; the fifteen-port cap belongs to USB 3.x SuperSpeed hubs alone,
 * and one of those cannot exceed it in the first place.
 *
 * Clamping on that false reason cost the ports themselves. Everything below
 * loops to h->ports, so ports above the clamp were never powered, never
 * polled, never reset and never named in a single line of the log: a device in
 * one of them was invisible for the life of the machine.
 *
 * What genuinely bounds this driver is two bitmaps of its own — `present` and
 * `occupied` below are uint32_t addressed as `1u << port`, which is undefined
 * from port 32 upwards. So the bound is thirty-one, it is named for what it
 * actually is, and it is the same number and the same reason as Linux's
 * USB_MAXCHILDREN ("Because the arrays need to add a bit for hub status data,
 * we use 31"). What the hub SAID is kept separately and is what the controller
 * is told, because xHCI 1.2 §4.5.2 and Table 6-5 want Number of Ports to be
 * bNbrPorts and not a number this driver preferred.
 */
#define HUB_MAX_TRACKED_PORTS    31

#define HUB_CTRL_TIMEOUT_MS      1000

/* USB 2.0 §7.1.7.5: a port reset is driven for at least 10 ms and the device
 * is allowed 10 ms more to recover. The hub REPORTS when it is done — that is
 * the fact this waits on, in C_PORT_RESET — and a device taken out of the
 * socket mid-reset is a second fact, in wPortStatus. This number is only the
 * outside edge behind both of them, for a hub that answers neither. */
#define HUB_RESET_TIMEOUT_MS     800
#define HUB_RESET_POLL_MS        10
#define HUB_RESET_RECOVERY_MS    20

/* A newly connected device must be given time to settle before it is reset —
 * §7.1.7.3 calls it debounce and asks for 100 ms. */
#define HUB_DEBOUNCE_MS          100

typedef struct XhciHub {
    struct XhciHub* next;
    /* The queue of hubs that have left g_hubs and are waiting to be freed. A
     * chain of its own, so that `next` keeps pointing where it did and a
     * service walk standing on a node when it was unlinked carries on over
     * the live list rather than into the queue. */
    struct XhciHub* released_next;
    xhci_controller_t*  ctrl;
    xhci_device_slot_t* slot;

    /* `ports` is how many of them this driver scans and keeps bits for;
     * `ports_reported` is bNbrPorts exactly as the hub descriptor gave it,
     * which is what the Slot Context's Number of Ports field is owed. They
     * differ only on a hub with more ports than HUB_MAX_TRACKED_PORTS. */
    uint8_t  ports;
    uint8_t  ports_reported;
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
 * ‼ A HUB IS UNLINKED WHERE IT IS RELEASED AND FREED SOMEWHERE ELSE, AND THAT
 * IS THE WHOLE OF IT.
 *
 * g_hubs is inserted and unlinked under g_hubs_lock, and its nodes used to be
 * kfree()d the instant after the unlink, outside it. Two readers walk the list
 * and neither took the lock:
 *
 *   hub_for_slot, reached from the transfer-event handler on the BSP. It
 *   stands on a node while another core unlinks and frees it, reads h->next
 *   out of the freed object — sizeof(XhciHub) puts it in the 64-byte slab
 *   class, whose free path writes a uint16_t head offset over its first two
 *   bytes, so only the low half of that pointer is corrupted and the walk
 *   usually carries on into unrelated memory rather than faulting — compares
 *   h->slot against arbitrary bytes, and on a match writes change_pending into
 *   something else's object.
 *
 *   xhci_hub_service, which is far worse: it holds `h` across hub_scan_ports,
 *   which is per port a GET_STATUS and a CLEAR_FEATURE at HUB_CTRL_TIMEOUT_MS
 *   apiece, a HUB_DEBOUNCE_MS pause and up to HUB_RESET_TIMEOUT_MS of reset
 *   polling, and only then reads h->next. That is hundreds of milliseconds
 *   with a pointer held live, and the node is reachable: hub_scan_ports ->
 *   hub_port_forget -> hub_port_gone -> xhci_slot_retire retires a child that
 *   may itself be a hub, and another core's xhci_slot_service then takes it
 *   down and releases it. It needs a second core, because MSI is targeted at
 *   the BSP alone — so this is a 4c/16c and live-board defect, not a
 *   single-core one.
 *
 * Locking the long walk is not the answer: g_hubs_lock masks interrupts, and
 * holding it across seconds of control transfers would stop the machine. So
 * the LIST is locked and the FREEING is moved. Every walk of g_hubs is now
 * under g_hubs_lock or is the service pass itself; xhci_hub_release unlinks
 * under the lock and leaves the node here instead of freeing it; and the next
 * service pass frees the queue as its first act, under g_hub_busy, at the one
 * moment when no walker of any kind can be standing on a node that has left
 * the list. An unlink leaves h->next pointing where it did, so a service walk
 * that was standing on the node when it went carries on over the live ones.
 */
static XhciHub* g_hubs_released = NULL;

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

/* Caller holds g_hubs_lock. The list is unlinked under that lock and this is
 * reached from the transfer-event handler, so a walk that does not hold it is
 * a walk standing on a node another core is taking away — see the note on
 * g_hubs_released. */
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

    h->ports_reported = d[2];
    h->ports          = d[2];
    if (h->ports > HUB_MAX_TRACKED_PORTS) {
        kprintf("[USB hub slot %u] says it has %u ports; this driver keeps one "
                "bit per port and has room for %u, so the rest are left "
                "alone\n",
                h->slot->slot_id, h->ports, HUB_MAX_TRACKED_PORTS);
        h->ports = HUB_MAX_TRACKED_PORTS;
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
 * The hub's own status, which nothing here used to ask for.
 *
 * USB 2.0 §11.24.2.6: bmRequestType 10100000B, GET_STATUS, wValue and wIndex
 * zero, four bytes back — wHubStatus, then wHubChange. It is a different
 * request from hub_port_status above, which is class-to-OTHER with the port in
 * wIndex, and it answers a question no port can: §11.12.5 says that a hub with
 * over-current detection on a hub basis puts every port into Powered-off and
 * that "neither C_PORT_OVER_CURRENT nor PORT_OVER_CURRENT is set for the
 * affected ports". Pulling the brick out of a self-powered hub is the same
 * shape — the ports go dark and only this word says why.
 */
static int hub_status(XhciHub* h, uint16_t* out_status, uint16_t* out_change)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0xA0,          /* device to host, class, device */
        .bRequest = HUB_REQ_GET_STATUS,
        .wValue = 0,
        .wIndex = 0,
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
 * Acknowledge something the hub said about itself.
 *
 * USB 2.0 §11.24.2.1: bmRequestType 00100000B, CLEAR_FEATURE, the selector in
 * wValue, wIndex and wLength zero. Both selectors are changes, and §11.12.2 is
 * why leaving one standing is not a cosmetic omission: "While a change bit is
 * set, the hub continues to report a status change when polled until all
 * change bits have been cleared by the USB System Software."
 */
static int hub_clear_feature(XhciHub* h, uint8_t feature)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0x20,          /* host to device, class, device */
        .bRequest = USB_REQ_CLEAR_FEATURE,
        .wValue = feature,
        .wIndex = 0,
        .wLength = 0
    };
    int code = xhci_control_transfer_sync(h->ctrl, h->slot, &setup, 0, 0, false,
                                          HUB_CTRL_TIMEOUT_MS);
    return (code == TRB_COMPLETION_SUCCESS) ? 0 : -1;
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

        /*
         * ‼ THE HAND THAT TOOK IT OUT, ASKED BEFORE THE CLOCK.
         *
         * A reset is being driven at something that may not be there any more,
         * and the hub says so in the same word it says everything else in:
         * wPortStatus bit 0 is whether anything is connected. Waiting out the
         * rest of the budget for a socket that is empty is time the machine
         * spends on a device that has left, and it is time the port cannot be
         * given to whatever is plugged in next.
         */
        if (!(status & PORT_STAT_CONNECTION)) {
            kprintf("[USB hub slot %u] port %u has nothing attached to reset "
                    "any more\n", h->slot->slot_id, port);
            hub_clear_changes(h, port, change);
            return -1;
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

    /* A hub cannot have more descendants than the controller has slots, so
     * that is the bound — it used to be a machine-wide 64 that had nothing to
     * do with this controller. */
    uint32_t guard_max = h->ctrl ? h->ctrl->slot_count : 0u;
    for (uint32_t guard = 0; guard < guard_max; guard++) {
        xhci_device_slot_t* child = NULL;
        /* Find a device whose parent is this hub port. Repeated rather than
         * recursive: releasing a child that is itself a hub releases its own
         * children, and the list is re-walked because it changed underneath. */
        for (uint8_t id = 1; id < 255; id++) {
            xhci_device_slot_t* s = xhci_get_device_slot_by_id(h->ctrl, id);
            if (s && s->parent_slot_id == h->slot->slot_id &&
                s->parent_port == port) {
                child = s;
                break;
            }
        }
        if (!child) {
            return;
        }
        xhci_slot_retire(h->ctrl, child);
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
 * Switch power back on, which is the ONLY way a port leaves Powered-off.
 *
 * USB 2.0 §11.5.1.3 enumerates the transitions out of that state, and exactly
 * one of them is something software can cause: "From the Powered-off state
 * when the hub receives a SetPortFeature(PORT_POWER) request". §11.5.1.2 is
 * why nothing else will ever do it for us — in Powered-off "the port's
 * differential and single-ended transmitters and receivers are disabled", so
 * the port cannot detect a connection and can never become interesting again
 * on its own.
 *
 * The delay afterwards is the hub's own number, bPwrOn2PwrGood, and it is the
 * only clock here. Whether the power took is then ASKED rather than assumed:
 * a port still over its limit is left off and said out loud, because
 * re-powering into a short over and over is not a repair.
 */
static int hub_repower_port(XhciHub* h, uint8_t port, uint16_t* out_status)
{
    if (hub_port_feature(h, port, PORT_FEAT_POWER, true) != 0) {
        kprintf("[USB hub slot %u] port %u would not take power again\n",
                h->slot->slot_id, port);
        return -1;
    }
    hub_pause_ms(h->power_on_delay_ms);

    uint16_t status = 0, change = 0;
    if (hub_port_status(h, port, &status, &change) != 0) {
        return -1;
    }
    hub_clear_changes(h, port, change);

    if (status & PORT_STAT_OVERCURRENT) {
        kprintf("[USB hub slot %u] port %u is still over its current limit — "
                "it stays off until whatever is drawing it is taken out\n",
                h->slot->slot_id, port);
        return -1;
    }

    if (out_status) {
        *out_status = status;
    }
    return 0;
}

/* Every port at once, and one wait for all of them. Used when the hub itself
 * has been the thing that went down: §11.12.5 step 4 asks the host to cycle
 * "power on to all of the necessary ports", and a hub-basis event names none
 * of them, so that is every port it has. */
static void hub_power_all_ports(XhciHub* h)
{
    for (uint8_t port = 1; port <= h->ports; port++) {
        hub_port_feature(h, port, PORT_FEAT_POWER, true);
    }
    hub_pause_ms(h->power_on_delay_ms);
}

/*
 * Listen for the next thing the hub has to say.
 *
 * Armed here and nowhere else, and in particular never from the interrupt
 * handler: the hub reports for as long as a change is outstanding, and the
 * change is cleared by the scan below. Arming before the scan is asking it to
 * repeat itself, at interrupt rate, into a core that will then never get far
 * enough to answer.
 *
 * ‼ AND A PIPE THE DEVICE HALTED IS REPAIRED HERE, BEFORE IT IS RUNG FOR.
 *
 * USB 2.0 §11.12.1: the host learns of a port or hub change only through this
 * endpoint, and only while an IN is outstanding on it — "If no hub or port
 * status change bits are set, then the hub returns an NAK when the Status
 * Change endpoint is polled." §11.12.3's Figure 11-21 accordingly ends its
 * handling loop with "Re-initialize Interrupt Pipe for Status Change
 * endpoint" before returning to the beginning. A hub that is not re-armed is
 * a hub that has stopped reporting, and nothing below it will ever be noticed
 * arriving or leaving again.
 *
 * The codes that get it into that state — Stall Error, Babble Detected, a USB
 * Transaction Error with the error count spent — all leave the endpoint
 * HALTED in the controller as well (xHCI 1.2b §4.10.2.1, §4.10.2.4,
 * §4.10.2.7), and §4.7 says "The xHC shall ignore doorbell references to
 * endpoints in the Halted or Error state". So a bare re-arm would put a
 * report on the ring that nothing would ever be asked for, and leave the
 * endpoint IN_FLIGHT — which makes this function itself return early for the
 * rest of the boot. The two-step repair §4.10.2.1 defines is done first, in
 * the order §4.10.2.1.1 gives it, and both steps are things this context is
 * allowed to do and the event drain is not.
 */
static void hub_arm_status(XhciHub* h)
{
    if (!h->slot->ep_interrupt_in || !h->slot->endpoints) {
        return;
    }
    uint8_t dci = h->slot->ep_interrupt_in;
    xhci_endpoint_t* ep = &h->slot->endpoints[dci];
    if (ep->xfer_state == XHCI_XFER_IN_FLIGHT) {
        return;
    }

    if (xhci_ep_context_state(h->ctrl, h->slot, dci) == XHCI_EP_STATE_HALTED) {
        /* The controller's half, and its completion waited for: §4.10.2.1
         * wants the Reset Endpoint Command to "complete successfully ...
         * before attempting to restart the Transfer Ring by ringing its
         * doorbell", and posting one is not the same as its having happened. */
        xhci_ep_recover(h->ctrl, h->slot, dci);
        xhci_command_wait_idle(h->ctrl, HUB_CTRL_TIMEOUT_MS);

        uint8_t state = xhci_ep_context_state(h->ctrl, h->slot, dci);
        if (state == XHCI_EP_STATE_HALTED) {
            kprintf("[USB hub slot %u] its status endpoint is still %s after a "
                    "Reset Endpoint — it cannot report anything below it until "
                    "the hub itself is replugged\n",
                    h->slot->slot_id, xhci_ep_state_name(state));
            return;
        }

        /* And the device's half, which §4.10.2.1.1 places after the endpoint
         * reaches Stopped and before any doorbell. Leaving it out is what made
         * the controller and the hub disagree about whether the pipe was
         * usable — and it is also what made the Reset Endpoint's cleared TSP
         * flag undefined rather than merely blunt (§4.6.8). */
        usb_setup_packet_t setup = {
            .bmRequestType = 0x02,      /* host to device, endpoint */
            .bRequest = USB_REQ_CLEAR_FEATURE,
            .wValue = USB_FEATURE_ENDPOINT_HALT,
            .wIndex = ep->addr,
            .wLength = 0
        };
        if (xhci_control_transfer_sync(h->ctrl, h->slot, &setup, 0, 0, false,
                                       HUB_CTRL_TIMEOUT_MS)
            != TRB_COMPLETION_SUCCESS) {
            kprintf("[USB hub slot %u] would not clear the halt on its own "
                    "status endpoint — it cannot report anything below it "
                    "until the hub itself is replugged\n", h->slot->slot_id);
            return;
        }
    }

    ep->xfer_state = XHCI_XFER_IDLE;
    xhci_ep_submit(h->ctrl, h->slot, dci, ep->buffer_phys, ep->max_packet);
}

/*
 * What the HUB has to say about ITSELF, which nothing here used to ask.
 *
 * USB 2.0 §11.12.4: "The hub reports any changes in hub status in bit zero of
 * the Hub and Port Status Change Bitmap." That bit was never read and the word
 * behind it was never fetched, so the two events that live in it were invisible
 * — and, worse, unclearable: §11.12.2 says "While a change bit is set, the hub
 * continues to report a status change when polled until all change bits have
 * been cleared by the USB System Software", so the hub reported, the service
 * pass walked every port and found nothing, re-armed, and was reported to
 * again, for the rest of the boot.
 *
 * The two events both take every device below the hub with them:
 *
 *   an over-current the hub reports on a hub basis. §11.12.5: "If the hub has
 *   over-current detection on a hub basis, then an over-current condition on
 *   the hub will cause all ports to enter the Powered-off state. However, in
 *   this case, neither C_PORT_OVER_CURRENT nor PORT_OVER_CURRENT is set for
 *   the affected ports" — so the per-port branch in the scan below cannot see
 *   it, and every port goes dark with nothing saying why;
 *
 *   the local supply lost, which is the brick pulled out of a self-powered
 *   hub. §11.5.1.2 makes the hub put every port in Powered-off for it.
 *
 * ‼ AND THE ORDER MATTERS. §11.11: "the power to a port is not turned on by a
 * SetPortFeature(PORT_POWER) if both C_HUB_LOCAL_POWER and Local Power Status
 * (in wHubStatus) are set to 1B at the time when the request is executed".
 * Acknowledging the change is therefore not bookkeeping to be done afterwards
 * — it is what makes the re-powering below happen at all.
 */
static void hub_service_own_status(XhciHub* h)
{
    uint16_t status = 0, change = 0;
    if (hub_status(h, &status, &change) != 0) {
        return;
    }
    if (!(change & (HUB_CHG_LOCAL_POWER | HUB_CHG_OVERCURRENT))) {
        return;
    }

    if (change & HUB_CHG_LOCAL_POWER) {
        kprintf("[USB hub slot %u] its own supply has changed — local power is "
                "%s\n", h->slot->slot_id,
                (status & HUB_STAT_LOCAL_POWER_LOST) ? "lost" : "good again");
        hub_clear_feature(h, HUB_FEAT_C_LOCAL_POWER);
    }
    if (change & HUB_CHG_OVERCURRENT) {
        kprintf("[USB hub slot %u] drew more current than its supply will "
                "give — it has switched off every one of its ports\n",
                h->slot->slot_id);
        hub_clear_feature(h, HUB_FEAT_C_OVER_CURRENT);
    }

    /* What went down with it is NOT guessed at here. A port that really
     * entered Powered-off reads as disconnected (§11.5.1.2 disables its
     * transmitters and receivers), and the scan that runs straight after this
     * retires exactly those and says so per port. Tearing the tree down from
     * here instead would also tear it down for a hub that merely reported a
     * stale power-source change on the way up, which some do.
     *
     * §11.12.5 step 4, and only once the hub says it has power to give —
     * otherwise §11.11 has the request ignored and the next report is what
     * brings this round again. */
    if (hub_status(h, &status, NULL) != 0) {
        kprintf("[USB hub slot %u] would not say whether it has power of its "
                "own — its ports are left as they are\n", h->slot->slot_id);
        return;
    }
    if (status & HUB_STAT_LOCAL_POWER_LOST) {
        kprintf("[USB hub slot %u] has no local power of its own to give its "
                "ports — they stay off until it says it has\n",
                h->slot->slot_id);
        return;
    }

    hub_power_all_ports(h);

    /* Asked, not assumed: the condition lasts as long as whatever caused it,
     * and a hub still over its limit has switched the ports straight back off
     * (§11.5.1.2). It reports again when that changes, which brings this pass
     * round once more — so there is nothing here to wait on. */
    if (hub_status(h, &status, NULL) != 0) {
        kprintf("[USB hub slot %u] would not say whether it is still over its "
                "current limit after its ports were powered\n",
                h->slot->slot_id);
    } else if (status & HUB_STAT_OVERCURRENT) {
        kprintf("[USB hub slot %u] is still over its current limit with the "
                "ports powered — they stay off until it is not\n",
                h->slot->slot_id);
    }

    /* Step 5 is the scan that runs straight after this. */
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

        /*
         * A port the hub has switched off, and the one thing that brings it
         * back.
         *
         * The hub has indeed cut power to it and will not restore that while
         * the condition lasts — but it will not restore it afterwards either.
         * §11.5.1.3 gives SetPortFeature(PORT_POWER) as the only exit from
         * Powered-off, and this driver used to issue that in exactly one
         * place, the one-shot loop in xhci_hub_attach. So the old "there is
         * nothing to bring up" read as a statement about the hub when it was
         * really a statement about this omission: a port that tripped once
         * was dark for the whole of that hub's attachment, and §11.5.1.2's
         * disabled receivers meant it could not even report the offending
         * device being unplugged.
         *
         * §11.12.5's recovery is followed here: the change is acknowledged
         * (hub_clear_changes, above), the port is given up, power is cycled,
         * and the port is read back — which is how "wait for the over-current
         * status bit to be cleared to 0" is answered without a clock. What
         * answers is then let through to the ordinary path below, so the
         * device on it is enumerated in this same pass.
         *
         * ‼ THE CHANGE BIT ALONE IS ENOUGH TO HAVE LOST THE PORT. §11.12.5,
         * on a limiter shared with a neighbour: "the affected port is placed
         * in the Powered-off state and C_PORT_OVER_CURRENT is set for the
         * port, but PORT_OVER_CURRENT is not set". Such a port took no branch
         * at all here and was never re-powered, so one trip killed several
         * ports and complained about one.
         */
        if ((status & PORT_STAT_OVERCURRENT) ||
            (change & PORT_CHG_OVERCURRENT)) {
            if (status & PORT_STAT_OVERCURRENT) {
                kprintf("[USB hub slot %u] port %u draws more current than the "
                        "hub will supply — it has switched the port off\n",
                        h->slot->slot_id, port);
            } else {
                kprintf("[USB hub slot %u] port %u was switched off by an "
                        "over-current elsewhere on the hub\n",
                        h->slot->slot_id, port);
            }
            hub_port_forget(h, port);
            acted++;
            if (hub_repower_port(h, port, &status) != 0) {
                continue;       /* still dark; the hub says when it is not */
            }
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
    if (!g_hubs_lock_ready) {
        return false;
    }
    spin_lock(&g_hubs_lock);
    bool found = hub_for_slot(slot) != NULL;
    spin_unlock(&g_hubs_lock);
    return found;
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

    /*
     * One translator per port is a mode to be entered, not a capability to be
     * reported. A hub that is not asked stays on the one it starts with.
     *
     * ‼ AND ONLY A HUB THAT HAS TRANSLATORS OF ITS OWN ANSWERS FOR THIS BIT.
     *
     * slot->multi_tt is the MTT field of xHCI 1.2 Table 6-4, and that field
     * says two things with one bit: a high-speed hub running its Multiple TT
     * Interface, and a slower device reached THROUGH such a hub. A full-speed
     * hub plugged into a multi-TT high-speed hub is the second kind — the
     * enumerator worked its bit out when it inherited the translator, and
     * clearing it unconditionally here told the controller the opposite of
     * what the parent had just enabled. A hub with translators of its own is
     * reached at high speed and therefore never stands behind one, so
     * tt_slot_id tells the two apart with nothing left over.
     */
    if (slot->tt_slot_id == 0) {
        slot->multi_tt = false;
        if (protocol == HUB_PROTOCOL_MULTI_TT) {
            hub_select_multi_tt(h);
        }
    }

    /* The controller has to be told this device is a hub before it can address
     * anything below it: a slot context without the Hub bit is scheduled as an
     * endpoint device, and the ports underneath do not exist as far as it is
     * concerned. The context is rewritten and re-evaluated. */
    /* What the hub said, not what this driver can keep bits for: xHCI 1.2
     * §4.5.2 has Number of Ports = bNbrPorts from the USB Hub Descriptor, and
     * Table 6-5 points straight at USB 2.0 Table 11-13, which puts no ceiling
     * on it. The controller schedules for the hub it is behind; how many of
     * those ports this driver polls is a separate question. */
    slot->hub_ports = h->ports_reported;

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
    if (xhci_post_configure_endpoint_cmd(ctrl, slot, slot->slot_id,
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
            (slot->tt_slot_id == 0 && slot->multi_tt)
                ? ", one translator per port" : "");

    /* Power every port, then wait once for all of them. No longer the only
     * place this happens — a port the hub switches off for an over-current is
     * brought back by the same request, and §11.5.1.3 has no other way. */
    hub_power_all_ports(h);

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
            /* Off the list, and said so: a service walk that was standing on
             * this node when it went reads the flag before it does anything
             * else with it. `next` is deliberately left pointing where it did
             * so that such a walk finishes over the live list. */
            h->attached = false;
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

    /*
     * And the memory is NOT given back here — see the note on g_hubs_released.
     * Another core may be inside the service pass with this very node in hand,
     * milliseconds deep in a control transfer that is reading h->buf_virt. The
     * queue is emptied at the top of the next pass, which is the one moment
     * nobody can be.
     */
    spin_lock(&g_hubs_lock);
    h->released_next = g_hubs_released;
    g_hubs_released  = h;
    spin_unlock(&g_hubs_lock);

    /* So that a pass comes for it even on a machine with nothing else to do. */
    xhci_hub_note_work();
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
    if (!g_hubs_lock_ready) {
        return;
    }

    /* Under the lock, flag and all: this runs in the transfer-event handler,
     * and the node it finds is one another core is entitled to be unlinking.
     * The lock masks interrupts, so taking it from here is safe, and the walk
     * it covers is a few pointers long. */
    spin_lock(&g_hubs_lock);
    XhciHub* h = hub_for_slot(slot);
    if (h) {
        h->change_pending = true;
    }
    spin_unlock(&g_hubs_lock);

    if (!h) {
        return;
    }
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

    /*
     * The hubs that have left, freed FIRST and only here.
     *
     * This is the one place in the driver where nothing can be standing on a
     * node that is off the list: g_hub_busy makes the long walk below
     * single-entry, so the previous pass has finished, and every other walk of
     * g_hubs is under g_hubs_lock and can only ever have seen live nodes. See
     * the note on g_hubs_released for why the release path leaves them here
     * instead of calling kfree with another core inside them.
     */
    spin_lock(&g_hubs_lock);
    XhciHub* dead   = g_hubs_released;
    g_hubs_released = NULL;
    spin_unlock(&g_hubs_lock);

    while (dead) {
        XhciHub* next = dead->released_next;
        if (dead->buf_phys) {
            pmm_free((void*)dead->buf_phys, 1);
        }
        kfree(dead);
        dead = next;
    }

    int acted = 0;

    /* Hubs the enumerator bound but nobody has spoken to yet. Attaching one
     * cannot happen where it was bound: that was the event handler, and every
     * step of this is a control transfer somebody has to wait for. */
    for (;;) {
        xhci_device_slot_t* pending = NULL;
        for (uint8_t id = 1; id < 255; id++) {
            xhci_device_slot_t* s = xhci_get_device_slot_by_id(ctrl, id);
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

    /*
     * The long walk, and the only one that is not under g_hubs_lock.
     *
     * It cannot be: each node is held across hub_scan_ports, which is control
     * transfers, a debounce and reset polling — hundreds of milliseconds with
     * interrupts masked if the lock were held. What makes it safe instead is
     * that nothing frees a node behind it (the queue above is the only freer,
     * and it runs before this walk begins, under the same g_hub_busy), and
     * that an unlink leaves `next` pointing where it did — so a node released
     * mid-walk is still a valid node and the walk carries on from it onto the
     * live list.
     *
     * ‼ THAT IS A GUARANTEE ABOUT MEMORY AND NOT ABOUT WORK, AND THE
     * DIFFERENCE MATTERS. `attached` is read once, at the top of each
     * iteration; a hub released while this core is already inside
     * hub_scan_ports for it is NOT skipped, and the scan goes on issuing
     * control transfers down a hub whose ports xhci_hub_release is taking
     * apart at the same moment. Those transfers fail against a slot that is
     * being retired, which is an answer and not a fault — every one of them
     * is waited for and every failure is reported — and the pass ends. What
     * the deferred free buys is that none of it touches freed memory. Making
     * the walk stop mid-scan would mean asking `attached` between every
     * control transfer inside hub_scan_ports, which is a different change
     * from this one and is not made here.
     *
     * Nothing inserts behind it either: the only insert is in
     * xhci_hub_attach, which is reached from the loop above and therefore
     * from this core, under this same g_hub_busy, before the head is read.
     */
    spin_lock(&g_hubs_lock);
    XhciHub* first = g_hubs;
    spin_unlock(&g_hubs_lock);

    for (XhciHub* h = first; h; h = h->next) {
        if (!h->attached) {
            continue;
        }

        if (h->change_pending) {
            h->change_pending = false;
            /* The hub's own word first: a hub-basis over-current or a lost
             * local supply shows in nothing a port says, and re-powering the
             * ports is what has to happen before the scan can find anything
             * on them. */
            hub_service_own_status(h);
            acted += hub_scan_ports(h, true);
        }

        /*
         * ‼ ARMED ON EVERY PASS, NOT ONLY AFTER A CHANGE WAS ACTED ON.
         *
         * This used to sit behind the `change_pending` test above, and
         * change_pending is raised in exactly one place — the good-completion
         * arm of the transfer handler. A status transfer that came back
         * halted therefore left the endpoint idle AND the hub unflagged, so
         * this loop skipped it for ever and nothing below it was ever noticed
         * again. Arming unconditionally costs nothing for a hub that is
         * already listening: the function returns at once on IN_FLIGHT.
         */
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
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_hub_service(xhci_controller_at(i));
    }
}
