#ifndef XHCI_TRB_H
#define XHCI_TRB_H

#include "ktypes.h"

typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

#define TRB_TYPE_NORMAL              1
#define TRB_TYPE_SETUP_STAGE         2
#define TRB_TYPE_DATA_STAGE          3
#define TRB_TYPE_STATUS_STAGE        4
#define TRB_TYPE_LINK                6
#define TRB_TYPE_NO_OP_CMD           23
#define TRB_TYPE_ENABLE_SLOT         9
#define TRB_TYPE_DISABLE_SLOT        10
#define TRB_TYPE_ADDRESS_DEVICE      11
#define TRB_TYPE_CONFIGURE_ENDPOINT  12
#define TRB_TYPE_EVALUATE_CONTEXT    13
#define TRB_TYPE_RESET_ENDPOINT      14
#define TRB_TYPE_STOP_ENDPOINT       15
#define TRB_TYPE_SET_TR_DEQUEUE      16

#define TRB_TYPE_TRANSFER_EVENT      32
#define TRB_TYPE_COMMAND_COMPLETION  33
#define TRB_TYPE_PORT_STATUS_CHANGE  34

#define TRB_C                        (1 << 0)
#define TRB_TC                       (1 << 1)
#define TRB_ENT                      (1 << 1)
#define TRB_ISP                      (1 << 2)
#define TRB_CH                       (1 << 4)
#define TRB_IOC                      (1 << 5)
#define TRB_IDT                      (1 << 6)

#define TRB_BSR                      (1 << 9)

#define TRB_TYPE_SHIFT               10
#define TRB_TYPE_MASK                (0x3F << TRB_TYPE_SHIFT)

#define TRB_SET_TYPE(type)           (((type) << TRB_TYPE_SHIFT) & TRB_TYPE_MASK)
#define TRB_GET_TYPE(ctrl)           (((ctrl) & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT)

#define TRB_COMPLETION_SUCCESS          1
#define TRB_COMPLETION_DATA_BUFFER_ERR  2
#define TRB_COMPLETION_BABBLE           3
#define TRB_COMPLETION_USB_TRANS_ERR    4
#define TRB_COMPLETION_TRB_ERROR        5
#define TRB_COMPLETION_STALL            6
#define TRB_COMPLETION_RESOURCE_ERR     7
#define TRB_COMPLETION_BANDWIDTH_ERR    8
#define TRB_COMPLETION_NO_SLOTS         9
#define TRB_COMPLETION_SLOT_NOT_ENABLED 11
#define TRB_COMPLETION_EP_NOT_ENABLED   12
#define TRB_COMPLETION_SHORT_PKT        13
#define TRB_COMPLETION_RING_UNDERRUN    14
#define TRB_COMPLETION_RING_OVERRUN     15
#define TRB_COMPLETION_PARAMETER_ERR    17
#define TRB_COMPLETION_BANDWIDTH_OVER   18
#define TRB_COMPLETION_CONTEXT_STATE    19
#define TRB_COMPLETION_NO_PING_RESPONSE 20
#define TRB_COMPLETION_EVENT_RING_FULL  21
#define TRB_COMPLETION_INCOMPATIBLE_DEV 22
#define TRB_COMPLETION_MISSED_SERVICE   23
#define TRB_COMPLETION_CMD_RING_STOPPED 24
#define TRB_COMPLETION_COMMAND_ABORTED  25
#define TRB_COMPLETION_STOPPED          26
#define TRB_COMPLETION_STOPPED_LENGTH   27
#define TRB_COMPLETION_SPLIT_TRANS_ERR  36

const char* xhci_completion_name(uint8_t code);

const char* xhci_command_name(uint8_t trb_type);

#endif