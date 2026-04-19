#include "xhci.h"
#include "xhci_transfer.h"
#include "xhci_command.h"
#include "xhci_enumeration.h"
#include "xhci_device.h"
#include "xhci_trb.h"
#include "xhci_hid.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "atomics.h"

static uint64_t pack_setup_packet(usb_setup_packet_t* setup) {
    uint64_t param = 0;
    param |= (uint64_t)setup->bmRequestType;
    param |= (uint64_t)setup->bRequest << 8;
    param |= (uint64_t)setup->wValue << 16;
    param |= (uint64_t)setup->wIndex << 32;
    param |= (uint64_t)setup->wLength << 48;
    return param;
}

int xhci_alloc_ep0_ring(xhci_controller_t* ctrl, xhci_device_slot_t* slot) {
    (void)ctrl;
    if (!slot) {
        return -1;
    }

    void* ring_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    if (!ring_phys) {
        debug_printf("[xHCI TRANSFER] Failed to allocate EP0 ring structure\n");
        return -1;
    }

    xhci_ring_t* ring = (xhci_ring_t*)vmm_phys_to_virt((uintptr_t)ring_phys);
    if (!ring) {
        pmm_free(ring_phys, 1);
        return -1;
    }

    if (xhci_ring_init(ring, 32, true) != 0) {
        debug_printf("[xHCI TRANSFER] Failed to init EP0 ring\n");
        pmm_free(ring_phys, 1);
        return -1;
    }

    slot->ep0_ring = ring;
    slot->ep0_ring_phys = (uint64_t)ring_phys;

    void* desc_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    if (!desc_phys) {
        xhci_ring_destroy(ring);
        pmm_free(ring_phys, 1);
        slot->ep0_ring = NULL;
        slot->ep0_ring_phys = 0;
        debug_printf("[xHCI TRANSFER] Failed to allocate descriptor buffer\n");
        return -1;
    }

    slot->descriptor_buffer_virt = vmm_phys_to_virt((uintptr_t)desc_phys);
    slot->descriptor_buffer_phys = (uint64_t)desc_phys;

    return 0;
}

void xhci_free_ep0_ring(xhci_device_slot_t* slot) {
    if (!slot) {
        return;
    }

    if (slot->ep0_ring) {
        xhci_ring_destroy(slot->ep0_ring);
        if (slot->ep0_ring_phys) {
            pmm_free((void*)slot->ep0_ring_phys, 1);
        }
        slot->ep0_ring = NULL;
        slot->ep0_ring_phys = 0;
    }

    if (slot->descriptor_buffer_phys) {
        pmm_free((void*)slot->descriptor_buffer_phys, 1);
        slot->descriptor_buffer_virt = NULL;
        slot->descriptor_buffer_phys = 0;
    }
}

int xhci_control_transfer(xhci_controller_t* ctrl,
                          xhci_device_slot_t* slot,
                          usb_setup_packet_t* setup,
                          uint64_t data_buffer_phys,
                          uint16_t data_length,
                          bool data_in) {
    if (!ctrl || !slot || !setup || !slot->ep0_ring) {
        return -1;
    }

    xhci_ring_t* ring = slot->ep0_ring;

    uint32_t trt = 0;
    if (data_length > 0) {
        trt = data_in ? 3 : 2;
    }

    xhci_trb_t setup_trb = {0};
    setup_trb.parameter = pack_setup_packet(setup);
    setup_trb.status = 8;
    setup_trb.control = TRB_SET_TYPE(TRB_TYPE_SETUP_STAGE) | TRB_IDT | (trt << 16);
    if (data_length > 0) {
        setup_trb.control |= TRB_CH;
    }
    xhci_ring_enqueue(ring, &setup_trb);

    if (data_length > 0) {
        xhci_trb_t data_trb = {0};
        data_trb.parameter = data_buffer_phys;
        data_trb.status = data_length;
        data_trb.control = TRB_SET_TYPE(TRB_TYPE_DATA_STAGE) | TRB_CH |
                           (data_in ? (1 << 16) : 0);
        xhci_ring_enqueue(ring, &data_trb);
    }

    bool status_in = (data_length == 0) || !data_in;
    xhci_trb_t status_trb = {0};
    status_trb.control = TRB_SET_TYPE(TRB_TYPE_STATUS_STAGE) | TRB_IOC |
                         (status_in ? (1 << 16) : 0);
    xhci_ring_enqueue(ring, &status_trb);

    __sync_synchronize();
    ctrl->doorbells->doorbells[slot->slot_id].doorbell = 1;

    return 0;
}

int xhci_queue_interrupt_transfer(xhci_device_slot_t* slot) {
    if (!slot || !slot->interrupt_ring) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = slot->interrupt_data_buffer_phys;
    trb.status = slot->keyboard_info.max_packet_size;
    trb.control = TRB_SET_TYPE(TRB_TYPE_NORMAL) | TRB_IOC;

    uint64_t trb_phys = xhci_ring_enqueue(slot->interrupt_ring, &trb);
    if (trb_phys == 0) {
        return -1;
    }

    return 0;
}

void xhci_handle_transfer_event(xhci_controller_t* ctrl, xhci_trb_t* event) {
    if (!ctrl || !event) {
        return;
    }

    uint8_t slot_id = (event->control >> 24) & 0xFF;
    uint8_t endpoint_id = (event->control >> 16) & 0x1F;
    uint8_t completion_code = (event->status >> 24) & 0xFF;

    xhci_device_slot_t* slot = xhci_get_device_slot(ctrl, slot_id);
    if (!slot) {
        return;
    }

    if (endpoint_id == slot->keyboard_endpoint_dci && slot->interrupt_ring) {
        if (completion_code == TRB_COMPLETION_SUCCESS ||
            completion_code == TRB_COMPLETION_SHORT_PKT) {
            usb_boot_keyboard_report_t* report =
                (usb_boot_keyboard_report_t*)slot->interrupt_data_buffer_virt;
            xhci_process_keyboard_report(report);
            xhci_queue_interrupt_transfer(slot);
            __sync_synchronize();
            ctrl->doorbells->doorbells[slot->slot_id].doorbell = endpoint_id;
        }
        return;
    }

    if (endpoint_id != 1) {
        return;
    }

    if (completion_code != TRB_COMPLETION_SUCCESS &&
        completion_code != TRB_COMPLETION_SHORT_PKT) {
        debug_printf("[xHCI TRANSFER] EP0 transfer failed: slot=%u code=%u\n",
                     slot_id, completion_code);
        slot->state = ENUM_STATE_ERROR;
        return;
    }

    if (slot->state == ENUM_STATE_WAIT_GET_DESCRIPTOR) {
        usb_device_desc_t* desc = (usb_device_desc_t*)slot->descriptor_buffer_virt;

        if (!usb_validate_device_desc(desc)) {
            debug_printf("[xHCI TRANSFER] Invalid device descriptor on slot %u\n", slot_id);
            slot->state = ENUM_STATE_ERROR;
            return;
        }

        memcpy(&slot->device_desc, desc, sizeof(usb_device_desc_t));

        debug_printf("[xHCI ENUM] Device descriptor: VID=%04x PID=%04x Class=%02x MaxPkt=%u\n",
                     desc->idVendor, desc->idProduct, desc->bDeviceClass, desc->bMaxPacketSize0);

        usb_setup_packet_t setup = {
            .bmRequestType = 0x80,
            .bRequest = USB_REQ_GET_DESCRIPTOR,
            .wValue = (USB_DT_CONFIG << 8) | 0,
            .wIndex = 0,
            .wLength = 64
        };

        if (xhci_control_transfer(ctrl, slot, &setup,
                                  slot->descriptor_buffer_phys, 64, true) < 0) {
            debug_printf("[xHCI TRANSFER] Failed to post Get Config Descriptor\n");
            slot->state = ENUM_STATE_ERROR;
            return;
        }

        slot->state = ENUM_STATE_WAIT_GET_CONFIG_DESC;
        return;
    }

    xhci_enum_advance_state(ctrl, slot_id, TRB_COMPLETION_SUCCESS);
}
