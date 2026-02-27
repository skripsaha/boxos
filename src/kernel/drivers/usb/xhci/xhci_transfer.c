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
#include "cpu_calibrate.h"

extern struct xhci_pending_cmd pending_cmds[];
extern spinlock_t pending_cmds_lock;
extern uint32_t cmd_sequence;

static struct xhci_pending_cmd *find_free_pending_cmd(void)
{
    spin_lock(&pending_cmds_lock);
    for (int i = 0; i < XHCI_MAX_PENDING_CMDS; i++)
    {
        if (pending_cmds[i].state == CMD_STATE_IDLE)
        {
            spin_unlock(&pending_cmds_lock);
            return &pending_cmds[i];
        }
    }
    spin_unlock(&pending_cmds_lock);
    return NULL;
}

int xhci_alloc_ep0_ring(xhci_controller_t *ctrl, xhci_device_slot_t *slot)
{
    if (!ctrl || !slot)
    {
        return -1;
    }

    void *ring_phys = pmm_alloc_zero(1);
    if (!ring_phys)
    {
        debug_printf("[xHCI TRANSFER] Failed to allocate EP0 ring structure\n");
        return -1;
    }

    xhci_ring_t *ring = (xhci_ring_t *)vmm_phys_to_virt((uintptr_t)ring_phys);
    if (!ring)
    {
        pmm_free(ring_phys, 1);
        debug_printf("[xHCI TRANSFER] Failed to map EP0 ring to virtual address\n");
        return -1;
    }

    size_t trb_pages = vmm_size_to_pages(16 * sizeof(xhci_trb_t));
    void *trbs_phys = pmm_alloc_zero(trb_pages);
    if (!trbs_phys)
    {
        pmm_free(ring_phys, 1);
        debug_printf("[xHCI TRANSFER] Failed to allocate EP0 TRB array\n");
        return -1;
    }

    ring->trbs = (xhci_trb_t *)vmm_phys_to_virt((uintptr_t)trbs_phys);
    ring->trbs_phys = (uint64_t)trbs_phys;
    ring->num_trbs = 16;
    ring->enqueue_idx = 0;
    ring->dequeue_idx = 0;
    ring->cycle_state = 1;
    spinlock_init(&ring->ring_lock);

    slot->ep0_ring = ring;
    slot->ep0_ring_phys = (uint64_t)ring_phys;

    void *desc_phys = pmm_alloc_zero(1);
    if (!desc_phys)
    {
        size_t trb_pages = vmm_size_to_pages(16 * sizeof(xhci_trb_t));
        pmm_free(trbs_phys, trb_pages);
        pmm_free(ring_phys, 1);
        slot->ep0_ring = NULL;
        debug_printf("[xHCI TRANSFER] Failed to allocate descriptor buffer\n");
        return -1;
    }

    slot->descriptor_buffer_virt = vmm_phys_to_virt((uintptr_t)desc_phys);
    slot->descriptor_buffer_phys = (uint64_t)desc_phys;

    if (!slot->descriptor_buffer_virt)
    {
        size_t trb_pages = vmm_size_to_pages(16 * sizeof(xhci_trb_t));
        pmm_free(trbs_phys, trb_pages);
        pmm_free(ring_phys, 1);
        pmm_free(desc_phys, 1);
        slot->ep0_ring = NULL;
        debug_printf("[xHCI TRANSFER] Failed to map descriptor buffer to virtual address\n");
        return -1;
    }

    debug_printf("[xHCI TRANSFER] EP0 ring allocated: phys=0x%llx size=%u\n",
                 slot->ep0_ring_phys, ring->num_trbs);

    return 0;
}

void xhci_free_ep0_ring(xhci_device_slot_t *slot)
{
    if (!slot)
    {
        return;
    }

    if (slot->ep0_ring)
    {
        if (slot->ep0_ring->trbs_phys)
        {
            size_t trb_pages = vmm_size_to_pages(slot->ep0_ring->num_trbs * sizeof(xhci_trb_t));
            pmm_free((void *)slot->ep0_ring->trbs_phys, trb_pages);
        }

        if (slot->ep0_ring_phys)
        {
            pmm_free((void *)slot->ep0_ring_phys, 1);
        }

        slot->ep0_ring = NULL;
        slot->ep0_ring_phys = 0;
    }

    if (slot->descriptor_buffer_phys)
    {
        pmm_free((void *)slot->descriptor_buffer_phys, 1);
        slot->descriptor_buffer_virt = NULL;
        slot->descriptor_buffer_phys = 0;
    }
}

int xhci_post_evaluate_context_cmd(xhci_controller_t *ctrl, uint8_t slot_id)
{
    if (!ctrl || slot_id == 0 || slot_id > ctrl->max_slots)
    {
        return -1;
    }

    xhci_device_slot_t *slot = xhci_get_device_slot(ctrl, slot_id);
    if (!slot || !slot->input_ctx_phys)
    {
        debug_printf("[xHCI TRANSFER] Invalid slot or missing input context\n");
        return -1;
    }

    xhci_input_control_context_t *input_ctrl =
        (xhci_input_control_context_t *)vmm_phys_to_virt((uintptr_t)slot->input_ctx_phys);

    if (!input_ctrl)
    {
        debug_printf("[xHCI TRANSFER] Failed to map input context\n");
        return -1;
    }

    memset(input_ctrl, 0, sizeof(xhci_input_control_context_t));
    input_ctrl->add_context_flags = (1 << 1);

    uint8_t *input_dev_ctx = (uint8_t *)input_ctrl + ctrl->context_size;
    memcpy(input_dev_ctx, slot->dev_ctx, 2048);

    xhci_device_context_t *dev_ctx = (xhci_device_context_t *)input_dev_ctx;
    xhci_endpoint_context_t *ep0_ctx = &dev_ctx->endpoints[0];

    uint64_t trb_array_phys = slot->ep0_ring->trbs_phys;
    uint8_t dcs = slot->ep0_ring->cycle_state & 0x1;

    ep0_ctx->dwords[2] = (uint32_t)(trb_array_phys & 0xFFFFFFF0) | dcs;
    ep0_ctx->dwords[3] = (uint32_t)(trb_array_phys >> 32);

    debug_printf("[xHCI TRANSFER] Evaluate Context: EP0 TR Dequeue=0x%llx DCS=%u\n",
                 trb_array_phys, dcs);

    struct xhci_pending_cmd *cmd = find_free_pending_cmd();
    if (!cmd)
    {
        debug_printf("[xHCI TRANSFER] No free command slots\n");
        uint32_t input_ctx_pages = (ctrl->context_size == 64) ? 3 : 2;
        pmm_free((void *)slot->input_ctx_phys, input_ctx_pages);
        slot->input_ctx_phys = 0;
        return -1;
    }

    xhci_trb_t *trb = &ctrl->command_ring.trbs[ctrl->command_ring.enqueue_idx];
    uint64_t trb_phys = ctrl->command_ring.trbs_phys +
                        (ctrl->command_ring.enqueue_idx * sizeof(xhci_trb_t));

    trb->parameter = slot->input_ctx_phys;
    trb->status = 0;
    trb->control = (13 << 10) |
                   (slot_id << 24) |
                   (ctrl->command_ring.cycle_state ? TRB_C : 0);

    cmd->trb_phys = trb_phys;
    cmd->timestamp_posted = rdtsc();
    cmd->slot_id = slot_id;
    cmd->state = CMD_STATE_POSTED;
    cmd->sequence = ++cmd_sequence;
    cmd->completion_code = 0;
    cmd->completion_param = 0;

    ctrl->command_ring.enqueue_idx++;
    if (ctrl->command_ring.enqueue_idx >= ctrl->command_ring.num_trbs)
    {
        ctrl->command_ring.enqueue_idx = 0;
        ctrl->command_ring.cycle_state ^= 1;
    }

    __sync_synchronize();

    ctrl->doorbells->doorbells[0].doorbell = 0;

    debug_printf("[xHCI TRANSFER] Evaluate Context command posted (slot=%u)\n", slot_id);

    return 0;
}

uint64_t xhci_pack_setup_packet(usb_setup_packet_t *setup)
{
    if (!setup)
    {
        return 0;
    }

    uint64_t param = 0;
    param |= (uint64_t)setup->bmRequestType;
    param |= (uint64_t)setup->bRequest << 8;
    param |= (uint64_t)setup->wValue << 16;
    param |= (uint64_t)setup->wIndex << 32;
    param |= (uint64_t)setup->wLength << 48;
    return param;
}

void xhci_enqueue_transfer_trb(xhci_ring_t *ring, xhci_trb_t *trb)
{
    if (!ring || !trb)
    {
        return;
    }

    spin_lock(&ring->ring_lock);

    trb->control = (trb->control & ~TRB_C) |
                   (ring->cycle_state ? TRB_C : 0);

    ring->trbs[ring->enqueue_idx] = *trb;

    ring->enqueue_idx++;
    if (ring->enqueue_idx >= ring->num_trbs)
    {
        ring->enqueue_idx = 0;
        ring->cycle_state ^= 1;
    }

    spin_unlock(&ring->ring_lock);
}

int xhci_control_transfer(xhci_controller_t *ctrl,
                          xhci_device_slot_t *slot,
                          usb_setup_packet_t *setup,
                          uint64_t data_buffer_phys,
                          uint16_t data_length,
                          bool data_in)
{
    if (!ctrl || !slot || !setup)
    {
        return -1;
    }

    xhci_ring_t *ring = slot->ep0_ring;
    if (!ring)
    {
        debug_printf("[xHCI TRANSFER] EP0 ring not allocated\n");
        return -1;
    }

    xhci_trb_t setup_trb = {0};
    setup_trb.parameter = xhci_pack_setup_packet(setup);
    setup_trb.status = 8;

    uint32_t trt = 0;
    if (data_length > 0)
    {
        trt = data_in ? (3 << 16) : (2 << 16);
    }
    setup_trb.control = (2 << 10) | TRB_IDT | trt;
    if (data_length > 0)
    {
        setup_trb.control |= TRB_CH;
    }

    xhci_trb_t data_trb = {0};
    if (data_length > 0)
    {
        data_trb.parameter = data_buffer_phys;
        data_trb.status = data_length;
        data_trb.control = (3 << 10) | TRB_CH | (data_in ? (1 << 16) : 0);
    }

    xhci_trb_t status_trb = {0};
    status_trb.parameter = 0;
    status_trb.status = 0;

    bool status_in = (data_length == 0) || !data_in;
    status_trb.control = (4 << 10) | (1 << 5) | (status_in ? (1 << 16) : 0);

    debug_printf("[xHCI TRANSFER] TRB Chain Dump (BEFORE enqueue):\n");
    debug_printf("  SETUP: param=0x%016llx status=0x%08x control=0x%08x\n",
                 setup_trb.parameter, setup_trb.status, setup_trb.control);
    if (data_length > 0) {
        debug_printf("  DATA:  param=0x%016llx status=0x%08x control=0x%08x\n",
                     data_trb.parameter, data_trb.status, data_trb.control);
    }
    debug_printf("  STATUS: param=0x%016llx status=0x%08x control=0x%08x\n",
                 status_trb.parameter, status_trb.status, status_trb.control);

    uint32_t setup_idx = ring->enqueue_idx;
    xhci_enqueue_transfer_trb(ring, &setup_trb);
    uint32_t data_idx = ring->enqueue_idx;
    if (data_length > 0)
    {
        xhci_enqueue_transfer_trb(ring, &data_trb);
    }
    uint32_t status_idx = ring->enqueue_idx;
    xhci_enqueue_transfer_trb(ring, &status_trb);

    debug_printf("[xHCI TRANSFER] TRB Chain Dump (AFTER enqueue):\n");
    debug_printf("  SETUP [%u]: control=0x%08x\n", setup_idx, ring->trbs[setup_idx].control);
    if (data_length > 0) {
        debug_printf("  DATA  [%u]: control=0x%08x\n", data_idx, ring->trbs[data_idx].control);
    }
    debug_printf("  STATUS[%u]: control=0x%08x\n", status_idx, ring->trbs[status_idx].control);

    __sync_synchronize();

    ctrl->doorbells->doorbells[slot->slot_id].doorbell = 1;

    debug_printf("[xHCI TRANSFER] Ring doorbell: slot=%u target=1\n", slot->slot_id);
    debug_printf("[xHCI TRANSFER] Control transfer posted: setup=%016llx len=%u dir=%s\n",
                 setup_trb.parameter, data_length, data_in ? "IN" : "OUT");

    return 0;
}

int xhci_queue_interrupt_transfer(xhci_device_slot_t *slot)
{
    if (!slot || !slot->interrupt_ring)
    {
        return -1;
    }

    xhci_ring_t *ring = slot->interrupt_ring;

    xhci_trb_t trb = {0};
    trb.parameter = slot->interrupt_data_buffer_phys;
    trb.status = 8;
    trb.control = (1 << 10) | (1 << 5);

    spin_lock(&ring->ring_lock);

    trb.control |= (ring->cycle_state ? TRB_C : 0);
    ring->trbs[ring->enqueue_idx] = trb;

    ring->enqueue_idx++;
    if (ring->enqueue_idx >= ring->num_trbs)
    {
        ring->enqueue_idx = 0;
        ring->cycle_state ^= 1;
    }

    spin_unlock(&ring->ring_lock);

    return 0;
}

void xhci_handle_transfer_event(xhci_controller_t *ctrl, xhci_trb_t *event_trb)
{
    if (!ctrl || !event_trb)
    {
        return;
    }

    uint8_t slot_id = (event_trb->control >> 24) & 0xFF;
    uint8_t endpoint_id = (event_trb->control >> 16) & 0x1F;
    uint8_t completion_code = (event_trb->status >> 24) & 0xFF;

    const char* code_str = "UNKNOWN";
    switch (completion_code) {
        case 1:  code_str = "SUCCESS"; break;
        case 4:  code_str = "USB_TXN_ERROR"; break;
        case 5:  code_str = "TRB_ERROR"; break;
        case 6:  code_str = "STALL"; break;
        case 13: code_str = "SHORT_PKT"; break;
        default: code_str = "UNKNOWN"; break;
    }

    uint32_t residual = event_trb->status & 0xFFFFFF;
    uint32_t expected = 8;
    uint32_t transferred = expected - residual;

    debug_printf("[xHCI TRANSFER EVENT] slot=%u ep=%u code=%u (%s) trb_ptr=0x%llx len=%u\n",
                 slot_id, endpoint_id, completion_code, code_str,
                 event_trb->parameter, event_trb->status & 0xFFFFFF);
    debug_printf("[xHCI TRANSFER] Transferred %u bytes (expected=%u residual=%u)\n",
                 transferred, expected, residual);

    xhci_device_slot_t *slot = xhci_get_device_slot(ctrl, slot_id);
    if (!slot)
    {
        debug_printf("[xHCI TRANSFER] Unknown slot ID %u\n", slot_id);
        return;
    }

    if (endpoint_id == slot->keyboard_endpoint_dci && slot->interrupt_ring)
    {
        if (completion_code == TRB_COMPLETION_SUCCESS ||
            completion_code == TRB_COMPLETION_SHORT_PKT)
        {
            usb_boot_keyboard_report_t *report =
                (usb_boot_keyboard_report_t *)slot->interrupt_data_buffer_virt;

            xhci_process_keyboard_report(report);

            xhci_queue_interrupt_transfer(slot);

            __sync_synchronize();
            ctrl->doorbells->doorbells[slot->slot_id].doorbell = endpoint_id;
        }
        return;
    }

    if (endpoint_id != 1)
    {
        debug_printf("[xHCI TRANSFER] Unexpected endpoint %u\n", endpoint_id);
        return;
    }

    if (completion_code != TRB_COMPLETION_SUCCESS &&
        completion_code != TRB_COMPLETION_SHORT_PKT)
    {
        debug_printf("[xHCI TRANSFER] Transfer failed: code=%u\n", completion_code);

        if (completion_code == 6)
        {
            debug_printf("[xHCI TRANSFER] Endpoint %u stalled - device error\n", endpoint_id);
            slot->state = ENUM_STATE_ERROR;
            return;
        }
        else if (completion_code == 3)
        {
            debug_printf("[xHCI TRANSFER] Babble error - device sent too much data\n");
            slot->state = ENUM_STATE_ERROR;
            return;
        }
        else
        {
            debug_printf("[xHCI TRANSFER] Unexpected completion code: %u\n", completion_code);
        }

        slot->state = ENUM_STATE_ERROR;
        return;
    }

    if (slot->state == ENUM_STATE_WAIT_GET_DESCRIPTOR)
    {
        usb_device_desc_t *desc = (usb_device_desc_t *)slot->descriptor_buffer_virt;

        if (!usb_validate_device_desc(desc))
        {
            debug_printf("[xHCI TRANSFER] Invalid device descriptor\n");
            slot->state = ENUM_STATE_ERROR;
            return;
        }

        memcpy(&slot->device_desc, desc, sizeof(usb_device_desc_t));

        debug_printf("[xHCI ENUM] Device enumerated:\n");
        debug_printf("  VID=%04x PID=%04x Class=%02x\n",
                     desc->idVendor, desc->idProduct, desc->bDeviceClass);
        debug_printf("  MaxPacketSize0=%u bcdUSB=%04x\n",
                     desc->bMaxPacketSize0, desc->bcdUSB);

        usb_setup_packet_t setup_config = {
            .bmRequestType = 0x80,
            .bRequest = USB_REQ_GET_DESCRIPTOR,
            .wValue = (USB_DESC_CONFIGURATION << 8) | 0,
            .wIndex = 0,
            .wLength = 64};

        if (xhci_control_transfer(ctrl, slot, &setup_config,
                                  slot->descriptor_buffer_phys, 64, true) < 0)
        {
            debug_printf("[xHCI TRANSFER] Failed to post Get Config Descriptor\n");
            slot->state = ENUM_STATE_ERROR;
            return;
        }

        slot->state = ENUM_STATE_WAIT_GET_CONFIG_DESC;
    }
}
