#ifndef XHCI_H
#define XHCI_H

#include "ktypes.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "xhci_device.h"
#include "klib.h"
#include "pci.h"
#include "boxos_limits.h"

/* MaxPorts in HCSPARAMS1 is an eight-bit field, so 256 entries covers every
 * value a controller can put there. This is the width of the register, not a
 * limit this driver chose. */
#define XHCI_PORT_MAP_ENTRIES 256

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

    /* One drainer of the event ring at a time. The interrupt handler is no
     * longer the only one: a transfer waiting for its completion drains the
     * ring itself rather than trusting that an interrupt will arrive to do it,
     * which is what lets bulk I/O work before interrupts are routed and on a
     * controller that has none. Two drainers without this would advance the
     * dequeue pointer past each other's events. */
    spinlock_t event_lock;
    xhci_erst_t event_ring_segment_table;

    xhci_dcbaa_t* dcbaa;
    uint64_t dcbaa_phys;

    uint8_t max_slots;
    uint8_t max_ports;
    uint16_t max_interrupters;
    uint8_t context_size;

    uint64_t mmio_base_phys;
    uint64_t mmio_size;

    /* Scratchpad — pages the controller asked for so it has somewhere to keep
     * its own internal state. It asks in HCSPARAMS2 and reads the answer out
     * of DCBAA[0]. A controller that asks and is not answered has nowhere to
     * put that state, which is not an error it reports; it is an error it
     * behaves. QEMU asks for none, which is why this went missing. */
    uint64_t* scratchpad_array;
    uint64_t  scratchpad_array_phys;
    uint32_t  scratchpad_count;

    /* What each root port actually is, from the Supported Protocol capability.
     * Indexed by the 1-based port number; zero means the controller never said
     * and this driver will not guess. On a real PCH the USB2 and USB3 port
     * numbers are disjoint ranges over the same physical sockets, so "port 3"
     * alone does not identify anything. */
    uint8_t port_major[XHCI_PORT_MAP_ENTRIES];
    uint8_t port_slot_type[XHCI_PORT_MAP_ENTRIES];

    uint8_t irq_line;       /* PCI interrupt line, for the INTx fallback */
    uint8_t irq_vector;     /* MSI vector when MSI is in use, else 0 */
    bool use_msi;
    bool use_polling;
    bool running;
    bool error_state;
    bool initialized;

    uint8_t num_devices;

    /* How this controller is named on the screen. A machine has several, and a
     * line that says "port 7" without saying whose port 7 is a line that
     * cannot be acted on — measured: an internal controller error was reported
     * by one of two and there was no way to tell which. */
    char    name[10];               /* "00:14.0" */

    /* How many interrupts this controller has actually delivered. Nothing
     * depends on it working — see the tick — but a device that never
     * enumerates is a different fault depending on whether its controller ever
     * spoke, and that is not something a photograph of a screen can otherwise
     * answer. */
    volatile uint32_t irq_count;
} xhci_controller_t;

int xhci_init(void);

/* Look at every root port until it has had the time the bus specifies to say
 * what is on it, enumerating whatever answers. Called once, after the
 * controller is running. */
void xhci_survey_root_ports(xhci_controller_t* ctrl);
int xhci_reset(xhci_controller_t* ctrl);
int xhci_start(xhci_controller_t* ctrl);
xhci_controller_t* xhci_get_controller(void);

/* Every controller in service. Anything that means "all the USB on this
 * machine" walks these rather than asking for "the" controller. */
uint8_t            xhci_controller_count(void);
xhci_controller_t* xhci_controller_at(uint8_t index);

#endif
