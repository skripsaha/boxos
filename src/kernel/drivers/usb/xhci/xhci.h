#ifndef XHCI_H
#define XHCI_H

#include "ktypes.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "xhci_device.h"
#include "klib.h"
#include "pci.h"
#include "boxos_limits.h"

#define XHCI_PORT_MAP_ENTRIES 256

typedef struct xhci_device_slot xhci_device_slot_t;

#define XHCI_CMD_RING_TRBS 256

typedef enum {
    XHCI_CMD_FREE = 0,
    XHCI_CMD_POSTED
} xhci_cmd_state_t;

typedef struct xhci_pending_cmd {
    xhci_device_slot_t* owner;
    uint32_t owner_epoch;
    uint64_t posted_at;
    uint8_t  state;
    uint8_t  trb_type;
    uint8_t  slot_id;
} xhci_pending_cmd_t;

typedef struct {
    pci_device_t pci_dev;

    xhci_cap_regs_t* cap_regs;
    xhci_op_regs_t* op_regs;
    xhci_runtime_regs_t* runtime_regs;
    xhci_doorbell_array_t* doorbells;
    xhci_port_regs_t* ports;
    xhci_interrupter_regs_t* interrupters;

    xhci_ring_t command_ring;
    xhci_ring_t event_ring;

    spinlock_t event_lock;
    xhci_erst_t event_ring_segment_table;

    xhci_dcbaa_t* dcbaa;
    uint64_t dcbaa_phys;

    uint8_t max_slots;

    struct xhci_device_slot*  slots;
    struct xhci_device_slot** by_id;
    uint8_t                   slot_count;
    spinlock_t                slots_lock;

    volatile uint32_t         retire_pending;
    volatile uint32_t         retire_busy;
    uint8_t max_ports;
    uint16_t max_interrupters;
    uint8_t context_size;

    uint64_t mmio_base_phys;
    uint64_t mmio_size;

    uint64_t* scratchpad_array;
    uint64_t  scratchpad_array_phys;
    uint32_t  scratchpad_count;

    uint8_t port_major[XHCI_PORT_MAP_ENTRIES];
    uint8_t port_slot_type[XHCI_PORT_MAP_ENTRIES];

    uint8_t port_pair[XHCI_PORT_MAP_ENTRIES];

    uint8_t irq_line;
    uint8_t irq_vector;
    bool use_msi;
    bool use_polling;
    bool running;
    bool error_state;
    bool initialized;

    uint8_t num_devices;

    char    name[10];

    volatile uint32_t irq_count;

    xhci_pending_cmd_t pending_cmds[XHCI_CMD_RING_TRBS];
    spinlock_t         pending_lock;

    uint64_t last_cmd_answer;

    uint32_t cmd_nudges;

    volatile uint32_t cmd_abort_wanted;

    volatile uint32_t drain_skips;

    volatile uint32_t drain_owner;

    uint32_t event_high_water;
    bool     event_pressure_said;

    uint32_t drain_longest_us;

    xhci_device_slot_t* enum_active;

    uint8_t enum_attempts[XHCI_PORT_MAP_ENTRIES];
} xhci_controller_t;

int xhci_init(void);

void xhci_survey_root_ports(xhci_controller_t* ctrl);
int xhci_reset(xhci_controller_t* ctrl);
int xhci_start(xhci_controller_t* ctrl);
xhci_controller_t* xhci_get_controller(void);

void xhci_recover_if_needed(void);

bool xhci_put_back_in_service(xhci_controller_t* ctrl);

uint8_t            xhci_controller_count(void);
xhci_controller_t* xhci_controller_at(uint8_t index);

#endif