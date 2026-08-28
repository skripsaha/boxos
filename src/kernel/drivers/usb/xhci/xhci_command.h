#ifndef XHCI_COMMAND_H
#define XHCI_COMMAND_H

#include "xhci.h"
#include "xhci_trb.h"
#include "klib.h"

/*
 * How long the controller is given to answer a command.
 *
 * The specification does not name a number; this is the one Linux uses. What
 * matters more than its value is what happens when it expires — see
 * xhci_check_command_timeouts, which aborts the Command Ring rather than
 * quietly forgetting a TRB the controller may still be about to read.
 */
#define XHCI_CMD_TIMEOUT_MS 5000

/* How long the controller is given to stop its Command Ring when asked to.
 * The specification allows 5 s for the abort to take effect (Section 4.6.1.2);
 * a controller that has not stopped by then is not going to. */
#define XHCI_CMD_ABORT_TIMEOUT_MS 5000

/* How long a single command may take before it is worth a line on the screen.
 * Not a limit — a command slower than this is reported and then waited for. */
#define XHCI_CMD_SLOW_MS 250

/*
 * How long the ring may be silent before it is nudged, and how many nudges it
 * gets before it is aborted.
 *
 * A doorbell is how software says "there is work on the ring". Ringing it again
 * costs one register write, cannot corrupt anything, and is the entire remedy
 * for a doorbell the controller did not act on — which is a real condition on
 * real silicon and looks exactly like a command that hangs. Aborting the ring
 * is the remedy for a controller that has genuinely stopped, and it destroys
 * every command in flight, so it is what happens after the cheap thing has
 * been tried and did not help.
 */
#define XHCI_CMD_QUIET_MS 1000

/*
 * ‼ One, not three. A second identical register write says nothing the first
 * one did not.
 *
 * The nudge exists for a doorbell the controller did not act on, and one nudge
 * settles that question completely: either the write takes, or the controller
 * is not acting on doorbells and no number of further ones will change it.
 * Three of them were three guesses at the same answer, and each cost the full
 * command budget — fifteen seconds of a boot spent asking a question already
 * answered.
 *
 * And it is answered, on the machine this is for: two separate runs on the
 * owner's board rang it three times against an Address Device that would not
 * complete, and all three did nothing. What moved it was the abort, because
 * the abort interrupts a command the controller is IN, which is what this
 * fault actually is. The doorbell is kept because a lost write is real; the
 * repetition is dropped because it was measured to be worthless.
 *
 * This is not a cosmetic number: it is half of how long the bus takes to give
 * up on a device and try it again, and anything waiting for the bus to finish
 * is waiting for exactly that.
 */
#define XHCI_CMD_NUDGES   1

void xhci_command_init(xhci_controller_t* ctrl);

/*
 * Post a command on behalf of a device.
 *
 * Every one of these names the slot that asked. That is not bookkeeping for
 * its own sake: the completion event carries the address of the command TRB,
 * so naming the asker at posting time is what lets the answer be delivered to
 * exactly the device that asked — instead of being matched back to a slot by
 * searching the table for one that looks like it might be waiting, which is
 * what this driver used to do and is where several days of a live board went.
 */
int xhci_post_enable_slot_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                              uint8_t slot_type);
int xhci_post_disable_slot_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                               uint8_t slot_id);
/*
 * Address Device, in whichever of its two halves is being asked for.
 *
 * xHCI 1.2 Section 4.3.4 describes addressing as TWO commands, not one, and
 * `block_set_address` is which of them this is. With it set the controller
 * assigns the slot its resources and moves it to the Default state, and puts
 * NOTHING on the bus — the device goes on answering the default address, which
 * is where it can be asked how large its control packets are. With it clear
 * the controller also sends the device a USB SET_ADDRESS.
 *
 * Section 6.4.3.4 puts the bit at 9 of the control dword.
 */
int xhci_post_address_device_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint64_t input_ctx_phys,
                                 bool block_set_address);
int xhci_post_configure_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                     uint8_t slot_id, uint64_t input_ctx_phys);

/* Endpoint recovery. Reset Endpoint clears a halt; Set TR Dequeue tells the
 * controller where to pick the ring up again. Neither belongs to enumeration,
 * and their completions must not be mistaken for one of its steps. */
int xhci_post_reset_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint8_t dci);

/*
 * Stop an endpoint that is RUNNING, because the transfer on it is not wanted
 * any more.
 *
 * xHCI 1.2 Section 4.6.9. The controller stops the endpoint and posts a
 * Transfer Event saying where it had got to, so the transfer leaves the ring
 * instead of being forgotten while the controller still owns it.
 *
 * ‼ THIS IS NOT Reset Endpoint, AND THE DIFFERENCE IS A MACHINE.
 *
 * Reset Endpoint (Section 4.6.8) is defined for a HALTED endpoint and for
 * nothing else. An endpoint whose device is merely SLOW is Running, so the
 * controller refuses the reset — and says so: `Reset Endpoint on slot 2 was
 * refused: Context State Error`, once per attempt, measured. A refused command
 * is a repair that did not happen, and the driver went on as though it had.
 *
 * Declared with the others and, until now, never posted by anything: the TRB
 * type has been defined in xhci_trb.h and named in the command table since the
 * driver was written.
 */
int xhci_post_stop_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                uint8_t slot_id, uint8_t dci);
int xhci_post_set_tr_dequeue_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint8_t dci,
                                 uint64_t dequeue_ptr_with_dcs);

/* Wait until this controller has nothing outstanding, draining its event ring
 * while doing so. A class driver that has just asked the controller to unhalt
 * one of its endpoints has to know that happened before it uses the endpoint
 * again. Other controllers are none of its business. */
int xhci_command_wait_idle(xhci_controller_t* ctrl, uint32_t timeout_ms);

/* True while a command asked on behalf of this device is still unanswered.
 * Whoever is about to hand the device's pages back to the allocator has to
 * know the controller has finished reading them. */
bool xhci_command_pending_for(xhci_controller_t* ctrl,
                              const xhci_device_slot_t* slot);

/* How many commands this controller has outstanding, and — for the one that
 * has been waiting longest — what it was and how long it has been waiting.
 * For the failure report, which has to be readable on its own. */
unsigned xhci_command_outstanding(xhci_controller_t* ctrl);
bool xhci_command_oldest_for(xhci_controller_t* ctrl,
                             const xhci_device_slot_t* slot,
                             uint8_t* out_trb_type, uint32_t* out_age_ms);

void xhci_handle_command_completion(xhci_controller_t* ctrl, xhci_trb_t* event);

/*
 * Has this controller answered anything lately, and ring the doorbell once if
 * it has not. Cheap and non-blocking, which is what lets it run from the timer
 * tick: when the ladder runs out it does not take the ring back, it ASKS for
 * the ring to be taken back and leaves.
 */
void xhci_check_command_timeouts(xhci_controller_t* ctrl);

/*
 * Take the command ring back, if the watchdog above has asked for it.
 *
 * Section 4.6.1.2: software sets CRCR.CA and waits for the controller to stop
 * the ring, which it is allowed five seconds to do. That wait is why this is
 * separate from the watchdog that decides it is needed — the watchdog runs in
 * the timer interrupt and this must not. Does nothing, cheaply, when nothing
 * has been asked for.
 */
void xhci_command_abort_if_wanted(xhci_controller_t* ctrl);

#endif
