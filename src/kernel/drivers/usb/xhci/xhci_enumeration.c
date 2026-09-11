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
#include "boardroom.h"

void xhci_enumeration_init(void) {
}

int xhci_slots_attach(xhci_controller_t* ctrl)
{
    if (!ctrl || ctrl->max_slots == 0) {
        return -1;
    }
    if (ctrl->slots) {
        return 0;
    }

    uint32_t count = ctrl->max_slots;
    ctrl->slots = (struct xhci_device_slot*)
                  kmalloc(sizeof(struct xhci_device_slot) * count);
    if (!ctrl->slots) {
        return -1;
    }
    ctrl->by_id = (struct xhci_device_slot**)
                  kmalloc(sizeof(struct xhci_device_slot*) * (count + 1u));
    if (!ctrl->by_id) {
        xhci_slots_release(ctrl);
        return -1;
    }

    memset(ctrl->slots, 0, sizeof(struct xhci_device_slot) * count);
    memset(ctrl->by_id, 0, sizeof(struct xhci_device_slot*) * (count + 1u));
    spinlock_init(&ctrl->slots_lock);
    ctrl->slot_count = (uint8_t)count;
    return 0;
}

void xhci_slots_release(xhci_controller_t* ctrl)
{
    if (!ctrl) {
        return;
    }
    if (ctrl->by_id) {
        kfree(ctrl->by_id);
        ctrl->by_id = NULL;
    }
    if (ctrl->slots) {
        kfree(ctrl->slots);
        ctrl->slots = NULL;
    }
    ctrl->slot_count = 0;
}

static void slot_take_down(xhci_controller_t* ctrl, xhci_device_slot_t* slot);


static bool slot_is_live(const struct xhci_device_slot* s) {
    uint8_t state = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    return state != ENUM_STATE_IDLE && state != ENUM_STATE_RETIRING;
}

uint32_t xhci_slot_epoch(const xhci_device_slot_t* slot)
{
    if (!slot) {
        return 0;
    }
    return __atomic_load_n(&slot->epoch, __ATOMIC_ACQUIRE);
}

bool xhci_slot_is_live(const xhci_device_slot_t* slot)
{
    return slot ? slot_is_live(slot) : false;
}

bool xhci_slot_still_is(const xhci_device_slot_t* slot, uint32_t epoch)
{
    if (!slot) {
        return false;
    }
    return __atomic_load_n(&slot->epoch, __ATOMIC_ACQUIRE) == epoch &&
           slot_is_live(slot);
}

static struct xhci_device_slot* find_free_slot(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->slots) {
        return NULL;
    }
    spin_lock(&ctrl->slots_lock);
    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        if (ctrl->slots[i].state == ENUM_STATE_IDLE) {
            __atomic_add_fetch(&ctrl->slots[i].epoch, 1, __ATOMIC_ACQ_REL);
            ctrl->slots[i].state = ENUM_STATE_CLAIMING;
            ctrl->slots[i].ctrl  = ctrl;
            spin_unlock(&ctrl->slots_lock);
            return &ctrl->slots[i];
        }
    }
    spin_unlock(&ctrl->slots_lock);
    return NULL;
}

static void slot_wipe(struct xhci_device_slot* slot)
{
    uint32_t epoch = slot->epoch;
    uint8_t  state = slot->state;
    xhci_controller_t* owner = slot->ctrl;

    if (owner && owner->by_id && slot->slot_id != 0 &&
        slot->slot_id <= owner->slot_count) {
        spin_lock(&owner->slots_lock);
        if (owner->by_id[slot->slot_id] == slot) {
            owner->by_id[slot->slot_id] = NULL;
        }
        spin_unlock(&owner->slots_lock);
    }

    memset(slot, 0, sizeof(*slot));
    slot->ctrl = owner;

    slot->epoch = epoch;
    slot->state = state;
}

static struct xhci_device_slot* find_slot_by_port(xhci_controller_t* ctrl,
                                                 uint8_t port) {
    if (!ctrl || !ctrl->slots) {
        return NULL;
    }
    spin_lock(&ctrl->slots_lock);
    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        if (ctrl->slots[i].port_num == port && slot_is_live(&ctrl->slots[i])) {
            spin_unlock(&ctrl->slots_lock);
            return &ctrl->slots[i];
        }
    }
    spin_unlock(&ctrl->slots_lock);
    return NULL;
}

xhci_device_slot_t* xhci_get_device_slot(xhci_controller_t* ctrl, uint8_t slot_id) {
    if (!ctrl || !ctrl->by_id || slot_id == 0 || slot_id > ctrl->slot_count) {
        return NULL;
    }

    spin_lock(&ctrl->slots_lock);
    struct xhci_device_slot* slot = ctrl->by_id[slot_id];
    if (slot && !slot_is_live(slot)) {
        slot = NULL;
    }
    spin_unlock(&ctrl->slots_lock);
    return slot;
}

xhci_device_slot_t* xhci_get_device_slot_by_port(xhci_controller_t* ctrl,
                                                 uint8_t port) {
    return find_slot_by_port(ctrl, port);
}

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

static int enum_begin_slot(xhci_controller_t* ctrl, struct xhci_device_slot* slot) {
    slot->state = ENUM_STATE_WAIT_ENABLE_SLOT;

    uint8_t slot_type = ctrl->port_slot_type[slot->port_num];

    if (xhci_post_enable_slot_cmd(ctrl, slot, slot_type) < 0) {
        debug_printf("[xHCI ENUM] Failed to post Enable Slot command\n");
        slot->state = ENUM_STATE_IDLE;
        slot->port_num = 0;
        return -1;
    }
    return 0;
}

static int enum_start(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    slot->timestamp_started = rdtsc();

    if (slot->depth != 0) {
        return enum_begin_slot(ctrl, slot) == 0 ? 0 : -4;
    }

    __atomic_store_n(&slot->state, (uint8_t)ENUM_STATE_WAIT_PORT_RESET,
                     __ATOMIC_RELEASE);

    uint8_t kind = XHCI_PORT_RESET_NONE;
    slot->reset_kind = XHCI_PORT_RESET_NONE;

    int reset = xhci_port_begin_reset(ctrl, slot->port_num, &kind);
    slot->reset_kind = kind;
    if (reset < 0) {
        debug_printf("[xHCI ENUM] Port %u could not be reset (%d)\n",
                     slot->port_num, reset);
        xhci_slot_retire(ctrl, slot);
        return -6;
    }
    return 0;
}

static int enum_admit(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    __atomic_store_n(&slot->state, (uint8_t)ENUM_STATE_QUEUED, __ATOMIC_RELEASE);
    xhci_enum_pump(ctrl);
    return 0;
}

void xhci_enum_pump(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->initialized) {
        return;
    }

    struct xhci_device_slot* next = NULL;
    struct xhci_device_slot* recovered = NULL;

    spin_lock(&ctrl->slots_lock);

    struct xhci_device_slot* active = ctrl->enum_active;

    if (active &&
        __atomic_load_n(&active->state, __ATOMIC_ACQUIRE) ==
            ENUM_STATE_WAIT_RESET_RECOVERY &&
        (int64_t)(rdtsc() - active->recovery_due) >= 0) {
        __atomic_store_n(&active->state, (uint8_t)ENUM_STATE_CLAIMING,
                         __ATOMIC_RELEASE);
        recovered = active;
    }

    if (recovered) {
        spin_unlock(&ctrl->slots_lock);
        enum_begin_slot(ctrl, recovered);
        return;
    }

    if (active) {
        uint8_t state = __atomic_load_n(&active->state, __ATOMIC_ACQUIRE);
        bool still_going = (state != ENUM_STATE_IDLE &&
                            state != ENUM_STATE_QUEUED &&
                            state != ENUM_STATE_CONFIGURED &&
                            state != ENUM_STATE_RETIRING);
        if (still_going) {
            spin_unlock(&ctrl->slots_lock);
            return;
        }
        ctrl->enum_active = NULL;
    }

    bool next_is_retry = true;
    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        if (__atomic_load_n(&ctrl->slots[i].state, __ATOMIC_ACQUIRE) !=
                ENUM_STATE_QUEUED) {
            continue;
        }
        uint8_t born = ctrl->slots[i].born_port;
        bool retry = (born != 0 && born <= ctrl->max_ports &&
                      ctrl->enum_attempts[born] > 0);

        if (!next || (next_is_retry && !retry)) {
            next          = &ctrl->slots[i];
            next_is_retry = retry;
            if (!retry) {
                break;
            }
        }
    }
    if (next) {
        ctrl->enum_active = next;
    }

    spin_unlock(&ctrl->slots_lock);

    if (next) {
        enum_start(ctrl, next);
    }
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

    if (find_slot_by_port(ctrl, port)) {
        return -4;
    }

    if (ctrl->enum_attempts[port] >= XHCI_ENUM_ATTEMPTS) {
        if (ctrl->enum_attempts[port] == XHCI_ENUM_ATTEMPTS) {
            ctrl->enum_attempts[port]++;
            kprintf("[xHCI %s] port %u: %u attempts and the device on it never "
                    "came up — leaving the port alone\n",
                    ctrl->name, port, XHCI_ENUM_ATTEMPTS);
        }
        return -8;
    }

    struct xhci_device_slot* slot = find_free_slot(ctrl);
    if (!slot) {
        kprintf("[xHCI] no free device slot for port %u\n", port);
        return -5;
    }

    slot_wipe(slot);
    slot->ctrl = ctrl;
    slot->port_num = port;
    slot->born_port = port;
    slot->timestamp_started = rdtsc();

    debug_printf("[xHCI ENUM] Starting enumeration for port %u\n", port);

    return enum_admit(ctrl, slot);
}

xhci_device_slot_t* xhci_get_device_slot_by_id(xhci_controller_t* ctrl,
                                               uint8_t slot_id)
{
    return xhci_get_device_slot(ctrl, slot_id);
}

bool xhci_slot_enter(xhci_device_slot_t* slot)
{
    if (!slot) {
        return false;
    }

    __atomic_fetch_add(&slot->visitors, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != ENUM_STATE_CONFIGURED) {
        __atomic_fetch_sub(&slot->visitors, 1, __ATOMIC_ACQ_REL);
        return false;
    }
    return true;
}

void xhci_slot_leave(xhci_device_slot_t* slot)
{
    if (!slot) {
        return;
    }
    __atomic_fetch_sub(&slot->visitors, 1, __ATOMIC_ACQ_REL);
}

void xhci_slot_retire(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    if (!slot) {
        return;
    }

    uint8_t was = __atomic_exchange_n(&slot->state,
                                      (uint8_t)ENUM_STATE_RETIRING,
                                      __ATOMIC_ACQ_REL);
    if (was == ENUM_STATE_IDLE || was == ENUM_STATE_RETIRING) {
        __atomic_store_n(&slot->state, was, __ATOMIC_RELEASE);
        return;
    }

    slot->port_num = 0;
    slot->retire_started = rdtsc();
    slot->retire_warned = false;

    if (ctrl && slot->slot_id != 0) {
        xhci_post_disable_slot_cmd(ctrl, slot, slot->slot_id);
    }

    __atomic_store_n(&ctrl->retire_pending, 1, __ATOMIC_RELEASE);
}

int xhci_slot_service(xhci_controller_t* ctrl)
{
    if (!ctrl) {
        return 0;
    }
    if (__atomic_load_n(&ctrl->retire_pending, __ATOMIC_ACQUIRE) == 0) {
        return 0;
    }
    if (__atomic_exchange_n(&ctrl->retire_busy, 1, __ATOMIC_ACQUIRE) != 0) {
        return 0;
    }

    __atomic_store_n(&ctrl->retire_pending, 0, __ATOMIC_RELEASE);

    int done = 0;
    bool more = false;

    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        struct xhci_device_slot* slot = &ctrl->slots[i];
        if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != ENUM_STATE_RETIRING) {
            continue;
        }

        uint32_t inside = __atomic_load_n(&slot->visitors, __ATOMIC_ACQUIRE);
        bool controller_busy = xhci_command_pending_for(ctrl, slot);

        if (inside != 0 || controller_busy) {
            if (!slot->retire_warned &&
                (int64_t)(rdtsc() - slot->retire_started) >
                (int64_t)cpu_ms_to_tsc(XHCI_RETIRE_PATIENCE_MS)) {
                slot->retire_warned = true;
                kprintf("[xHCI] slot %u has been leaving for %u ms — %u caller(s) "
                        "still inside it%s\n",
                        slot->slot_id, XHCI_RETIRE_PATIENCE_MS, inside,
                        controller_busy ? " and the controller has not answered"
                                        : "");
            }
            more = true;
            continue;
        }

        slot_take_down(ctrl, slot);
        done++;
    }

    if (more) {
        __atomic_store_n(&ctrl->retire_pending, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&ctrl->retire_busy, 0, __ATOMIC_RELEASE);
    return done;
}

bool xhci_slot_retire_pending(void)
{
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_controller_t* c = xhci_controller_at(i);
        if (c && __atomic_load_n(&c->retire_pending, __ATOMIC_ACQUIRE) != 0) {
            return true;
        }
    }
    return false;
}

void xhci_slot_service_if_pending(void)
{
    if (!xhci_slot_retire_pending()) {
        return;
    }
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_slot_service(xhci_controller_at(i));
    }
}

int xhci_enumerate_behind_hub(xhci_controller_t* ctrl,
                              xhci_device_slot_t* hub,
                              uint8_t hub_port, uint8_t speed)
{
    if (!ctrl || !ctrl->running || !hub || hub_port == 0) {
        return -1;
    }

    if (hub_port > 15) {
        kprintf("[xHCI] hub slot %u has no port %u the bus could name — four "
                "bits per tier stop at 15\n", hub->slot_id, hub_port);
        return -5;
    }

    if (hub->depth >= 5) {
        kprintf("[xHCI] a device on hub slot %u port %u is six hubs deep, "
                "which is one more than the bus can address\n",
                hub->slot_id, hub_port);
        return -2;
    }

    struct xhci_device_slot* slot = find_free_slot(ctrl);
    if (!slot) {
        kprintf("[xHCI] no free device slot for hub slot %u port %u\n",
                hub->slot_id, hub_port);
        return -3;
    }

    slot_wipe(slot);
    slot->ctrl = ctrl;
    slot->port_num = hub->port_num;
    slot->speed = speed;
    slot->reset_kind = XHCI_PORT_RESET_HUB;

    slot->route_string   = hub->route_string | ((uint32_t)hub_port << (4 * hub->depth));
    slot->depth          = (uint8_t)(hub->depth + 1);
    slot->parent_slot_id = hub->slot_id;
    slot->parent_port    = hub_port;

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

    return enum_admit(ctrl, slot);
}

void xhci_enum_port_reset_done(xhci_controller_t* ctrl, uint8_t port)
{
    if (!ctrl) {
        return;
    }

    struct xhci_device_slot* slot = find_slot_by_port(ctrl, port);
    if (!slot || slot->state != ENUM_STATE_WAIT_PORT_RESET) {
        return;
    }

    if (!xhci_port_reset_finished(ctrl, port)) {
        if (slot->reset_kind == XHCI_PORT_RESET_HOT &&
            xhci_port_protocol(ctrl, port) >= 3 &&
            xhci_port_warm_reset(ctrl, port) == 0) {
            slot->reset_kind = XHCI_PORT_RESET_WARM;
            slot->timestamp_started = rdtsc();
            return;
        }

        kprintf("[xHCI %s] port %u: the %s ended with the port still disabled "
                "(PORTSC 0x%08x) — no usable device\n",
                ctrl->name, port, xhci_port_reset_kind_name(slot->reset_kind),
                xhci_get_port_status(ctrl, port));
        xhci_slot_retire(ctrl, slot);
        return;
    }

    slot->reset_took_ms = (uint32_t)cpu_tsc_to_ms(rdtsc() - slot->timestamp_started);

    uint32_t portsc = xhci_get_port_status(ctrl, port);
    kprintf("[xHCI %s] port %u: %s finished in %u ms — PORTSC 0x%08x, link %u, "
            "speed %u\n",
            ctrl->name, port, xhci_port_reset_kind_name(slot->reset_kind),
            slot->reset_took_ms, portsc, XHCI_PORTSC_PLS(portsc),
            (unsigned)XHCI_PORTSC_SPEED(portsc));

    slot->recovery_due = rdtsc() + cpu_ms_to_tsc(XHCI_PORT_RESET_RECOVERY_MS);
    __atomic_store_n(&slot->state, (uint8_t)ENUM_STATE_WAIT_RESET_RECOVERY,
                     __ATOMIC_RELEASE);
}

static void slot_take_down(xhci_controller_t* ctrl, xhci_device_slot_t* slot) {
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

    if (ctrl && slot->slot_id > 0 && slot->slot_id <= ctrl->max_slots &&
        ctrl->dcbaa->device_context_ptrs[slot->slot_id] == slot->dev_ctx_phys) {
        ctrl->dcbaa->device_context_ptrs[slot->slot_id] = 0;
    }

    if (slot->dev_ctx_phys) {
        pmm_free((void*)slot->dev_ctx_phys, 1);
        slot->dev_ctx_phys = 0;
        slot->dev_ctx = NULL;
    }

    uint8_t retry_port = (!slot->ever_configured && ctrl) ? slot->born_port : 0;

    if (ctrl && ctrl->by_id && slot->slot_id != 0 &&
        slot->slot_id <= ctrl->slot_count) {
        spin_lock(&ctrl->slots_lock);
        if (ctrl->by_id[slot->slot_id] == slot) {
            ctrl->by_id[slot->slot_id] = NULL;
        }
        spin_unlock(&ctrl->slots_lock);
    }

    slot->slot_id = 0;
    slot->port_num = 0;
    slot->born_port = 0;
    slot->state = ENUM_STATE_IDLE;
    slot->timestamp_started = 0;
    slot->driver = XHCI_DRIVER_NONE;

    if (retry_port && retry_port <= ctrl->max_ports &&
        ctrl->enum_attempts[retry_port] < XHCI_ENUM_ATTEMPTS &&
        xhci_port_has_device(ctrl, retry_port)) {
        ctrl->enum_attempts[retry_port]++;
        kprintf("[xHCI %s] port %u: trying again on a freshly reset port "
                "(attempt %u of %u)\n", ctrl->name, retry_port,
                ctrl->enum_attempts[retry_port] + 1, XHCI_ENUM_ATTEMPTS + 1);
        xhci_enumerate_device(ctrl, retry_port);
    }
}

static void enum_address_device(xhci_controller_t* ctrl, struct xhci_device_slot* slot,
                                uint16_t max_packet, bool block_set_address)
{
    uint32_t pages = xhci_input_ctx_pages(ctrl);
    void* input_ctx_phys = pmm_alloc_zero(pages, PHYS_TAG_DMA32);
    if (!input_ctx_phys) {
        kprintf("[xHCI] out of memory addressing the device on port %u\n",
                slot->port_num);
        xhci_slot_retire(ctrl, slot);
        return;
    }
    slot->input_ctx_phys = (uint64_t)input_ctx_phys;

    uint8_t* input_base = (uint8_t*)vmm_phys_to_virt((uintptr_t)input_ctx_phys);

    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)input_base;
    icc->add_context_flags = (1 << 0) | (1 << 1);

    xhci_slot_context_t* slot_ctx =
        (xhci_slot_context_t*)(input_base + ctrl->context_size);
    xhci_fill_slot_context(slot_ctx, slot);

    slot->ep0_max_packet = max_packet;
    uint64_t resume = slot->ep0_ring->trbs_phys +
                      (uint64_t)slot->ep0_ring->enqueue_idx * sizeof(xhci_trb_t);
    resume |= slot->ep0_ring->cycle_state ? 1u : 0u;

    xhci_endpoint_context_t* ep0_ctx =
        (xhci_endpoint_context_t*)(input_base + ctrl->context_size * 2);
    xhci_init_ep0_context(ep0_ctx, resume, max_packet);

    slot->state = block_set_address ? ENUM_STATE_WAIT_ADDRESS_DEVICE_BSR
                                    : ENUM_STATE_WAIT_ADDRESS_DEVICE;

    if (xhci_post_address_device_cmd(ctrl, slot, slot->slot_id,
                                     (uint64_t)input_ctx_phys,
                                     block_set_address) < 0) {
        debug_printf("[xHCI ENUM] Failed to post Address Device\n");
        xhci_slot_retire(ctrl, slot);
    }
}

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

    if (slot->descriptor_buffer_virt) {
        memset(slot->descriptor_buffer_virt, 0, length);
    }

    return xhci_control_transfer(ctrl, slot, &setup,
                                 slot->descriptor_buffer_phys, length, true);
}

static bool enum_got_it_all(xhci_controller_t* ctrl, struct xhci_device_slot* slot,
                            uint16_t wanted)
{
    if (slot->ctl_received >= wanted) {
        return true;
    }

    kprintf("[xHCI %s] port %u: %s — asked for %u bytes and %u came back\n",
            ctrl->name, slot->port_num, xhci_enum_state_name(slot->state),
            wanted, slot->ctl_received);
    return false;
}

static void enum_ask_again(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    if (!xhci_enum_step_can_be_asked_again(slot->state) ||
        slot->step_retry >= XHCI_STEP_RETRIES) {
        kprintf("[xHCI %s] port %u: %s never delivered — releasing the slot\n",
                ctrl->name, slot->port_num, xhci_enum_state_name(slot->state));
        xhci_slot_retire(ctrl, slot);
        return;
    }

    slot->step_retry++;
    kprintf("[xHCI %s] port %u: asking again (attempt %u of %u)\n",
            ctrl->name, slot->port_num, slot->step_retry, XHCI_STEP_RETRIES);

    xhci_enum_recover_ep0(ctrl, slot, slot->state, true);
}

static void enum_free_input_ctx(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    if (slot->input_ctx_phys) {
        pmm_free((void*)slot->input_ctx_phys, xhci_input_ctx_pages(ctrl));
        slot->input_ctx_phys = 0;
    }
}

static void enum_settle_unclaimed(struct xhci_device_slot* slot,
                                  const void* cfg, uint16_t cfg_len)
{
    uint8_t isoch = usb_count_isoch_endpoints(cfg, cfg_len);
    if (isoch != 0) {
        kprintf("[xHCI] port %u: it has %u isochronous endpoint(s), and this "
                "kernel configures none — that part of it is unused\n",
                slot->port_num, isoch);
    }

    kprintf("[xHCI] port %u: %s-speed device %04x:%04x class %02x/%02x/%02x "
            "configured, no driver claims it\n",
            slot->port_num, speed_name(slot->speed),
            slot->device_desc.idVendor, slot->device_desc.idProduct,
            slot->interface_class, slot->interface_subclass,
            slot->interface_protocol);
    slot->ever_configured = true;
    if (slot->ctrl && slot->born_port) {
        slot->ctrl->enum_attempts[slot->born_port] = 0;
    }
    slot->state = ENUM_STATE_CONFIGURED;

    xhci_touch_device_arrived(slot);
}

bool xhci_enum_stall_is_tolerable(uint8_t state)
{
    return state == ENUM_STATE_WAIT_SET_PROTOCOL ||
           state == ENUM_STATE_WAIT_SET_IDLE;
}

bool xhci_enum_fault_is_retryable(uint8_t completion_code)
{
    return completion_code == TRB_COMPLETION_STALL ||
           completion_code == TRB_COMPLETION_BABBLE ||
           completion_code == TRB_COMPLETION_USB_TRANS_ERR ||
           completion_code == TRB_COMPLETION_SPLIT_TRANS_ERR;
}

static int enum_reissue(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    switch (slot->state) {

    case ENUM_STATE_WAIT_GET_DESC_HEADER:
        return enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 8);

    case ENUM_STATE_WAIT_GET_DESCRIPTOR:
        return enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 18);

    case ENUM_STATE_WAIT_GET_CONFIG_HEADER:
        return enum_get_descriptor(ctrl, slot, USB_DT_CONFIG, 9);

    case ENUM_STATE_WAIT_GET_CONFIG_DESC:
        return enum_get_descriptor(ctrl, slot, USB_DT_CONFIG,
                                   slot->config_total_len);

    case ENUM_STATE_WAIT_SET_CONFIGURATION: {
        usb_setup_packet_t setup = {
            .bmRequestType = 0x00,
            .bRequest = USB_REQ_SET_CONFIGURATION,
            .wValue = slot->config_value,
            .wIndex = 0,
            .wLength = 0
        };
        return xhci_control_transfer(ctrl, slot, &setup, 0, 0, false);
    }

    case ENUM_STATE_WAIT_SET_PROTOCOL: {
        usb_setup_packet_t setup = {
            .bmRequestType = 0x21,
            .bRequest = HID_REQ_SET_PROTOCOL,
            .wValue = 0,
            .wIndex = slot->interface_num,
            .wLength = 0
        };
        return xhci_control_transfer(ctrl, slot, &setup, 0, 0, false);
    }

    case ENUM_STATE_WAIT_SET_IDLE: {
        usb_setup_packet_t setup = {
            .bmRequestType = 0x21,
            .bRequest = HID_REQ_SET_IDLE,
            .wValue = 0,
            .wIndex = slot->interface_num,
            .wLength = 0
        };
        return xhci_control_transfer(ctrl, slot, &setup, 0, 0, false);
    }

    default:
        return -1;
    }
}

bool xhci_enum_step_can_be_asked_again(uint8_t state)
{
    switch (state) {
        case ENUM_STATE_WAIT_GET_DESC_HEADER:
        case ENUM_STATE_WAIT_GET_DESCRIPTOR:
        case ENUM_STATE_WAIT_GET_CONFIG_HEADER:
        case ENUM_STATE_WAIT_GET_CONFIG_DESC:
        case ENUM_STATE_WAIT_SET_CONFIGURATION:
        case ENUM_STATE_WAIT_SET_PROTOCOL:
        case ENUM_STATE_WAIT_SET_IDLE:
            return true;
        default:
            return false;
    }
}

void xhci_enum_recover_ep0(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                           uint8_t resume_at, bool ask_again)
{
    if (!ctrl || !slot) {
        return;
    }

    slot->step_resume  = resume_at;
    slot->step_reissue = ask_again;
    slot->state = ENUM_STATE_WAIT_EP0_RESET;

    if (xhci_post_reset_endpoint_cmd(ctrl, slot, slot->slot_id, 1) < 0) {
        xhci_slot_retire(ctrl, slot);
    }
}

static void enum_resume_after_recovery(xhci_controller_t* ctrl,
                                       struct xhci_device_slot* slot,
                                       uint8_t slot_id)
{
    slot->state = slot->step_resume;

    if (!slot->step_reissue) {
        xhci_enum_advance_state(ctrl, slot, slot_id, TRB_COMPLETION_SUCCESS);
        return;
    }

    slot->step_reissue = false;
    if (enum_reissue(ctrl, slot) < 0) {
        kprintf("[xHCI %s] port %u: %s could not be asked again — releasing "
                "the slot\n", ctrl->name, slot->port_num,
                xhci_enum_state_name(slot->state));
        xhci_slot_retire(ctrl, slot);
    }
}

const char* xhci_enum_state_name(uint8_t state) {
    switch (state) {
        case ENUM_STATE_IDLE:                   return "idle";
        case ENUM_STATE_CLAIMING:               return "claiming a slot";
        case ENUM_STATE_QUEUED:                 return "waiting its turn on the bus";
        case ENUM_STATE_WAIT_PORT_RESET:        return "waiting for the port reset";
        case ENUM_STATE_WAIT_RESET_RECOVERY:    return "letting the device come up after its reset";
        case ENUM_STATE_WAIT_ENABLE_SLOT:       return "waiting for Enable Slot";
        case ENUM_STATE_WAIT_ADDRESS_DEVICE_BSR:return "waiting for Address Device, with the bus left alone";
        case ENUM_STATE_WAIT_ADDRESS_DEVICE:    return "waiting for Address Device";
        case ENUM_STATE_WAIT_GET_DESC_HEADER:   return "reading the first 8 descriptor bytes";
        case ENUM_STATE_WAIT_GET_DESCRIPTOR:    return "reading the device descriptor";
        case ENUM_STATE_WAIT_GET_CONFIG_HEADER: return "reading the configuration header";
        case ENUM_STATE_WAIT_GET_CONFIG_DESC:   return "reading the configuration";
        case ENUM_STATE_WAIT_SET_CONFIGURATION: return "waiting for Set Configuration";
        case ENUM_STATE_WAIT_SET_PROTOCOL:      return "waiting for Set Protocol";
        case ENUM_STATE_WAIT_SET_IDLE:          return "waiting for Set Idle";
        case ENUM_STATE_WAIT_EP0_RESET:         return "clearing a stalled control pipe";
        case ENUM_STATE_WAIT_EP0_STOP:          return "taking back a control transfer that was never answered";
        case ENUM_STATE_WAIT_EP0_DEQUEUE:       return "repositioning the control ring";
        case ENUM_STATE_WAIT_CONFIGURE_ENDPOINT:return "waiting for Configure Endpoint";
        case ENUM_STATE_CONFIGURED:             return "configured";
        default:                                return "?";
    }
}

typedef struct {
    usb_endpoint_info_t intr_in, bulk_in, bulk_out;
    uint8_t  intr_in_dci, bulk_in_dci, bulk_out_dci;
} enum_pick_t;

static bool enum_pick_visit(void* ctx, const usb_endpoint_info_t* ep)
{
    enum_pick_t* p = (enum_pick_t*)ctx;
    bool in = (ep->addr & 0x80) != 0;

    switch (ep->attributes & 0x03) {
        case USB_EP_XFER_INTERRUPT:
            if (in && !p->intr_in_dci) {
                p->intr_in_dci = xhci_dci_of(ep->addr);
                p->intr_in     = *ep;
            }
            break;
        case USB_EP_XFER_BULK:
            if (in && !p->bulk_in_dci) {
                p->bulk_in_dci = xhci_dci_of(ep->addr);
                p->bulk_in     = *ep;
            } else if (!in && !p->bulk_out_dci) {
                p->bulk_out_dci = xhci_dci_of(ep->addr);
                p->bulk_out     = *ep;
            }
            break;
        default:
            break;
    }
    return true;
}

static void enum_bind_driver(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    void*    cfg = slot->descriptor_buffer_virt;
    uint16_t len = slot->config_total_len;
    uint8_t  iface = 0;
    enum_pick_t pick;

    memset(&pick, 0, sizeof(pick));
    if (usb_walk_interface(cfg, len, USB_HID_CLASS, USB_HID_SUBCLASS_BOOT,
                           USB_HID_PROTOCOL_KEYBOARD, &iface,
                           enum_pick_visit, &pick) && pick.intr_in_dci) {

        slot->driver          = XHCI_DRIVER_KEYBOARD;
        slot->interface_num   = iface;
        slot->ep_interrupt_in = pick.intr_in_dci;

        if (xhci_ep_prepare(slot, pick.intr_in_dci, XHCI_EP_TYPE_INTERRUPT_IN,
                            &pick.intr_in, pick.intr_in.max_packet) != 0) {
            kprintf("[xHCI] port %u: no memory for the keyboard endpoint\n",
                    slot->port_num);
            xhci_slot_retire(ctrl, slot);
            return;
        }

        usb_setup_packet_t setup = {
            .bmRequestType = 0x21,
            .bRequest = HID_REQ_SET_PROTOCOL,
            .wValue = 0,
            .wIndex = iface,
            .wLength = 0
        };
        slot->state = ENUM_STATE_WAIT_SET_PROTOCOL;
        if (xhci_control_transfer(ctrl, slot, &setup, 0, 0, false) < 0) {
            xhci_slot_retire(ctrl, slot);
        }
        return;
    }

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
                            &pick.bulk_in, 0) != 0 ||
            xhci_ep_prepare(slot, pick.bulk_out_dci, XHCI_EP_TYPE_BULK_OUT,
                            &pick.bulk_out, 0) != 0) {
            kprintf("[xHCI] port %u: no memory for the storage endpoints\n",
                    slot->port_num);
            xhci_slot_retire(ctrl, slot);
            return;
        }

        slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;
        if (xhci_ep_configure(ctrl, slot) != 0) {
            xhci_slot_retire(ctrl, slot);
        }
        return;
    }

    memset(&pick, 0, sizeof(pick));
    if (usb_walk_interface(cfg, len, USB_CLASS_HUB, USB_CLASS_ANY,
                           USB_CLASS_ANY, &iface,
                           enum_pick_visit, &pick) && pick.intr_in_dci) {

        slot->driver          = XHCI_DRIVER_HUB;
        slot->interface_num   = iface;
        slot->ep_interrupt_in = pick.intr_in_dci;

        slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;
        if (xhci_ep_prepare(slot, pick.intr_in_dci, XHCI_EP_TYPE_INTERRUPT_IN,
                            &pick.intr_in, pick.intr_in.max_packet) != 0 ||
            xhci_ep_configure(ctrl, slot) != 0) {
            kprintf("[xHCI] port %u: could not configure the hub\n",
                    slot->port_num);
            xhci_slot_retire(ctrl, slot);
        }
        return;
    }

    enum_settle_unclaimed(slot, cfg, len);
}

static void enum_driver_start(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    switch (slot->driver) {

    case XHCI_DRIVER_KEYBOARD: {
        xhci_endpoint_t* ep = &slot->endpoints[slot->ep_interrupt_in];
        if (xhci_ep_submit(ctrl, slot, slot->ep_interrupt_in,
                           ep->buffer_phys, ep->max_packet) != 0) {
            kprintf("[xHCI] port %u: keyboard endpoint could not be primed\n",
                    slot->port_num);
            xhci_slot_retire(ctrl, slot);
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
        kprintf("[xHCI] port %u: %s-speed hub %04x:%04x on slot %u\n",
                slot->port_num, speed_name(slot->speed),
                slot->device_desc.idVendor, slot->device_desc.idProduct,
                slot->slot_id);
        xhci_hub_note_work();
        break;

    case XHCI_DRIVER_STORAGE:
        kprintf("[xHCI] port %u: %s-speed mass storage %04x:%04x on slot %u "
                "(bulk in %u, out %u)\n",
                slot->port_num, speed_name(slot->speed),
                slot->device_desc.idVendor, slot->device_desc.idProduct,
                slot->slot_id, slot->ep_bulk_in, slot->ep_bulk_out);

        BoardroomNoteArrival();
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

        xhci_check_command_timeouts(ctrl);

        xhci_command_abort_if_wanted(ctrl);

        xhci_enum_watchdog(ctrl);
        xhci_enum_pump(ctrl);

        xhci_slot_service(ctrl);

        int busy = 0;
        for (uint32_t i = 0; i < ctrl->slot_count; i++) {
            uint8_t state = __atomic_load_n(&ctrl->slots[i].state,
                                            __ATOMIC_ACQUIRE);
            if (state != ENUM_STATE_IDLE && state != ENUM_STATE_CONFIGURED &&
                state != ENUM_STATE_RETIRING) {
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

void xhci_enum_for_each_configured(xhci_controller_t* ctrl,
                                   xhci_slot_visitor visit, void* ctx)
{
    if (!visit || !ctrl) {
        return;
    }
    if (!ctrl->slots) {
        return;
    }
    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        if (ctrl->slots[i].state == ENUM_STATE_CONFIGURED) {
            visit(ctx, &ctrl->slots[i]);
        }
    }
}

static void xhci_report_stuck(xhci_controller_t* ctrl,
                              struct xhci_device_slot* slot, uint8_t state)
{
    uint8_t  cmd_type = 0;
    uint32_t cmd_age  = 0;
    bool     waiting  = xhci_command_oldest_for(ctrl, slot, &cmd_type, &cmd_age);

    kprintf("[xHCI %s] port %u (slot %u): gave up after %u ms while %s\n",
            ctrl->name, slot->port_num, slot->slot_id, XHCI_ENUM_TIMEOUT_MS,
            xhci_enum_state_name(state));

    kprintf("[xHCI %s]   the controller says: slot %s, address %u; "
            "PORTSC 0x%08x; %u interrupt(s) so far\n",
            ctrl->name,
            xhci_slot_state_name(xhci_slot_context_state(slot)),
            xhci_slot_context_address(slot),
            xhci_get_port_status(ctrl, slot->port_num),
            __atomic_load_n(&ctrl->irq_count, __ATOMIC_RELAXED));

    kprintf("[xHCI %s]   the port was %s%s (USB %u), %u ms; EP0 packet %u\n",
            ctrl->name, xhci_port_reset_kind_name(slot->reset_kind),
            slot->depth ? ", behind a hub" : "",
            xhci_port_protocol(ctrl, slot->port_num),
            slot->reset_took_ms, slot->ep0_max_packet);

    if (slot->ep0_ring) {
        kprintf("[xHCI %s]   its control pipe is %s; software has queued to %u "
                "and the controller has answered up to %u\n",
                ctrl->name,
                xhci_ep_state_name(xhci_ep_context_state(ctrl, slot, 1)),
                slot->ep0_ring->enqueue_idx, slot->ep0_ring->dequeue_idx);
    }

    if (waiting) {
        kprintf("[xHCI %s]   still waiting on %s, posted %u ms ago\n",
                ctrl->name, xhci_command_name(cmd_type), cmd_age);
    } else {
        kprintf("[xHCI %s]   no command outstanding for it — the last answer "
                "arrived and moved nothing\n", ctrl->name);
    }

}

void xhci_enum_watchdog(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->initialized) {
        return;
    }

    uint64_t now = rdtsc();
    uint64_t budget = cpu_ms_to_tsc(XHCI_ENUM_TIMEOUT_MS);
    static bool reported_controller = false;

    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        struct xhci_device_slot* slot = &ctrl->slots[i];

        uint8_t state = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
        if (state == ENUM_STATE_IDLE || state == ENUM_STATE_CONFIGURED ||
            state == ENUM_STATE_RETIRING) {
            continue;
        }
        if (state == ENUM_STATE_QUEUED) {
            continue;
        }
        if (slot->timestamp_started == 0) {
            continue;
        }

        if (slot->port_num != 0 &&
            xhci_port_says_gone(ctrl, slot->port_num)) {
            kprintf("[xHCI %s] port %u: nothing is attached there any more — "
                    "%s was for a device that has gone\n",
                    ctrl->name, slot->port_num, xhci_enum_state_name(state));
            xhci_slot_retire(ctrl, slot);
            continue;
        }

        if (xhci_command_pending_for(ctrl, slot)) {
            slot->watch_state = state;
            slot->watch_since = now;
            continue;
        }

        if (slot->watch_state != state || slot->watch_since == 0) {
            slot->watch_state = state;
            slot->watch_since = now;
            continue;
        }
        if ((int64_t)(now - slot->watch_since) < (int64_t)budget) {
            continue;
        }

        xhci_report_stuck(ctrl, slot, state);

        if (!reported_controller) {
            reported_controller = true;
            xhci_ring_t* er = &ctrl->event_ring;
            uint32_t at_dequeue = er->trbs ? er->trbs[er->dequeue_idx].control : 0;
            kprintf("[xHCI %s] state: USBSTS=0x%08x USBCMD=0x%08x "
                    "CRCR=0x%08x ERDP=0x%08x IMAN=0x%08x MFINDEX=0x%08x | "
                    "event ring at %u expecting cycle %u, TRB there 0x%08x | "
                    "command ring at %u cycle %u | %u command(s) outstanding\n",
                    ctrl->name,
                    ctrl->op_regs->usbsts, ctrl->op_regs->usbcmd,
                    (uint32_t)ctrl->op_regs->crcr,
                    (uint32_t)ctrl->runtime_regs->interrupters[0].erdp,
                    ctrl->runtime_regs->interrupters[0].iman,
                    ctrl->runtime_regs->mfindex,
                    er->dequeue_idx, er->cycle_state, at_dequeue,
                    ctrl->command_ring.enqueue_idx,
                    ctrl->command_ring.cycle_state,
                    xhci_command_outstanding(ctrl));
        }

        if (xhci_enum_step_can_be_asked_again(state) &&
            slot->step_retry < XHCI_STEP_RETRIES &&
            slot->ep0_ring && slot->slot_id != 0) {

            slot->step_retry++;
            kprintf("[xHCI %s] port %u: %s — taking the transfer back and "
                    "asking again (attempt %u of %u)\n",
                    ctrl->name, slot->port_num, xhci_enum_state_name(state),
                    slot->step_retry, XHCI_STEP_RETRIES);

            slot->step_resume  = state;
            slot->step_reissue = true;
            __atomic_store_n(&slot->state, (uint8_t)ENUM_STATE_WAIT_EP0_STOP,
                             __ATOMIC_RELEASE);

            slot->watch_state = ENUM_STATE_WAIT_EP0_STOP;
            slot->watch_since = now;

            if (xhci_post_stop_endpoint_cmd(ctrl, slot, slot->slot_id, 1) < 0) {
                xhci_slot_retire(ctrl, slot);
            }
            continue;
        }

        xhci_slot_retire(ctrl, slot);
    }
}

void xhci_enum_advance_state(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                             uint8_t slot_id, uint8_t completion_code) {
    if (!ctrl || !slot) {
        return;
    }


    if (completion_code != TRB_COMPLETION_SUCCESS) {
        if (slot->state == ENUM_STATE_WAIT_ADDRESS_DEVICE_BSR) {
            kprintf("[xHCI %s] port %u: this controller refused to address a "
                    "device without speaking to it (%s) — doing it in one step "
                    "instead\n", ctrl->name, slot->port_num,
                    xhci_completion_name(completion_code));
            enum_free_input_ctx(ctrl, slot);
            enum_address_device(ctrl, slot,
                                ep0_initial_max_packet(slot->speed), false);
            return;
        }

        if (slot->state == ENUM_STATE_WAIT_EP0_RESET &&
            completion_code == TRB_COMPLETION_CONTEXT_STATE) {
            kprintf("[xHCI %s] port %u: the control pipe was not halted after "
                    "all — carrying on\n", ctrl->name, slot->port_num);
            enum_resume_after_recovery(ctrl, slot, slot_id);
            return;
        }

        kprintf("[xHCI %s] port %u: enumeration step %s failed — %s (code %u); "
                "releasing the slot\n",
                ctrl->name, slot->port_num, xhci_enum_state_name(slot->state),
                xhci_completion_name(completion_code), completion_code);
        xhci_slot_retire(ctrl, slot);
        return;
    }

    switch (slot->state) {

        case ENUM_STATE_WAIT_ENABLE_SLOT: {
            if (slot_id == 0 || slot_id > ctrl->max_slots) {
                kprintf("[xHCI] Enable Slot returned slot id %u, which this "
                        "controller cannot have\n", slot_id);
                xhci_slot_retire(ctrl, slot);
                return;
            }

            slot->slot_id = slot_id;
            if (ctrl->by_id && slot_id <= ctrl->slot_count) {
                spin_lock(&ctrl->slots_lock);
                ctrl->by_id[slot_id] = slot;
                spin_unlock(&ctrl->slots_lock);
            }

            if (slot->depth == 0) {
                slot->speed = xhci_get_port_speed(ctrl, slot->port_num);
            }

            debug_printf("[xHCI ENUM] Slot enabled: slot_id=%u port=%u speed=%u\n",
                         slot_id, slot->port_num, slot->speed);

            if (xhci_ep_table_alloc(slot) != 0) {
                kprintf("[xHCI] port %u: no memory for the endpoint table\n",
                        slot->port_num);
                xhci_slot_retire(ctrl, slot);
                return;
            }

            if (xhci_alloc_ep0_ring(ctrl, slot) < 0) {
                debug_printf("[xHCI ENUM] Failed to allocate EP0 ring\n");
                xhci_slot_retire(ctrl, slot);
                return;
            }

            void* dev_ctx_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
            if (!dev_ctx_phys) {
                debug_printf("[xHCI ENUM] Failed to allocate Device Context\n");
                xhci_slot_retire(ctrl, slot);
                return;
            }
            slot->dev_ctx_phys = (uint64_t)dev_ctx_phys;
            slot->dev_ctx = vmm_phys_to_virt((uintptr_t)dev_ctx_phys);

            ctrl->dcbaa->device_context_ptrs[slot_id] = slot->dev_ctx_phys;

            enum_address_device(ctrl, slot,
                                ep0_initial_max_packet(slot->speed), true);
            break;
        }

        case ENUM_STATE_WAIT_ADDRESS_DEVICE_BSR: {
            enum_free_input_ctx(ctrl, slot);

            slot->state = ENUM_STATE_WAIT_GET_DESC_HEADER;
            if (enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 8) < 0) {
                kprintf("[xHCI %s] port %u: could not ask for the device "
                        "descriptor\n", ctrl->name, slot->port_num);
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_ADDRESS_DEVICE: {
            debug_printf("[xHCI ENUM] Device addressed: slot=%u\n", slot_id);
            enum_free_input_ctx(ctrl, slot);

            slot->state = ENUM_STATE_WAIT_GET_DESCRIPTOR;
            if (enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 18) < 0) {
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_GET_DESC_HEADER: {
            if (!enum_got_it_all(ctrl, slot, 8)) {
                enum_ask_again(ctrl, slot);
                return;
            }

            usb_device_desc_t* desc = (usb_device_desc_t*)slot->descriptor_buffer_virt;
            uint8_t mps0 = desc->bMaxPacketSize0;

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
                xhci_slot_retire(ctrl, slot);
                return;
            }

            enum_address_device(ctrl, slot, real_mps, false);
            break;
        }

        case ENUM_STATE_WAIT_GET_DESCRIPTOR: {
            if (!enum_got_it_all(ctrl, slot, 18)) {
                enum_ask_again(ctrl, slot);
                return;
            }

            usb_device_desc_t* desc = (usb_device_desc_t*)slot->descriptor_buffer_virt;

            if (!usb_validate_device_desc(desc)) {
                kprintf("[xHCI] port %u: device descriptor is malformed "
                        "(length %u, type %u, EP0 packet %u)\n",
                        slot->port_num, desc->bLength, desc->bDescriptorType,
                        desc->bMaxPacketSize0);
                xhci_slot_retire(ctrl, slot);
                return;
            }

            memcpy(&slot->device_desc, desc, sizeof(usb_device_desc_t));

            debug_printf("[xHCI ENUM] Device descriptor: VID=%04x PID=%04x "
                         "Class=%02x MaxPkt=%u\n",
                         desc->idVendor, desc->idProduct, desc->bDeviceClass,
                         desc->bMaxPacketSize0);

            slot->state = ENUM_STATE_WAIT_GET_CONFIG_HEADER;
            if (enum_get_descriptor(ctrl, slot, USB_DT_CONFIG, 9) < 0) {
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_GET_CONFIG_HEADER: {
            if (!enum_got_it_all(ctrl, slot, 9)) {
                enum_ask_again(ctrl, slot);
                return;
            }

            usb_config_desc_t* cfg = (usb_config_desc_t*)slot->descriptor_buffer_virt;

            if (cfg->bDescriptorType != USB_DESC_CONFIGURATION || cfg->bLength < 9) {
                kprintf("[xHCI] port %u: configuration descriptor is malformed "
                        "(length %u, type %u)\n",
                        slot->port_num, cfg->bLength, cfg->bDescriptorType);
                xhci_slot_retire(ctrl, slot);
                return;
            }

            uint16_t total = cfg->wTotalLength;
            if (total < 9) {
                total = 9;
            }
            if (total > XHCI_DESC_BUFFER_BYTES) {
                kprintf("[xHCI] port %u: configuration descriptor is %u bytes, "
                        "reading the first %u\n",
                        slot->port_num, total, XHCI_DESC_BUFFER_BYTES);
                total = XHCI_DESC_BUFFER_BYTES;
            }
            slot->config_total_len = total;

            slot->state = ENUM_STATE_WAIT_GET_CONFIG_DESC;
            if (enum_get_descriptor(ctrl, slot, USB_DT_CONFIG, total) < 0) {
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_GET_CONFIG_DESC: {
            if (!enum_got_it_all(ctrl, slot, slot->config_total_len)) {
                enum_ask_again(ctrl, slot);
                return;
            }

            usb_config_desc_t* cfg = (usb_config_desc_t*)slot->descriptor_buffer_virt;
            slot->config_value = cfg->bConfigurationValue;

            usb_config_first_interface(slot->descriptor_buffer_virt,
                                       slot->config_total_len,
                                       &slot->interface_class,
                                       &slot->interface_subclass,
                                       &slot->interface_protocol);

            kprintf("[xHCI %s] port %u: configuration %u of %u, %u byte(s), "
                    "first interface class %02x/%02x/%02x\n",
                    ctrl->name, slot->port_num, slot->config_value,
                    slot->device_desc.bNumConfigurations,
                    slot->config_total_len, slot->interface_class,
                    slot->interface_subclass, slot->interface_protocol);

            usb_setup_packet_t setup = {
                .bmRequestType = 0x00,
                .bRequest = USB_REQ_SET_CONFIGURATION,
                .wValue = slot->config_value,
                .wIndex = 0,
                .wLength = 0
            };

            slot->state = ENUM_STATE_WAIT_SET_CONFIGURATION;
            if (xhci_control_transfer(ctrl, slot, &setup, 0, 0, false) < 0) {
                kprintf("[xHCI %s] port %u: could not ask it to take its "
                        "configuration\n", ctrl->name, slot->port_num);
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_SET_CONFIGURATION: {
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

            slot->state = ENUM_STATE_WAIT_SET_IDLE;
            if (xhci_control_transfer(ctrl, slot, &setup, 0, 0, false) < 0) {
                kprintf("[xHCI %s] port %u: could not ask it to stop idling\n",
                        ctrl->name, slot->port_num);
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_EP0_STOP:
        case ENUM_STATE_WAIT_EP0_RESET: {
            uint64_t resume = slot->ep0_ring->trbs_phys +
                              (uint64_t)slot->ep0_ring->enqueue_idx *
                              sizeof(xhci_trb_t);

            xhci_ring_abandon(slot->ep0_ring);

            slot->state = ENUM_STATE_WAIT_EP0_DEQUEUE;

            if (xhci_post_set_tr_dequeue_cmd(ctrl, slot, slot->slot_id, 1,
                    resume | (slot->ep0_ring->cycle_state ? 1u : 0u)) < 0) {
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_EP0_DEQUEUE: {
            enum_resume_after_recovery(ctrl, slot, slot_id);
            break;
        }

        case ENUM_STATE_WAIT_SET_IDLE: {
            slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;
            if (xhci_ep_configure(ctrl, slot) != 0) {
                kprintf("[xHCI] port %u: could not configure the keyboard "
                        "endpoint\n", slot->port_num);
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_CONFIGURE_ENDPOINT: {
            enum_free_input_ctx(ctrl, slot);
            slot->ep_pending_add = 0;
            slot->ever_configured = true;
            if (slot->born_port) {
                ctrl->enum_attempts[slot->born_port] = 0;
            }
            slot->state = ENUM_STATE_CONFIGURED;
            enum_driver_start(ctrl, slot);
            break;
        }

        default:
            kprintf("[xHCI %s] slot %u answered a step it was not on (%s)\n",
                    ctrl->name, slot_id, xhci_enum_state_name(slot->state));
            break;
    }
}