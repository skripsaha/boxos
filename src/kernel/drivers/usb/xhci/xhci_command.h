#ifndef XHCI_COMMAND_H
#define XHCI_COMMAND_H

#include "xhci.h"
#include "xhci_trb.h"
#include "klib.h"

typedef enum {
    CMD_STATE_IDLE = 0,
    CMD_STATE_POSTED,
    CMD_STATE_COMPLETED,
    CMD_STATE_TIMEOUT,
    CMD_STATE_ERROR
} xhci_cmd_state_t;

struct xhci_pending_cmd {
    uint64_t trb_phys;
    uint64_t timestamp_posted;
    uint32_t sequence;
    uint8_t slot_id;
    uint8_t state;
    uint8_t trb_type;           /* which command this was */
    uint8_t completion_code;
    uint32_t completion_param;
};

#define XHCI_MAX_PENDING_CMDS 16
#define XHCI_CMD_TIMEOUT_MS 5000

extern struct xhci_pending_cmd pending_cmds[XHCI_MAX_PENDING_CMDS];
extern spinlock_t pending_cmds_lock;
extern uint32_t cmd_sequence;

void xhci_command_init(void);

int xhci_post_enable_slot_cmd(xhci_controller_t* ctrl);
int xhci_post_disable_slot_cmd(xhci_controller_t* ctrl, uint8_t slot_id);
int xhci_post_address_device_cmd(xhci_controller_t* ctrl, uint8_t slot_id, uint64_t input_ctx_phys);
int xhci_post_configure_endpoint_cmd(xhci_controller_t* ctrl, uint8_t slot_id, uint64_t input_ctx_phys);
int xhci_post_evaluate_context_cmd(xhci_controller_t* ctrl, uint8_t slot_id, uint64_t input_ctx_phys);

/* Endpoint recovery. Reset Endpoint clears a halt; Set TR Dequeue tells the
 * controller where to pick the ring up again. Neither belongs to enumeration,
 * and their completions must not be mistaken for one of its steps. */
int xhci_post_reset_endpoint_cmd(xhci_controller_t* ctrl, uint8_t slot_id, uint8_t dci);
int xhci_post_set_tr_dequeue_cmd(xhci_controller_t* ctrl, uint8_t slot_id,
                                 uint8_t dci, uint64_t dequeue_ptr_with_dcs);

/* Wait until no command is outstanding, draining the event ring while doing
 * so. A class driver that has just asked the controller to unhalt one of its
 * endpoints has to know that happened before it uses the endpoint again. */
int xhci_command_wait_idle(xhci_controller_t* ctrl, uint32_t timeout_ms);

void xhci_handle_command_completion(xhci_controller_t* ctrl, xhci_trb_t* event);
void xhci_check_command_timeouts(xhci_controller_t* ctrl);

#endif
