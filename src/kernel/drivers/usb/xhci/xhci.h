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

/*
 * The Command Ring, in TRBs.
 *
 * Stated here rather than at the call site because the table that remembers
 * what was posted is exactly parallel to it. A command's identity is not a
 * number this driver invents and then has to search for — it is the position
 * the TRB was written in, which is precisely what the controller hands back in
 * the Command Completion Event (xHCI 1.2 Section 6.4.2.2). One entry per ring
 * slot means the lookup is arithmetic, there is nothing to allocate, and a
 * completion either falls inside this controller's ring or it does not.
 */
#define XHCI_CMD_RING_TRBS 256

typedef enum {
    XHCI_CMD_FREE = 0,
    XHCI_CMD_POSTED
} xhci_cmd_state_t;

/*
 * One command the controller has been asked for and has not yet answered.
 *
 * `owner` is the device this command was asked on behalf of, and `owner_epoch`
 * is which tenancy of that slot asked. A slot structure outlives the devices
 * that pass through it, so the pointer alone would let an answer to a question
 * the previous occupant asked be handed to the one that replaced it — which is
 * a real fault this driver has already been bitten by once, in the form of a
 * slot NUMBER being reused. The epoch closes it for the pointer as well.
 */
typedef struct xhci_pending_cmd {
    xhci_device_slot_t* owner;
    uint32_t owner_epoch;
    uint64_t posted_at;             /* TSC */
    uint8_t  state;                 /* xhci_cmd_state_t */
    uint8_t  trb_type;
    uint8_t  slot_id;               /* as posted; 0 for Enable Slot */
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

    /*
     * What this controller has been asked and has not yet answered.
     *
     * Per controller, not per machine. It used to be one table shared by every
     * xHCI on the board, and the two watchdogs that walk it take a controller
     * as their argument: the first controller's tick reaped the second's
     * commands, looked the slot up in its OWN table, found nothing, and
     * dropped the command without retiring the device that was waiting for
     * it. On a desktop with a chipset controller and one inside a graphics
     * card that happens on every single timeout.
     */
    xhci_pending_cmd_t pending_cmds[XHCI_CMD_RING_TRBS];
    spinlock_t         pending_lock;

    /*
     * When this controller last answered anything.
     *
     * The command ring is a queue and the controller works through it in
     * order, so a completion for a later command is proof that every earlier
     * one has already been answered. That makes "nothing has been answered for
     * N milliseconds" the only honest test for a stuck ring — and "this
     * particular command has been outstanding for N milliseconds" the wrong
     * one, because it fires on a controller that is merely slower than the
     * budget while it is visibly still working.
     *
     * Measured on a live board: a driver using the second test aborted the
     * ring, and the Address Device it had given up on then completed with
     * Success. Two devices were thrown away for being answered late.
     */
    uint64_t last_cmd_answer;

    /* How many times the ring has been nudged since it last answered. Reset by
     * any completion, because a controller that answered is a controller that
     * heard. */
    uint32_t cmd_nudges;

    /*
     * How many times a drain found somebody else already draining and went
     * away.
     *
     * The drain is a trylock: whoever holds it will reach every event on the
     * ring, so queueing behind them adds nothing and risks a core waiting for
     * a lock its own stack frame holds. That is right — but it is also the one
     * way this driver can stop reading the ring without anything saying so,
     * and a controller that has executed a command whose completion nobody
     * collects is indistinguishable from one that never executed it.
     */
    volatile uint32_t drain_skips;

    /* Which core is inside the drain, one-based; zero when nobody is. A
     * nested call from the SAME core must leave — it would be waiting for a
     * lock its own stack frame holds — while a call from another core can
     * afford to wait, because the holder will finish. Telling those two apart
     * is the difference between "somebody is reading the ring" and "nobody
     * is". */
    volatile uint32_t drain_owner;

    /*
     * The deepest the event ring has ever been, and whether that has been said.
     *
     * The controller writes events into a ring software owns, and when it runs
     * out of room the events stop being written — a Transfer Event that was
     * never posted is a transfer nobody will ever be told about. Nothing here
     * could ever have noticed: the ring's occupancy was not a number anything
     * kept, so "we are one burst away from losing events" and "we have plenty
     * of room" looked identical from every line of this driver.
     *
     * Kept as the high-water mark of a single drain rather than an instant
     * reading, because that is the quantity that matters: it is how far behind
     * the controller software was allowed to fall.
     */
    uint32_t event_high_water;
    bool     event_pressure_said;

    /*
     * The one device this controller is bringing up right now, or NULL.
     *
     * Enumeration is serialised per controller because the bus requires it:
     * a device between its port reset and its address answers to address zero,
     * and only one may be doing that at a time. Everything else found waits in
     * ENUM_STATE_QUEUED for its turn.
     */
    xhci_device_slot_t* enum_active;

    /* How many times each root port has been tried and failed. Cleared when a
     * device on it is finally configured, so a port that works after two goes
     * costs two goes and not a budget for the life of the machine. */
    uint8_t enum_attempts[XHCI_PORT_MAP_ENTRIES];
} xhci_controller_t;

int xhci_init(void);

/* Look at every root port until it has had the time the bus specifies to say
 * what is on it, enumerating whatever answers. Called once, after the
 * controller is running. */
void xhci_survey_root_ports(xhci_controller_t* ctrl);
int xhci_reset(xhci_controller_t* ctrl);
int xhci_start(xhci_controller_t* ctrl);
xhci_controller_t* xhci_get_controller(void);

/*
 * Bring back any controller that has stopped itself with an internal or host
 * system error. Called from the K-Core guide loop, because it waits — up to a
 * second for a reset — and because that is where deferred work in this kernel
 * actually runs. Does nothing, cheaply, when nothing has failed.
 */
void xhci_recover_if_needed(void);

/* Every controller in service. Anything that means "all the USB on this
 * machine" walks these rather than asking for "the" controller. */
uint8_t            xhci_controller_count(void);
xhci_controller_t* xhci_controller_at(uint8_t index);

#endif
