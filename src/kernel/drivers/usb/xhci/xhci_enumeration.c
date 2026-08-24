#include "xhci_enumeration.h"
#include "xhci_command.h"
#include "xhci_device.h"
#include "xhci_port.h"
#include "xhci_transfer.h"
#include "xhci_hid.h"
#include "xhci_endpoint.h"
#include "xhci_msd.h"
#include "xhci_hub.h"
#include "xhci_interrupt.h"
#include "usb_common.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "atomics.h"
#include "cpu_calibrate.h"

static struct xhci_device_slot device_slots[XHCI_MAX_DEVICE_SLOTS];
static spinlock_t device_slots_lock;

void xhci_enumeration_init(void) {
    spinlock_init(&device_slots_lock);
    memset(device_slots, 0, sizeof(device_slots));
}

static struct xhci_device_slot* find_free_slot(void) {
    spin_lock(&device_slots_lock);
    for (int i = 0; i < XHCI_MAX_DEVICE_SLOTS; i++) {
        if (device_slots[i].state == ENUM_STATE_IDLE) {
            device_slots[i].state = ENUM_STATE_CLAIMING;
            spin_unlock(&device_slots_lock);
            return &device_slots[i];
        }
    }
    spin_unlock(&device_slots_lock);
    return NULL;
}

static struct xhci_device_slot* find_slot_by_port(uint8_t port) {
    spin_lock(&device_slots_lock);
    for (int i = 0; i < XHCI_MAX_DEVICE_SLOTS; i++) {
        if (device_slots[i].port_num == port &&
            device_slots[i].state != ENUM_STATE_IDLE) {
            spin_unlock(&device_slots_lock);
            return &device_slots[i];
        }
    }
    spin_unlock(&device_slots_lock);
    return NULL;
}

xhci_device_slot_t* xhci_get_device_slot(xhci_controller_t* ctrl, uint8_t slot_id) {
    if (!ctrl || slot_id == 0 || slot_id > ctrl->max_slots) {
        return NULL;
    }

    spin_lock(&device_slots_lock);
    for (int i = 0; i < XHCI_MAX_DEVICE_SLOTS; i++) {
        if (device_slots[i].slot_id == slot_id &&
            device_slots[i].state != ENUM_STATE_IDLE) {
            spin_unlock(&device_slots_lock);
            return &device_slots[i];
        }
    }
    spin_unlock(&device_slots_lock);
    return NULL;
}

xhci_device_slot_t* xhci_get_device_slot_by_port(uint8_t port) {
    return find_slot_by_port(port);
}

/*
 * EP0's max packet size is the one endpoint parameter a device cannot be asked
 * about until it has already been talked to. At every speed but full it is
 * fixed by the specification and can simply be written down:
 *
 *   low         8 bytes
 *   high       64 bytes
 *   super     512 bytes  (the field is an exponent there: 2^9)
 *
 * Full speed is the exception — 8, 16, 32 or 64, the device's choice — so it
 * gets 8, which every full-speed device must accept, and the real value is
 * read out of the first eight bytes of its descriptor and installed with an
 * Evaluate Context. Getting this wrong is not a slow device; it is a device
 * that answers with packets larger than the controller was told to expect,
 * and the controller calls that babble and gives up.
 */
static uint16_t ep0_initial_max_packet(uint8_t speed) {
    switch (speed) {
        case XHCI_PORT_SPEED_LOW:      return 8;
        case XHCI_PORT_SPEED_HIGH:     return 64;
        case XHCI_PORT_SPEED_SUPER:
        case XHCI_PORT_SPEED_SUPER_10: return 512;
        case XHCI_PORT_SPEED_FULL:
        default:                       return 8;
    }
}

static const char* speed_name(uint8_t speed) {
    switch (speed) {
        case XHCI_PORT_SPEED_FULL:     return "full";
        case XHCI_PORT_SPEED_LOW:      return "low";
        case XHCI_PORT_SPEED_HIGH:     return "high";
        case XHCI_PORT_SPEED_SUPER:    return "super";
        case XHCI_PORT_SPEED_SUPER_10: return "super+";
        default:                       return "?";
    }
}

/* Post an Enable Slot and start waiting for it. Reached either straight from
 * xhci_enumerate_device, when the port needed no reset, or from the
 * port-status change that says the reset is over. */
static int enum_begin_slot(xhci_controller_t* ctrl, struct xhci_device_slot* slot) {
    slot->state = ENUM_STATE_WAIT_ENABLE_SLOT;

    if (xhci_post_enable_slot_cmd(ctrl) < 0) {
        debug_printf("[xHCI ENUM] Failed to post Enable Slot command\n");
        slot->state = ENUM_STATE_IDLE;
        slot->port_num = 0;
        return -1;
    }
    return 0;
}

int xhci_enumerate_device(xhci_controller_t* ctrl, uint8_t port) {
    if (!ctrl || !ctrl->running) {
        debug_printf("[xHCI ENUM] Controller not ready\n");
        return -1;
    }

    if (port == 0 || port > ctrl->max_ports) {
        debug_printf("[xHCI ENUM] Invalid port: %u\n", port);
        return -2;
    }

    if (!xhci_port_has_device(ctrl, port)) {
        return -3;
    }

    if (find_slot_by_port(port)) {
        return -4;
    }

    struct xhci_device_slot* slot = find_free_slot();
    if (!slot) {
        kprintf("[xHCI] no free device slot for port %u\n", port);
        return -5;
    }

    // slot->state is already ENUM_STATE_CLAIMING — zero individual fields only
    slot->slot_id = 0;
    slot->port_num = port;
    slot->speed = 0;
    slot->dev_ctx = NULL;
    slot->dev_ctx_phys = 0;
    slot->input_ctx_phys = 0;
    slot->ep0_ring = NULL;
    slot->ep0_ring_phys = 0;
    slot->ep0_max_packet = 0;
    slot->config_total_len = 0;
    slot->stall_resume = 0;
    slot->interface_class = 0;
    slot->interface_subclass = 0;
    slot->interface_protocol = 0;
    slot->descriptor_buffer_virt = NULL;
    slot->descriptor_buffer_phys = 0;
    slot->endpoints = NULL;
    slot->max_dci = 0;
    slot->ep_pending_add = 0;
    slot->driver = XHCI_DRIVER_NONE;
    slot->ep_interrupt_in = 0;
    slot->ep_bulk_in = 0;
    slot->ep_bulk_out = 0;
    slot->config_value = 0;
    slot->interface_num = 0;
    slot->route_string = 0;
    slot->depth = 0;
    slot->parent_slot_id = 0;
    slot->parent_port = 0;
    slot->tt_slot_id = 0;
    slot->tt_port = 0;
    slot->hub_ports = 0;
    slot->tt_think_time = 0;
    slot->multi_tt = false;
    memset(&slot->device_desc, 0, sizeof(slot->device_desc));
    slot->timestamp_started = rdtsc();

    debug_printf("[xHCI ENUM] Starting enumeration for port %u\n", port);

    /* Say what is being waited for BEFORE doing the thing that ends the wait.
     *
     * Asserting the reset is a store to a device register, and the device is
     * entitled to finish the reset and raise its interrupt inside that store.
     * It does: under emulation the port reset completes synchronously, so the
     * port-status change handler ran on this very core with the state still
     * reading "claiming", found nothing waiting on a reset, and returned. The
     * reset then completed for a slot that spent the rest of the boot waiting
     * for an event that had already been and gone — two seconds later the
     * watchdog said so, which is how this was found at all.
     *
     * Real hardware takes tens of milliseconds over the same reset, which
     * makes the window smaller and the bug no less real. Publishing the state
     * first closes it: the release store cannot be moved after the register
     * write, so any handler that runs sees the wait it is meant to satisfy. */
    __atomic_store_n(&slot->state, (uint8_t)ENUM_STATE_WAIT_PORT_RESET,
                     __ATOMIC_RELEASE);

    /* Ask the port to reset and step away. On a USB 3 port that trained itself
     * there is nothing to do and this returns 1; otherwise the reset is now in
     * flight and the port will say when it is finished. Either way this
     * function does not spend the tens of milliseconds a reset takes — it is
     * reached from the interrupt handler, and every other interrupt on this
     * core would have waited behind it. */
    int reset = xhci_port_begin_reset(ctrl, port);

    if (reset < 0) {
        debug_printf("[xHCI ENUM] Port %u could not be reset (%d)\n", port, reset);
        slot->state = ENUM_STATE_IDLE;
        slot->port_num = 0;
        return -6;
    }

    /* A port that needed no reset will never announce one, so this is the only
     * place that can carry it forward — but only if nothing already has. */
    if (reset == 1 &&
        __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == ENUM_STATE_WAIT_PORT_RESET) {
        return enum_begin_slot(ctrl, slot) == 0 ? 0 : -7;
    }

    return 0;
}

xhci_device_slot_t* xhci_get_device_slot_by_id(uint8_t slot_id)
{
    if (slot_id == 0) {
        return NULL;
    }
    spin_lock(&device_slots_lock);
    for (int i = 0; i < XHCI_MAX_DEVICE_SLOTS; i++) {
        if (device_slots[i].slot_id == slot_id &&
            device_slots[i].state != ENUM_STATE_IDLE) {
            spin_unlock(&device_slots_lock);
            return &device_slots[i];
        }
    }
    spin_unlock(&device_slots_lock);
    return NULL;
}

/*
 * A device found on a hub port.
 *
 * The hub has already powered the port, reset it and read back what speed
 * answered, so there is no port reset to wait for here — this joins the same
 * conversation a root-port device joins after its own reset, at Enable Slot.
 *
 * What is different is everything the controller needs in order to find the
 * device at all: the root port the branch hangs off, the route down through
 * the hubs, and, when the device is slower than the bus it is reached over,
 * which hub is translating for it.
 */
int xhci_enumerate_behind_hub(xhci_controller_t* ctrl,
                              xhci_device_slot_t* hub,
                              uint8_t hub_port, uint8_t speed)
{
    if (!ctrl || !ctrl->running || !hub || hub_port == 0) {
        return -1;
    }

    /* Four bits name the port at each tier, so fifteen is the highest port the
     * bus can be told about — which is why the specification stops hubs there
     * too, and why a hub claiming more of them has some that cannot be used. */
    if (hub_port > 15) {
        kprintf("[xHCI] hub slot %u has no port %u the bus could name — four "
                "bits per tier stop at 15\n", hub->slot_id, hub_port);
        return -5;
    }

    /* Five tiers is the whole of the route string, and the specification says
     * so: four bits each, twenty bits, and no sixth place to put a number. */
    if (hub->depth >= 5) {
        kprintf("[xHCI] a device on hub slot %u port %u is six hubs deep, "
                "which is one more than the bus can address\n",
                hub->slot_id, hub_port);
        return -2;
    }

    struct xhci_device_slot* slot = find_free_slot();
    if (!slot) {
        kprintf("[xHCI] no free device slot for hub slot %u port %u\n",
                hub->slot_id, hub_port);
        return -3;
    }

    slot->slot_id = 0;
    slot->port_num = hub->port_num;          /* the root port, still */
    slot->speed = speed;
    slot->dev_ctx = NULL;
    slot->dev_ctx_phys = 0;
    slot->input_ctx_phys = 0;
    slot->ep0_ring = NULL;
    slot->ep0_ring_phys = 0;
    slot->ep0_max_packet = 0;
    slot->config_total_len = 0;
    slot->interface_class = 0;
    slot->interface_subclass = 0;
    slot->interface_protocol = 0;
    slot->descriptor_buffer_virt = NULL;
    slot->descriptor_buffer_phys = 0;
    slot->endpoints = NULL;
    slot->max_dci = 0;
    slot->ep_pending_add = 0;
    slot->driver = XHCI_DRIVER_NONE;
    slot->ep_interrupt_in = 0;
    slot->ep_bulk_in = 0;
    slot->ep_bulk_out = 0;
    slot->config_value = 0;
    slot->interface_num = 0;
    slot->hub_ports = 0;
    slot->tt_think_time = 0;
    slot->multi_tt = false;
    memset(&slot->device_desc, 0, sizeof(slot->device_desc));

    /* Four bits per tier, and the tier is how deep the HUB is — a device on a
     * hub that is itself on a root port occupies the first four bits.
     *
     * A port number above fifteen does not fit in those four bits, and the old
     * code clamped it to fifteen. That does not address port sixteen; it
     * addresses port fifteen, and hands whatever is plugged into that one the
     * transfers meant for its neighbour. A port the bus cannot name is a port
     * this driver says it cannot reach. */
    slot->route_string   = hub->route_string | ((uint32_t)hub_port << (4 * hub->depth));
    slot->depth          = (uint8_t)(hub->depth + 1);
    slot->parent_slot_id = hub->slot_id;
    slot->parent_port    = hub_port;

    /* Who translates. A low or full speed device reached through a high speed
     * hub is spoken to by that hub on the controller's behalf, and the
     * controller has to be told which one. A device deeper down inherits
     * whichever translator was already standing in the way. */
    if ((speed == XHCI_PORT_SPEED_LOW || speed == XHCI_PORT_SPEED_FULL) &&
        hub->speed == XHCI_PORT_SPEED_HIGH) {
        slot->tt_slot_id = hub->slot_id;
        slot->tt_port    = hub_port;
    } else {
        slot->tt_slot_id = hub->tt_slot_id;
        slot->tt_port    = hub->tt_port;
    }

    slot->timestamp_started = rdtsc();

    debug_printf("[xHCI ENUM] device on hub slot %u port %u, route 0x%05x, "
                 "depth %u\n", hub->slot_id, hub_port,
                 slot->route_string, slot->depth);

    if (enum_begin_slot(ctrl, slot) != 0) {
        return -4;
    }
    return 0;
}

void xhci_enum_port_reset_done(xhci_controller_t* ctrl, uint8_t port)
{
    if (!ctrl) {
        return;
    }

    struct xhci_device_slot* slot = find_slot_by_port(port);
    if (!slot || slot->state != ENUM_STATE_WAIT_PORT_RESET) {
        return;                 /* a reset nobody was waiting on */
    }

    if (!xhci_port_reset_finished(ctrl, port)) {
        /* The reset ended and the port is not enabled: the device on it did
         * not answer. Nothing further is possible, and holding the slot for it
         * would keep the port unusable for whatever is plugged in next. */
        kprintf("[xHCI] port %u reset but did not enable — no usable device\n",
                port);
        xhci_device_slot_cleanup(ctrl, slot);
        return;
    }

    debug_printf("[xHCI ENUM] Port %u reset complete\n", port);
    enum_begin_slot(ctrl, slot);
}

void xhci_device_slot_cleanup(xhci_controller_t* ctrl, xhci_device_slot_t* slot) {
    if (!slot) {
        return;
    }

    if (slot->ep0_ring) {
        xhci_free_ep0_ring(slot);
    }

    if (slot->driver == XHCI_DRIVER_STORAGE) {
        xhci_msd_release(slot);
    }
    if (slot->driver == XHCI_DRIVER_HUB) {
        xhci_hub_release(ctrl, slot);
    }

    xhci_ep_table_free(slot);

    if (slot->input_ctx_phys && ctrl) {
        pmm_free((void*)slot->input_ctx_phys, xhci_input_ctx_pages(ctrl));
        slot->input_ctx_phys = 0;
    }

    if (slot->dev_ctx_phys) {
        pmm_free((void*)slot->dev_ctx_phys, 1);
        slot->dev_ctx_phys = 0;
        slot->dev_ctx = NULL;
    }

    if (ctrl && slot->slot_id > 0 && slot->slot_id <= ctrl->max_slots) {
        ctrl->dcbaa->device_context_ptrs[slot->slot_id] = 0;
    }

    slot->slot_id = 0;
    slot->port_num = 0;
    slot->state = ENUM_STATE_IDLE;
    slot->timestamp_started = 0;
    slot->driver = XHCI_DRIVER_NONE;
}

/* Build an Input Context describing the slot and EP0, and ask the controller
 * to address the device with it. */
static void enum_address_device(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    uint32_t pages = xhci_input_ctx_pages(ctrl);
    void* input_ctx_phys = pmm_alloc_zero(pages, PHYS_TAG_DMA32);
    if (!input_ctx_phys) {
        kprintf("[xHCI] out of memory addressing the device on port %u\n",
                slot->port_num);
        xhci_device_slot_cleanup(ctrl, slot);
        return;
    }
    slot->input_ctx_phys = (uint64_t)input_ctx_phys;

    uint8_t* input_base = (uint8_t*)vmm_phys_to_virt((uintptr_t)input_ctx_phys);

    /* Input Control Context: add Slot (A0) and EP0 (A1). */
    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)input_base;
    icc->add_context_flags = (1 << 0) | (1 << 1);

    xhci_slot_context_t* slot_ctx =
        (xhci_slot_context_t*)(input_base + ctrl->context_size);
    xhci_fill_slot_context(slot_ctx, slot);

    slot->ep0_max_packet = ep0_initial_max_packet(slot->speed);
    xhci_endpoint_context_t* ep0_ctx =
        (xhci_endpoint_context_t*)(input_base + ctrl->context_size * 2);
    xhci_init_ep0_context(ep0_ctx, slot->ep0_ring->trbs_phys, slot->ep0_max_packet);

    slot->state = ENUM_STATE_WAIT_ADDRESS_DEVICE;

    if (xhci_post_address_device_cmd(ctrl, slot->slot_id,
                                     (uint64_t)input_ctx_phys) < 0) {
        debug_printf("[xHCI ENUM] Failed to post Address Device\n");
        xhci_device_slot_cleanup(ctrl, slot);
    }
}

/* Ask for `length` bytes of a descriptor into the slot's scratch page. */
static int enum_get_descriptor(xhci_controller_t* ctrl, struct xhci_device_slot* slot,
                               uint8_t type, uint16_t length)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0x80,
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((uint16_t)type << 8),
        .wIndex = 0,
        .wLength = length
    };

    return xhci_control_transfer(ctrl, slot, &setup,
                                 slot->descriptor_buffer_phys, length, true);
}

/*
 * Install a corrected EP0 max packet size with an Evaluate Context command.
 * Only the endpoint context is being changed, so only A1 is set — the slot
 * context comes along because the specification requires A0 to accompany it.
 */
static void enum_evaluate_ep0(xhci_controller_t* ctrl, struct xhci_device_slot* slot,
                              uint16_t max_packet)
{
    uint32_t pages = xhci_input_ctx_pages(ctrl);
    void* input_ctx_phys = pmm_alloc_zero(pages, PHYS_TAG_DMA32);
    if (!input_ctx_phys) {
        kprintf("[xHCI] out of memory correcting EP0 on port %u\n", slot->port_num);
        xhci_device_slot_cleanup(ctrl, slot);
        return;
    }
    slot->input_ctx_phys = (uint64_t)input_ctx_phys;

    uint8_t* input_base = (uint8_t*)vmm_phys_to_virt((uintptr_t)input_ctx_phys);

    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)input_base;
    icc->add_context_flags = (1 << 1);           /* EP0 only */

    xhci_endpoint_context_t* ep0_ctx =
        (xhci_endpoint_context_t*)(input_base + ctrl->context_size * 2);
    xhci_init_ep0_context(ep0_ctx, slot->ep0_ring->trbs_phys, max_packet);

    slot->ep0_max_packet = max_packet;
    slot->state = ENUM_STATE_WAIT_EVALUATE_CONTEXT;

    if (xhci_post_evaluate_context_cmd(ctrl, slot->slot_id,
                                       (uint64_t)input_ctx_phys) < 0) {
        debug_printf("[xHCI ENUM] Failed to post Evaluate Context\n");
        xhci_device_slot_cleanup(ctrl, slot);
    }
}

static void enum_free_input_ctx(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    if (slot->input_ctx_phys) {
        pmm_free((void*)slot->input_ctx_phys, xhci_input_ctx_pages(ctrl));
        slot->input_ctx_phys = 0;
    }
}

/* A device this kernel has no driver for is still a device. It is addressed,
 * configured and left alone, with its slot intact — which is both the honest
 * outcome and the ground a class driver stands on when one arrives. */
static void enum_settle_unclaimed(struct xhci_device_slot* slot)
{
    kprintf("[xHCI] port %u: %s-speed device %04x:%04x class %02x/%02x/%02x "
            "configured, no driver claims it\n",
            slot->port_num, speed_name(slot->speed),
            slot->device_desc.idVendor, slot->device_desc.idProduct,
            slot->interface_class, slot->interface_subclass,
            slot->interface_protocol);
    slot->state = ENUM_STATE_CONFIGURED;

    /* "No driver claims it" is a statement about right now, not about ever.
     * The announcement goes out all the same, carrying everything a driver
     * would need to recognise its own device — which is how a class driver
     * comes to own this one without anybody editing the state machine above
     * to know about it. */
    xhci_touch_device_arrived(slot);
}

/*
 * A device is allowed to say no.
 *
 * SET_IDLE and SET_PROTOCOL are class requests a HID device may simply not
 * implement, and the way USB says "I do not implement that" is to stall the
 * control pipe. Plenty of real keyboards stall SET_IDLE — it is unremarkable,
 * and it is not a reason to throw the keyboard away.
 *
 * But a stall halts the pipe, so nothing further can be asked of the device
 * until the halt is cleared: a Reset Endpoint to clear it, then a Set TR
 * Dequeue Pointer to step over the transfer that stalled, and only then does
 * enumeration carry on from where it left off. Treating the stall as a failure
 * instead — which is what this driver did — turns "this keyboard does not
 * implement an optional request" into "this keyboard does not work".
 */
bool xhci_enum_stall_is_tolerable(uint8_t state)
{
    return state == ENUM_STATE_WAIT_SET_PROTOCOL ||
           state == ENUM_STATE_WAIT_SET_IDLE;
}

void xhci_enum_recover_ep0(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    if (!ctrl || !slot) {
        return;
    }

    kprintf("[xHCI] port %u: device declined an optional request — clearing "
            "the control pipe and carrying on\n", slot->port_num);

    slot->stall_resume = slot->state;
    slot->state = ENUM_STATE_WAIT_EP0_RESET;

    if (xhci_post_reset_endpoint_cmd(ctrl, slot->slot_id, 1) < 0) {
        xhci_device_slot_cleanup(ctrl, slot);
    }
}

static const char* enum_state_name(uint8_t state) {
    switch (state) {
        case ENUM_STATE_IDLE:                   return "idle";
        case ENUM_STATE_CLAIMING:               return "claiming a slot";
        case ENUM_STATE_WAIT_PORT_RESET:        return "waiting for the port reset";
        case ENUM_STATE_WAIT_ENABLE_SLOT:       return "waiting for Enable Slot";
        case ENUM_STATE_WAIT_ADDRESS_DEVICE:    return "waiting for Address Device";
        case ENUM_STATE_WAIT_GET_DESC_HEADER:   return "reading the first 8 descriptor bytes";
        case ENUM_STATE_WAIT_EVALUATE_CONTEXT:  return "correcting the EP0 packet size";
        case ENUM_STATE_WAIT_GET_DESCRIPTOR:    return "reading the device descriptor";
        case ENUM_STATE_WAIT_GET_CONFIG_HEADER: return "reading the configuration header";
        case ENUM_STATE_WAIT_GET_CONFIG_DESC:   return "reading the configuration";
        case ENUM_STATE_WAIT_SET_CONFIGURATION: return "waiting for Set Configuration";
        case ENUM_STATE_WAIT_SET_PROTOCOL:      return "waiting for Set Protocol";
        case ENUM_STATE_WAIT_SET_IDLE:          return "waiting for Set Idle";
        case ENUM_STATE_WAIT_EP0_RESET:         return "clearing a stalled control pipe";
        case ENUM_STATE_WAIT_EP0_DEQUEUE:       return "repositioning the control ring";
        case ENUM_STATE_WAIT_CONFIGURE_ENDPOINT:return "waiting for Configure Endpoint";
        case ENUM_STATE_CONFIGURED:             return "configured";
        default:                                return "?";
    }
}

/*
 * The step that never finished.
 *
 * Every state above waits on an event, and an event that never arrives is a
 * device that is simply never mentioned again — the slot stays claimed, the
 * port stays unusable, and nothing anywhere says which of a dozen steps it was
 * that went quiet. That is the worst kind of failure to meet on a machine you
 * can only read by photographing its screen.
 *
 * So the wait has an outside edge. Not as a substitute for the event — the
 * event is still what drives everything — but so that its absence becomes a
 * printed fact naming the exact step, and the port is handed back for whatever
 * is plugged in next.
 */
/*
 * What the device offers, gathered while walking one interface.
 *
 * The walk reports endpoints one at a time rather than filling an array, so
 * this keeps only the ones any driver here knows what to do with. An interface
 * that offers six endpoints is not a problem to be capped; it is five endpoints
 * nobody asked about.
 */
typedef struct {
    uint8_t  intr_in_dci,  intr_addr,  intr_interval;
    uint16_t intr_mps;
    uint8_t  bulk_in_dci,  bulk_in_addr;
    uint16_t bulk_in_mps;
    uint8_t  bulk_out_dci, bulk_out_addr;
    uint16_t bulk_out_mps;
} enum_pick_t;

static bool enum_pick_visit(void* ctx, const usb_endpoint_info_t* ep)
{
    enum_pick_t* p = (enum_pick_t*)ctx;
    bool in = (ep->addr & 0x80) != 0;

    switch (ep->attributes & 0x03) {
        case USB_EP_XFER_INTERRUPT:
            if (in && !p->intr_in_dci) {
                p->intr_in_dci   = xhci_dci_of(ep->addr);
                p->intr_addr     = ep->addr;
                p->intr_mps      = ep->max_packet;
                p->intr_interval = ep->interval;
            }
            break;
        case USB_EP_XFER_BULK:
            if (in && !p->bulk_in_dci) {
                p->bulk_in_dci  = xhci_dci_of(ep->addr);
                p->bulk_in_addr = ep->addr;
                p->bulk_in_mps  = ep->max_packet;
            } else if (!in && !p->bulk_out_dci) {
                p->bulk_out_dci  = xhci_dci_of(ep->addr);
                p->bulk_out_addr = ep->addr;
                p->bulk_out_mps  = ep->max_packet;
            }
            break;
        default:
            break;
    }
    return true;
}

/*
 * Who drives this device?
 *
 * The device has just accepted its configuration, and the descriptor that
 * described it is still in the scratch page. This is the one place where a
 * device is matched against the drivers this kernel has — and where a device
 * matching none of them is settled rather than discarded.
 *
 * It is deliberately not a table of drivers scanning for devices. Each branch
 * asks the descriptor a question it already knows the answer to, prepares the
 * endpoints it needs, and hands the slot back to the state machine.
 */
static void enum_bind_driver(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    void*    cfg = slot->descriptor_buffer_virt;
    uint16_t len = slot->config_total_len;
    uint8_t  iface = 0;
    enum_pick_t pick;

    /* A boot-protocol keyboard, which is the one HID shape a kernel can read
     * without parsing a report descriptor. */
    memset(&pick, 0, sizeof(pick));
    if (usb_walk_interface(cfg, len, USB_HID_CLASS, USB_HID_SUBCLASS_BOOT,
                           USB_HID_PROTOCOL_KEYBOARD, &iface,
                           enum_pick_visit, &pick) && pick.intr_in_dci) {

        slot->driver          = XHCI_DRIVER_KEYBOARD;
        slot->interface_num   = iface;
        slot->ep_interrupt_in = pick.intr_in_dci;

        if (xhci_ep_prepare(slot, pick.intr_in_dci, XHCI_EP_TYPE_INTERRUPT_IN,
                            pick.intr_addr, pick.intr_mps, pick.intr_interval,
                            pick.intr_mps) != 0) {
            kprintf("[xHCI] port %u: no memory for the keyboard endpoint\n",
                    slot->port_num);
            xhci_device_slot_cleanup(ctrl, slot);
            return;
        }

        usb_setup_packet_t setup = {
            .bmRequestType = 0x21,
            .bRequest = HID_REQ_SET_PROTOCOL,
            .wValue = 0,                /* 0 = boot protocol */
            .wIndex = iface,
            .wLength = 0
        };
        if (xhci_control_transfer(ctrl, slot, &setup, 0, 0, false) < 0) {
            xhci_device_slot_cleanup(ctrl, slot);
            return;
        }
        slot->state = ENUM_STATE_WAIT_SET_PROTOCOL;
        return;
    }

    /* Mass storage over Bulk-Only Transport. The subclass is not part of the
     * match: 0x06 (SCSI transparent) is what everything modern reports, and
     * the older ones answer the same READ(10) and WRITE(10) anyway. What
     * matters is the transport, because that is what decides the shape of
     * every exchange after this point. */
    memset(&pick, 0, sizeof(pick));
    if (usb_walk_interface(cfg, len, USB_CLASS_MASS_STORAGE, USB_CLASS_ANY,
                           USB_MSD_PROTOCOL_BOT, &iface,
                           enum_pick_visit, &pick) &&
        pick.bulk_in_dci && pick.bulk_out_dci) {

        slot->driver        = XHCI_DRIVER_STORAGE;
        slot->interface_num = iface;
        slot->ep_bulk_in    = pick.bulk_in_dci;
        slot->ep_bulk_out   = pick.bulk_out_dci;

        if (xhci_ep_prepare(slot, pick.bulk_in_dci, XHCI_EP_TYPE_BULK_IN,
                            pick.bulk_in_addr, pick.bulk_in_mps, 0, 0) != 0 ||
            xhci_ep_prepare(slot, pick.bulk_out_dci, XHCI_EP_TYPE_BULK_OUT,
                            pick.bulk_out_addr, pick.bulk_out_mps, 0, 0) != 0) {
            kprintf("[xHCI] port %u: no memory for the storage endpoints\n",
                    slot->port_num);
            xhci_device_slot_cleanup(ctrl, slot);
            return;
        }

        if (xhci_ep_configure(ctrl, slot) != 0) {
            xhci_device_slot_cleanup(ctrl, slot);
            return;
        }
        slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;
        return;
    }

    /* A hub, which is an ordinary device that happens to have ports. It gets
     * one interrupt endpoint, on which it says when something below it has
     * changed — and nothing else here, because everything a hub is for needs
     * control transfers and this is the interrupt handler. */
    memset(&pick, 0, sizeof(pick));
    if (usb_walk_interface(cfg, len, USB_CLASS_HUB, USB_CLASS_ANY,
                           USB_CLASS_ANY, &iface,
                           enum_pick_visit, &pick) && pick.intr_in_dci) {

        slot->driver          = XHCI_DRIVER_HUB;
        slot->interface_num   = iface;
        slot->ep_interrupt_in = pick.intr_in_dci;

        if (xhci_ep_prepare(slot, pick.intr_in_dci, XHCI_EP_TYPE_INTERRUPT_IN,
                            pick.intr_addr, pick.intr_mps, pick.intr_interval,
                            pick.intr_mps) != 0 ||
            xhci_ep_configure(ctrl, slot) != 0) {
            kprintf("[xHCI] port %u: could not configure the hub\n",
                    slot->port_num);
            xhci_device_slot_cleanup(ctrl, slot);
            return;
        }
        slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;
        return;
    }

    enum_settle_unclaimed(slot);
}

/* The device is configured and the controller knows its endpoints. Whoever
 * claimed it takes it from here. */
static void enum_driver_start(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    switch (slot->driver) {

    case XHCI_DRIVER_KEYBOARD: {
        xhci_endpoint_t* ep = &slot->endpoints[slot->ep_interrupt_in];
        if (xhci_ep_submit(ctrl, slot, slot->ep_interrupt_in,
                           ep->buffer_phys, ep->max_packet) != 0) {
            kprintf("[xHCI] port %u: keyboard endpoint could not be primed\n",
                    slot->port_num);
            xhci_device_slot_cleanup(ctrl, slot);
            return;
        }
        kprintf("[xHCI] port %u: %s-speed keyboard %04x:%04x is live "
                "(slot %u, endpoint %u)\n",
                slot->port_num, speed_name(slot->speed),
                slot->device_desc.idVendor, slot->device_desc.idProduct,
                slot->slot_id, slot->ep_interrupt_in);
        break;
    }

    case XHCI_DRIVER_HUB:
        /* Same reasoning as storage: everything a hub needs doing is a control
         * transfer, and this runs inside the event handler. Saying that there
         * is a hub waiting is the whole of what can be done from here. */
        kprintf("[xHCI] port %u: %s-speed hub %04x:%04x on slot %u\n",
                slot->port_num, speed_name(slot->speed),
                slot->device_desc.idVendor, slot->device_desc.idProduct,
                slot->slot_id);
        xhci_hub_note_work();
        break;

    case XHCI_DRIVER_STORAGE:
        /* Deliberately no SCSI here. Everything in this function runs inside
         * the event handler, and asking a disk how big it is means bulk
         * transfers, and waiting for a bulk transfer means draining the event
         * ring — from inside the drain that called us. The conversation with
         * the disk happens in ordinary kernel context, where somebody wants to
         * read from it; this only records that there is a disk to have it
         * with. */
        kprintf("[xHCI] port %u: %s-speed mass storage %04x:%04x on slot %u "
                "(bulk in %u, out %u)\n",
                slot->port_num, speed_name(slot->speed),
                slot->device_desc.idVendor, slot->device_desc.idProduct,
                slot->slot_id, slot->ep_bulk_in, slot->ep_bulk_out);
        break;

    default:
        break;
    }

    xhci_touch_device_arrived(slot);
}

int xhci_enum_settle(xhci_controller_t* ctrl, uint32_t timeout_ms)
{
    if (!ctrl || !ctrl->initialized) {
        return 0;
    }

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);

    for (;;) {
        xhci_process_events();

        int busy = 0;
        for (int i = 0; i < XHCI_MAX_DEVICE_SLOTS; i++) {
            uint8_t state = device_slots[i].state;
            if (state != ENUM_STATE_IDLE && state != ENUM_STATE_CONFIGURED) {
                busy++;
            }
        }

        if (busy == 0) {
            return 0;
        }
        if ((int64_t)(rdtsc() - deadline) >= 0) {
            return busy;
        }
        cpu_pause();
    }
}

void xhci_enum_for_each_configured(xhci_slot_visitor visit, void* ctx)
{
    if (!visit) {
        return;
    }
    for (int i = 0; i < XHCI_MAX_DEVICE_SLOTS; i++) {
        if (device_slots[i].state == ENUM_STATE_CONFIGURED) {
            visit(ctx, &device_slots[i]);
        }
    }
}

void xhci_enum_watchdog(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->initialized) {
        return;
    }

    uint64_t now = rdtsc();
    uint64_t budget = cpu_ms_to_tsc(XHCI_ENUM_TIMEOUT_MS);

    for (int i = 0; i < XHCI_MAX_DEVICE_SLOTS; i++) {
        struct xhci_device_slot* slot = &device_slots[i];

        uint8_t state = slot->state;
        if (state == ENUM_STATE_IDLE || state == ENUM_STATE_CONFIGURED) {
            continue;
        }
        if (slot->timestamp_started == 0) {
            continue;
        }
        if ((int64_t)(now - slot->timestamp_started) < (int64_t)budget) {
            continue;
        }

        kprintf("[xHCI] port %u: gave up after %u ms while %s\n",
                slot->port_num, XHCI_ENUM_TIMEOUT_MS, enum_state_name(state));
        xhci_device_slot_cleanup(ctrl, slot);
    }
}

void xhci_enum_advance_state(xhci_controller_t* ctrl, uint8_t slot_id, uint8_t completion_code) {
    if (!ctrl) {
        return;
    }

    struct xhci_device_slot* slot = NULL;

    /* Enable Slot completion doesn't carry a valid slot_id in the event yet —
       find the slot waiting for it by state. */
    if (slot_id == 0) {
        spin_lock(&device_slots_lock);
        for (int i = 0; i < XHCI_MAX_DEVICE_SLOTS; i++) {
            if (device_slots[i].state == ENUM_STATE_WAIT_ENABLE_SLOT) {
                slot = &device_slots[i];
                break;
            }
        }
        spin_unlock(&device_slots_lock);
    } else {
        /* For all other commands, slot_id from the completion event is valid. */
        spin_lock(&device_slots_lock);
        for (int i = 0; i < XHCI_MAX_DEVICE_SLOTS; i++) {
            if (device_slots[i].slot_id == slot_id &&
                device_slots[i].state != ENUM_STATE_IDLE) {
                slot = &device_slots[i];
                break;
            }
        }
        /* Fallback: also check for WAIT_ENABLE_SLOT with slot_id still 0. */
        if (!slot) {
            for (int i = 0; i < XHCI_MAX_DEVICE_SLOTS; i++) {
                if (device_slots[i].state == ENUM_STATE_WAIT_ENABLE_SLOT &&
                    device_slots[i].slot_id == 0) {
                    slot = &device_slots[i];
                    break;
                }
            }
        }
        spin_unlock(&device_slots_lock);
    }

    if (!slot) {
        debug_printf("[xHCI ENUM] No slot found for state advance (slot_id=%u)\n", slot_id);
        return;
    }

    if (completion_code != TRB_COMPLETION_SUCCESS) {
        kprintf("[xHCI] port %u: enumeration step %u failed with completion "
                "code %u — releasing the slot\n",
                slot->port_num, slot->state, completion_code);
        xhci_device_slot_cleanup(ctrl, slot);
        return;
    }

    switch (slot->state) {

        case ENUM_STATE_WAIT_ENABLE_SLOT: {
            if (slot_id == 0 || slot_id > ctrl->max_slots) {
                kprintf("[xHCI] Enable Slot returned slot id %u, which this "
                        "controller cannot have\n", slot_id);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->slot_id = slot_id;
            slot->speed = xhci_get_port_speed(ctrl, slot->port_num);

            debug_printf("[xHCI ENUM] Slot enabled: slot_id=%u port=%u speed=%u\n",
                         slot_id, slot->port_num, slot->speed);

            /* The endpoint table is a property of the device, so it comes
             * into being with the device's slot and dies with it. */
            if (xhci_ep_table_alloc(slot) != 0) {
                kprintf("[xHCI] port %u: no memory for the endpoint table\n",
                        slot->port_num);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            /* Allocate EP0 ring now — needed before Address Device. */
            if (xhci_alloc_ep0_ring(ctrl, slot) < 0) {
                debug_printf("[xHCI ENUM] Failed to allocate EP0 ring\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            /* Allocate Device Context — xHCI controller writes here directly. */
            void* dev_ctx_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
            if (!dev_ctx_phys) {
                debug_printf("[xHCI ENUM] Failed to allocate Device Context\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }
            slot->dev_ctx_phys = (uint64_t)dev_ctx_phys;
            slot->dev_ctx = vmm_phys_to_virt((uintptr_t)dev_ctx_phys);

            /* Register in DCBAA. */
            ctrl->dcbaa->device_context_ptrs[slot_id] = slot->dev_ctx_phys;

            enum_address_device(ctrl, slot);
            break;
        }

        case ENUM_STATE_WAIT_ADDRESS_DEVICE: {
            debug_printf("[xHCI ENUM] Device addressed: slot=%u\n", slot_id);
            enum_free_input_ctx(ctrl, slot);

            /* Eight bytes first. That is everything up to and including
             * bMaxPacketSize0, which is the field that decides whether the rest
             * of this conversation can even be held at the packet size the
             * endpoint context currently names. */
            if (enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 8) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Get Device Descriptor\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->state = ENUM_STATE_WAIT_GET_DESC_HEADER;
            break;
        }

        case ENUM_STATE_WAIT_GET_DESC_HEADER: {
            usb_device_desc_t* desc = (usb_device_desc_t*)slot->descriptor_buffer_virt;
            uint8_t mps0 = desc->bMaxPacketSize0;

            /* A SuperSpeed device states the field as an exponent. */
            uint16_t real_mps;
            if (slot->speed == XHCI_PORT_SPEED_SUPER ||
                slot->speed == XHCI_PORT_SPEED_SUPER_10) {
                real_mps = (mps0 <= 15) ? (uint16_t)(1u << mps0) : 512;
            } else {
                real_mps = mps0;
            }

            if (real_mps != 8 && real_mps != 16 && real_mps != 32 &&
                real_mps != 64 && real_mps != 512) {
                kprintf("[xHCI] port %u: device names an impossible EP0 packet "
                        "size (%u) — refusing it\n", slot->port_num, real_mps);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            if (real_mps != slot->ep0_max_packet) {
                debug_printf("[xHCI ENUM] EP0 packet size is %u, not %u — "
                             "correcting\n", real_mps, slot->ep0_max_packet);
                enum_evaluate_ep0(ctrl, slot, real_mps);
                return;
            }

            if (enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 18) < 0) {
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }
            slot->state = ENUM_STATE_WAIT_GET_DESCRIPTOR;
            break;
        }

        case ENUM_STATE_WAIT_EVALUATE_CONTEXT: {
            enum_free_input_ctx(ctrl, slot);

            if (enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 18) < 0) {
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }
            slot->state = ENUM_STATE_WAIT_GET_DESCRIPTOR;
            break;
        }

        case ENUM_STATE_WAIT_GET_DESCRIPTOR: {
            usb_device_desc_t* desc = (usb_device_desc_t*)slot->descriptor_buffer_virt;

            if (!usb_validate_device_desc(desc)) {
                kprintf("[xHCI] port %u: device descriptor is malformed\n",
                        slot->port_num);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            memcpy(&slot->device_desc, desc, sizeof(usb_device_desc_t));

            debug_printf("[xHCI ENUM] Device descriptor: VID=%04x PID=%04x "
                         "Class=%02x MaxPkt=%u\n",
                         desc->idVendor, desc->idProduct, desc->bDeviceClass,
                         desc->bMaxPacketSize0);

            /* Nine bytes of the configuration descriptor: enough to learn how
             * long the whole thing is. Asking for a fixed 64 was a guess that
             * truncates every composite device — and a keyboard that also
             * carries media keys is a composite device, with its boot-keyboard
             * interface described past whatever the guess covered. */
            if (enum_get_descriptor(ctrl, slot, USB_DT_CONFIG, 9) < 0) {
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }
            slot->state = ENUM_STATE_WAIT_GET_CONFIG_HEADER;
            break;
        }

        case ENUM_STATE_WAIT_GET_CONFIG_HEADER: {
            usb_config_desc_t* cfg = (usb_config_desc_t*)slot->descriptor_buffer_virt;

            if (cfg->bDescriptorType != USB_DESC_CONFIGURATION || cfg->bLength < 9) {
                kprintf("[xHCI] port %u: configuration descriptor is malformed\n",
                        slot->port_num);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            uint16_t total = cfg->wTotalLength;
            if (total < 9) {
                total = 9;
            }
            if (total > XHCI_DESC_BUFFER_BYTES) {
                /* One page is what the scratch buffer is. A configuration
                 * larger than that exists in principle and has never been
                 * seen; read what fits and say so rather than overrun. */
                kprintf("[xHCI] port %u: configuration descriptor is %u bytes, "
                        "reading the first %u\n",
                        slot->port_num, total, XHCI_DESC_BUFFER_BYTES);
                total = XHCI_DESC_BUFFER_BYTES;
            }
            slot->config_total_len = total;

            if (enum_get_descriptor(ctrl, slot, USB_DT_CONFIG, total) < 0) {
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }
            slot->state = ENUM_STATE_WAIT_GET_CONFIG_DESC;
            break;
        }

        case ENUM_STATE_WAIT_GET_CONFIG_DESC: {
            usb_config_desc_t* cfg = (usb_config_desc_t*)slot->descriptor_buffer_virt;
            slot->config_value = cfg->bConfigurationValue;

            usb_config_first_interface(slot->descriptor_buffer_virt,
                                       slot->config_total_len,
                                       &slot->interface_class,
                                       &slot->interface_subclass,
                                       &slot->interface_protocol);

            /* Every device gets configured, driver or no driver. Leaving one
             * in the Addressed state is leaving it half spoken to.
             *
             * The device is told which configuration to adopt before the
             * controller is told which endpoints to expect, and that order is
             * deliberate: the endpoints do not exist until the device has
             * selected the configuration that describes them. */
            usb_setup_packet_t setup = {
                .bmRequestType = 0x00,
                .bRequest = USB_REQ_SET_CONFIGURATION,
                .wValue = slot->config_value,
                .wIndex = 0,
                .wLength = 0
            };

            if (xhci_control_transfer(ctrl, slot, &setup, 0, 0, false) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Set Configuration\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->state = ENUM_STATE_WAIT_SET_CONFIGURATION;
            break;
        }

        case ENUM_STATE_WAIT_SET_CONFIGURATION: {
            /* The configuration descriptor is still in the scratch page — Set
             * Configuration carries no data — so this is where the device is
             * matched against the drivers this kernel has, and the endpoints
             * it will actually use are prepared. */
            enum_bind_driver(ctrl, slot);
            break;
        }

        case ENUM_STATE_WAIT_SET_PROTOCOL: {
            usb_setup_packet_t setup = {
                .bmRequestType = 0x21,
                .bRequest = HID_REQ_SET_IDLE,
                .wValue = 0,
                .wIndex = slot->interface_num,
                .wLength = 0
            };

            if (xhci_control_transfer(ctrl, slot, &setup, 0, 0, false) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Set Idle\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->state = ENUM_STATE_WAIT_SET_IDLE;
            break;
        }

        case ENUM_STATE_WAIT_EP0_RESET: {
            /* The halt is gone. The transfer that stalled is still sitting on
             * the ring in front of the controller's dequeue pointer, so tell
             * it where software has actually got to. */
            uint64_t resume = slot->ep0_ring->trbs_phys +
                              (uint64_t)slot->ep0_ring->enqueue_idx *
                              sizeof(xhci_trb_t);

            slot->state = ENUM_STATE_WAIT_EP0_DEQUEUE;

            if (xhci_post_set_tr_dequeue_cmd(ctrl, slot->slot_id, 1,
                    resume | (slot->ep0_ring->cycle_state ? 1u : 0u)) < 0) {
                xhci_device_slot_cleanup(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_EP0_DEQUEUE: {
            /* Back where enumeration was, with the refused step behind us. */
            slot->state = slot->stall_resume;
            xhci_enum_advance_state(ctrl, slot_id, TRB_COMPLETION_SUCCESS);
            break;
        }

        case ENUM_STATE_WAIT_SET_IDLE: {
            if (xhci_ep_configure(ctrl, slot) != 0) {
                kprintf("[xHCI] port %u: could not configure the keyboard "
                        "endpoint\n", slot->port_num);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }
            slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;
            break;
        }

        case ENUM_STATE_WAIT_CONFIGURE_ENDPOINT: {
            enum_free_input_ctx(ctrl, slot);
            slot->ep_pending_add = 0;
            slot->state = ENUM_STATE_CONFIGURED;
            enum_driver_start(ctrl, slot);
            break;
        }

        default:
            debug_printf("[xHCI ENUM] Unexpected state %u for slot %u\n",
                         slot->state, slot_id);
            break;
    }
}
