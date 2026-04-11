#include "xhci_enumeration.h"
#include "xhci_command.h"
#include "xhci_device.h"
#include "xhci_port.h"
#include "xhci_transfer.h"
#include "xhci_hid.h"
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
            device_slots[i].state != ENUM_STATE_IDLE &&
            device_slots[i].state != ENUM_STATE_ERROR) {
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
        debug_printf("[xHCI ENUM] No free device slots\n");
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
    slot->descriptor_buffer_virt = NULL;
    slot->descriptor_buffer_phys = 0;
    slot->interrupt_ring = NULL;
    slot->interrupt_ring_phys = 0;
    slot->interrupt_data_buffer_virt = NULL;
    slot->interrupt_data_buffer_phys = 0;
    slot->keyboard_endpoint_dci = 0;
    slot->is_keyboard = false;
    memset(&slot->device_desc, 0, sizeof(slot->device_desc));
    memset(&slot->keyboard_info, 0, sizeof(slot->keyboard_info));
    slot->timestamp_started = rdtsc();
    slot->state = ENUM_STATE_RESETTING_PORT;

    debug_printf("[xHCI ENUM] Starting enumeration for port %u\n", port);

    if (xhci_reset_port(ctrl, port) != 0) {
        debug_printf("[xHCI ENUM] Port %u reset failed\n", port);
        slot->state = ENUM_STATE_IDLE;
        return -6;
    }

    slot->state = ENUM_STATE_WAIT_ENABLE_SLOT;

    if (xhci_post_enable_slot_cmd(ctrl) < 0) {
        debug_printf("[xHCI ENUM] Failed to post Enable Slot command\n");
        slot->state = ENUM_STATE_IDLE;
        return -7;
    }

    return 0;
}

void xhci_device_slot_cleanup(xhci_controller_t* ctrl, xhci_device_slot_t* slot) {
    if (!slot) {
        return;
    }

    if (slot->ep0_ring) {
        xhci_free_ep0_ring(slot);
    }

    if (slot->interrupt_ring) {
        xhci_ring_destroy(slot->interrupt_ring);
        if (slot->interrupt_ring_phys) {
            pmm_free((void*)slot->interrupt_ring_phys, 1);
        }
        slot->interrupt_ring = NULL;
        slot->interrupt_ring_phys = 0;

        if (slot->interrupt_data_buffer_phys) {
            pmm_free((void*)slot->interrupt_data_buffer_phys, 1);
        }
        slot->interrupt_data_buffer_virt = NULL;
        slot->interrupt_data_buffer_phys = 0;
    }

    if (slot->input_ctx_phys) {
        uint32_t input_ctx_pages = (ctrl && ctrl->context_size == 64) ? 3 : 2;
        pmm_free((void*)slot->input_ctx_phys, input_ctx_pages);
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
    slot->is_keyboard = false;
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
        debug_printf("[xHCI ENUM] Command failed: slot=%u code=%u state=%u\n",
                     slot_id, completion_code, slot->state);
        slot->state = ENUM_STATE_ERROR;
        return;
    }

    switch (slot->state) {

        case ENUM_STATE_WAIT_ENABLE_SLOT: {
            if (slot_id == 0 || slot_id > ctrl->max_slots) {
                debug_printf("[xHCI ENUM] Invalid slot_id %u from Enable Slot\n", slot_id);
                slot->state = ENUM_STATE_ERROR;
                return;
            }

            slot->slot_id = slot_id;
            uint32_t speed = xhci_get_port_speed(ctrl, slot->port_num);
            slot->speed = (uint8_t)speed;

            debug_printf("[xHCI ENUM] Slot enabled: slot_id=%u port=%u speed=%u\n",
                         slot_id, slot->port_num, speed);

            /* Allocate EP0 ring now — needed before Address Device. */
            if (xhci_alloc_ep0_ring(ctrl, slot) < 0) {
                debug_printf("[xHCI ENUM] Failed to allocate EP0 ring\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            /* Allocate Device Context. */
            void* dev_ctx_phys = pmm_alloc_zero(1);
            if (!dev_ctx_phys) {
                debug_printf("[xHCI ENUM] Failed to allocate Device Context\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }
            slot->dev_ctx_phys = (uint64_t)dev_ctx_phys;
            slot->dev_ctx = vmm_phys_to_virt((uintptr_t)dev_ctx_phys);

            /* Register in DCBAA. */
            ctrl->dcbaa->device_context_ptrs[slot_id] = slot->dev_ctx_phys;

            /* Allocate Input Context (size depends on CSZ bit). */
            uint32_t input_ctx_pages = (ctrl->context_size == 64) ? 3 : 2;
            void* input_ctx_phys = pmm_alloc_zero(input_ctx_pages);
            if (!input_ctx_phys) {
                debug_printf("[xHCI ENUM] Failed to allocate Input Context\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }
            slot->input_ctx_phys = (uint64_t)input_ctx_phys;

            uint8_t* input_base = (uint8_t*)vmm_phys_to_virt((uintptr_t)input_ctx_phys);

            /* Input Control Context: add Slot (A0) and EP0 (A1). */
            xhci_input_control_context_t* icc = (xhci_input_control_context_t*)input_base;
            icc->add_context_flags = (1 << 0) | (1 << 1);

            /* Slot Context at offset context_size. */
            xhci_slot_context_t* slot_ctx =
                (xhci_slot_context_t*)(input_base + ctrl->context_size);
            xhci_init_slot_context(slot_ctx, slot->port_num, speed);

            /* EP0 Context at offset context_size * 2. */
            uint16_t max_packet = 8;
            if (speed == XHCI_PORT_SPEED_HIGH || speed == XHCI_PORT_SPEED_SUPER) {
                max_packet = 64;
            }
            xhci_endpoint_context_t* ep0_ctx =
                (xhci_endpoint_context_t*)(input_base + ctrl->context_size * 2);
            xhci_init_ep0_context(ep0_ctx, slot->ep0_ring->trbs_phys, max_packet);

            slot->state = ENUM_STATE_WAIT_ADDRESS_DEVICE;

            if (xhci_post_address_device_cmd(ctrl, slot_id, (uint64_t)input_ctx_phys) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Address Device\n");
                xhci_device_slot_cleanup(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_ADDRESS_DEVICE: {
            debug_printf("[xHCI ENUM] Device addressed: slot=%u\n", slot_id);

            /* Free the input context — no longer needed after addressing. */
            if (slot->input_ctx_phys) {
                uint32_t pages = (ctrl->context_size == 64) ? 3 : 2;
                pmm_free((void*)slot->input_ctx_phys, pages);
                slot->input_ctx_phys = 0;
            }

            /* Get Device Descriptor (18 bytes). */
            usb_setup_packet_t setup = {
                .bmRequestType = 0x80,
                .bRequest = USB_REQ_GET_DESCRIPTOR,
                .wValue = (USB_DT_DEVICE << 8) | 0,
                .wIndex = 0,
                .wLength = 18
            };

            if (xhci_control_transfer(ctrl, slot, &setup,
                                      slot->descriptor_buffer_phys, 18, true) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Get Device Descriptor\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->state = ENUM_STATE_WAIT_GET_DESCRIPTOR;
            break;
        }

        case ENUM_STATE_WAIT_GET_CONFIG_DESC: {
            int parse_result = xhci_parse_config_descriptor(
                slot->descriptor_buffer_virt, 64, &slot->keyboard_info);

            if (parse_result < 0) {
                debug_printf("[xHCI ENUM] Not a keyboard (code=%d)\n", parse_result);
                slot->state = ENUM_STATE_ERROR;
                return;
            }

            slot->is_keyboard = true;
            slot->keyboard_endpoint_dci = slot->keyboard_info.endpoint_dci;

            debug_printf("[xHCI ENUM] Keyboard: iface=%u ep=0x%02x dci=%u\n",
                         slot->keyboard_info.interface_num,
                         slot->keyboard_info.endpoint_addr,
                         slot->keyboard_info.endpoint_dci);

            usb_setup_packet_t setup = {
                .bmRequestType = 0x00,
                .bRequest = USB_REQ_SET_CONFIGURATION,
                .wValue = slot->keyboard_info.config_value,
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
            usb_setup_packet_t setup = {
                .bmRequestType = 0x21,
                .bRequest = HID_REQ_SET_PROTOCOL,
                .wValue = 0,
                .wIndex = slot->keyboard_info.interface_num,
                .wLength = 0
            };

            if (xhci_control_transfer(ctrl, slot, &setup, 0, 0, false) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Set Protocol\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->state = ENUM_STATE_WAIT_SET_PROTOCOL;
            break;
        }

        case ENUM_STATE_WAIT_SET_PROTOCOL: {
            usb_setup_packet_t setup = {
                .bmRequestType = 0x21,
                .bRequest = HID_REQ_SET_IDLE,
                .wValue = 0,
                .wIndex = slot->keyboard_info.interface_num,
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

        case ENUM_STATE_WAIT_SET_IDLE: {
            /* Allocate interrupt ring. */
            void* ring_phys = pmm_alloc_zero(1);
            if (!ring_phys) {
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            xhci_ring_t* ring = (xhci_ring_t*)vmm_phys_to_virt((uintptr_t)ring_phys);
            if (!ring) {
                pmm_free(ring_phys, 1);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            if (xhci_ring_init(ring, 32, true) != 0) {
                pmm_free(ring_phys, 1);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->interrupt_ring = ring;
            slot->interrupt_ring_phys = (uint64_t)ring_phys;

            /* Allocate interrupt data buffer. */
            void* data_phys = pmm_alloc_zero(1);
            if (!data_phys) {
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }
            slot->interrupt_data_buffer_virt = vmm_phys_to_virt((uintptr_t)data_phys);
            slot->interrupt_data_buffer_phys = (uint64_t)data_phys;

            /* Allocate Input Context for Configure Endpoint. */
            uint32_t input_ctx_pages = (ctrl->context_size == 64) ? 3 : 2;
            void* input_ctx_phys = pmm_alloc_zero(input_ctx_pages);
            if (!input_ctx_phys) {
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }
            slot->input_ctx_phys = (uint64_t)input_ctx_phys;

            uint8_t* input_base = (uint8_t*)vmm_phys_to_virt((uintptr_t)input_ctx_phys);
            uint8_t dci = slot->keyboard_endpoint_dci;

            /* Input Control Context: add Slot (A0) + EPn (A_dci). */
            xhci_input_control_context_t* icc = (xhci_input_control_context_t*)input_base;
            icc->add_context_flags = (1 << 0) | (1 << dci);

            /* Slot Context: update Context Entries to cover the new endpoint. */
            xhci_slot_context_t* slot_ctx =
                (xhci_slot_context_t*)(input_base + ctrl->context_size);
            slot_ctx->dwords[0] = ((uint32_t)dci << 27) | ((uint32_t)slot->speed << 20);
            slot_ctx->dwords[1] = ((uint32_t)slot->port_num << 16);

            /* Interrupt endpoint interval: convert bInterval to xHCI exponent format. */
            uint8_t xhci_interval;
            if (slot->speed == XHCI_PORT_SPEED_FULL || slot->speed == XHCI_PORT_SPEED_LOW) {
                /* Full/Low speed: interval is in ms frames; xHCI wants log2(interval * 8). */
                xhci_interval = slot->keyboard_info.interval + 2;
            } else {
                /* High/Super speed: interval is already 2^(n-1) * 125us; subtract 1. */
                xhci_interval = (slot->keyboard_info.interval > 0)
                                 ? slot->keyboard_info.interval - 1 : 0;
            }

            /* Interrupt endpoint context at offset context_size * (dci + 1). */
            xhci_endpoint_context_t* ep_ctx =
                (xhci_endpoint_context_t*)(input_base + ctrl->context_size * (dci + 1));
            memset(ep_ctx, 0, sizeof(xhci_endpoint_context_t));

            /* dword0: Interval field [23:16]. */
            ep_ctx->dwords[0] = (uint32_t)xhci_interval << 16;

            /* dword1: CErr=3 [2:1], EP Type=7 (Interrupt IN) [5:3], Max Packet Size [31:16]. */
            ep_ctx->dwords[1] = (3 << 1) | (XHCI_EP_TYPE_INTERRUPT_IN << 3) |
                                 ((uint32_t)slot->keyboard_info.max_packet_size << 16);

            /* dword2-3: TR Dequeue Pointer | DCS=1. */
            uint64_t ring_addr = slot->interrupt_ring->trbs_phys;
            ep_ctx->dwords[2] = (uint32_t)(ring_addr & 0xFFFFFFF0) | 1;
            ep_ctx->dwords[3] = (uint32_t)(ring_addr >> 32);

            /* dword4: Average TRB Length = max_packet_size (keyboard report). */
            ep_ctx->dwords[4] = slot->keyboard_info.max_packet_size;

            slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;

            if (xhci_post_configure_endpoint_cmd(ctrl, slot->slot_id,
                                                 (uint64_t)input_ctx_phys) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Configure Endpoint\n");
                xhci_device_slot_cleanup(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_CONFIGURE_ENDPOINT: {
            debug_printf("[xHCI ENUM] Endpoint configured, keyboard active on slot %u\n",
                         slot_id);

            /* Free input context — no longer needed. */
            if (slot->input_ctx_phys) {
                uint32_t pages = (ctrl->context_size == 64) ? 3 : 2;
                pmm_free((void*)slot->input_ctx_phys, pages);
                slot->input_ctx_phys = 0;
            }

            if (xhci_queue_interrupt_transfer(slot) < 0) {
                debug_printf("[xHCI ENUM] Failed to queue interrupt transfer\n");
                slot->state = ENUM_STATE_ERROR;
                return;
            }

            __sync_synchronize();
            ctrl->doorbells->doorbells[slot->slot_id].doorbell = slot->keyboard_endpoint_dci;

            slot->state = ENUM_STATE_CONFIGURED;
            break;
        }

        default:
            debug_printf("[xHCI ENUM] Unexpected state %u for slot %u\n",
                         slot->state, slot_id);
            break;
    }
}
