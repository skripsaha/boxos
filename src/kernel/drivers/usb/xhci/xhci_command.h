#ifndef XHCI_COMMAND_H
#define XHCI_COMMAND_H

#include "xhci.h"
#include "xhci_trb.h"
#include "klib.h"

#define XHCI_CMD_TIMEOUT_MS 5000

#define XHCI_CMD_ABORT_TIMEOUT_MS 5000

#define XHCI_CMD_SLOW_MS 250

#define XHCI_CMD_QUIET_MS 1000

#define XHCI_CMD_NUDGES   1

void xhci_command_init(xhci_controller_t* ctrl);

int xhci_post_enable_slot_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                              uint8_t slot_type);
int xhci_post_disable_slot_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                               uint8_t slot_id);
int xhci_post_address_device_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint64_t input_ctx_phys,
                                 bool block_set_address);
int xhci_post_configure_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                     uint8_t slot_id, uint64_t input_ctx_phys);

int xhci_post_reset_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint8_t dci);

int xhci_post_stop_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                uint8_t slot_id, uint8_t dci);
int xhci_post_set_tr_dequeue_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint8_t dci,
                                 uint64_t dequeue_ptr_with_dcs);

int xhci_command_wait_idle(xhci_controller_t* ctrl, uint32_t timeout_ms);

bool xhci_command_pending_for(xhci_controller_t* ctrl,
                              const xhci_device_slot_t* slot);

unsigned xhci_command_outstanding(xhci_controller_t* ctrl);
bool xhci_command_oldest_for(xhci_controller_t* ctrl,
                             const xhci_device_slot_t* slot,
                             uint8_t* out_trb_type, uint32_t* out_age_ms);

void xhci_handle_command_completion(xhci_controller_t* ctrl, xhci_trb_t* event);

void xhci_check_command_timeouts(xhci_controller_t* ctrl);

void xhci_command_abort_if_wanted(xhci_controller_t* ctrl);

#endif