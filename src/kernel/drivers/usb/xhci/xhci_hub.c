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

#define PORT_FEAT_ENABLE         1
#define PORT_FEAT_RESET          4
#define PORT_FEAT_POWER          8
#define PORT_FEAT_C_CONNECTION   16
#define PORT_FEAT_C_ENABLE       17
#define PORT_FEAT_C_OVERCURRENT  19
#define PORT_FEAT_C_RESET        20
#define PORT_FEAT_C_LINK_STATE   25
#define PORT_FEAT_C_CONFIG_ERROR 26
#define PORT_FEAT_BH_RESET       28
#define PORT_FEAT_C_BH_RESET     29

#define PORT_STAT_CONNECTION     (1u << 0)
#define PORT_STAT_ENABLE         (1u << 1)
#define PORT_STAT_OVERCURRENT    (1u << 3)
#define PORT_STAT_RESET          (1u << 4)

#define PORT_STAT_POWER          (1u << 8)
#define PORT_STAT_LOW_SPEED      (1u << 9)
#define PORT_STAT_HIGH_SPEED     (1u << 10)

#define SS_PORT_STAT_LINK_STATE  (0xFu << 5)
#define SS_PORT_STAT_POWER       (1u << 9)
#define SS_PORT_STAT_SPEED       (0x7u << 10)

#define PORT_CHG_CONNECTION      (1u << 0)
#define PORT_CHG_ENABLE          (1u << 1)
#define PORT_CHG_OVERCURRENT     (1u << 3)
#define PORT_CHG_RESET           (1u << 4)
#define SS_PORT_CHG_BH_RESET     (1u << 5)
#define SS_PORT_CHG_LINK_STATE   (1u << 6)
#define SS_PORT_CHG_CONFIG_ERR   (1u << 7)

#define SS_LINK_U0               0
#define SS_LINK_SS_INACTIVE      6
#define SS_LINK_COMPLIANCE       10

#define HUB_MAX_ROUTABLE_PORTS   15

#define HUB_CTRL_TIMEOUT_MS      1000

#define HUB_RESET_TIMEOUT_MS     800
#define HUB_RESET_POLL_MS        10
#define HUB_RESET_RECOVERY_MS    20

#define HUB_DEBOUNCE_MS          100

typedef struct XhciHub {
    struct XhciHub* next;
    xhci_controller_t*  ctrl;
    xhci_device_slot_t* slot;

    uint8_t  ports;
    bool     superspeed;
    bool     attached;

    uint16_t power_on_delay_ms;

    volatile bool change_pending;

    uint32_t present;
    uint32_t occupied;

    void*    buf_virt;
    uint64_t buf_phys;
} XhciHub;

static XhciHub* g_hubs = NULL;
static spinlock_t g_hubs_lock;
static bool g_hubs_lock_ready = false;
static volatile uint32_t g_hub_work = 0;

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

static bool ss_link_usable(uint16_t status)
{
    uint16_t state = (status & SS_PORT_STAT_LINK_STATE) >> 5;
    return state != SS_LINK_SS_INACTIVE && state != SS_LINK_COMPLIANCE;
}


#define HUB_DESCRIBE_ATTEMPTS 4

static int hub_get_descriptor_once(XhciHub* h)
{
    uint8_t  type = h->superspeed ? USB_DESC_HUB_SS : USB_DESC_HUB;
    uint16_t want = h->superspeed ? HUB_DESC_LEN_SS : HUB_DESC_LEN;

    usb_setup_packet_t setup = {
        .bmRequestType = 0xA0,
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

    uint16_t characteristics = (uint16_t)(d[3] | ((uint16_t)d[4] << 8));
    h->slot->tt_think_time = h->superspeed
                           ? 0
                           : (uint8_t)((characteristics >> 5) & 0x3);

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

static int hub_set_depth(XhciHub* h)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0x20,
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

static void hub_select_multi_tt(XhciHub* h)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0x01,
        .bRequest = USB_REQ_SET_INTERFACE,
        .wValue = 1,
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
        .bmRequestType = 0x23,
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
        .bmRequestType = 0xA3,
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

static uint8_t hub_port_speed(const XhciHub* h, uint16_t status)
{
    if (h->superspeed) {
        return XHCI_PORT_SPEED_SUPER;
    }
    if (status & PORT_STAT_LOW_SPEED)  return XHCI_PORT_SPEED_LOW;
    if (status & PORT_STAT_HIGH_SPEED) return XHCI_PORT_SPEED_HIGH;
    return XHCI_PORT_SPEED_FULL;
}


static int hub_reset_port(XhciHub* h, uint8_t port, bool warm,
                          uint16_t* out_status)
{
    uint8_t  feature = warm ? PORT_FEAT_BH_RESET : PORT_FEAT_RESET;
    uint16_t finished = warm ? SS_PORT_CHG_BH_RESET : PORT_CHG_RESET;

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

static int hub_bring_up_port(XhciHub* h, uint8_t port)
{
    uint16_t status = 0, change = 0;

    hub_pause_ms(HUB_DEBOUNCE_MS);

    if (hub_port_status(h, port, &status, &change) != 0) {
        return -1;
    }
    if (!(status & PORT_STAT_CONNECTION)) {
        return -1;
    }

    if (hub_reset_port(h, port, false, &status) != 0) {
        return -1;
    }

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

static void hub_port_gone(XhciHub* h, uint8_t port)
{
    kprintf("[USB hub slot %u] port %u: device removed\n",
            h->slot->slot_id, port);

    uint32_t guard_max = h->ctrl ? h->ctrl->slot_count : 0u;
    for (uint32_t guard = 0; guard < guard_max; guard++) {
        xhci_device_slot_t* child = NULL;
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

static void hub_port_forget(XhciHub* h, uint8_t port)
{
    if (h->occupied & (1u << port)) {
        hub_port_gone(h, port);
    }
    h->present  &= ~(1u << port);
    h->occupied &= ~(1u << port);
}

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
            kprintf("[USB hub slot %u] port %u draws more current than the hub "
                    "will supply — it has switched the port off\n",
                    h->slot->slot_id, port);
            hub_port_forget(h, port);
            acted++;
            continue;
        }

        bool connected = (status & PORT_STAT_CONNECTION) != 0;
        bool answered  = (h->present & (1u << port)) != 0;

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

    if (h->superspeed && hub_set_depth(h) != 0) {
        xhci_hub_release(ctrl, slot);
        return -1;
    }

    slot->multi_tt = false;
    if (protocol == HUB_PROTOCOL_MULTI_TT) {
        hub_select_multi_tt(h);
    }

    slot->hub_ports = h->ports;

    uint32_t pages = xhci_input_ctx_pages(ctrl);
    void* input_phys = pmm_alloc_zero(pages, PHYS_TAG_DMA32);
    if (!input_phys) {
        xhci_hub_release(ctrl, slot);
        return -1;
    }
    uint8_t* base = (uint8_t*)vmm_phys_to_virt((uintptr_t)input_phys);
    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)base;
    icc->add_context_flags = (1u << 0);

    xhci_slot_context_t* sctx =
        (xhci_slot_context_t*)(base + ctrl->context_size);
    xhci_fill_slot_context(sctx, slot);
    sctx->dwords[0] = (sctx->dwords[0] & ~(0x1Fu << 27)) |
                      ((uint32_t)slot->max_dci << 27);

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
            slot->multi_tt ? ", one translator per port" : "");

    for (uint8_t port = 1; port <= h->ports; port++) {
        hub_port_feature(h, port, PORT_FEAT_POWER, true);
    }
    hub_pause_ms(h->power_on_delay_ms);

    h->attached = true;

    hub_scan_ports(h, false);

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
        return 0;
    }

    __atomic_store_n(&g_hub_work, 0, __ATOMIC_RELEASE);

    int acted = 0;

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
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_hub_service(xhci_controller_at(i));
    }
}