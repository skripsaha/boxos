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

    /*
     * Found, and waiting its turn.
     *
     * Only one device on a controller is brought up at a time. Between a port
     * reset and the address that ends it, a device answers to address zero —
     * that is what the Default state IS — and a bus with two of them on it is
     * a bus where the host cannot tell which one replied. USB 2.0 §9.1.1.3 and
     * §7.1.7.5 say so; the xHCI command ring enforces it the hard way, because
     * it executes commands strictly in order and one Address Device that will
     * not complete blocks every command queued behind it.
     *
     * Measured on two different machines: five devices found at boot, five
     * port resets issued together, five Address Devices posted back to back —
     * and the controller executed three of them and stopped for twenty
     * seconds, with the other two devices and three Evaluate Contexts stuck
     * behind the one that never answered. It came up about half the time,
     * depending on which device won.
     */
    ENUM_STATE_QUEUED,
    ENUM_STATE_WAIT_PORT_RESET,

    /*
     * The reset is over and the device is coming up in the Default state.
     *
     * USB 2.0 §7.1.7.5 gives it TRSTRCY — ten milliseconds — before the host
     * may address it, and this driver used to spend them spinning. Where it
     * spun is the problem: the port-status change arrives on the event ring,
     * so that code runs inside the drain, which holds the ring lock, and this
     * kernel's spinlocks keep interrupts disabled for as long as they are held
     * (klib.h). Ten milliseconds per device with no interrupt served on the
     * core, inside the interrupt handler that was serving one.
     *
     * The device is owed the time, not the core. So the wait became a deadline
     * the slot carries, and the pump — which already runs from both the tick
     * and the settle loop — starts Enable Slot when the clock says it may.
     */
    ENUM_STATE_WAIT_RESET_RECOVERY,

    ENUM_STATE_WAIT_ENABLE_SLOT,

    /*
     * The first of the two Address Device commands — the one that says
     * nothing on the bus.
     *
     * xHCI 1.2 Section 4.3.4 describes addressing as two steps with a
     * descriptor read between them, and this driver used to do only the
     * second. That made SET_ADDRESS the very first thing ever said to a
     * device, and a device not ready to hear it failed a COMMAND — which the
     * Command Ring cannot skip past, so every device queued behind it stopped
     * too and the only way out was to take the ring away from the controller.
     * Measured on a live board as `Address Device on slot 3 took 10005 ms`
     * followed by a reset of a controller with twenty-four ports on it.
     */
    ENUM_STATE_WAIT_ADDRESS_DEVICE_BSR,

    ENUM_STATE_WAIT_GET_DESC_HEADER,

    /* The second: the controller sends the device its address, using the
     * packet size the device named in the eight bytes above. */
    ENUM_STATE_WAIT_ADDRESS_DEVICE,

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
    /* Which controller handed out this slot.
     *
     * Slot numbers belong to a controller, not to the machine: two controllers
     * each start handing them out at one. Without this, a completion for slot 1
     * on the chipset controller and a device on slot 1 of the one on a graphics
     * card are the same entry in this table. */
    xhci_controller_t* ctrl;

    /*
     * Which tenancy of this table entry this is.
     *
     * The entry outlives the devices that pass through it, so "the slot that
     * asked" is only an answer while it is still the same device asking. Bumped
     * every time the entry is claimed; a command carries the value it was
     * posted under, and an answer that does not match is an answer to somebody
     * who has already gone.
     */
    uint32_t epoch;

    uint8_t slot_id;
    uint8_t port_num;
    uint8_t state;          /* xhci_enum_state_t */
    uint8_t speed;

    uint64_t timestamp_started;

    /*
     * What the watchdog last saw this slot doing, and when it started seeing
     * it. Written by nothing else — enumeration advances through twenty-nine
     * places and none of them has to remember to stamp a clock.
     *
     * ‼ THE BUDGET IS ON A STEP, WHICH IS WHAT IT ALWAYS SAID IT WAS.
     *
     * It used to be measured from `timestamp_started`, which is stamped when
     * the device's turn begins and never again — so it was a budget for the
     * WHOLE of enumeration wearing the words "any single step". A device whose
     * Address Device took ten seconds (measured, on the owner's board) arrived
     * at its next step with the budget already spent, and was thrown away
     * without being given any time at all: not for being stuck, for being
     * slow. Watching the state itself makes the sentence true — the clock runs
     * from the last thing that HAPPENED, and anything that happens resets it.
     */
    uint8_t  watch_state;
    uint64_t watch_since;

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

    /* Where to carry on from once a halted EP0 has been cleared. A device is
     * allowed to refuse an optional request, and refusing it halts the pipe;
     * so does babbling at one, and so does a transaction the controller could
     * not complete. This is the step that was being attempted when that
     * happened. */
    uint8_t  step_resume;

    /* How many times the control pipe has been cleared and the same step tried
     * again. A device is entitled to refuse once; it is not entitled to refuse
     * for ever, and a slot retried without a bound is a port held hostage. */
    uint8_t  step_retry;

    /* Whether resuming means asking the step again or stepping over it. A
     * device that declined an optional request is answered by moving on; one
     * whose answer never arrived is answered by asking again, and the two go
     * through the same clearing of the pipe. */
    bool     step_reissue;

    /*
     * The control transfer in flight, and how much of it actually arrived.
     *
     * A control transfer is three transfer descriptors and can raise TWO
     * events: one from a data stage that came up short, and one from the
     * status stage that ends it. An answer therefore has to say which of the
     * two it is, and the TRB it names is what says so.
     *
     * received starts equal to requested because that is what "no short packet
     * was reported" means. Without this pair the state machine had no way at
     * all to tell a descriptor that arrived from one that did not, and parsed
     * whatever the previous read had left in the scratch page.
     */
    uint64_t ctl_data_trb;
    uint64_t ctl_status_trb;
    uint16_t ctl_requested;
    uint16_t ctl_received;

    /* What was done to this device's port before it was spoken to, and how
     * long the reset took. A device answers the default address only in the
     * Default state, and it enters the Default state only through a reset —
     * so on a machine that can only be read by photographing its screen, this
     * is the first thing a silent Address Device has to be checked against. */
    uint8_t  reset_kind;            /* XHCI_PORT_RESET_* */
    uint32_t reset_took_ms;

    /* When the recovery the bus owes this device is up, and it may be
     * addressed. Only meaningful in ENUM_STATE_WAIT_RESET_RECOVERY. */
    uint64_t recovery_due;

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

    /*
     * The root port this device was found on, kept across the teardown, and
     * whether it ever got as far as being configured.
     *
     * A first attempt at a device fails for reasons that a second attempt on a
     * freshly reset port does not repeat — a descriptor read that babbled, a
     * request the device refused while it was still settling, an Address
     * Device the controller never answered. Every USB host retries; this one
     * released the port and moved on, so a flash drive that needed a second
     * go was a machine that booted without a filesystem.
     */
    uint8_t  born_port;
    bool     ever_configured;

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
xhci_device_slot_t* xhci_get_device_slot_by_id(xhci_controller_t* ctrl, uint8_t slot_id);
/*
 * A command asked on behalf of `slot` has been answered.
 *
 * The slot is named, not searched for. It used to be looked up from the slot
 * id in the completion event — which works for every command except the one
 * that matters most: Enable Slot goes out with no slot id at all, so its
 * answer was matched to "the first device in the table that appears to be
 * waiting for one". With one device coming up that is correct by luck; with
 * four, as on a desktop with a keyboard, a mouse and two sticks in it, it is
 * four guesses in a row and nothing in the log when one of them is wrong.
 *
 * `slot_id` is still passed because for Enable Slot it is the answer itself —
 * the number the controller has just handed out.
 */
void xhci_enum_advance_state(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                             uint8_t slot_id, uint8_t completion_code);

/* Which tenancy of a slot entry this is, and whether it is still that one.
 * A command records the first when it is posted and the answer checks the
 * second, so an answer can never be delivered to the device that replaced the
 * one which asked. NULL is a valid argument to both. */
uint32_t xhci_slot_epoch(const xhci_device_slot_t* slot);
bool     xhci_slot_still_is(const xhci_device_slot_t* slot, uint32_t epoch);

/* Is there anybody in this place at all — as opposed to the stronger question
 * xhci_slot_still_is asks, which is whether it is the SAME anybody. Idle means
 * nothing is there; retiring means something was and is being taken down, and
 * speaking to either is speaking to a device context that is about to stop
 * existing. */
bool     xhci_slot_is_live(const xhci_device_slot_t* slot);

/* The port finished the reset enumeration asked for. Called from the
 * port-status change path, which is the only thing that knows when. */
void xhci_enum_port_reset_done(xhci_controller_t* ctrl, uint8_t port);

/*
 * How long any single step of enumeration may go unanswered — and it is a step
 * now, watched by xhci_enum_watchdog through `watch_state`, rather than the
 * whole of a device's arrival wearing those words.
 *
 * Every step is one round trip on the bus. The specification puts numbers on
 * the ones that are control transfers: a standard request with no data stage
 * completes in 50 ms and one with data keeps its stages 500 ms apart (USB 2.0
 * §9.2.6.4). Two seconds is four times the slowest of those, and it is reached
 * only after the facts below have been asked and have said nothing is wrong.
 *
 * ‼ IT IS THE LAST RESORT, NOT THE TEST. A device that has been unplugged is
 * ended by its port saying so, and a step with a command outstanding belongs
 * to the command watchdog, which asks whether the CONTROLLER has answered
 * anything rather than how long this one request has lived.
 */
#define XHCI_ENUM_TIMEOUT_MS 2000

/* Report and release any enumeration that stopped being answered. Driven from
 * the timer tick. */
void xhci_enum_watchdog(xhci_controller_t* ctrl);

/*
 * Hand the bus to the next device waiting its turn.
 *
 * Enumeration is serialised per controller, and this is what moves the queue
 * along. Written so it cannot leak the turn: it asks whether the device
 * holding it is still being enumerated and takes it back if it is not, rather
 * than trusting every path that finishes with a device to say so. Cheap when
 * there is nothing to do — one lock and two loads.
 */
void xhci_enum_pump(xhci_controller_t* ctrl);

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
void xhci_enum_for_each_configured(xhci_controller_t* ctrl,
                                   xhci_slot_visitor visit, void* ctx);

/* True when a STALL at this point in enumeration is the device declining an
 * optional request rather than the conversation failing. */
bool xhci_enum_stall_is_tolerable(uint8_t state);

/*
 * True when a failed control transfer is worth asking again on a clean pipe.
 *
 * Three completion codes mean "this exchange did not happen", and all three
 * leave the control pipe halted (xHCI 1.2 Section 4.10.2.1): the device
 * refused, the device sent more than the endpoint was told to expect, or the
 * controller could not complete the transaction after its own CErr retries.
 * None of them is a statement about the device being unusable — they are what
 * a bus that was disturbed looks like from the host — and every USB host asks
 * again. This driver used to ask again only after a refusal, and threw the
 * device away for the other two.
 */
bool xhci_enum_fault_is_retryable(uint8_t completion_code);

/*
 * True for a step that is a control transfer and can therefore simply be made
 * again on a cleared pipe.
 *
 * ‼ What this replaced was a table of "the state that issued this step", which
 * the recovery then resumed at. That is a different thing: every case of this
 * state machine both checks the previous answer and issues the next request,
 * so resuming at one re-checks a scratch page that now holds a different
 * descriptor — and two of that table's five entries could therefore only ever
 * end in the slot being released. Asking again is the request alone.
 */
bool xhci_enum_step_can_be_asked_again(uint8_t state);

/* What a slot is waiting for, in words rather than a number. */
const char* xhci_enum_state_name(uint8_t state);

/* How many times a step may be retried before the device is let go. */
#define XHCI_STEP_RETRIES 3

/* How many times a root port is tried again after a device on it failed to
 * come up. The first attempt is not counted, so this is three retries after
 * the original try. */
#define XHCI_ENUM_ATTEMPTS 3

/*
 * Clear a halted control pipe and carry on at `resume_at`.
 *
 * ask_again false: the state machine runs that state as if its answer had
 * arrived, which steps over the request that failed — the right answer to a
 * device declining something optional.
 * ask_again true: the request that state is waiting for is simply made again.
 */
void xhci_enum_recover_ep0(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                           uint8_t resume_at, bool ask_again);
xhci_device_slot_t* xhci_get_device_slot(xhci_controller_t* ctrl, uint8_t slot_id);
xhci_device_slot_t* xhci_get_device_slot_by_port(xhci_controller_t* ctrl, uint8_t port);

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
