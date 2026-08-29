#ifndef XHCI_ENDPOINT_H
#define XHCI_ENDPOINT_H

#include "ktypes.h"
#include "xhci.h"
#include "xhci_rings.h"
#include "usb_descriptors.h"

/* By pointer only: an endpoint may carry somewhere for its answer to go, and
 * the queue that carries it there belongs to the storage deck. Including that
 * header here would make every USB translation unit depend on the deck. */
struct StorageCompletion;

/* Endpoint types as the controller numbers them (xHCI Table 6-9). The value
 * goes straight into the endpoint context, so these are the controller's
 * numbers and not a private encoding. */
#define XHCI_EP_TYPE_INVALID        0
#define XHCI_EP_TYPE_ISOCH_OUT      1
#define XHCI_EP_TYPE_BULK_OUT       2
#define XHCI_EP_TYPE_INTERRUPT_OUT  3
#define XHCI_EP_TYPE_CONTROL        4
#define XHCI_EP_TYPE_ISOCH_IN       5
#define XHCI_EP_TYPE_BULK_IN        6
#define XHCI_EP_TYPE_INTERRUPT_IN   7

/* A device context has room for 31 endpoints beyond the slot, and the Device
 * Context Index that names one is five bits wide. 32 entries is therefore the
 * whole of what the hardware can address, not a limit chosen here. */
#define XHCI_MAX_DCI 31
#define XHCI_DCI_COUNT (XHCI_MAX_DCI + 1)

/* Where a transfer stands. One transfer may be in flight per endpoint, which
 * is not a simplification but a description: Bulk-Only Transport is a strictly
 * serial conversation (command, then data, then status), and an interrupt
 * endpoint re-arms one report at a time. An endpoint that needs a deeper queue
 * needs a different structure, and saying so here is cheaper than pretending
 * this one has it. */
#define XHCI_XFER_IDLE      0
#define XHCI_XFER_IN_FLIGHT 1
#define XHCI_XFER_DONE      2

typedef struct xhci_endpoint {
    xhci_ring_t* ring;
    uint64_t     ring_page_phys;    /* the page the ring structure itself is in */

    /* Some endpoints own a buffer for their whole life — an interrupt endpoint
     * re-armed with the same report page. Bulk endpoints are handed one per
     * transfer by whoever asked for it, and leave these zero. */
    void*        buffer_virt;
    uint64_t     buffer_phys;
    uint32_t     buffer_bytes;

    uint16_t     max_packet;
    uint8_t      addr;              /* bEndpointAddress as the device stated it */
    uint8_t      type;              /* XHCI_EP_TYPE_* */
    uint8_t      interval;          /* bInterval as the device stated it */

    /* How many packets may go back to back, one less than the count, and for
     * a periodic endpoint how many bytes that comes to per service interval.
     * Both as the device stated them — from the SuperSpeed companion where
     * there is one, from the packet-size field where there is not. */
    uint8_t      max_burst;
    uint8_t      mult;
    uint16_t     bytes_per_interval;

    bool         active;

    /* The transfer currently in flight on this endpoint, and what became of
     * it. Written by the event handler, read by whoever is waiting. */
    volatile uint64_t xfer_trb_phys;
    volatile uint8_t  xfer_state;
    volatile uint8_t  xfer_code;
    volatile uint32_t xfer_residual;   /* bytes the device did NOT transfer */

    /*
     * Where this endpoint's answer goes when it arrives, for a caller that did
     * not stay to hear it.
     *
     * NULL means the old arrangement: the answer is recorded on the endpoint
     * and whoever asked is expected to be looking at it. Set, it means the
     * answer is handed to a K-Core — the completion is posted the instant the
     * event is drained, from wherever that drain happens to be, and the work
     * that follows from it runs in a context where waiting is allowed.
     *
     * The node lives inside the job it belongs to, so posting it needs no
     * allocation and cannot fail for want of a slot: a transfer completion
     * that went missing would leave its caller waiting for ever.
     */
    struct StorageCompletion* xfer_done;
} xhci_endpoint_t;

/* Device Context Index of an endpoint address: the endpoint number doubled,
 * plus one when the direction is IN. EP0 is DCI 1 and is bidirectional. */
static inline uint8_t xhci_dci_of(uint8_t endpoint_addr) {
    uint8_t num = endpoint_addr & 0x0F;
    if (num == 0) return 1;
    return (uint8_t)(num * 2 + ((endpoint_addr & 0x80) ? 1 : 0));
}

/* Endpoint type from a bmAttributes/direction pair, as the descriptor gives
 * them. Returns XHCI_EP_TYPE_INVALID for a control endpoint that is not EP0,
 * which this driver does not configure. */
uint8_t xhci_ep_type_of(uint8_t attributes, uint8_t endpoint_addr);

/* Give a slot its endpoint table. Called once, when the slot is enabled. */
int  xhci_ep_table_alloc(xhci_device_slot_t* slot);
void xhci_ep_table_free(xhci_device_slot_t* slot);

/* Prepare one endpoint: transfer ring, optional persistent buffer, and the
 * bookkeeping. Does not talk to the controller — xhci_ep_configure does that
 * for every prepared endpoint at once, because Configure Endpoint takes them
 * together and a command per endpoint would be three round trips where the
 * hardware asked for one. */
uint32_t xhci_input_ctx_pages(xhci_controller_t* ctrl);

/* Checks the two endpoint-context fields whose isochronous half no boot ever
 * computes, against xHCI 1.2 Table 6-45 and Section 6.2.3.5. Says what it
 * found; run once when the first controller comes up. */
void xhci_ep_context_self_test(void);

int  xhci_ep_prepare(xhci_device_slot_t* slot, uint8_t dci, uint8_t type,
                     const usb_endpoint_info_t* info, uint32_t buffer_bytes);

/* Post Configure Endpoint for every prepared endpoint that the controller does
 * not yet know about. The slot advances on the command completion. */
int  xhci_ep_configure(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

/* Submit one transfer and return without waiting. The answer is recorded on
 * the endpoint for whoever is watching it. */
int  xhci_ep_submit(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                    uint8_t dci, uint64_t buffer_phys, uint32_t length);

/*
 * The same, for a caller that is not going to stand and watch.
 *
 * `done` is posted to a K-Core when the transfer is answered, and the
 * continuation inside it runs there — which is the difference that matters,
 * because the drain that receives the answer may be an interrupt handler and
 * the work that follows from a completed transfer is usually another transfer.
 *
 * The node is registered BEFORE the doorbell is rung. It has to be: the
 * controller may answer inside that write, and an answer that arrives before
 * anybody has said where to send it is an answer thrown away. That rule was
 * learned twice in this driver already (34bcee6, ac0fcfd).
 */
int  xhci_ep_submit_async(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                          uint8_t dci, uint64_t buffer_phys, uint32_t length,
                          struct StorageCompletion* done);

/* Wait for the transfer submitted above.
 *
 * The wait drains the event ring itself rather than trusting that an interrupt
 * will arrive to do it — the same code has to work when this is called with
 * interrupts disabled, on a controller with no usable interrupt at all, and
 * before any of the machinery that would otherwise deliver one exists. Draining
 * is safe from here because the event ring is under a lock the interrupt
 * handler takes too.
 *
 * Reports the RESIDUAL — what the device did not transfer — because that is
 * what the event carries. xhci_ep_transfer turns it into the number everyone
 * actually wants.
 *
 * Returns the completion code, or a negative value on timeout. */
int  xhci_ep_wait(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                  uint8_t dci, uint32_t timeout_ms, uint32_t* out_residual);

/* Submit and wait, which is what every caller that is not an interrupt
 * endpoint actually wants. */
int  xhci_ep_transfer(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                      uint8_t dci, uint64_t buffer_phys, uint32_t length,
                      uint32_t timeout_ms, uint32_t* out_transferred);

/*
 * Take the answer off an endpoint and leave it ready for the next transfer.
 *
 * What xhci_ep_wait does at the end of its wait, for a caller that did not
 * wait: the completion code and the residual are read out and the endpoint
 * goes back to idle. Returns false when there is no answer there yet.
 */
bool xhci_ep_take_result(xhci_device_slot_t* slot, uint8_t dci,
                         uint8_t* out_code, uint32_t* out_residual);

/*
 * What the CONTROLLER says an endpoint is doing, as opposed to what this
 * driver last asked it to do.
 *
 * The Output Endpoint Context's EP State (xHCI 1.2 Section 6.2.3, dword0 bits
 * 2:0) is written by the controller and never by software. It is the one thing
 * that separates an endpoint the device HALTED from one that is simply still
 * Running because the device is slow — and those two need opposite commands,
 * which is why guessing cost this driver every recovery it ever attempted on a
 * slow medium.
 *
 * ‼ Read with the controller's own context size, never through
 * xhci_device_context_t: that structure is declared for 32-byte contexts, and
 * on a controller with CSZ set every endpoint in it is at the wrong offset.
 */
#define XHCI_EP_STATE_DISABLED 0
#define XHCI_EP_STATE_RUNNING  1
#define XHCI_EP_STATE_HALTED   2
#define XHCI_EP_STATE_STOPPED  3
#define XHCI_EP_STATE_ERROR    4

uint8_t     xhci_ep_context_state(xhci_controller_t* ctrl,
                                  const xhci_device_slot_t* slot, uint8_t dci);
const char* xhci_ep_state_name(uint8_t state);

/* Clear a halted endpoint and put its dequeue pointer where software is.
 *
 * Asks the controller whether the endpoint IS halted before it says so. It used
 * to assert it — the line it prints is "endpoint N halted — clearing it" — and
 * on a merely slow endpoint that sentence was false and the two commands that
 * followed it were both refused with Context State Error. */
void xhci_ep_recover(xhci_controller_t* ctrl, xhci_device_slot_t* slot, uint8_t dci);

/*
 * Nobody is coming for this answer.
 *
 * The opposite door to xhci_ep_take_result, and until now there was none: a
 * caller that gave up on a transfer simply stopped looking, and the endpoint
 * was never told. Two things then stayed true for the rest of the boot.
 *
 * `xfer_state` stayed IN_FLIGHT, so every later submission on that endpoint was
 * refused with "one transfer at a time" — ONE slow read and the disk was never
 * readable again. Measured: a medium held to 512 bytes a second for a single
 * block, and afterwards, at full speed, the machine could not run a program off
 * it and did not answer a keystroke.
 *
 * And `xfer_done` went on pointing into the job that was about to be freed, so
 * a device answering LATE — which is exactly what a flash drive doing its own
 * garbage collection does — handed a K-Core a freed pointer to run.
 *
 * The controller is told with the command that means it: Stop Endpoint for one
 * that is Running, Reset Endpoint for one the device halted. Then the dequeue
 * pointer is moved past what was abandoned, so the next transfer starts where
 * software stands.
 *
 * Does nothing, cheaply, when the endpoint has nothing outstanding — so it is
 * safe to say at the end of any transfer, whether or not one was given up on.
 *
 * ‼ For a device that has finished coming up. The commands it may post are
 * ones the enumeration state machine claims as its own steps while a slot is
 * still being brought up (xhci_command.c, cmd_is_enumeration_step), so saying
 * this mid-enumeration would answer a question nobody asked. Everything that
 * reaches here today holds the slot open, which means CONFIGURED.
 */
void xhci_ep_abandon(xhci_controller_t* ctrl, xhci_device_slot_t* slot, uint8_t dci);

/* Record a completion against the endpoint it belongs to. Called from the
 * transfer-event handler. */
void xhci_ep_complete(xhci_device_slot_t* slot, uint8_t dci,
                      uint8_t completion_code, uint32_t residual,
                      uint64_t trb_phys);

#endif
