#ifndef XHCI_H
#define XHCI_H

#include "ktypes.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "xhci_device.h"
#include "pci.h"
#include "boxos_limits.h"

#define XHCI_MAX_PORTS       127

typedef struct xhci_device_slot xhci_device_slot_t;
typedef struct xhci_pending_cmd xhci_pending_cmd_t;

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
    xhci_erst_t event_ring_segment_table;

    xhci_dcbaa_t* dcbaa;
    uint64_t dcbaa_phys;

    uint8_t max_slots;
    uint8_t max_ports;
    uint16_t max_interrupters;
    uint8_t context_size;

    uint64_t mmio_base_phys;
    uint64_t mmio_size;

    uint8_t irq_line;
    bool use_polling;
    bool running;
    bool error_state;
    bool initialized;

    uint8_t num_devices;
} xhci_controller_t;

int xhci_init(void);
int xhci_reset(xhci_controller_t* ctrl);
int xhci_start(xhci_controller_t* ctrl);
xhci_controller_t* xhci_get_controller(void);

#endif
