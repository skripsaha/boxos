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

#define HID_PROTOCOL_BOOT       0
#define HID_PROTOCOL_REPORT     1
#define HID_BMREQUEST_TYPE_OUT  0x21

static struct xhci_device_slot device_slots[XHCI_MAX_SLOTS];
static spinlock_t device_slots_lock;

void xhci_enumeration_init(void) {
    spinlock_init(&device_slots_lock);
    memset(device_slots, 0, sizeof(device_slots));
}

static struct xhci_device_slot* find_free_slot(void) {
    spin_lock(&device_slots_lock);
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        if (device_slots[i].state == ENUM_STATE_IDLE) {
            spin_unlock(&device_slots_lock);
            return &device_slots[i];
        }
    }
    spin_unlock(&device_slots_lock);
    return NULL;
}

static struct xhci_device_slot* find_slot_by_port(uint8_t port) {
    spin_lock(&device_slots_lock);
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
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
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        if (device_slots[i].slot_id == slot_id &&
            device_slots[i].state != ENUM_STATE_IDLE) {
            spin_unlock(&device_slots_lock);
            return &device_slots[i];
        }
    }
    spin_unlock(&device_slots_lock);
    return NULL;
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
        debug_printf("[xHCI ENUM] No device on port %u\n", port);
        return -3;
    }

    struct xhci_device_slot* existing = find_slot_by_port(port);
    if (existing) {
        debug_printf("[xHCI ENUM] Port %u already has active slot\n", port);
        return -4;
    }

    struct xhci_device_slot* slot = find_free_slot();
    if (!slot) {
        debug_printf("[xHCI ENUM] No free device slots\n");
        return -5;
    }

    memset(slot, 0, sizeof(struct xhci_device_slot));
    slot->port_num = port;
    slot->state = ENUM_STATE_WAIT_ENABLE_SLOT;
    slot->timestamp_started = rdtsc();

    debug_printf("[xHCI ENUM] Starting enumeration for port %u\n", port);

    int ret = xhci_post_enable_slot_cmd(ctrl);
    if (ret < 0) {
        debug_printf("[xHCI ENUM] Failed to post Enable Slot command\n");
        slot->state = ENUM_STATE_IDLE;
        return -6;
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
        if (slot->interrupt_ring->trbs_phys) {
            size_t trb_pages = vmm_size_to_pages(slot->interrupt_ring->num_trbs * sizeof(xhci_trb_t));
            pmm_free((void*)slot->interrupt_ring->trbs_phys, trb_pages);
        }

        if (slot->interrupt_ring_phys) {
            pmm_free((void*)slot->interrupt_ring_phys, 1);
        }

        slot->interrupt_ring = NULL;
        slot->interrupt_ring_phys = 0;
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
}

void xhci_enum_advance_state(xhci_controller_t* ctrl, uint8_t slot_id, uint8_t completion_code) {
    if (!ctrl) {
        return;
    }

    struct xhci_device_slot* slot = NULL;

    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        if (device_slots[i].state == ENUM_STATE_WAIT_ENABLE_SLOT) {
            slot = &device_slots[i];
            break;
        }
    }

    if (!slot && slot_id != 0) {
        slot = xhci_get_device_slot(ctrl, slot_id);
    }

    if (!slot) {
        debug_printf("[xHCI ENUM] No slot found for state advance (slot_id=%u)\n", slot_id);
        return;
    }

    if (completion_code != TRB_COMPLETION_SUCCESS) {
        debug_printf("[xHCI ENUM] Command failed: slot=%u code=%u\n", slot_id, completion_code);

        switch (completion_code) {
            case 11:
                debug_printf("[xHCI ENUM]   TRB Error - invalid TRB parameters\n");
                break;
            case 9:
                debug_printf("[xHCI ENUM]   Slot Not Enabled\n");
                break;
            default:
                debug_printf("[xHCI ENUM]   Unknown error code %u\n", completion_code);
        }

        slot->state = ENUM_STATE_ERROR;
        return;
    }

    switch (slot->state) {
        case ENUM_STATE_WAIT_ENABLE_SLOT: {
            if (slot_id == 0 || slot_id > ctrl->max_slots) {
                debug_printf("[xHCI ENUM] Invalid slot_id %u from Enable Slot completion\n", slot_id);
                slot->state = ENUM_STATE_ERROR;
                return;
            }

            slot->slot_id = slot_id;
            debug_printf("[xHCI ENUM] Slot enabled: slot_id=%u, port=%u\n",
                         slot_id, slot->port_num);

            void* dev_ctx_phys = pmm_alloc_zero(1);
            if (!dev_ctx_phys) {
                debug_printf("[xHCI ENUM] Failed to allocate Device Context\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->dev_ctx_phys = (uint64_t)dev_ctx_phys;
            slot->dev_ctx = vmm_phys_to_virt((uintptr_t)dev_ctx_phys);

            if (!slot->dev_ctx) {
                debug_printf("[xHCI ENUM] Failed to map Device Context to virtual address\n");
                pmm_free(dev_ctx_phys, 1);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            xhci_device_context_t* dev_ctx = (xhci_device_context_t*)slot->dev_ctx;

            uint32_t speed = xhci_get_port_speed(ctrl, slot->port_num);
            slot->speed = speed;
            xhci_init_slot_context(&dev_ctx->slot, slot->port_num, speed);

            dev_ctx->slot.dwords[0] = (31 << 27) | (speed << 20);

            uint16_t max_packet = 8;
            if (speed == XHCI_PORT_SPEED_HIGH || speed == XHCI_PORT_SPEED_SUPER) {
                max_packet = 64;
            }

            xhci_init_ep0_context(&dev_ctx->endpoints[0], 0, max_packet);

            dev_ctx->endpoints[0].dwords[2] = 0 | (1 << 0);
            dev_ctx->endpoints[0].dwords[3] = 0;

            debug_printf("[xHCI ENUM] Device Context allocated at phys=0x%llx\n",
                         slot->dev_ctx_phys);

            uint32_t input_ctx_pages = (ctrl->context_size == 64) ? 3 : 2;
            void* input_ctx_phys = pmm_alloc_zero(input_ctx_pages);
            if (!input_ctx_phys) {
                debug_printf("[xHCI ENUM] Failed to allocate Input Context\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            xhci_input_control_context_t* input_ctrl = vmm_phys_to_virt((uintptr_t)input_ctx_phys);
            if (!input_ctrl) {
                pmm_free(input_ctx_phys, 2);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            input_ctrl->add_context_flags = (1 << 0) | (1 << 1);

            uint8_t* input_dev_ctx = (uint8_t*)input_ctrl + ctrl->context_size;
            memcpy(input_dev_ctx, slot->dev_ctx, 2048);

            ctrl->dcbaa->device_context_ptrs[slot_id] = slot->dev_ctx_phys;

            slot->input_ctx_phys = (uint64_t)input_ctx_phys;
            slot->state = ENUM_STATE_WAIT_ADDRESS_DEVICE;

            int ret = xhci_post_address_device_cmd(ctrl, slot_id, (uint64_t)input_ctx_phys);
            if (ret < 0) {
                debug_printf("[xHCI ENUM] Failed to post Address Device command\n");
                xhci_device_slot_cleanup(ctrl, slot);
            }

            break;
        }

        case ENUM_STATE_WAIT_ADDRESS_DEVICE: {
            debug_printf("[xHCI ENUM] Device addressed: slot_id=%u\n", slot_id);

            if (xhci_alloc_ep0_ring(ctrl, slot) < 0) {
                debug_printf("[xHCI ENUM] Failed to allocate EP0 ring\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            uint32_t input_ctx_pages = (ctrl->context_size == 64) ? 3 : 2;
            void* input_ctx_phys = pmm_alloc_zero(input_ctx_pages);
            if (!input_ctx_phys) {
                debug_printf("[xHCI ENUM] Failed to allocate Input Context for Evaluate\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->input_ctx_phys = (uint64_t)input_ctx_phys;

            if (xhci_post_evaluate_context_cmd(ctrl, slot->slot_id) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Evaluate Context\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->state = ENUM_STATE_WAIT_EVALUATE_CONTEXT;
            break;
        }

        case ENUM_STATE_WAIT_EVALUATE_CONTEXT: {
            debug_printf("[xHCI ENUM] EP0 context updated (slot=%u)\n", slot_id);

            xhci_device_context_t* dev_ctx = (xhci_device_context_t*)slot->dev_ctx;
            xhci_endpoint_context_t* ep0_ctx = &dev_ctx->endpoints[0];
            uint8_t ep_state = ep0_ctx->dwords[0] & 0x7;

            debug_printf("[xHCI] EP0 State after Evaluate: %u ", ep_state);
            switch (ep_state) {
                case 0: debug_printf("(Disabled)\n"); break;
                case 1: debug_printf("(Running)\n"); break;
                case 2: debug_printf("(Halted)\n"); break;
                case 3: debug_printf("(Stopped)\n"); break;
                case 4: debug_printf("(Error)\n"); break;
                default: debug_printf("(Unknown)\n"); break;
            }

            if (ep_state != 1) {
                debug_printf("[xHCI] ERROR: EP0 not in Running state! Cannot accept transfers.\n");
            }

            if (slot->input_ctx_phys) {
                uint32_t input_ctx_pages = (ctrl->context_size == 64) ? 3 : 2;
                pmm_free((void*)slot->input_ctx_phys, input_ctx_pages);
                slot->input_ctx_phys = 0;
            }

            usb_setup_packet_t setup = {
                .bmRequestType = 0x80,
                .bRequest = USB_REQ_GET_DESCRIPTOR,
                .wValue = (USB_DESC_DEVICE << 8) | 0,
                .wIndex = 0,
                .wLength = 18
            };

            if (xhci_control_transfer(ctrl, slot, &setup,
                                      slot->descriptor_buffer_phys,
                                      18, true) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Get Descriptor\n");
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
                debug_printf("[xHCI ENUM] Not a keyboard device (code=%d)\n", parse_result);
                slot->state = ENUM_STATE_ERROR;
                return;
            }

            debug_printf("[xHCI ENUM] Keyboard found: iface=%u ep=0x%02x dci=%u\n",
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
            debug_printf("[xHCI ENUM] Device configured\n");

            usb_setup_packet_t set_protocol = {
                .bmRequestType = 0x21,
                .bRequest = 0x0B,
                .wValue = 0,
                .wIndex = slot->keyboard_info.interface_num,
                .wLength = 0
            };

            if (xhci_control_transfer(ctrl, slot, &set_protocol, 0, 0, false) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Set Protocol\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->state = ENUM_STATE_WAIT_SET_PROTOCOL;
            break;
        }

        case ENUM_STATE_WAIT_SET_PROTOCOL: {
            debug_printf("[xHCI ENUM] Boot Protocol set\n");

            usb_setup_packet_t set_idle = {
                .bmRequestType = 0x21,
                .bRequest = 0x0A,
                .wValue = 0,
                .wIndex = slot->keyboard_info.interface_num,
                .wLength = 0
            };

            if (xhci_control_transfer(ctrl, slot, &set_idle, 0, 0, false) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Set Idle\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->state = ENUM_STATE_WAIT_SET_IDLE;
            break;
        }

        case ENUM_STATE_WAIT_SET_IDLE: {
            debug_printf("[xHCI ENUM] Idle rate set\n");

            void* ring_phys = pmm_alloc_zero(1);
            if (!ring_phys) {
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            xhci_ring_t* ring = vmm_phys_to_virt((uintptr_t)ring_phys);
            if (!ring) {
                pmm_free(ring_phys, 1);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            size_t trb_pages = vmm_size_to_pages(256 * sizeof(xhci_trb_t));
            void* trbs_phys = pmm_alloc_zero(trb_pages);
            if (!trbs_phys) {
                pmm_free(ring_phys, 1);
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            ring->trbs = (xhci_trb_t*)vmm_phys_to_virt((uintptr_t)trbs_phys);
            ring->trbs_phys = (uint64_t)trbs_phys;
            ring->num_trbs = 256;
            ring->cycle_state = 1;
            ring->enqueue_idx = 0;
            spinlock_init(&ring->ring_lock);

            slot->interrupt_ring = ring;
            slot->interrupt_ring_phys = (uint64_t)ring_phys;
            slot->keyboard_endpoint_dci = slot->keyboard_info.endpoint_dci;

            void* data_phys = pmm_alloc_zero(1);
            slot->interrupt_data_buffer_virt = vmm_phys_to_virt((uintptr_t)data_phys);
            slot->interrupt_data_buffer_phys = (uint64_t)data_phys;

            uint32_t input_ctx_pages = (ctrl->context_size == 64) ? 3 : 2;
            void* input_ctx_phys = pmm_alloc_zero(input_ctx_pages);
            slot->input_ctx_phys = (uint64_t)input_ctx_phys;

            xhci_input_control_context_t* input_ctrl = vmm_phys_to_virt((uintptr_t)input_ctx_phys);
            uint8_t dci = slot->keyboard_endpoint_dci;
            input_ctrl->add_context_flags = (1 << dci) | (1 << 0);

            uint8_t xhci_interval;
            if (slot->speed == XHCI_PORT_SPEED_FULL) {
                xhci_interval = slot->keyboard_info.interval + 2;
            } else {
                xhci_interval = slot->keyboard_info.interval > 0 ?
                                slot->keyboard_info.interval - 1 : 0;
            }

            uint8_t* ep_ctx_ptr = (uint8_t*)input_ctrl + (dci + 1) * ctrl->context_size;
            xhci_endpoint_context_t* ep_ctx = (xhci_endpoint_context_t*)ep_ctx_ptr;

            ep_ctx->dwords[0] = (xhci_interval << 16) | slot->keyboard_info.max_packet_size;
            ep_ctx->dwords[1] = (XHCI_EP_TYPE_INTERRUPT_IN << 3) | (3 << 1);

            uint64_t ring_addr = slot->interrupt_ring_phys | 1;
            ep_ctx->dwords[2] = (uint32_t)(ring_addr & 0xFFFFFFFF);
            ep_ctx->dwords[3] = (uint32_t)(ring_addr >> 32);

            if (xhci_post_configure_endpoint_cmd(ctrl, slot->slot_id, (uint64_t)input_ctx_phys) < 0) {
                debug_printf("[xHCI ENUM] Failed to post Configure Endpoint\n");
                xhci_device_slot_cleanup(ctrl, slot);
                return;
            }

            slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;
            break;
        }

        case ENUM_STATE_WAIT_CONFIGURE_ENDPOINT: {
            debug_printf("[xHCI ENUM] Endpoint configured, starting keyboard polling\n");

            if (xhci_queue_interrupt_transfer(slot) < 0) {
                debug_printf("[xHCI ENUM] Failed to queue interrupt transfer\n");
                slot->state = ENUM_STATE_ERROR;
                return;
            }

            __sync_synchronize();
            ctrl->doorbells->doorbells[slot->slot_id].doorbell = slot->keyboard_endpoint_dci;

            slot->state = ENUM_STATE_CONFIGURED;
            debug_printf("[xHCI] Keyboard active on slot %u\n", slot->slot_id);
            break;
        }

        default:
            debug_printf("[xHCI ENUM] Unexpected state: %u\n", slot->state);
            break;
    }
}
