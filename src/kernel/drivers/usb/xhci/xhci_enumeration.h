#ifndef XHCI_ENUMERATION_H
#define XHCI_ENUMERATION_H

#include "ktypes.h"
#include "xhci.h"
#include "usb_descriptors.h"

/* Enumeration is a conversation, and every step of it waits on something the
 * hardware will say when it is ready — a command completion, a transfer
 * event, a port-status change. There is no step here that waits by counting.
 * The state is what the slot is listening for next. */
typedef enum {
    ENUM_STATE_IDLE = 0,
    ENUM_STATE_CLAIMING,
    ENUM_STATE_WAIT_PORT_RESET,
    ENUM_STATE_WAIT_ENABLE_SLOT,
    ENUM_STATE_WAIT_ADDRESS_DEVICE,
    ENUM_STATE_WAIT_GET_DESC_HEADER,
    ENUM_STATE_WAIT_EVALUATE_CONTEXT,
    ENUM_STATE_WAIT_GET_DESCRIPTOR,
    ENUM_STATE_WAIT_GET_CONFIG_HEADER,
    ENUM_STATE_WAIT_GET_CONFIG_DESC,
    ENUM_STATE_WAIT_SET_CONFIGURATION,
    ENUM_STATE_WAIT_SET_PROTOCOL,
    ENUM_STATE_WAIT_SET_IDLE,
    ENUM_STATE_WAIT_EP0_RESET,
    ENUM_STATE_WAIT_EP0_DEQUEUE,
    ENUM_STATE_WAIT_CONFIGURE_ENDPOINT,
    ENUM_STATE_CONFIGURED,

    /* The device has gone and what is left of it is being taken down.
     *
     * Not idle, because the memory is still there and two other parties may
     * still have their hands on it — the controller, which does not stop
     * reading a device context until it is told to, and whatever kernel code
     * was mid-transfer when the plug came out. Not live either: nothing new
     * starts on a device that is on its way out. */
    ENUM_STATE_RETIRING
} xhci_enum_state_t;

/* There is no ENUM_STATE_ERROR. There used to be, and nothing could reach it:
 * every path that once set it now releases the slot instead, because a slot
 * parked in a state it can never leave holds a port hostage for the rest of
 * the boot. A failure here is a device you can unplug and plug back in. */

#define XHCI_MAX_DEVICE_SLOTS 64

struct xhci_device_slot {
    uint8_t slot_id;
    uint8_t port_num;
    uint8_t state;          /* xhci_enum_state_t */
    uint8_t speed;

    uint64_t timestamp_started;

    /* Device Context (allocated, pointed to by DCBAA) */
    void* dev_ctx;
    uint64_t dev_ctx_phys;

    /* Input Context (temporary, for commands) */
    uint64_t input_ctx_phys;

    /* EP0 Transfer Ring */
    xhci_ring_t* ep0_ring;
    uint64_t ep0_ring_phys;

    /* Descriptor buffer (one page, reused for all GET_DESCRIPTOR) */
    void* descriptor_buffer_virt;
    uint64_t descriptor_buffer_phys;

    /* Cached device descriptor */
    usb_device_desc_t device_desc;

    /* EP0 max packet size actually in the endpoint context. A full-speed
     * device names its own, and the number is not known until eight bytes of
     * its descriptor have been read using the wrong one. */
    uint16_t ep0_max_packet;

    /* How long the configuration descriptor said it was, and what the first
     * interface turned out to be. The class is what decides who drives this
     * device; a device nobody drives is still enumerated and left addressed
     * rather than abandoned half-configured. */
    uint16_t config_total_len;

    /* Where to carry on from once a stalled EP0 has been cleared. A device is
     * allowed to refuse an optional request, and refusing it halts the pipe;
     * this is the step that was being attempted when that happened. */
    uint8_t  stall_resume;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;

    /* Where this device sits on the bus.
     *
     * port_num is the ROOT port the whole branch hangs off, and stays that for
     * every device below it — it is what the controller is told, and what the
     * port-change handler matches on. The rest describes the path down from
     * there: the route string names the hub port at each tier, four bits per
     * tier, and is how the controller finds a device that is not plugged into
     * it directly.
     *
     * A low or full speed device behind a high speed hub also needs to name
     * that hub, because the hub is doing the speed translation and the
     * controller has to address the translator, not just the device. */
    uint32_t route_string;
    uint8_t  depth;             /* 0 = plugged into a root port */
    uint8_t  parent_slot_id;    /* the hub above, 0 when there is none */
    uint8_t  parent_port;       /* which of its ports, 1-based */
    uint8_t  tt_slot_id;        /* the high-speed hub doing translation, 0 = none */
    uint8_t  tt_port;

    /* Non-zero when this device is itself a hub, and the controller has to be
     * told so — it schedules differently for something with ports of its own. */
    uint8_t  hub_ports;
    uint8_t  tt_think_time;     /* as the hub characteristics state it, 0..3 */
    bool     multi_tt;

    /* Which configuration was selected, and the interface this driver drives. */
    uint8_t  config_value;
    uint8_t  interface_num;

    /* Every endpoint beyond EP0, indexed by Device Context Index. Allocated
     * when the slot is enabled, because the number of them is a property of
     * the device and not of this driver. EP0 is not in here — it belongs to
     * enumeration itself and lives in ep0_ring above. */
    struct xhci_endpoint* endpoints;
    uint8_t  max_dci;               /* highest DCI this device uses */
    uint32_t ep_pending_add;        /* DCI bitmap awaiting Configure Endpoint */

    /* How many callers are inside this device right now.
     *
     * A disk is pulled out of a running machine by a hand, and the hand does
     * not wait for the filesystem to finish its sentence. The endpoints, the
     * rings they hang off and the buffers those point at are not taken away
     * while anybody is still using them — which is a different question from
     * whether the device is still plugged in, and has to be asked separately.
     */
    volatile uint32_t visitors;

    /* When the departure was noticed, and whether anybody has been told it is
     * taking an unreasonable time. Only meaningful while retiring. */
    uint64_t retire_started;
    bool     retire_warned;

    /* Who ended up driving this device, and the endpoints it uses. */
    uint8_t  driver;                /* XHCI_DRIVER_* */
    uint8_t  ep_interrupt_in;       /* DCI, 0 when none */
    uint8_t  ep_bulk_in;
    uint8_t  ep_bulk_out;
};

/* Who claimed a device. A device nobody claims is not a failure — it is
 * enumerated, configured and left addressed, which is exactly the ground a
 * class driver stands on when one arrives. */
#define XHCI_DRIVER_NONE     0
#define XHCI_DRIVER_KEYBOARD 1
#define XHCI_DRIVER_STORAGE  2
#define XHCI_DRIVER_HUB      3

void xhci_enumeration_init(void);
int xhci_enumerate_device(xhci_controller_t* ctrl, uint8_t port);

/* Enumerate a device found on a port of a hub. The hub has already reset the
 * port and knows what speed answered, so this starts where a root-port
 * enumeration starts after its own reset: at Enable Slot. */
int xhci_enumerate_behind_hub(xhci_controller_t* ctrl,
                              xhci_device_slot_t* hub,
                              uint8_t hub_port, uint8_t speed);

/* The slot this device's hub occupies, or NULL. */
xhci_device_slot_t* xhci_get_device_slot_by_id(uint8_t slot_id);
void xhci_enum_advance_state(xhci_controller_t* ctrl, uint8_t slot_id, uint8_t completion_code);

/* The port finished the reset enumeration asked for. Called from the
 * port-status change path, which is the only thing that knows when. */
void xhci_enum_port_reset_done(xhci_controller_t* ctrl, uint8_t port);

/* How long any single step of enumeration may go unanswered. Every step is one
 * round trip on the bus, so this is generous by three orders of magnitude —
 * it is here to name a failure, never to pace a success. */
#define XHCI_ENUM_TIMEOUT_MS 2000

/* Report and release any enumeration that stopped being answered. Driven from
 * the timer tick. */
void xhci_enum_watchdog(xhci_controller_t* ctrl);

/* Wait until nothing is mid-enumeration, draining events while waiting.
 *
 * Enumeration is driven by interrupts, so a device present at power-on is
 * still being asked who it is while the rest of the kernel carries on booting.
 * Anything that needs to know what is attached — a filesystem looking for its
 * volume, say — has to wait for the conversation to finish rather than ask
 * before it has. Returns the number of slots still unsettled, so zero means
 * everything that was going to arrive has. */
int xhci_enum_settle(xhci_controller_t* ctrl, uint32_t timeout_ms);

/* Walk every device that has finished enumerating. */
typedef void (*xhci_slot_visitor)(void* ctx, xhci_device_slot_t* slot);
void xhci_enum_for_each_configured(xhci_slot_visitor visit, void* ctx);

/* True when a STALL at this point in enumeration is the device declining an
 * optional request rather than the conversation failing. */
bool xhci_enum_stall_is_tolerable(uint8_t state);

/* Clear a halted EP0 and resume enumeration from where it stalled. */
void xhci_enum_recover_ep0(xhci_controller_t* ctrl, xhci_device_slot_t* slot);
xhci_device_slot_t* xhci_get_device_slot(xhci_controller_t* ctrl, uint8_t slot_id);
xhci_device_slot_t* xhci_get_device_slot_by_port(uint8_t port);

/*
 * Unplugging is not a free().
 *
 * When a device goes away, three parties have to agree before its memory can
 * go back: the controller has to stop reading the device context and the
 * transfer rings, which it does when it has carried out a Disable Slot; any
 * kernel caller mid-transfer has to come out; and only then is there nothing
 * left pointing at the pages.
 *
 * So a departure happens in two steps. The first runs wherever the departure
 * was noticed — an interrupt handler, usually — and takes nothing away. The
 * second runs from a service pass, where waiting is allowed, and dismantles
 * what nobody is holding any more.
 */

/* Step inside a device. False when it has gone or is going, and then nothing
 * inside it may be touched. Every caller that gets true must leave. */
bool xhci_slot_enter(xhci_device_slot_t* slot);
void xhci_slot_leave(xhci_device_slot_t* slot);

/* This device has gone. Marks it, and asks the controller for the slot back.
 * Frees nothing. Safe from an interrupt handler. */
void xhci_slot_retire(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

/* Take down every device that has been retired and that nobody is inside any
 * more. Returns how many were taken down. Must be called somewhere that is
 * allowed to wait — never from an interrupt handler. */
int  xhci_slot_service(xhci_controller_t* ctrl);

/* Cheap enough for the idle loop: a single atomic load when there is nothing
 * to take down, which is almost always. */
bool xhci_slot_retire_pending(void);
void xhci_slot_service_if_pending(void);

/* How long a device may take to let go of before anybody is told about it.
 * A transfer that will never answer times out in five seconds, so a device
 * still held after this is one something is genuinely stuck on. */
#define XHCI_RETIRE_PATIENCE_MS 15000

#endif
