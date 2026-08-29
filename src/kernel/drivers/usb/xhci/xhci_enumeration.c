#include "xhci_enumeration.h"
#include "xhci_command.h"
#include "xhci_device.h"
#include "xhci_port.h"
#include "xhci_transfer.h"
#include "xhci_hid.h"
#include "xhci_endpoint.h"
#include "xhci_msd.h"
#include "xhci_hub.h"
#include "xhci_interrupt.h"
#include "usb_common.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "boardroom.h"

/*
 * ‼ THERE IS NO TABLE HERE ANY MORE, and that is the point.
 *
 * The records live on the controller that handed out the slots, sized by what
 * that controller says it can address. See xhci_slots_attach and the note on
 * xhci_controller_t::slots.
 */
void xhci_enumeration_init(void) {
    /*
     * Deliberately nothing.
     *
     * This used to memset a machine-wide array — which is why offering it to
     * userspace as `hw.usb.init` was withdrawn: calling it a second time wiped
     * every live device on the machine, including the one the volume was
     * mounted from, while drivers still held pointers into it. There is no
     * such array left to wipe.
     */
}

int xhci_slots_attach(xhci_controller_t* ctrl)
{
    if (!ctrl || ctrl->max_slots == 0) {
        return -1;
    }
    if (ctrl->slots) {
        return 0;                       /* already standing */
    }

    uint32_t count = ctrl->max_slots;
    ctrl->slots = (struct xhci_device_slot*)
                  kmalloc(sizeof(struct xhci_device_slot) * count);
    if (!ctrl->slots) {
        return -1;
    }
    ctrl->by_id = (struct xhci_device_slot**)
                  kmalloc(sizeof(struct xhci_device_slot*) * (count + 1u));
    if (!ctrl->by_id) {
        xhci_slots_release(ctrl);
        return -1;
    }

    memset(ctrl->slots, 0, sizeof(struct xhci_device_slot) * count);
    memset(ctrl->by_id, 0, sizeof(struct xhci_device_slot*) * (count + 1u));
    spinlock_init(&ctrl->slots_lock);
    ctrl->slot_count = (uint8_t)count;
    return 0;
}

/*
 * ‼ A CONTROLLER THAT DID NOT COME UP TOOK ITS RECORDS WITH IT.
 *
 * The records are the FIRST thing bring-up asks for and were the one thing its
 * failure path never gave back: it released the scratchpad, the mapping, the
 * device context array, both rings and the event ring table, and left two
 * allocations of its own standing. They were not even leaked in a way anything
 * could find afterwards — xhci_init leaves the controller count where it was
 * when bring-up fails, so the next controller found is written into the same
 * entry with a memset over the whole of it, pointers included. Thirteen
 * kilobytes at sixty-four slots, per controller that refuses.
 *
 * Safe on a controller that never got them, and safe twice: the count goes
 * with the memory, so nothing walks a table that is no longer there.
 */
void xhci_slots_release(xhci_controller_t* ctrl)
{
    if (!ctrl) {
        return;
    }
    if (ctrl->by_id) {
        kfree(ctrl->by_id);
        ctrl->by_id = NULL;
    }
    if (ctrl->slots) {
        kfree(ctrl->slots);
        ctrl->slots = NULL;
    }
    ctrl->slot_count = 0;
}

static void slot_take_down(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

/* Raised when something has been retired and not yet taken down; lowered by
 * the pass that takes it down. One core does that at a time, and the others
 * go away rather than queue up behind it — the same arrangement the hubs use,
 * and for the same reason. */
/* The retirement flags live on the controller now — see xhci_controller_t. */

/*
 * A slot somebody may still speak to.
 *
 * Idle means nothing is there. Retiring means something was, and what is left
 * of it is being taken down — a lookup that returns one of those hands out a
 * device context that is about to stop existing, and a port number that
 * belongs to whatever is plugged in next.
 */
static bool slot_is_live(const struct xhci_device_slot* s) {
    uint8_t state = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    return state != ENUM_STATE_IDLE && state != ENUM_STATE_RETIRING;
}

uint32_t xhci_slot_epoch(const xhci_device_slot_t* slot)
{
    if (!slot) {
        return 0;
    }
    return __atomic_load_n(&slot->epoch, __ATOMIC_ACQUIRE);
}

bool xhci_slot_is_live(const xhci_device_slot_t* slot)
{
    return slot ? slot_is_live(slot) : false;
}

bool xhci_slot_still_is(const xhci_device_slot_t* slot, uint32_t epoch)
{
    if (!slot) {
        return false;
    }
    return __atomic_load_n(&slot->epoch, __ATOMIC_ACQUIRE) == epoch &&
           slot_is_live(slot);
}

static struct xhci_device_slot* find_free_slot(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->slots) {
        return NULL;
    }
    spin_lock(&ctrl->slots_lock);
    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        if (ctrl->slots[i].state == ENUM_STATE_IDLE) {
            /* A new tenancy, and it says so before anything can be posted on
             * its behalf. Answers to the previous occupant's questions carry
             * the old number and are turned away. */
            __atomic_add_fetch(&ctrl->slots[i].epoch, 1, __ATOMIC_ACQ_REL);
            ctrl->slots[i].state = ENUM_STATE_CLAIMING;
            ctrl->slots[i].ctrl  = ctrl;
            spin_unlock(&ctrl->slots_lock);
            return &ctrl->slots[i];
        }
    }
    spin_unlock(&ctrl->slots_lock);
    return NULL;
}

/*
 * Everything about the previous tenant, gone.
 *
 * Two things survive, and they are about the ENTRY rather than the device in
 * it: which tenancy this is, so that answers to the last device's questions
 * are still turned away, and the claim itself.
 *
 * What this replaced was two lists of individual assignments, one per claim
 * path, thirty lines each — and they had already drifted apart: the hub path
 * did not clear the retry count or the resume state, so a device coming up
 * behind a hub inherited them from whatever had used the entry before. A list
 * of fields to remember to zero is a list somebody will add to only once.
 */
static void slot_wipe(struct xhci_device_slot* slot)
{
    uint32_t epoch = slot->epoch;
    uint8_t  state = slot->state;
    /* A record belongs to the controller whose table it lives in, for the
     * whole life of the machine. Wiping that would leave an entry nothing
     * could be posted on behalf of. */
    xhci_controller_t* owner = slot->ctrl;

    /*
     * ‼ AND THE INDEX GOES WITH THE NUMBER.
     *
     * by_id maps a controller's slot id to the record holding it. A record
     * that stops holding one must leave the index, and this is the single
     * place every record is reset — clearing it only where a slot is taken
     * down leaves a stale pointer, and a stale pointer here is not harmless:
     * the record is reused, gets a DIFFERENT id, and becomes live again while
     * the old id still points at it. An event naming the old id would then be
     * delivered to a device that is not the one it is about.
     */
    if (owner && owner->by_id && slot->slot_id != 0 &&
        slot->slot_id <= owner->slot_count) {
        spin_lock(&owner->slots_lock);
        if (owner->by_id[slot->slot_id] == slot) {
            owner->by_id[slot->slot_id] = NULL;
        }
        spin_unlock(&owner->slots_lock);
    }

    memset(slot, 0, sizeof(*slot));
    slot->ctrl = owner;

    slot->epoch = epoch;
    slot->state = state;
}

static struct xhci_device_slot* find_slot_by_port(xhci_controller_t* ctrl,
                                                 uint8_t port) {
    if (!ctrl || !ctrl->slots) {
        return NULL;
    }
    spin_lock(&ctrl->slots_lock);
    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        if (ctrl->slots[i].port_num == port && slot_is_live(&ctrl->slots[i])) {
            spin_unlock(&ctrl->slots_lock);
            return &ctrl->slots[i];
        }
    }
    spin_unlock(&ctrl->slots_lock);
    return NULL;
}

/*
 * The device an event names, without looking for it.
 *
 * Called from the event drain for every transfer and every command answer,
 * which is why it is worth being an index rather than a walk: this ran a
 * sixty-four-entry search under a machine-wide lock, with interrupts off, per
 * event.
 */
xhci_device_slot_t* xhci_get_device_slot(xhci_controller_t* ctrl, uint8_t slot_id) {
    if (!ctrl || !ctrl->by_id || slot_id == 0 || slot_id > ctrl->slot_count) {
        return NULL;
    }

    spin_lock(&ctrl->slots_lock);
    struct xhci_device_slot* slot = ctrl->by_id[slot_id];
    if (slot && !slot_is_live(slot)) {
        slot = NULL;
    }
    spin_unlock(&ctrl->slots_lock);
    return slot;
}

xhci_device_slot_t* xhci_get_device_slot_by_port(xhci_controller_t* ctrl,
                                                 uint8_t port) {
    return find_slot_by_port(ctrl, port);
}

/*
 * EP0's max packet size is the one endpoint parameter a device cannot be asked
 * about until it has already been talked to. At every speed but full it is
 * fixed by the specification and can simply be written down:
 *
 *   low         8 bytes
 *   high       64 bytes
 *   super     512 bytes  (the field is an exponent there: 2^9)
 *
 * Full speed is the exception — 8, 16, 32 or 64, the device's choice — so it
 * gets 8, which every full-speed device must accept, and the real value is
 * read out of the first eight bytes of its descriptor and installed with an
 * Evaluate Context. Getting this wrong is not a slow device; it is a device
 * that answers with packets larger than the controller was told to expect,
 * and the controller calls that babble and gives up.
 */
static uint16_t ep0_initial_max_packet(uint8_t speed) {
    switch (speed) {
        case XHCI_PORT_SPEED_LOW:      return 8;
        case XHCI_PORT_SPEED_HIGH:     return 64;
        case XHCI_PORT_SPEED_SUPER:
        case XHCI_PORT_SPEED_SUPER_10: return 512;
        case XHCI_PORT_SPEED_FULL:
        default:                       return 8;
    }
}

static const char* speed_name(uint8_t speed) {
    switch (speed) {
        case XHCI_PORT_SPEED_FULL:     return "full";
        case XHCI_PORT_SPEED_LOW:      return "low";
        case XHCI_PORT_SPEED_HIGH:     return "high";
        case XHCI_PORT_SPEED_SUPER:    return "super";
        case XHCI_PORT_SPEED_SUPER_10: return "super+";
        default:                       return "?";
    }
}

/* Post an Enable Slot and start waiting for it. Reached either straight from
 * xhci_enumerate_device, when the port needed no reset, or from the
 * port-status change that says the reset is over. */
static int enum_begin_slot(xhci_controller_t* ctrl, struct xhci_device_slot* slot) {
    slot->state = ENUM_STATE_WAIT_ENABLE_SLOT;

    /* Which kind of slot, as the Supported Protocol capability covering this
     * root port states it (xHCI 1.2 Section 6.4.3.2). A device behind a hub
     * takes the kind of the root port its branch hangs off, which is what
     * port_num holds for every tier. */
    uint8_t slot_type = ctrl->port_slot_type[slot->port_num];

    if (xhci_post_enable_slot_cmd(ctrl, slot, slot_type) < 0) {
        debug_printf("[xHCI ENUM] Failed to post Enable Slot command\n");
        slot->state = ENUM_STATE_IDLE;
        slot->port_num = 0;
        return -1;
    }
    return 0;
}

/*
 * Begin the conversation with a device whose turn has come.
 *
 * A root-port device starts with its port reset; a device found on a hub has
 * already been reset by that hub and joins at Enable Slot. The clock the
 * watchdog runs on starts here rather than when the device was found, so a
 * device that waited its turn is not given up on for the waiting.
 */
static int enum_start(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    slot->timestamp_started = rdtsc();

    if (slot->depth != 0) {
        return enum_begin_slot(ctrl, slot) == 0 ? 0 : -4;
    }

    /* Say what is being waited for BEFORE doing the thing that ends the wait.
     *
     * Asserting the reset is a store to a device register, and the device is
     * entitled to finish the reset and raise its interrupt inside that store.
     * It does: under emulation the port reset completes synchronously, so the
     * port-status change handler ran on this very core with the state still
     * reading "claiming", found nothing waiting on a reset, and returned. The
     * reset then completed for a slot that spent the rest of the boot waiting
     * for an event that had already been and gone. */
    __atomic_store_n(&slot->state, (uint8_t)ENUM_STATE_WAIT_PORT_RESET,
                     __ATOMIC_RELEASE);

    uint8_t kind = XHCI_PORT_RESET_NONE;
    slot->reset_kind = XHCI_PORT_RESET_NONE;

    /* Every root port is reset, at every speed, and there is no longer a path
     * that skips it — see xhci_port_begin_reset for why a SuperSpeed link that
     * is already up says nothing about whether the device on it will answer
     * the default address. So the reset event always follows, and this never
     * has to carry the conversation forward itself. */
    int reset = xhci_port_begin_reset(ctrl, slot->port_num, &kind);
    slot->reset_kind = kind;
    if (reset < 0) {
        debug_printf("[xHCI ENUM] Port %u could not be reset (%d)\n",
                     slot->port_num, reset);
        xhci_slot_retire(ctrl, slot);
        return -6;
    }
    return 0;
}

/*
 * One at a time.
 *
 * Between its port reset and the address that ends it, a device answers to
 * address zero. Two devices doing that at once is a bus on which the host
 * cannot tell which one replied, and the specification says so plainly
 * (USB 2.0 §9.1.1.3). The xHCI command ring then enforces it the hard way: it
 * executes commands strictly in order, so one Address Device that will not
 * complete blocks every command queued behind it — including the ones that
 * would have brought up the devices that were fine.
 *
 * Measured on two different machines, five devices at boot: five resets
 * together, five Address Devices back to back, three executed, and the
 * controller sat on the fourth for twenty seconds with everything else stuck
 * behind it. Which devices won was a coin toss, which is what "it boots about
 * half the time" is made of. Under emulation the whole exchange completes
 * inside the register write that starts it, so five at once and one at a time
 * are the same thing and always were.
 */
static int enum_admit(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    /*
     * Everybody joins the queue, and ONE place decides who goes next.
     *
     * This used to take the bus for itself whenever it happened to be free,
     * which made two places that hand out the same turn. The one that reaches
     * here that way is slot_take_down, retrying a port that has just failed —
     * and it runs at the very moment that failure released the bus, so it
     * could go round again ahead of a device that has never had a first
     * attempt at all.
     *
     * ‼ AND THE WINDOW IS IN THIS FUNCTION, not in the caller. The old shape
     * released the lock and only THEN marked the slot as queued:
     *
     *     spin_unlock(&ctrl->slots_lock);
     *     if (!mine) { ...store QUEUED... }
     *
     * Between those two lines the slot is waiting for the bus and is INVISIBLE
     * to anybody looking for somebody waiting for the bus. A pump that runs in
     * that gap finds an empty queue, drops enum_active to NULL, and the next
     * caller — the retry of the port that has just failed — takes the turn
     * that belonged to the device standing right there. Microseconds wide, and
     * on a board with six cores the pump runs on a different one.
     *
     * Setting the state FIRST and then asking closes it by construction: there
     * is no moment at which this slot wants the bus and cannot be seen to.
     *
     * The second half is the rule underneath. There wasn't one: the queue was
     * walked in slot-table order and the first QUEUED entry won, which is
     * wherever find_free_slot happened to put it.
     */
    __atomic_store_n(&slot->state, (uint8_t)ENUM_STATE_QUEUED, __ATOMIC_RELEASE);
    xhci_enum_pump(ctrl);
    return 0;
}

/*
 * Hand the bus to whoever is next.
 *
 * Written so that it cannot leak the turn: rather than trusting every path
 * that finishes with a device to say so, this asks whether the device holding
 * the turn is still being enumerated, and takes it back if it is not. Called
 * from the settle loop and from the tick, so a turn dropped by any route is
 * picked up on the next pass rather than stopping the bus for good.
 */
void xhci_enum_pump(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->initialized) {
        return;
    }

    struct xhci_device_slot* next = NULL;
    struct xhci_device_slot* recovered = NULL;

    spin_lock(&ctrl->slots_lock);

    struct xhci_device_slot* active = ctrl->enum_active;

    /* A device whose port reset is over and whose recovery time is up. Claimed
     * under the lock so that two cores arriving together cannot both start
     * Enable Slot for it. */
    if (active &&
        __atomic_load_n(&active->state, __ATOMIC_ACQUIRE) ==
            ENUM_STATE_WAIT_RESET_RECOVERY &&
        (int64_t)(rdtsc() - active->recovery_due) >= 0) {
        __atomic_store_n(&active->state, (uint8_t)ENUM_STATE_CLAIMING,
                         __ATOMIC_RELEASE);
        recovered = active;
    }

    if (recovered) {
        spin_unlock(&ctrl->slots_lock);
        enum_begin_slot(ctrl, recovered);
        return;
    }

    if (active) {
        uint8_t state = __atomic_load_n(&active->state, __ATOMIC_ACQUIRE);
        bool still_going = (state != ENUM_STATE_IDLE &&
                            state != ENUM_STATE_QUEUED &&
                            state != ENUM_STATE_CONFIGURED &&
                            state != ENUM_STATE_RETIRING);
        if (still_going) {
            spin_unlock(&ctrl->slots_lock);
            return;                     /* its turn is not over */
        }
        ctrl->enum_active = NULL;
    }

    /*
     * A first attempt outranks a retry.
     *
     * Enumeration is one at a time on a controller, so the order of this queue
     * is the order devices come up in, and a port that has already failed once
     * is the least likely of them to succeed. Letting it go first means every
     * device behind it waits out its whole budget again, three more times.
     * What this replaces is not a worse rule but no rule: the queue was walked
     * in slot-table order and the first QUEUED entry won, which is wherever
     * find_free_slot happened to put it.
     *
     * born_port is zero for a device found on a hub, and enum_attempts[0] is
     * never incremented, so those count as first attempts. That is right: the
     * retry counter is about root ports, and a hub rescans its own.
     */
    bool next_is_retry = true;
    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        if (__atomic_load_n(&ctrl->slots[i].state, __ATOMIC_ACQUIRE) !=
                ENUM_STATE_QUEUED) {
            continue;
        }
        uint8_t born = ctrl->slots[i].born_port;
        bool retry = (born != 0 && born <= ctrl->max_ports &&
                      ctrl->enum_attempts[born] > 0);

        if (!next || (next_is_retry && !retry)) {
            next          = &ctrl->slots[i];
            next_is_retry = retry;
            if (!retry) {
                break;      /* nothing outranks a first attempt */
            }
        }
    }
    if (next) {
        ctrl->enum_active = next;
    }

    spin_unlock(&ctrl->slots_lock);

    if (next) {
        enum_start(ctrl, next);
    }
}

int xhci_enumerate_device(xhci_controller_t* ctrl, uint8_t port) {
    if (!ctrl || !ctrl->running) {
        debug_printf("[xHCI ENUM] Controller not ready\n");
        return -1;
    }

    if (port == 0 || port > ctrl->max_ports) {
        debug_printf("[xHCI ENUM] Invalid port: %u\n", port);
        return -2;
    }

    if (!xhci_port_has_device(ctrl, port)) {
        return -3;
    }

    if (find_slot_by_port(ctrl, port)) {
        return -4;
    }

    if (ctrl->enum_attempts[port] >= XHCI_ENUM_ATTEMPTS) {
        /* Said once, and then the port is left alone until something is
         * unplugged from it — a port retried for ever is a port that spends
         * the machine's boot on a device which is not going to answer. */
        if (ctrl->enum_attempts[port] == XHCI_ENUM_ATTEMPTS) {
            ctrl->enum_attempts[port]++;
            kprintf("[xHCI %s] port %u: %u attempts and the device on it never "
                    "came up — leaving the port alone\n",
                    ctrl->name, port, XHCI_ENUM_ATTEMPTS);
        }
        return -8;
    }

    struct xhci_device_slot* slot = find_free_slot(ctrl);
    if (!slot) {
        kprintf("[xHCI] no free device slot for port %u\n", port);
        return -5;
    }

    slot_wipe(slot);
    slot->ctrl = ctrl;
    slot->port_num = port;
    slot->born_port = port;
    /* Stamped from the moment it is claimed, so a slot that never gets as far
     * as starting is still something the watchdog can reap. enum_start stamps
     * it again when the device's turn actually comes, so a device is never
     * given up on for the time it spent waiting. */
    slot->timestamp_started = rdtsc();

    debug_printf("[xHCI ENUM] Starting enumeration for port %u\n", port);

    return enum_admit(ctrl, slot);
}

xhci_device_slot_t* xhci_get_device_slot_by_id(xhci_controller_t* ctrl,
                                               uint8_t slot_id)
{
    return xhci_get_device_slot(ctrl, slot_id);
}

bool xhci_slot_enter(xhci_device_slot_t* slot)
{
    if (!slot) {
        return false;
    }

    /*
     * Counted first, then checked.
     *
     * The other order leaves a window: a retirement that lands between the
     * check and the count sees nobody inside and takes the endpoints away
     * underneath a caller that has just satisfied itself they are there.
     * Counting first closes it — either the retirement sees this visitor, or
     * this visitor sees the retirement. One of the two always happens.
     */
    __atomic_fetch_add(&slot->visitors, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != ENUM_STATE_CONFIGURED) {
        __atomic_fetch_sub(&slot->visitors, 1, __ATOMIC_ACQ_REL);
        return false;
    }
    return true;
}

void xhci_slot_leave(xhci_device_slot_t* slot)
{
    if (!slot) {
        return;
    }
    __atomic_fetch_sub(&slot->visitors, 1, __ATOMIC_ACQ_REL);
}

void xhci_slot_retire(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    if (!slot) {
        return;
    }

    /* Whoever gets there first retires it; a second caller for the same
     * departure finds it already going and has nothing to add. */
    uint8_t was = __atomic_exchange_n(&slot->state,
                                      (uint8_t)ENUM_STATE_RETIRING,
                                      __ATOMIC_ACQ_REL);
    if (was == ENUM_STATE_IDLE || was == ENUM_STATE_RETIRING) {
        __atomic_store_n(&slot->state, was, __ATOMIC_RELEASE);
        return;
    }

    /* The socket is free for whatever is plugged in next, and that is a
     * different device: nothing may find this one by port any more. */
    slot->port_num = 0;
    slot->retire_started = rdtsc();
    slot->retire_warned = false;

    /*
     * Give the slot back to the controller.
     *
     * A slot is a resource the controller hands out on Enable Slot and takes
     * back on Disable Slot, and this used to be issued on exactly one of the
     * many paths that finish with a device — the root-port unplug. Every
     * other one, including every enumeration that failed and every device
     * unplugged from a hub, freed the driver's memory and left the controller
     * believing the slot was still in use. There are sixty-four of them.
     *
     * Posted, not waited for: this is reached from the event handler. The
     * memory the command names stays where it is until the command has been
     * answered, which is what the second step below is waiting for.
     */
    if (ctrl && slot->slot_id != 0) {
        xhci_post_disable_slot_cmd(ctrl, slot, slot->slot_id);
    }

    __atomic_store_n(&ctrl->retire_pending, 1, __ATOMIC_RELEASE);
}

int xhci_slot_service(xhci_controller_t* ctrl)
{
    if (!ctrl) {
        return 0;
    }
    if (__atomic_load_n(&ctrl->retire_pending, __ATOMIC_ACQUIRE) == 0) {
        return 0;
    }
    if (__atomic_exchange_n(&ctrl->retire_busy, 1, __ATOMIC_ACQUIRE) != 0) {
        return 0;                       /* somebody is already doing this */
    }

    __atomic_store_n(&ctrl->retire_pending, 0, __ATOMIC_RELEASE);

    int done = 0;
    bool more = false;

    /*
     * This controller's records, because they are the only ones it has.
     *
     * A whole special case used to live here: the table was shared, so a pass
     * by one controller met the other's devices, could not touch them — a
     * Disable Slot posted to silicon that never enabled the slot, an input
     * context freed at the wrong controller's context size — and could not
     * ignore them either, because the "something is retiring" flag was one
     * flag for the machine. Measured on a board with an Intel and an NVIDIA
     * controller before that was understood: fifteen chairs from ONE flash
     * drive and not a single "is gone" in the log. The records moved onto the
     * controller and the special case has nothing left to be about.
     */
    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        struct xhci_device_slot* slot = &ctrl->slots[i];
        if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != ENUM_STATE_RETIRING) {
            continue;
        }

        uint32_t inside = __atomic_load_n(&slot->visitors, __ATOMIC_ACQUIRE);
        bool controller_busy = xhci_command_pending_for(ctrl, slot);

        if (inside != 0 || controller_busy) {
            /*
             * Waiting, not forcing. A transfer that will never be answered
             * gives up after five seconds and its caller leaves, so this
             * resolves itself; and freeing pages a caller is still inside or
             * the controller is still reading is the very thing this whole
             * arrangement exists to prevent. A slot held too long is said out
             * loud once and then kept — a leaked slot is a smaller problem
             * than a heap somebody else is writing into.
             */
            if (!slot->retire_warned &&
                (int64_t)(rdtsc() - slot->retire_started) >
                (int64_t)cpu_ms_to_tsc(XHCI_RETIRE_PATIENCE_MS)) {
                slot->retire_warned = true;
                kprintf("[xHCI] slot %u has been leaving for %u ms — %u caller(s) "
                        "still inside it%s\n",
                        slot->slot_id, XHCI_RETIRE_PATIENCE_MS, inside,
                        controller_busy ? " and the controller has not answered"
                                        : "");
            }
            more = true;
            continue;
        }

        slot_take_down(ctrl, slot);
        done++;
    }

    if (more) {
        __atomic_store_n(&ctrl->retire_pending, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&ctrl->retire_busy, 0, __ATOMIC_RELEASE);
    return done;
}

bool xhci_slot_retire_pending(void)
{
    /* Any controller with work waiting. The idle loop asks before walking
     * them, so this stays a question about the machine even though the answer
     * is kept per controller. */
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_controller_t* c = xhci_controller_at(i);
        if (c && __atomic_load_n(&c->retire_pending, __ATOMIC_ACQUIRE) != 0) {
            return true;
        }
    }
    return false;
}

void xhci_slot_service_if_pending(void)
{
    if (!xhci_slot_retire_pending()) {
        return;
    }
    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_slot_service(xhci_controller_at(i));
    }
}

/*
 * A device found on a hub port.
 *
 * The hub has already powered the port, reset it and read back what speed
 * answered, so there is no port reset to wait for here — this joins the same
 * conversation a root-port device joins after its own reset, at Enable Slot.
 *
 * What is different is everything the controller needs in order to find the
 * device at all: the root port the branch hangs off, the route down through
 * the hubs, and, when the device is slower than the bus it is reached over,
 * which hub is translating for it.
 */
int xhci_enumerate_behind_hub(xhci_controller_t* ctrl,
                              xhci_device_slot_t* hub,
                              uint8_t hub_port, uint8_t speed)
{
    if (!ctrl || !ctrl->running || !hub || hub_port == 0) {
        return -1;
    }

    /* Four bits name the port at each tier, so fifteen is the highest port the
     * bus can be told about — which is why the specification stops hubs there
     * too, and why a hub claiming more of them has some that cannot be used. */
    if (hub_port > 15) {
        kprintf("[xHCI] hub slot %u has no port %u the bus could name — four "
                "bits per tier stop at 15\n", hub->slot_id, hub_port);
        return -5;
    }

    /* Five tiers is the whole of the route string, and the specification says
     * so: four bits each, twenty bits, and no sixth place to put a number. */
    if (hub->depth >= 5) {
        kprintf("[xHCI] a device on hub slot %u port %u is six hubs deep, "
                "which is one more than the bus can address\n",
                hub->slot_id, hub_port);
        return -2;
    }

    struct xhci_device_slot* slot = find_free_slot(ctrl);
    if (!slot) {
        kprintf("[xHCI] no free device slot for hub slot %u port %u\n",
                hub->slot_id, hub_port);
        return -3;
    }

    slot_wipe(slot);
    slot->ctrl = ctrl;
    slot->port_num = hub->port_num;          /* the root port, still */
    slot->speed = speed;
    /* The hub did the resetting, and it is the hub that knows what answered. */
    slot->reset_kind = XHCI_PORT_RESET_HUB;

    /* Four bits per tier, and the tier is how deep the HUB is — a device on a
     * hub that is itself on a root port occupies the first four bits.
     *
     * A port number above fifteen does not fit in those four bits, and the old
     * code clamped it to fifteen. That does not address port sixteen; it
     * addresses port fifteen, and hands whatever is plugged into that one the
     * transfers meant for its neighbour. A port the bus cannot name is a port
     * this driver says it cannot reach. */
    slot->route_string   = hub->route_string | ((uint32_t)hub_port << (4 * hub->depth));
    slot->depth          = (uint8_t)(hub->depth + 1);
    slot->parent_slot_id = hub->slot_id;
    slot->parent_port    = hub_port;

    /* Who translates. A low or full speed device reached through a high speed
     * hub is spoken to by that hub on the controller's behalf, and the
     * controller has to be told which one. A device deeper down inherits
     * whichever translator was already standing in the way. */
    if ((speed == XHCI_PORT_SPEED_LOW || speed == XHCI_PORT_SPEED_FULL) &&
        hub->speed == XHCI_PORT_SPEED_HIGH) {
        slot->tt_slot_id = hub->slot_id;
        slot->tt_port    = hub_port;
    } else {
        slot->tt_slot_id = hub->tt_slot_id;
        slot->tt_port    = hub->tt_port;
    }

    slot->timestamp_started = rdtsc();

    debug_printf("[xHCI ENUM] device on hub slot %u port %u, route 0x%05x, "
                 "depth %u\n", hub->slot_id, hub_port,
                 slot->route_string, slot->depth);

    return enum_admit(ctrl, slot);
}

void xhci_enum_port_reset_done(xhci_controller_t* ctrl, uint8_t port)
{
    if (!ctrl) {
        return;
    }

    struct xhci_device_slot* slot = find_slot_by_port(ctrl, port);
    if (!slot || slot->state != ENUM_STATE_WAIT_PORT_RESET) {
        return;                 /* a reset nobody was waiting on */
    }

    if (!xhci_port_reset_finished(ctrl, port)) {
        /*
         * The reset ended and the port is not enabled.
         *
         * On a SuperSpeed port that is a link which did not come back, and the
         * answer is the reset that works out of band — a hot reset is carried
         * over the very link that is not working. Escalating here rather than
         * giving up is the difference between a stick that needs its socket
         * wiggled and one that comes up on the second try. Once, and only from
         * a hot reset: a warm one that did not work either is a port with
         * nothing usable on it.
         */
        if (slot->reset_kind == XHCI_PORT_RESET_HOT &&
            xhci_port_protocol(ctrl, port) >= 3 &&
            xhci_port_warm_reset(ctrl, port) == 0) {
            slot->reset_kind = XHCI_PORT_RESET_WARM;
            slot->timestamp_started = rdtsc();
            return;                 /* still waiting on a reset, a louder one */
        }

        /* Nothing further is possible, and holding the slot for it would keep
         * the port unusable for whatever is plugged in next. */
        kprintf("[xHCI %s] port %u: the %s ended with the port still disabled "
                "(PORTSC 0x%08x) — no usable device\n",
                ctrl->name, port, xhci_port_reset_kind_name(slot->reset_kind),
                xhci_get_port_status(ctrl, port));
        xhci_slot_retire(ctrl, slot);
        return;
    }

    /* How long the reset itself took, measured before the recovery is owed so
     * that the number is the hardware's and not this driver's. */
    slot->reset_took_ms = (uint32_t)cpu_tsc_to_ms(rdtsc() - slot->timestamp_started);

    /*
     * What the port looks like at the one moment that decides everything after
     * it: the device is now supposed to be in the Default state, answering to
     * address zero, and the very next thing said to it is Enable Slot followed
     * by the Address Device that talks to that address. If this line and the
     * Address Device that never came back are both on the screen, the reset is
     * ruled in or out without another boot.
     */
    uint32_t portsc = xhci_get_port_status(ctrl, port);
    kprintf("[xHCI %s] port %u: %s finished in %u ms — PORTSC 0x%08x, link %u, "
            "speed %u\n",
            ctrl->name, port, xhci_port_reset_kind_name(slot->reset_kind),
            slot->reset_took_ms, portsc, XHCI_PORTSC_PLS(portsc),
            (unsigned)XHCI_PORTSC_SPEED(portsc));

    /*
     * The recovery the bus owes the DEVICE, which is not the same as time this
     * core has to spend.
     *
     * USB 2.0 §7.1.7.5: after a port reset ends, a device is given TRSTRCY —
     * ten milliseconds — before it may be addressed. It spends them coming up
     * in the Default state and is not obliged to answer anything until it has.
     * Addressing it early is not a slow device; it is a device that does not
     * respond to the first thing the host says to it.
     *
     * ‼ This used to be spent spinning, right here. And right here is inside
     * the event drain — the port-status change arrives on the event ring — so
     * it held the ring lock, and a held spinlock on this kernel keeps
     * interrupts off for as long as it is held (klib.h). Ten milliseconds per
     * device, on the core, inside the interrupt handler that was serving one.
     * Measured by putting it back: the longest drain went from 4471 us, all of
     * which is the emulator's cost of touching a device register, to 12444.
     *
     * So the device gets its ten milliseconds and the core gets none of them:
     * the deadline goes on the slot, and the pump — which already runs from
     * both the timer tick and the settle loop — starts Enable Slot when the
     * clock says it may.
     */
    slot->recovery_due = rdtsc() + cpu_ms_to_tsc(XHCI_PORT_RESET_RECOVERY_MS);
    __atomic_store_n(&slot->state, (uint8_t)ENUM_STATE_WAIT_RESET_RECOVERY,
                     __ATOMIC_RELEASE);
}

/* The second half of a departure: what is left of the device, taken apart.
 *
 * Reached only from xhci_slot_service, and only once the controller has said
 * it is finished with the slot and nobody is inside the device any more. It is
 * deliberately not something any other file can call — every path that once
 * did now retires the slot and lets the service pass get here in its own
 * time. */
static void slot_take_down(xhci_controller_t* ctrl, xhci_device_slot_t* slot) {
    if (!slot) {
        return;
    }

    if (slot->ep0_ring) {
        xhci_free_ep0_ring(slot);
    }

    if (slot->driver == XHCI_DRIVER_STORAGE) {
        xhci_msd_release(slot);
    }
    if (slot->driver == XHCI_DRIVER_HUB) {
        xhci_hub_release(ctrl, slot);
    }

    xhci_ep_table_free(slot);

    if (slot->input_ctx_phys && ctrl) {
        pmm_free((void*)slot->input_ctx_phys, xhci_input_ctx_pages(ctrl));
        slot->input_ctx_phys = 0;
    }

    /* Take the controller's pointer away BEFORE the page it points at goes back
     * to the allocator — the other order hands a live pointer to the next
     * tenant of that page. And only while it still points at THIS device: by
     * the time a departure is taken down, the controller may have given the
     * same slot number to whatever was plugged in next, and clearing it then
     * would erase the newcomer's device context instead. */
    if (ctrl && slot->slot_id > 0 && slot->slot_id <= ctrl->max_slots &&
        ctrl->dcbaa->device_context_ptrs[slot->slot_id] == slot->dev_ctx_phys) {
        ctrl->dcbaa->device_context_ptrs[slot->slot_id] = 0;
    }

    if (slot->dev_ctx_phys) {
        pmm_free((void*)slot->dev_ctx_phys, 1);
        slot->dev_ctx_phys = 0;
        slot->dev_ctx = NULL;
    }

    /*
     * And then try again, on a port freshly reset.
     *
     * A device that failed once is not a device that will fail again: the
     * first attempt met a descriptor read that babbled, or a request refused
     * while the device was still settling, or an Address Device the controller
     * never answered — and every one of those is something a second attempt on
     * a clean port does not repeat. Every USB host does this; this driver
     * released the port and moved on, which on a machine that boots from a
     * flash drive is a machine that boots without a filesystem.
     *
     * Only for a device that never got as far as being configured, only for
     * root ports (a device on a hub is retried by the hub's own scan), only
     * while something is still plugged in, and only three times — a port
     * retried for ever is a port that spends the whole boot on a device which
     * is not going to answer. The attempt is counted here rather than at the
     * start, so a device that came up costs nothing.
     */
    uint8_t retry_port = (!slot->ever_configured && ctrl) ? slot->born_port : 0;

    if (ctrl && ctrl->by_id && slot->slot_id != 0 &&
        slot->slot_id <= ctrl->slot_count) {
        spin_lock(&ctrl->slots_lock);
        if (ctrl->by_id[slot->slot_id] == slot) {
            ctrl->by_id[slot->slot_id] = NULL;
        }
        spin_unlock(&ctrl->slots_lock);
    }

    slot->slot_id = 0;
    slot->port_num = 0;
    slot->born_port = 0;
    slot->state = ENUM_STATE_IDLE;
    slot->timestamp_started = 0;
    slot->driver = XHCI_DRIVER_NONE;

    if (retry_port && retry_port <= ctrl->max_ports &&
        ctrl->enum_attempts[retry_port] < XHCI_ENUM_ATTEMPTS &&
        xhci_port_has_device(ctrl, retry_port)) {
        ctrl->enum_attempts[retry_port]++;
        kprintf("[xHCI %s] port %u: trying again on a freshly reset port "
                "(attempt %u of %u)\n", ctrl->name, retry_port,
                ctrl->enum_attempts[retry_port] + 1, XHCI_ENUM_ATTEMPTS + 1);
        xhci_enumerate_device(ctrl, retry_port);
    }
}

/*
 * Addressing a device, which the specification describes as TWO commands.
 *
 * xHCI 1.2 Section 4.3.4. The first carries Block Set Address Request: the
 * controller assigns the slot its internal resources and moves it to the
 * Default state, and puts NOTHING on the bus. The device goes on answering the
 * default address — which is where it can be asked, on its own control pipe,
 * how large that pipe's packets are. The second carries BSR clear, and it is
 * that one which makes the controller send the device a USB SET_ADDRESS.
 *
 * ‼ WHAT THE ONE-STEP VERSION COST, MEASURED ON THE OWNER'S BOARD
 *
 * SET_ADDRESS was the FIRST thing this driver ever said to a device, ten
 * milliseconds after its port reset ended. When a device was not ready to hear
 * it, what failed was a COMMAND — and a command that is never answered is not
 * a device this driver can give up on and move past. The Command Ring executes
 * strictly in order, so everything behind it stops too, and the only way out
 * is to take the ring away from the controller: five seconds of silence, a
 * doorbell, five more, a five-second abort, and then a reset of the whole
 * controller with twenty-four ports on it. `Address Device on slot 3 took
 * 10005 ms` is that ladder, printed.
 *
 * With the two steps, the same device failing at the same moment fails a
 * TRANSFER instead. A transfer that goes wrong raises a Transfer Event, names
 * itself, halts one endpoint, and is retried on a cleared pipe by machinery
 * this driver already has. Nothing else on the bus notices.
 *
 * That is the point of the change, and it holds whether or not the device's
 * unreadiness was ever the root cause: it moves the failure off the one queue
 * that has no way to skip an entry.
 *
 * `max_packet` is what EP0 is declared with. On the first step it is the
 * specification's value for the speed; on the second it is what the device
 * itself said in the first eight bytes of its descriptor — which is why this
 * driver no longer needs an Evaluate Context to correct it afterwards.
 */
static void enum_address_device(xhci_controller_t* ctrl, struct xhci_device_slot* slot,
                                uint16_t max_packet, bool block_set_address)
{
    uint32_t pages = xhci_input_ctx_pages(ctrl);
    void* input_ctx_phys = pmm_alloc_zero(pages, PHYS_TAG_DMA32);
    if (!input_ctx_phys) {
        kprintf("[xHCI] out of memory addressing the device on port %u\n",
                slot->port_num);
        xhci_slot_retire(ctrl, slot);
        return;
    }
    slot->input_ctx_phys = (uint64_t)input_ctx_phys;

    uint8_t* input_base = (uint8_t*)vmm_phys_to_virt((uintptr_t)input_ctx_phys);

    /* Input Control Context: add Slot (A0) and EP0 (A1). */
    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)input_base;
    icc->add_context_flags = (1 << 0) | (1 << 1);

    xhci_slot_context_t* slot_ctx =
        (xhci_slot_context_t*)(input_base + ctrl->context_size);
    xhci_fill_slot_context(slot_ctx, slot);

    /*
     * Where the controller should pick the control ring up.
     *
     * The first step names the ring's beginning, because nothing has been put
     * on it yet. The second names where software actually stands — the eight
     * descriptor bytes read between the two steps left three executed TRBs
     * behind, and handing back the base address would tell the controller to
     * run them again. The cycle bit rides in bit 0 for the same reason it does
     * everywhere else: the controller has to know which half of the ring's
     * life this position belongs to.
     */
    slot->ep0_max_packet = max_packet;
    uint64_t resume = slot->ep0_ring->trbs_phys +
                      (uint64_t)slot->ep0_ring->enqueue_idx * sizeof(xhci_trb_t);
    resume |= slot->ep0_ring->cycle_state ? 1u : 0u;

    xhci_endpoint_context_t* ep0_ctx =
        (xhci_endpoint_context_t*)(input_base + ctrl->context_size * 2);
    xhci_init_ep0_context(ep0_ctx, resume, max_packet);

    slot->state = block_set_address ? ENUM_STATE_WAIT_ADDRESS_DEVICE_BSR
                                    : ENUM_STATE_WAIT_ADDRESS_DEVICE;

    if (xhci_post_address_device_cmd(ctrl, slot, slot->slot_id,
                                     (uint64_t)input_ctx_phys,
                                     block_set_address) < 0) {
        debug_printf("[xHCI ENUM] Failed to post Address Device\n");
        xhci_slot_retire(ctrl, slot);
    }
}

/*
 * Ask for `length` bytes of a descriptor into the slot's scratch page.
 *
 * The page is emptied first. The scratch page is reused for every descriptor
 * this device is asked for, so a read that comes up short leaves the tail of
 * the PREVIOUS descriptor sitting exactly where the parser will look — and a
 * descriptor is self-describing, so the leftovers parse. Zeroing costs a page
 * memset per step and turns "the read was short" into a bLength of zero, which
 * every walk in this driver already stops on.
 */
static int enum_get_descriptor(xhci_controller_t* ctrl, struct xhci_device_slot* slot,
                               uint8_t type, uint16_t length)
{
    usb_setup_packet_t setup = {
        .bmRequestType = 0x80,
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((uint16_t)type << 8),
        .wIndex = 0,
        .wLength = length
    };

    if (slot->descriptor_buffer_virt) {
        memset(slot->descriptor_buffer_virt, 0, length);
    }

    return xhci_control_transfer(ctrl, slot, &setup,
                                 slot->descriptor_buffer_phys, length, true);
}

/*
 * Did the last descriptor read deliver what it was asked for?
 *
 * A device is allowed to answer a control read with less than was requested,
 * and the way it says so is a short packet. Every descriptor step below asks
 * for exactly the number of bytes the descriptor is supposed to be, so short
 * here always means the conversation went wrong — and until the data stage
 * carried Interrupt On Short Packet there was no way to find out, which meant
 * every one of these steps parsed the scratch page unconditionally.
 */
static bool enum_got_it_all(xhci_controller_t* ctrl, struct xhci_device_slot* slot,
                            uint16_t wanted)
{
    if (slot->ctl_received >= wanted) {
        return true;
    }

    kprintf("[xHCI %s] port %u: %s — asked for %u bytes and %u came back\n",
            ctrl->name, slot->port_num, xhci_enum_state_name(slot->state),
            wanted, slot->ctl_received);
    return false;
}

/*
 * A descriptor step that did not deliver, treated exactly like one the device
 * refused: clear the control pipe and ask again, up to the same bound. A short
 * answer and a stalled one are the same fault seen from two sides — the device
 * did not say what it was asked.
 */
static void enum_ask_again(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    if (!xhci_enum_step_can_be_asked_again(slot->state) ||
        slot->step_retry >= XHCI_STEP_RETRIES) {
        kprintf("[xHCI %s] port %u: %s never delivered — releasing the slot\n",
                ctrl->name, slot->port_num, xhci_enum_state_name(slot->state));
        xhci_slot_retire(ctrl, slot);
        return;
    }

    slot->step_retry++;
    kprintf("[xHCI %s] port %u: asking again (attempt %u of %u)\n",
            ctrl->name, slot->port_num, slot->step_retry, XHCI_STEP_RETRIES);

    /* A short answer does not halt the pipe, but clearing it costs two
     * commands and makes the second attempt start from the same place a
     * recovered one does — which is one path through this instead of two. */
    xhci_enum_recover_ep0(ctrl, slot, slot->state, true);
}

static void enum_free_input_ctx(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    if (slot->input_ctx_phys) {
        pmm_free((void*)slot->input_ctx_phys, xhci_input_ctx_pages(ctrl));
        slot->input_ctx_phys = 0;
    }
}

/* A device this kernel has no driver for is still a device. It is addressed,
 * configured and left alone, with its slot intact — which is both the honest
 * outcome and the ground a class driver stands on when one arrives. */
static void enum_settle_unclaimed(struct xhci_device_slot* slot,
                                  const void* cfg, uint16_t cfg_len)
{
    /*
     * ‼ AND IF IT HAD ISOCHRONOUS ENDPOINTS, SAY SO RATHER THAN DROPPING THEM
     * IN SILENCE.
     *
     * enum_pick_visit answers three kinds of endpoint and lets everything else
     * fall through its default, so a headset, a microphone or a camera is
     * addressed, configured and settled exactly like a device with nothing on
     * it at all. Nothing anywhere said that the part of it that matters had
     * been ignored — and "the machine saw my microphone and did nothing" is a
     * different fault from "the machine did not see my microphone", which is
     * the whole reason this line exists.
     *
     * It is a statement of what is missing, not a promise: this kernel
     * configures no isochronous endpoint, because it has nothing that would
     * read one. When it does, this line is where the work starts.
     */
    uint8_t isoch = usb_count_isoch_endpoints(cfg, cfg_len);
    if (isoch != 0) {
        kprintf("[xHCI] port %u: it has %u isochronous endpoint(s), and this "
                "kernel configures none — that part of it is unused\n",
                slot->port_num, isoch);
    }

    kprintf("[xHCI] port %u: %s-speed device %04x:%04x class %02x/%02x/%02x "
            "configured, no driver claims it\n",
            slot->port_num, speed_name(slot->speed),
            slot->device_desc.idVendor, slot->device_desc.idProduct,
            slot->interface_class, slot->interface_subclass,
            slot->interface_protocol);
    slot->ever_configured = true;
    if (slot->ctrl && slot->born_port) {
        slot->ctrl->enum_attempts[slot->born_port] = 0;
    }
    slot->state = ENUM_STATE_CONFIGURED;

    /* "No driver claims it" is a statement about right now, not about ever.
     * The announcement goes out all the same, carrying everything a driver
     * would need to recognise its own device — which is how a class driver
     * comes to own this one without anybody editing the state machine above
     * to know about it. */
    xhci_touch_device_arrived(slot);
}

/*
 * A device is allowed to say no.
 *
 * SET_IDLE and SET_PROTOCOL are class requests a HID device may simply not
 * implement, and the way USB says "I do not implement that" is to stall the
 * control pipe. Plenty of real keyboards stall SET_IDLE — it is unremarkable,
 * and it is not a reason to throw the keyboard away.
 *
 * But a stall halts the pipe, so nothing further can be asked of the device
 * until the halt is cleared: a Reset Endpoint to clear it, then a Set TR
 * Dequeue Pointer to step over the transfer that stalled, and only then does
 * enumeration carry on from where it left off. Treating the stall as a failure
 * instead — which is what this driver did — turns "this keyboard does not
 * implement an optional request" into "this keyboard does not work".
 */
bool xhci_enum_stall_is_tolerable(uint8_t state)
{
    return state == ENUM_STATE_WAIT_SET_PROTOCOL ||
           state == ENUM_STATE_WAIT_SET_IDLE;
}

/*
 * Which faults leave a usable device on the other end of a halted pipe.
 *
 * All three halt the control endpoint (xHCI 1.2 Section 4.10.2.1) and none of
 * them says the device is broken. STALL is the device declining. Babble is the
 * device sending more than the endpoint was told to expect — which on a bus
 * that was disturbed mid-answer is what the tail of somebody else's transfer
 * looks like. USB Transaction Error is the controller giving up after its own
 * CErr retries, which is what a marginal cable produces.
 *
 * Every USB host asks again after all three. This driver asked again only
 * after a refusal, and threw the device away for the other two — on the board,
 * a descriptor read that babbled once cost the device its slot.
 */
bool xhci_enum_fault_is_retryable(uint8_t completion_code)
{
    return completion_code == TRB_COMPLETION_STALL ||
           completion_code == TRB_COMPLETION_BABBLE ||
           completion_code == TRB_COMPLETION_USB_TRANS_ERR ||
           completion_code == TRB_COMPLETION_SPLIT_TRANS_ERR;
}

/*
 * Ask the current step again — the request only, not the state that led to it.
 *
 * ‼ This replaces a table that named, for each step, "the state that issued
 * it", and resumed the state machine there. That is not the same thing and two
 * of its five entries could not work: the cases of this state machine BOTH
 * check the previous answer AND issue the next request, so replaying one
 * re-checks a scratch page that now holds a different descriptor. Resuming a
 * failed configuration-header read at "reading the device descriptor" made
 * that case validate nine bytes of configuration as an eighteen-byte device
 * descriptor, call it malformed, and release the slot — which is a retry that
 * could only ever fail.
 *
 * Asking again is the second half alone, and this is the only place that knows
 * what each step actually asks for.
 */
static int enum_reissue(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    switch (slot->state) {

    case ENUM_STATE_WAIT_GET_DESC_HEADER:
        return enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 8);

    case ENUM_STATE_WAIT_GET_DESCRIPTOR:
        return enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 18);

    case ENUM_STATE_WAIT_GET_CONFIG_HEADER:
        return enum_get_descriptor(ctrl, slot, USB_DT_CONFIG, 9);

    case ENUM_STATE_WAIT_GET_CONFIG_DESC:
        return enum_get_descriptor(ctrl, slot, USB_DT_CONFIG,
                                   slot->config_total_len);

    case ENUM_STATE_WAIT_SET_CONFIGURATION: {
        usb_setup_packet_t setup = {
            .bmRequestType = 0x00,
            .bRequest = USB_REQ_SET_CONFIGURATION,
            .wValue = slot->config_value,
            .wIndex = 0,
            .wLength = 0
        };
        return xhci_control_transfer(ctrl, slot, &setup, 0, 0, false);
    }

    case ENUM_STATE_WAIT_SET_PROTOCOL: {
        usb_setup_packet_t setup = {
            .bmRequestType = 0x21,
            .bRequest = HID_REQ_SET_PROTOCOL,
            .wValue = 0,                /* 0 = boot protocol */
            .wIndex = slot->interface_num,
            .wLength = 0
        };
        return xhci_control_transfer(ctrl, slot, &setup, 0, 0, false);
    }

    case ENUM_STATE_WAIT_SET_IDLE: {
        usb_setup_packet_t setup = {
            .bmRequestType = 0x21,
            .bRequest = HID_REQ_SET_IDLE,
            .wValue = 0,
            .wIndex = slot->interface_num,
            .wLength = 0
        };
        return xhci_control_transfer(ctrl, slot, &setup, 0, 0, false);
    }

    default:
        return -1;              /* not a step made of a control transfer */
    }
}

bool xhci_enum_step_can_be_asked_again(uint8_t state)
{
    switch (state) {
        case ENUM_STATE_WAIT_GET_DESC_HEADER:
        case ENUM_STATE_WAIT_GET_DESCRIPTOR:
        case ENUM_STATE_WAIT_GET_CONFIG_HEADER:
        case ENUM_STATE_WAIT_GET_CONFIG_DESC:
        case ENUM_STATE_WAIT_SET_CONFIGURATION:
        case ENUM_STATE_WAIT_SET_PROTOCOL:
        case ENUM_STATE_WAIT_SET_IDLE:
            return true;
        default:
            return false;
    }
}

void xhci_enum_recover_ep0(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                           uint8_t resume_at, bool ask_again)
{
    if (!ctrl || !slot) {
        return;
    }

    slot->step_resume  = resume_at;
    slot->step_reissue = ask_again;
    slot->state = ENUM_STATE_WAIT_EP0_RESET;

    if (xhci_post_reset_endpoint_cmd(ctrl, slot, slot->slot_id, 1) < 0) {
        xhci_slot_retire(ctrl, slot);
    }
}

/*
 * The pipe is clear and enumeration carries on — either by stepping over the
 * request that failed, or by making it again. One place, because both routes
 * out of the recovery arrive here and getting them the wrong way round is a
 * device that skips a step it needed or repeats one it did not.
 */
static void enum_resume_after_recovery(xhci_controller_t* ctrl,
                                       struct xhci_device_slot* slot,
                                       uint8_t slot_id)
{
    slot->state = slot->step_resume;

    if (!slot->step_reissue) {
        xhci_enum_advance_state(ctrl, slot, slot_id, TRB_COMPLETION_SUCCESS);
        return;
    }

    slot->step_reissue = false;
    if (enum_reissue(ctrl, slot) < 0) {
        kprintf("[xHCI %s] port %u: %s could not be asked again — releasing "
                "the slot\n", ctrl->name, slot->port_num,
                xhci_enum_state_name(slot->state));
        xhci_slot_retire(ctrl, slot);
    }
}

/* What a slot is waiting for, in words. Exported because the transfer path
 * reports failures against it and "step 10" is not something anybody can
 * read off a screen. */
const char* xhci_enum_state_name(uint8_t state) {
    switch (state) {
        case ENUM_STATE_IDLE:                   return "idle";
        case ENUM_STATE_CLAIMING:               return "claiming a slot";
        case ENUM_STATE_QUEUED:                 return "waiting its turn on the bus";
        case ENUM_STATE_WAIT_PORT_RESET:        return "waiting for the port reset";
        case ENUM_STATE_WAIT_RESET_RECOVERY:    return "letting the device come up after its reset";
        case ENUM_STATE_WAIT_ENABLE_SLOT:       return "waiting for Enable Slot";
        case ENUM_STATE_WAIT_ADDRESS_DEVICE_BSR:return "waiting for Address Device, with the bus left alone";
        case ENUM_STATE_WAIT_ADDRESS_DEVICE:    return "waiting for Address Device";
        case ENUM_STATE_WAIT_GET_DESC_HEADER:   return "reading the first 8 descriptor bytes";
        case ENUM_STATE_WAIT_GET_DESCRIPTOR:    return "reading the device descriptor";
        case ENUM_STATE_WAIT_GET_CONFIG_HEADER: return "reading the configuration header";
        case ENUM_STATE_WAIT_GET_CONFIG_DESC:   return "reading the configuration";
        case ENUM_STATE_WAIT_SET_CONFIGURATION: return "waiting for Set Configuration";
        case ENUM_STATE_WAIT_SET_PROTOCOL:      return "waiting for Set Protocol";
        case ENUM_STATE_WAIT_SET_IDLE:          return "waiting for Set Idle";
        case ENUM_STATE_WAIT_EP0_RESET:         return "clearing a stalled control pipe";
        case ENUM_STATE_WAIT_EP0_STOP:          return "taking back a control transfer that was never answered";
        case ENUM_STATE_WAIT_EP0_DEQUEUE:       return "repositioning the control ring";
        case ENUM_STATE_WAIT_CONFIGURE_ENDPOINT:return "waiting for Configure Endpoint";
        case ENUM_STATE_CONFIGURED:             return "configured";
        default:                                return "?";
    }
}

/*
 * The step that never finished.
 *
 * Every state above waits on an event, and an event that never arrives is a
 * device that is simply never mentioned again — the slot stays claimed, the
 * port stays unusable, and nothing anywhere says which of a dozen steps it was
 * that went quiet. That is the worst kind of failure to meet on a machine you
 * can only read by photographing its screen.
 *
 * So the wait has an outside edge. Not as a substitute for the event — the
 * event is still what drives everything — but so that its absence becomes a
 * printed fact naming the exact step, and the port is handed back for whatever
 * is plugged in next.
 */
/*
 * What the device offers, gathered while walking one interface.
 *
 * The walk reports endpoints one at a time rather than filling an array, so
 * this keeps only the ones any driver here knows what to do with. An interface
 * that offers six endpoints is not a problem to be capped; it is five endpoints
 * nobody asked about.
 */
typedef struct {
    /* Kept whole rather than picked apart. Every field of what the device said
     * about an endpoint ends up in its endpoint context, and copying out three
     * of them was how the burst size came to be dropped on the floor for every
     * SuperSpeed device on the bus. */
    usb_endpoint_info_t intr_in, bulk_in, bulk_out;
    uint8_t  intr_in_dci, bulk_in_dci, bulk_out_dci;
} enum_pick_t;

static bool enum_pick_visit(void* ctx, const usb_endpoint_info_t* ep)
{
    enum_pick_t* p = (enum_pick_t*)ctx;
    bool in = (ep->addr & 0x80) != 0;

    switch (ep->attributes & 0x03) {
        case USB_EP_XFER_INTERRUPT:
            if (in && !p->intr_in_dci) {
                p->intr_in_dci = xhci_dci_of(ep->addr);
                p->intr_in     = *ep;
            }
            break;
        case USB_EP_XFER_BULK:
            if (in && !p->bulk_in_dci) {
                p->bulk_in_dci = xhci_dci_of(ep->addr);
                p->bulk_in     = *ep;
            } else if (!in && !p->bulk_out_dci) {
                p->bulk_out_dci = xhci_dci_of(ep->addr);
                p->bulk_out     = *ep;
            }
            break;
        default:
            break;
    }
    return true;
}

/*
 * Who drives this device?
 *
 * The device has just accepted its configuration, and the descriptor that
 * described it is still in the scratch page. This is the one place where a
 * device is matched against the drivers this kernel has — and where a device
 * matching none of them is settled rather than discarded.
 *
 * It is deliberately not a table of drivers scanning for devices. Each branch
 * asks the descriptor a question it already knows the answer to, prepares the
 * endpoints it needs, and hands the slot back to the state machine.
 */
static void enum_bind_driver(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    void*    cfg = slot->descriptor_buffer_virt;
    uint16_t len = slot->config_total_len;
    uint8_t  iface = 0;
    enum_pick_t pick;

    /* A boot-protocol keyboard, which is the one HID shape a kernel can read
     * without parsing a report descriptor. */
    memset(&pick, 0, sizeof(pick));
    if (usb_walk_interface(cfg, len, USB_HID_CLASS, USB_HID_SUBCLASS_BOOT,
                           USB_HID_PROTOCOL_KEYBOARD, &iface,
                           enum_pick_visit, &pick) && pick.intr_in_dci) {

        slot->driver          = XHCI_DRIVER_KEYBOARD;
        slot->interface_num   = iface;
        slot->ep_interrupt_in = pick.intr_in_dci;

        if (xhci_ep_prepare(slot, pick.intr_in_dci, XHCI_EP_TYPE_INTERRUPT_IN,
                            &pick.intr_in, pick.intr_in.max_packet) != 0) {
            kprintf("[xHCI] port %u: no memory for the keyboard endpoint\n",
                    slot->port_num);
            xhci_slot_retire(ctrl, slot);
            return;
        }

        usb_setup_packet_t setup = {
            .bmRequestType = 0x21,
            .bRequest = HID_REQ_SET_PROTOCOL,
            .wValue = 0,                /* 0 = boot protocol */
            .wIndex = iface,
            .wLength = 0
        };
        slot->state = ENUM_STATE_WAIT_SET_PROTOCOL;
        if (xhci_control_transfer(ctrl, slot, &setup, 0, 0, false) < 0) {
            xhci_slot_retire(ctrl, slot);
        }
        return;
    }

    /* Mass storage over Bulk-Only Transport. The subclass is not part of the
     * match: 0x06 (SCSI transparent) is what everything modern reports, and
     * the older ones answer the same READ(10) and WRITE(10) anyway. What
     * matters is the transport, because that is what decides the shape of
     * every exchange after this point. */
    memset(&pick, 0, sizeof(pick));
    if (usb_walk_interface(cfg, len, USB_CLASS_MASS_STORAGE, USB_CLASS_ANY,
                           USB_MSD_PROTOCOL_BOT, &iface,
                           enum_pick_visit, &pick) &&
        pick.bulk_in_dci && pick.bulk_out_dci) {

        slot->driver        = XHCI_DRIVER_STORAGE;
        slot->interface_num = iface;
        slot->ep_bulk_in    = pick.bulk_in_dci;
        slot->ep_bulk_out   = pick.bulk_out_dci;

        if (xhci_ep_prepare(slot, pick.bulk_in_dci, XHCI_EP_TYPE_BULK_IN,
                            &pick.bulk_in, 0) != 0 ||
            xhci_ep_prepare(slot, pick.bulk_out_dci, XHCI_EP_TYPE_BULK_OUT,
                            &pick.bulk_out, 0) != 0) {
            kprintf("[xHCI] port %u: no memory for the storage endpoints\n",
                    slot->port_num);
            xhci_slot_retire(ctrl, slot);
            return;
        }

        slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;
        if (xhci_ep_configure(ctrl, slot) != 0) {
            xhci_slot_retire(ctrl, slot);
        }
        return;
    }

    /* A hub, which is an ordinary device that happens to have ports. It gets
     * one interrupt endpoint, on which it says when something below it has
     * changed — and nothing else here, because everything a hub is for needs
     * control transfers and this is the interrupt handler. */
    memset(&pick, 0, sizeof(pick));
    if (usb_walk_interface(cfg, len, USB_CLASS_HUB, USB_CLASS_ANY,
                           USB_CLASS_ANY, &iface,
                           enum_pick_visit, &pick) && pick.intr_in_dci) {

        slot->driver          = XHCI_DRIVER_HUB;
        slot->interface_num   = iface;
        slot->ep_interrupt_in = pick.intr_in_dci;

        slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;
        if (xhci_ep_prepare(slot, pick.intr_in_dci, XHCI_EP_TYPE_INTERRUPT_IN,
                            &pick.intr_in, pick.intr_in.max_packet) != 0 ||
            xhci_ep_configure(ctrl, slot) != 0) {
            kprintf("[xHCI] port %u: could not configure the hub\n",
                    slot->port_num);
            xhci_slot_retire(ctrl, slot);
        }
        return;
    }

    enum_settle_unclaimed(slot, cfg, len);
}

/* The device is configured and the controller knows its endpoints. Whoever
 * claimed it takes it from here. */
static void enum_driver_start(xhci_controller_t* ctrl, struct xhci_device_slot* slot)
{
    switch (slot->driver) {

    case XHCI_DRIVER_KEYBOARD: {
        xhci_endpoint_t* ep = &slot->endpoints[slot->ep_interrupt_in];
        if (xhci_ep_submit(ctrl, slot, slot->ep_interrupt_in,
                           ep->buffer_phys, ep->max_packet) != 0) {
            kprintf("[xHCI] port %u: keyboard endpoint could not be primed\n",
                    slot->port_num);
            xhci_slot_retire(ctrl, slot);
            return;
        }
        kprintf("[xHCI] port %u: %s-speed keyboard %04x:%04x is live "
                "(slot %u, endpoint %u)\n",
                slot->port_num, speed_name(slot->speed),
                slot->device_desc.idVendor, slot->device_desc.idProduct,
                slot->slot_id, slot->ep_interrupt_in);
        break;
    }

    case XHCI_DRIVER_HUB:
        /* Same reasoning as storage: everything a hub needs doing is a control
         * transfer, and this runs inside the event handler. Saying that there
         * is a hub waiting is the whole of what can be done from here. */
        kprintf("[xHCI] port %u: %s-speed hub %04x:%04x on slot %u\n",
                slot->port_num, speed_name(slot->speed),
                slot->device_desc.idVendor, slot->device_desc.idProduct,
                slot->slot_id);
        xhci_hub_note_work();
        break;

    case XHCI_DRIVER_STORAGE:
        /* Deliberately no SCSI here. Everything in this function runs inside
         * the event handler, and asking a disk how big it is means bulk
         * transfers, and waiting for a bulk transfer means draining the event
         * ring — from inside the drain that called us. The conversation with
         * the disk happens in ordinary kernel context, where somebody wants to
         * read from it; this only records that there is a disk to have it
         * with. */
        kprintf("[xHCI] port %u: %s-speed mass storage %04x:%04x on slot %u "
                "(bulk in %u, out %u)\n",
                slot->port_num, speed_name(slot->speed),
                slot->device_desc.idVendor, slot->device_desc.idProduct,
                slot->slot_id, slot->ep_bulk_in, slot->ep_bulk_out);

        /* And the room is told there is a medium to seat. A flag, because
         * seating one means asking it how large it is, and that is a transfer
         * this handler cannot wait for. */
        BoardroomNoteArrival();
        break;

    default:
        break;
    }

    xhci_touch_device_arrived(slot);
}

int xhci_enum_settle(xhci_controller_t* ctrl, uint32_t timeout_ms)
{
    if (!ctrl || !ctrl->initialized) {
        return 0;
    }

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);

    for (;;) {
        xhci_process_events();

        /*
         * The watchdogs run from here as well as from the timer tick.
         *
         * This is reached during boot, and boot is exactly the stretch where
         * the tick may not be running — so a device that stops answering would
         * hold this loop for its whole budget with nothing to end the wait, and
         * the two facilities written to say why it stopped would never once
         * have run at the moment they were needed. They are idempotent and
         * cost a walk of two tables when there is nothing to find.
         */
        xhci_check_command_timeouts(ctrl);

        /*
         * And the abort the watchdog above only ASKS for.
         *
         * It asks rather than acts because it is also reached from the timer
         * interrupt, where a five-second poll of CRCR would be five seconds of
         * the machine's clock (xhci.h, cmd_abort_wanted). The guide loop is
         * what carries it out at runtime — and the guide loop does not exist
         * yet here: this runs during boot, before `sti`, on the one core there
         * is. So this loop is the guide loop for as long as boot lasts, and
         * without this line a ring that stopped answering during enumeration
         * would never be taken back at all.
         */
        xhci_command_abort_if_wanted(ctrl);

        xhci_enum_watchdog(ctrl);
        xhci_enum_pump(ctrl);

        /* This is a context that may wait, so anything that departed while the
         * bus was settling gets taken down here rather than waiting for a core
         * to go idle — which during boot may be a while. */
        xhci_slot_service(ctrl);

        int busy = 0;
        for (uint32_t i = 0; i < ctrl->slot_count; i++) {
            uint8_t state = __atomic_load_n(&ctrl->slots[i].state,
                                            __ATOMIC_ACQUIRE);
            if (state != ENUM_STATE_IDLE && state != ENUM_STATE_CONFIGURED &&
                state != ENUM_STATE_RETIRING) {
                busy++;     /* mid-enumeration; leaving is not enumerating */
            }
        }

        if (busy == 0) {
            return 0;
        }
        if ((int64_t)(rdtsc() - deadline) >= 0) {
            return busy;
        }
        cpu_pause();
    }
}

void xhci_enum_for_each_configured(xhci_controller_t* ctrl,
                                   xhci_slot_visitor visit, void* ctx)
{
    if (!visit || !ctrl) {
        return;
    }
    if (!ctrl->slots) {
        return;
    }
    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        if (ctrl->slots[i].state == ENUM_STATE_CONFIGURED) {
            visit(ctx, &ctrl->slots[i]);
        }
    }
}

/*
 * The one report a photograph of a screen has to be able to answer from.
 *
 * A device that stops halfway through being enumerated has a small number of
 * possible causes, and they are told apart by facts that are all readable at
 * the moment it is given up on. Every one of them is printed here, together,
 * because the alternative — piecing the story together from lines printed
 * seconds earlier — does not survive the ten screens of self-tests that run
 * between the two, and has not survived them in seven attempts.
 *
 * The decisive line is the controller's own: an Output Slot Context still
 * reading Disabled says the Address Device never took effect, and one reading
 * Addressed says it did and its answer went missing. Those are opposite
 * faults, and nothing else in the machine distinguishes them.
 */
static void xhci_report_stuck(xhci_controller_t* ctrl,
                              struct xhci_device_slot* slot, uint8_t state)
{
    uint8_t  cmd_type = 0;
    uint32_t cmd_age  = 0;
    bool     waiting  = xhci_command_oldest_for(ctrl, slot, &cmd_type, &cmd_age);

    kprintf("[xHCI %s] port %u (slot %u): gave up after %u ms while %s\n",
            ctrl->name, slot->port_num, slot->slot_id, XHCI_ENUM_TIMEOUT_MS,
            xhci_enum_state_name(state));

    kprintf("[xHCI %s]   the controller says: slot %s, address %u; "
            "PORTSC 0x%08x; %u interrupt(s) so far\n",
            ctrl->name,
            xhci_slot_state_name(xhci_slot_context_state(slot)),
            xhci_slot_context_address(slot),
            xhci_get_port_status(ctrl, slot->port_num),
            __atomic_load_n(&ctrl->irq_count, __ATOMIC_RELAXED));

    /* And what was done to the port before any of this was said to the device.
     * A device only answers the default address from the Default state, and it
     * only reaches the Default state through a reset — so an Address Device
     * that was never answered on a port that was never reset is a different
     * fault from the same silence on a port that was. */
    kprintf("[xHCI %s]   the port was %s%s (USB %u), %u ms; EP0 packet %u\n",
            ctrl->name, xhci_port_reset_kind_name(slot->reset_kind),
            slot->depth ? ", behind a hub" : "",
            xhci_port_protocol(ctrl, slot->port_num),
            slot->reset_took_ms, slot->ep0_max_packet);

    /*
     * ‼ AND WHAT THE CONTROL PIPE ITSELF IS DOING, WHICH IS THE ONE FACT THAT
     * SEPARATES THE THREE WAYS A DESCRIPTOR READ CAN GO QUIET.
     *
     * The lines above describe the SLOT, and a slot reading Default with an
     * address of zero is the correct, healthy state for a device that has been
     * addressed with BSR set and is about to be asked who it is. It is
     * therefore identical on a bus that is working and on the board where the
     * next step never answered — which is why seven reports of this failure
     * named nothing that could be acted on.
     *
     * The endpoint answers it, and the controller writes that answer itself:
     *
     *   Running, and the ring still queued  — the stages are there and the
     *       device is NAKing them. A NAK is not an error and is retried for
     *       ever, so this is the only failure on the bus that reports itself as
     *       silence: the device has not finished coming up. Look at the
     *       recovery it was given (XHCI_PORT_RESET_RECOVERY_MS).
     *   Halted   — the transfer failed and the event that said so was lost.
     *   Error / Disabled — the context is wrong, and nothing was ever tried.
     *
     * The two ring positions are what "still queued" is made of: enqueue is
     * where software wrote the last stage, dequeue only moves when a transfer
     * event names a TRB. Equal means every stage has been answered; apart means
     * the controller has not finished with them.
     */
    if (slot->ep0_ring) {
        kprintf("[xHCI %s]   its control pipe is %s; software has queued to %u "
                "and the controller has answered up to %u\n",
                ctrl->name,
                xhci_ep_state_name(xhci_ep_context_state(ctrl, slot, 1)),
                slot->ep0_ring->enqueue_idx, slot->ep0_ring->dequeue_idx);
    }

    if (waiting) {
        kprintf("[xHCI %s]   still waiting on %s, posted %u ms ago\n",
                ctrl->name, xhci_command_name(cmd_type), cmd_age);
    } else {
        kprintf("[xHCI %s]   no command outstanding for it — the last answer "
                "arrived and moved nothing\n", ctrl->name);
    }

    /* Those three lines ARE the report, and they are written to stand on their
     * own: everything they name is read at the moment of the failure, so
     * nothing has to be pieced together from lines that scrolled past earlier.
     * That is what replaced holding the screen for them. */
}

/*
 * Two watchdogs, and only one of them owns any given wait.
 *
 * A step that is waiting on a command belongs to the command watchdog, which
 * gives the controller its full budget and then aborts the ring properly. This
 * one covers the rest — a port event or a transfer that never came — and it
 * must not reap a device out from under the other, because the two would then
 * be taking the same slot apart from opposite ends: a Disable Slot racing an
 * Address Device that the controller has not finished with.
 */
void xhci_enum_watchdog(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->initialized) {
        return;
    }

    uint64_t now = rdtsc();
    uint64_t budget = cpu_ms_to_tsc(XHCI_ENUM_TIMEOUT_MS);
    static bool reported_controller = false;

    for (uint32_t i = 0; i < ctrl->slot_count; i++) {
        struct xhci_device_slot* slot = &ctrl->slots[i];

        uint8_t state = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
        if (state == ENUM_STATE_IDLE || state == ENUM_STATE_CONFIGURED ||
            state == ENUM_STATE_RETIRING) {
            continue;   /* nothing started, finished, or already leaving */
        }
        if (state == ENUM_STATE_QUEUED) {
            continue;   /* waiting its turn is not being stuck */
        }
        if (slot->timestamp_started == 0) {
            continue;
        }

        /*
         * ── the fact first ────────────────────────────────────────────────
         *
         * A device that has been taken out of the socket is not a device that
         * is answering slowly, and the port says which. Only the NEGATIVE
         * answer means anything: a device behind a hub carries the ROOT port,
         * so a healthy reading there describes the branch and says nothing
         * about the device — the same rule the disk driver already lives by.
         *
         * Without this, an unplug during enumeration cost the whole budget
         * before anybody looked, and the socket could not be given to whatever
         * was plugged in next until it had.
         */
        if (slot->port_num != 0 &&
            xhci_port_says_gone(ctrl, slot->port_num)) {
            kprintf("[xHCI %s] port %u: nothing is attached there any more — "
                    "%s was for a device that has gone\n",
                    ctrl->name, slot->port_num, xhci_enum_state_name(state));
            xhci_slot_retire(ctrl, slot);
            continue;
        }

        if (xhci_command_pending_for(ctrl, slot)) {
            /* The command watchdog has this one, and it asks a better question
             * than this loop can: whether the CONTROLLER has answered anything
             * at all, rather than how long this one request has lived. The
             * step clock is restarted so that a device which spent seconds
             * inside a command arrives at its NEXT step with a full budget
             * instead of none — which is what threw devices away on a board
             * where an Address Device took ten seconds. */
            slot->watch_state = state;
            slot->watch_since = now;
            continue;
        }

        /*
         * ── and only then the clock, on the STEP ──────────────────────────
         *
         * The state this slot is in IS the thing it is waiting for, so a state
         * that has not changed is a step that has not been answered. Anything
         * that happens to the device moves it, and moving it starts the clock
         * again — which is what the budget's own words have always claimed and
         * what measuring from `timestamp_started` never did.
         */
        if (slot->watch_state != state || slot->watch_since == 0) {
            slot->watch_state = state;
            slot->watch_since = now;
            continue;
        }
        if ((int64_t)(now - slot->watch_since) < (int64_t)budget) {
            continue;
        }

        xhci_report_stuck(ctrl, slot, state);

        /* And the controller as a whole, once per boot: whether it is still
         * running, whether its command ring is, whether events are sitting on
         * the ring unread, and whether the cycle bit this driver expects still
         * matches what the controller is writing. */
        if (!reported_controller) {
            reported_controller = true;
            xhci_ring_t* er = &ctrl->event_ring;
            uint32_t at_dequeue = er->trbs ? er->trbs[er->dequeue_idx].control : 0;
            kprintf("[xHCI %s] state: USBSTS=0x%08x USBCMD=0x%08x "
                    "CRCR=0x%08x ERDP=0x%08x IMAN=0x%08x MFINDEX=0x%08x | "
                    "event ring at %u expecting cycle %u, TRB there 0x%08x | "
                    "command ring at %u cycle %u | %u command(s) outstanding\n",
                    ctrl->name,
                    ctrl->op_regs->usbsts, ctrl->op_regs->usbcmd,
                    (uint32_t)ctrl->op_regs->crcr,
                    (uint32_t)ctrl->runtime_regs->interrupters[0].erdp,
                    ctrl->runtime_regs->interrupters[0].iman,
                    ctrl->runtime_regs->mfindex,
                    er->dequeue_idx, er->cycle_state, at_dequeue,
                    ctrl->command_ring.enqueue_idx,
                    ctrl->command_ring.cycle_state,
                    xhci_command_outstanding(ctrl));
        }

        /*
         * ‼ AND THEN THE SAME ROAD THE ERROR TAKES, BECAUSE IT IS THE SAME FACT.
         *
         * "This step did not deliver" reaches this driver two ways, and until
         * now they cost wildly different amounts. A device that answers with an
         * error goes to xhci_enum_recover_ep0: the pipe is cleared and the step
         * is asked again, in microseconds, keeping its slot. A device that
         * answers NOTHING came here and had its slot thrown away — Disable
         * Slot, a teardown, a fresh port reset, Enable Slot and two Address
         * Devices, two seconds of boot, and then the same question asked again
         * anyway.
         *
         * Measured on the owner's board, one run: the error road was taken
         * ELEVEN times and recovered eleven times. The silence road was taken
         * once and also recovered — the long way round. The first control
         * transaction after a port reset is simply unreliable on that machine,
         * which is what every USB host is built to expect.
         *
         * ‼ WHY Stop Endpoint AND NOT xhci_ep_abandon, WHICH ALREADY EXISTS.
         *
         * Two reasons, and both are fatal to the shorter version. This function
         * is reached from xhci_tick, which runs INSIDE IRQ0 and does not send
         * its end-of-interrupt until it returns — and xhci_ep_abandon waits for
         * the command ring twice, up to five seconds each. That is the fault
         * this driver already spent a session taking out of the tick. And
         * xhci_ep_abandon does nothing at all unless the endpoint is marked
         * in-flight, which enumeration's control transfers never are: they are
         * driven by the state machine, not waited on.
         *
         * So the command is POSTED and the state says what is being waited for,
         * exactly like every other step here. Nothing waits.
         *
         * Bounded by the same budget the error road uses, so a device that is
         * genuinely not going to answer still loses its slot — after the same
         * number of tries, not before them.
         */
        if (xhci_enum_step_can_be_asked_again(state) &&
            slot->step_retry < XHCI_STEP_RETRIES &&
            slot->ep0_ring && slot->slot_id != 0) {

            slot->step_retry++;
            kprintf("[xHCI %s] port %u: %s — taking the transfer back and "
                    "asking again (attempt %u of %u)\n",
                    ctrl->name, slot->port_num, xhci_enum_state_name(state),
                    slot->step_retry, XHCI_STEP_RETRIES);

            slot->step_resume  = state;
            slot->step_reissue = true;
            __atomic_store_n(&slot->state, (uint8_t)ENUM_STATE_WAIT_EP0_STOP,
                             __ATOMIC_RELEASE);

            /* The step's clock starts again here rather than at the next pass:
             * this IS something happening to the device, and the watchdog
             * measures silence, not elapsed life. */
            slot->watch_state = ENUM_STATE_WAIT_EP0_STOP;
            slot->watch_since = now;

            if (xhci_post_stop_endpoint_cmd(ctrl, slot, slot->slot_id, 1) < 0) {
                xhci_slot_retire(ctrl, slot);
            }
            continue;
        }

        xhci_slot_retire(ctrl, slot);
    }
}

/*
 * ‼ THE RULE THIS FUNCTION IS BUILT ON
 *
 * Say what is being waited for BEFORE doing the thing that ends the wait.
 *
 * Every step below ends by asking the bus or the controller for something and
 * writing down which answer it is now waiting for. Those two must happen in
 * that order — the note first, the request second. A request is answered by an
 * event, an event is drained by whichever core reaches the ring first, and on
 * a real machine that is routinely a different core from the one still walking
 * through this function. If the request goes out first, the answer can arrive
 * and be looked up against a slot whose note still names the previous step:
 * there is nowhere to put it, it is dropped, and the device waits for an event
 * that has already been and gone.
 *
 * This was learned once on the port reset and applied only there. It cost four
 * devices on a live board, twice: three Configure Endpoint sites and all eight
 * transfer sites had it the wrong way round. Under emulation the drain is very
 * nearly always the same core that posted, so none of it was visible.
 */
void xhci_enum_advance_state(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                             uint8_t slot_id, uint8_t completion_code) {
    if (!ctrl || !slot) {
        return;
    }

    /*
     * No search, and therefore no guess.
     *
     * There were two here, layered on each other: match the completion's slot
     * id against the device table, and failing that take the first device
     * sitting in WAIT_ENABLE_SLOT. Both were needed because the command layer
     * had thrown away the one fact that settles it — which device asked. It
     * does not any more: the command carries its asker, the answer carries the
     * command, and this function is simply told.
     */

    if (completion_code != TRB_COMPLETION_SUCCESS) {
        /*
         * Clearing a pipe that was not halted is not a failure.
         *
         * Reset Endpoint answers Context State Error when the endpoint is not
         * in the Halted state — which happens when the halt cleared itself, or
         * when the stall this driver saw belonged to a transfer the controller
         * had already unwound. The pipe is usable either way, and throwing the
         * device away for it turns a recovered stall into a lost device.
         */
        /*
         * A controller that will not do the first half of addressing on its own.
         *
         * Blocking the SET_ADDRESS is what every xHCI since 1.0 offers and what
         * Section 4.3.4 is written around, but the bit is one this driver hands
         * to somebody else's silicon and the refusal is a completion code, not
         * a crash. So a controller that answers the blocked command with
         * anything other than Success is asked the old way instead — one step,
         * SET_ADDRESS straight away — which is what this driver did for its
         * whole life and is worse than the two steps rather than broken.
         *
         * Said out loud, because it is a permanent property of the machine and
         * it explains every enumeration after it.
         */
        if (slot->state == ENUM_STATE_WAIT_ADDRESS_DEVICE_BSR) {
            kprintf("[xHCI %s] port %u: this controller refused to address a "
                    "device without speaking to it (%s) — doing it in one step "
                    "instead\n", ctrl->name, slot->port_num,
                    xhci_completion_name(completion_code));
            enum_free_input_ctx(ctrl, slot);
            enum_address_device(ctrl, slot,
                                ep0_initial_max_packet(slot->speed), false);
            return;
        }

        if (slot->state == ENUM_STATE_WAIT_EP0_RESET &&
            completion_code == TRB_COMPLETION_CONTEXT_STATE) {
            kprintf("[xHCI %s] port %u: the control pipe was not halted after "
                    "all — carrying on\n", ctrl->name, slot->port_num);
            enum_resume_after_recovery(ctrl, slot, slot_id);
            return;
        }

        kprintf("[xHCI %s] port %u: enumeration step %s failed — %s (code %u); "
                "releasing the slot\n",
                ctrl->name, slot->port_num, xhci_enum_state_name(slot->state),
                xhci_completion_name(completion_code), completion_code);
        xhci_slot_retire(ctrl, slot);
        return;
    }

    switch (slot->state) {

        case ENUM_STATE_WAIT_ENABLE_SLOT: {
            if (slot_id == 0 || slot_id > ctrl->max_slots) {
                kprintf("[xHCI] Enable Slot returned slot id %u, which this "
                        "controller cannot have\n", slot_id);
                xhci_slot_retire(ctrl, slot);
                return;
            }

            slot->slot_id = slot_id;
            /* And the index the event drain reads, so the next event naming
             * this slot finds it without a search. */
            if (ctrl->by_id && slot_id <= ctrl->slot_count) {
                spin_lock(&ctrl->slots_lock);
                ctrl->by_id[slot_id] = slot;
                spin_unlock(&ctrl->slots_lock);
            }

            /*
             * A root-port device learns its speed from the port. One behind a
             * hub was told by the hub, and only the hub can know: port_num is
             * the ROOT port for every tier, so what the register reports there
             * is the speed of the link to the hub. A full-speed keyboard on a
             * high-speed hub was being written into its Slot Context as a
             * high-speed device — wrong speed for the controller to schedule
             * by, wrong starting packet size for EP0, and an interval
             * converted by the rule for the wrong half of the bus.
             */
            if (slot->depth == 0) {
                slot->speed = xhci_get_port_speed(ctrl, slot->port_num);
            }

            debug_printf("[xHCI ENUM] Slot enabled: slot_id=%u port=%u speed=%u\n",
                         slot_id, slot->port_num, slot->speed);

            /* The endpoint table is a property of the device, so it comes
             * into being with the device's slot and dies with it. */
            if (xhci_ep_table_alloc(slot) != 0) {
                kprintf("[xHCI] port %u: no memory for the endpoint table\n",
                        slot->port_num);
                xhci_slot_retire(ctrl, slot);
                return;
            }

            /* Allocate EP0 ring now — needed before Address Device. */
            if (xhci_alloc_ep0_ring(ctrl, slot) < 0) {
                debug_printf("[xHCI ENUM] Failed to allocate EP0 ring\n");
                xhci_slot_retire(ctrl, slot);
                return;
            }

            /* Allocate Device Context — xHCI controller writes here directly. */
            void* dev_ctx_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
            if (!dev_ctx_phys) {
                debug_printf("[xHCI ENUM] Failed to allocate Device Context\n");
                xhci_slot_retire(ctrl, slot);
                return;
            }
            slot->dev_ctx_phys = (uint64_t)dev_ctx_phys;
            slot->dev_ctx = vmm_phys_to_virt((uintptr_t)dev_ctx_phys);

            /* Register in DCBAA. */
            ctrl->dcbaa->device_context_ptrs[slot_id] = slot->dev_ctx_phys;

            /* The first of the two: the slot moves to Default and nothing is
             * said on the bus (Section 4.3.4). The packet size is the one the
             * specification fixes for this speed, because the device has not
             * been asked yet — full speed is the exception and gets 8, which
             * every full-speed device must accept. */
            enum_address_device(ctrl, slot,
                                ep0_initial_max_packet(slot->speed), true);
            break;
        }

        case ENUM_STATE_WAIT_ADDRESS_DEVICE_BSR: {
            enum_free_input_ctx(ctrl, slot);

            /* Eight bytes, asked of a device that still answers the default
             * address. That is everything up to and including bMaxPacketSize0,
             * which is the one endpoint parameter a device cannot be asked
             * about until it has already been talked to. */
            slot->state = ENUM_STATE_WAIT_GET_DESC_HEADER;
            if (enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 8) < 0) {
                kprintf("[xHCI %s] port %u: could not ask for the device "
                        "descriptor\n", ctrl->name, slot->port_num);
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_ADDRESS_DEVICE: {
            debug_printf("[xHCI ENUM] Device addressed: slot=%u\n", slot_id);
            enum_free_input_ctx(ctrl, slot);

            slot->state = ENUM_STATE_WAIT_GET_DESCRIPTOR;
            if (enum_get_descriptor(ctrl, slot, USB_DT_DEVICE, 18) < 0) {
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_GET_DESC_HEADER: {
            if (!enum_got_it_all(ctrl, slot, 8)) {
                enum_ask_again(ctrl, slot);
                return;
            }

            usb_device_desc_t* desc = (usb_device_desc_t*)slot->descriptor_buffer_virt;
            uint8_t mps0 = desc->bMaxPacketSize0;

            /* A SuperSpeed device states the field as an exponent. */
            uint16_t real_mps;
            if (slot->speed == XHCI_PORT_SPEED_SUPER ||
                slot->speed == XHCI_PORT_SPEED_SUPER_10) {
                real_mps = (mps0 <= 15) ? (uint16_t)(1u << mps0) : 512;
            } else {
                real_mps = mps0;
            }

            if (real_mps != 8 && real_mps != 16 && real_mps != 32 &&
                real_mps != 64 && real_mps != 512) {
                kprintf("[xHCI] port %u: device names an impossible EP0 packet "
                        "size (%u) — refusing it\n", slot->port_num, real_mps);
                xhci_slot_retire(ctrl, slot);
                return;
            }

            /*
             * And now the device may be given its address.
             *
             * The packet size it just named goes into this command's Input
             * Context, so the endpoint is described correctly the first time
             * it matters. What that replaced was an Evaluate Context posted
             * afterwards to correct a context which had been wrong in between
             * — a whole extra command, and one more thing that could be left
             * half-done on a bus that had already gone quiet.
             */
            enum_address_device(ctrl, slot, real_mps, false);
            break;
        }

        case ENUM_STATE_WAIT_GET_DESCRIPTOR: {
            if (!enum_got_it_all(ctrl, slot, 18)) {
                enum_ask_again(ctrl, slot);
                return;
            }

            usb_device_desc_t* desc = (usb_device_desc_t*)slot->descriptor_buffer_virt;

            if (!usb_validate_device_desc(desc)) {
                kprintf("[xHCI] port %u: device descriptor is malformed "
                        "(length %u, type %u, EP0 packet %u)\n",
                        slot->port_num, desc->bLength, desc->bDescriptorType,
                        desc->bMaxPacketSize0);
                xhci_slot_retire(ctrl, slot);
                return;
            }

            memcpy(&slot->device_desc, desc, sizeof(usb_device_desc_t));

            debug_printf("[xHCI ENUM] Device descriptor: VID=%04x PID=%04x "
                         "Class=%02x MaxPkt=%u\n",
                         desc->idVendor, desc->idProduct, desc->bDeviceClass,
                         desc->bMaxPacketSize0);

            /* Nine bytes of the configuration descriptor: enough to learn how
             * long the whole thing is. Asking for a fixed 64 was a guess that
             * truncates every composite device — and a keyboard that also
             * carries media keys is a composite device, with its boot-keyboard
             * interface described past whatever the guess covered. */
            slot->state = ENUM_STATE_WAIT_GET_CONFIG_HEADER;
            if (enum_get_descriptor(ctrl, slot, USB_DT_CONFIG, 9) < 0) {
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_GET_CONFIG_HEADER: {
            if (!enum_got_it_all(ctrl, slot, 9)) {
                enum_ask_again(ctrl, slot);
                return;
            }

            usb_config_desc_t* cfg = (usb_config_desc_t*)slot->descriptor_buffer_virt;

            if (cfg->bDescriptorType != USB_DESC_CONFIGURATION || cfg->bLength < 9) {
                kprintf("[xHCI] port %u: configuration descriptor is malformed "
                        "(length %u, type %u)\n",
                        slot->port_num, cfg->bLength, cfg->bDescriptorType);
                xhci_slot_retire(ctrl, slot);
                return;
            }

            uint16_t total = cfg->wTotalLength;
            if (total < 9) {
                total = 9;
            }
            if (total > XHCI_DESC_BUFFER_BYTES) {
                /* One page is what the scratch buffer is. A configuration
                 * larger than that exists in principle and has never been
                 * seen; read what fits and say so rather than overrun. */
                kprintf("[xHCI] port %u: configuration descriptor is %u bytes, "
                        "reading the first %u\n",
                        slot->port_num, total, XHCI_DESC_BUFFER_BYTES);
                total = XHCI_DESC_BUFFER_BYTES;
            }
            slot->config_total_len = total;

            slot->state = ENUM_STATE_WAIT_GET_CONFIG_DESC;
            if (enum_get_descriptor(ctrl, slot, USB_DT_CONFIG, total) < 0) {
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_GET_CONFIG_DESC: {
            /* Asked for exactly what the header said the configuration is, so
             * anything less is the exchange going wrong rather than the device
             * being brief. Walking a configuration that only half arrived
             * reports endpoints out of whatever the page held before. */
            if (!enum_got_it_all(ctrl, slot, slot->config_total_len)) {
                enum_ask_again(ctrl, slot);
                return;
            }

            usb_config_desc_t* cfg = (usb_config_desc_t*)slot->descriptor_buffer_virt;
            slot->config_value = cfg->bConfigurationValue;

            usb_config_first_interface(slot->descriptor_buffer_virt,
                                       slot->config_total_len,
                                       &slot->interface_class,
                                       &slot->interface_subclass,
                                       &slot->interface_protocol);

            /*
             * The number about to be sent, before it is sent.
             *
             * A device that refuses Set Configuration refuses the VALUE, and
             * the value is the one thing the log never said — so three
             * identical refusals on the board were three refusals of a number
             * nobody could see. It comes off the wire like everything else
             * here and there is no reason to believe it without looking.
             */
            kprintf("[xHCI %s] port %u: configuration %u of %u, %u byte(s), "
                    "first interface class %02x/%02x/%02x\n",
                    ctrl->name, slot->port_num, slot->config_value,
                    slot->device_desc.bNumConfigurations,
                    slot->config_total_len, slot->interface_class,
                    slot->interface_subclass, slot->interface_protocol);

            /* Every device gets configured, driver or no driver. Leaving one
             * in the Addressed state is leaving it half spoken to.
             *
             * The device is told which configuration to adopt before the
             * controller is told which endpoints to expect, and that order is
             * deliberate: the endpoints do not exist until the device has
             * selected the configuration that describes them. */
            usb_setup_packet_t setup = {
                .bmRequestType = 0x00,
                .bRequest = USB_REQ_SET_CONFIGURATION,
                .wValue = slot->config_value,
                .wIndex = 0,
                .wLength = 0
            };

            slot->state = ENUM_STATE_WAIT_SET_CONFIGURATION;
            if (xhci_control_transfer(ctrl, slot, &setup, 0, 0, false) < 0) {
                kprintf("[xHCI %s] port %u: could not ask it to take its "
                        "configuration\n", ctrl->name, slot->port_num);
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_SET_CONFIGURATION: {
            /* The configuration descriptor is still in the scratch page — Set
             * Configuration carries no data — so this is where the device is
             * matched against the drivers this kernel has, and the endpoints
             * it will actually use are prepared. */
            enum_bind_driver(ctrl, slot);
            break;
        }

        case ENUM_STATE_WAIT_SET_PROTOCOL: {
            usb_setup_packet_t setup = {
                .bmRequestType = 0x21,
                .bRequest = HID_REQ_SET_IDLE,
                .wValue = 0,
                .wIndex = slot->interface_num,
                .wLength = 0
            };

            slot->state = ENUM_STATE_WAIT_SET_IDLE;
            if (xhci_control_transfer(ctrl, slot, &setup, 0, 0, false) < 0) {
                kprintf("[xHCI %s] port %u: could not ask it to stop idling\n",
                        ctrl->name, slot->port_num);
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        /*
         * ‼ TWO ROADS, ONE JUNCTION, AND EVERYTHING AFTER IT IS SHARED.
         *
         * A device that answered with an error halted the pipe, and the halt
         * was cleared with Reset Endpoint. A device that answered NOTHING
         * halted nothing, and the transfer was taken off it with Stop
         * Endpoint. Two different commands, because the specification defines
         * each of them for a different endpoint state — and the same thing to
         * do next either way: the TRBs in front of the controller will never
         * be executed, so it is told where software actually stands.
         *
         * Falling through rather than copying is the point. What separates the
         * two cases is one command; what follows is one repair, and it should
         * be impossible to fix one of them and not the other.
         */
        case ENUM_STATE_WAIT_EP0_STOP:
        case ENUM_STATE_WAIT_EP0_RESET: {
            /* The transfer is off the endpoint, by whichever of the two
             * commands was right for the state it was in. What it left on the
             * ring is still in front of the controller's dequeue pointer, so
             * tell it where software has actually got to. */
            uint64_t resume = slot->ep0_ring->trbs_phys +
                              (uint64_t)slot->ep0_ring->enqueue_idx *
                              sizeof(xhci_trb_t);

            /* Those TRBs will never be executed and will never be answered, so
             * their slots come back now — otherwise every cleared stall costs
             * the ring a transfer's worth of space for good. */
            xhci_ring_abandon(slot->ep0_ring);

            slot->state = ENUM_STATE_WAIT_EP0_DEQUEUE;

            if (xhci_post_set_tr_dequeue_cmd(ctrl, slot, slot->slot_id, 1,
                    resume | (slot->ep0_ring->cycle_state ? 1u : 0u)) < 0) {
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_EP0_DEQUEUE: {
            /* The pipe is clear and the ring is where software stands. Either
             * the refused step is now behind us, or it is about to be made
             * again. */
            enum_resume_after_recovery(ctrl, slot, slot_id);
            break;
        }

        case ENUM_STATE_WAIT_SET_IDLE: {
            slot->state = ENUM_STATE_WAIT_CONFIGURE_ENDPOINT;
            if (xhci_ep_configure(ctrl, slot) != 0) {
                kprintf("[xHCI] port %u: could not configure the keyboard "
                        "endpoint\n", slot->port_num);
                xhci_slot_retire(ctrl, slot);
            }
            break;
        }

        case ENUM_STATE_WAIT_CONFIGURE_ENDPOINT: {
            enum_free_input_ctx(ctrl, slot);
            slot->ep_pending_add = 0;
            slot->ever_configured = true;
            if (slot->born_port) {
                ctrl->enum_attempts[slot->born_port] = 0;
            }
            slot->state = ENUM_STATE_CONFIGURED;
            enum_driver_start(ctrl, slot);
            break;
        }

        default:
            /*
             * A completion for a step this device was not on.
             *
             * This is what it looks like when an answer overtakes the question:
             * the command was posted, another core drained its completion, and
             * the state saying what was being waited for had not been written
             * yet. The slot is then left waiting for an event that has already
             * been and gone. Silent until now, and the exact shape of a device
             * that enumerates on an emulator and stops on a real machine.
             */
            kprintf("[xHCI %s] slot %u answered a step it was not on (%s)\n",
                    ctrl->name, slot_id, xhci_enum_state_name(slot->state));
            break;
    }
}
