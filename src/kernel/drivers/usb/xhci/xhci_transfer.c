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

/*
 * A control transfer is THREE transfer descriptors, not one.
 *
 * The Setup Stage, the optional Data Stage and the Status Stage are separate
 * TDs, and the Chain bit is what joins TRBs *within* one TD — a scatter-gather
 * data stage, say. The old code set Chain on the Setup and Data stages, which
 * welded all three into a single TD, and that is not a cosmetic difference:
 *
 *   when a device returns fewer bytes than were asked for, the controller
 *   finishes the current TD early and skips the rest of it.
 *
 * With all three welded together, "the rest of it" is the Status Stage — the
 * only TRB carrying Interrupt On Completion. The device answers correctly, the
 * controller does exactly what the specification says, and the driver waits
 * forever for an event that was skipped along with the stage that would have
 * raised it. Asking a device for more descriptor than it has is not an
 * unusual thing to do; it is how you find out how much it has.
 *
 * So: no Chain anywhere, Interrupt On Completion on the Status Stage alone,
 * and therefore exactly one Transfer Event per control transfer. Interrupt On
 * Short Packet is deliberately not set — it would add a second event per short
 * transfer for a length this driver does not need, because every descriptor it
 * reads states its own.
 */
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

    /* Transfer Type in the Setup Stage: 0 = no data, 2 = OUT data, 3 = IN. */
    uint32_t trt = 0;
    if (data_length > 0) {
        trt = data_in ? 3 : 2;
    }

    xhci_trb_t setup_trb = {0};
    setup_trb.parameter = pack_setup_packet(setup);
    setup_trb.status = 8;
    setup_trb.control = TRB_SET_TYPE(TRB_TYPE_SETUP_STAGE) | TRB_IDT | (trt << 16);
    if (xhci_ring_enqueue(ring, &setup_trb) == 0) {
        debug_printf("[xHCI TRANSFER] EP0 ring full posting Setup Stage\n");
        return -1;
    }

    if (data_length > 0) {
        xhci_trb_t data_trb = {0};
        data_trb.parameter = data_buffer_phys;
        data_trb.status = data_length;
        data_trb.control = TRB_SET_TYPE(TRB_TYPE_DATA_STAGE) |
                           (data_in ? (1u << 16) : 0);
        if (xhci_ring_enqueue(ring, &data_trb) == 0) {
            debug_printf("[xHCI TRANSFER] EP0 ring full posting Data Stage\n");
            return -1;
        }
    }

    /* The Status Stage runs opposite to the data: an IN data stage is
     * acknowledged with an OUT status, and a transfer with no data at all is
     * acknowledged IN. */
    bool status_in = (data_length == 0) || !data_in;
    xhci_trb_t status_trb = {0};
    status_trb.control = TRB_SET_TYPE(TRB_TYPE_STATUS_STAGE) | TRB_IOC |
                         (status_in ? (1u << 16) : 0);
    if (xhci_ring_enqueue(ring, &status_trb) == 0) {
        debug_printf("[xHCI TRANSFER] EP0 ring full posting Status Stage\n");
        return -1;
    }

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
    /* Interrupt On Short Packet as well as On Completion: a keyboard report
     * shorter than the endpoint's maximum is ordinary, and without ISP such a
     * transfer would complete with nothing to say it had. */
    trb.control = TRB_SET_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP;

    uint64_t trb_phys = xhci_ring_enqueue(slot->interrupt_ring, &trb);
    if (trb_phys == 0) {
        return -1;
    }

    return 0;
}

/*
 * A stalled endpoint stays stalled.
 *
 * Once the controller halts an endpoint, nothing queued on it runs again until
 * a Reset Endpoint command clears the halt and a Set TR Dequeue Pointer tells
 * the controller where to resume. Without that, a keyboard that stalls once —
 * one glitch, one marginal cable — is a keyboard that never types again for
 * the rest of the boot, and nothing anywhere says why.
 */
static void xhci_recover_stalled_endpoint(xhci_controller_t* ctrl,
                                          xhci_device_slot_t* slot,
                                          uint8_t dci)
{
    kprintf("[xHCI] slot %u endpoint %u stalled — resetting it\n",
            slot->slot_id, dci);

    if (xhci_post_reset_endpoint_cmd(ctrl, slot->slot_id, dci) < 0) {
        return;
    }

    /* Resume where the ring now stands. The reset leaves the endpoint's
     * dequeue pointer on the TRB that stalled; the ring's own enqueue position
     * is where the next transfer will be written. */
    xhci_ring_t* ring = (dci == 1) ? slot->ep0_ring : slot->interrupt_ring;
    if (!ring) {
        return;
    }

    uint64_t resume = ring->trbs_phys +
                      (uint64_t)ring->enqueue_idx * sizeof(xhci_trb_t);
    xhci_post_set_tr_dequeue_cmd(ctrl, slot->slot_id, dci,
                                 resume | (ring->cycle_state ? 1u : 0u));
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

    bool ok = (completion_code == TRB_COMPLETION_SUCCESS ||
               completion_code == TRB_COMPLETION_SHORT_PKT);

    /* EP0 is always DCI 1. Test it first, so that a device whose interrupt
     * endpoint somehow reported the same number cannot divert control
     * transfers into the keyboard path. */
    if (endpoint_id == 1) {
        if (!ok) {
            /* A stall on an optional class request is the device declining it,
             * not the device failing. Clear the pipe and carry on from there.
             *
             * Tearing the slot down here — which is what happened before —
             * also had a second fault: the recovery commands it issued
             * referenced a transfer ring the very next statement freed, so the
             * controller was left pointed at memory the kernel had given back. */
            if (completion_code == TRB_COMPLETION_STALL &&
                xhci_enum_stall_is_tolerable(slot->state)) {
                xhci_enum_recover_ep0(ctrl, slot);
                return;
            }

            kprintf("[xHCI] slot %u: control transfer failed with completion "
                    "code %u at enumeration step %u\n",
                    slot_id, completion_code, slot->state);
            xhci_device_slot_cleanup(ctrl, slot);
            return;
        }

        /* Every step of enumeration lives in one place. This used to decode
         * descriptors here as well, which meant two code paths took turns
         * driving the same state machine. */
        xhci_enum_advance_state(ctrl, slot_id, TRB_COMPLETION_SUCCESS);
        return;
    }

    if (slot->interrupt_ring && endpoint_id == slot->keyboard_endpoint_dci) {
        if (ok) {
            usb_boot_keyboard_report_t* report =
                (usb_boot_keyboard_report_t*)slot->interrupt_data_buffer_virt;
            xhci_process_keyboard_report(report);
        } else if (completion_code == TRB_COMPLETION_STALL) {
            xhci_recover_stalled_endpoint(ctrl, slot, endpoint_id);
        } else {
            /* Transaction errors are what a marginal cable looks like, and the
             * controller has already retried them CErr times. Re-arming is the
             * right answer; going quiet is not. */
            debug_printf("[xHCI] slot %u keyboard transfer completion %u\n",
                         slot_id, completion_code);
        }

        /* Re-arm regardless, and after the recovery above rather than before
         * it: a Set TR Dequeue Pointer names where the controller will resume,
         * and the transfer queued here lands on exactly that TRB. Swapping the
         * two would point the endpoint past the very transfer meant to restart
         * it. An interrupt endpoint that is not re-armed after every report
         * simply stops delivering. */
        if (xhci_queue_interrupt_transfer(slot) == 0) {
            __sync_synchronize();
            ctrl->doorbells->doorbells[slot->slot_id].doorbell = endpoint_id;
        }
        return;
    }
}
