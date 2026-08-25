#include "xhci_command.h"
#include "xhci_interrupt.h"
#include "xhci_enumeration.h"
#include "xhci_device.h"
#include "xhci_port.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"

const char* xhci_completion_name(uint8_t code)
{
    switch (code) {
        case TRB_COMPLETION_SUCCESS:          return "Success";
        case TRB_COMPLETION_DATA_BUFFER_ERR:  return "Data Buffer Error";
        case TRB_COMPLETION_BABBLE:           return "Babble Detected";
        case TRB_COMPLETION_USB_TRANS_ERR:    return "USB Transaction Error";
        case TRB_COMPLETION_TRB_ERROR:        return "TRB Error";
        case TRB_COMPLETION_STALL:            return "Stall";
        case TRB_COMPLETION_RESOURCE_ERR:     return "Resource Error";
        case TRB_COMPLETION_BANDWIDTH_ERR:    return "Bandwidth Error";
        case TRB_COMPLETION_NO_SLOTS:         return "No Slots Available";
        case TRB_COMPLETION_SLOT_NOT_ENABLED: return "Slot Not Enabled";
        case TRB_COMPLETION_EP_NOT_ENABLED:   return "Endpoint Not Enabled";
        case TRB_COMPLETION_SHORT_PKT:        return "Short Packet";
        case TRB_COMPLETION_RING_UNDERRUN:    return "Ring Underrun";
        case TRB_COMPLETION_RING_OVERRUN:     return "Ring Overrun";
        case TRB_COMPLETION_PARAMETER_ERR:    return "Parameter Error";
        case TRB_COMPLETION_BANDWIDTH_OVER:   return "Bandwidth Overrun";
        case TRB_COMPLETION_CONTEXT_STATE:    return "Context State Error";
        case TRB_COMPLETION_NO_PING_RESPONSE: return "No Ping Response";
        case TRB_COMPLETION_EVENT_RING_FULL:  return "Event Ring Full";
        case TRB_COMPLETION_INCOMPATIBLE_DEV: return "Incompatible Device";
        case TRB_COMPLETION_MISSED_SERVICE:   return "Missed Service Error";
        case TRB_COMPLETION_CMD_RING_STOPPED: return "Command Ring Stopped";
        case TRB_COMPLETION_COMMAND_ABORTED:  return "Command Aborted";
        case TRB_COMPLETION_STOPPED:          return "Stopped";
        case TRB_COMPLETION_STOPPED_LENGTH:   return "Stopped - Length Invalid";
        case TRB_COMPLETION_SPLIT_TRANS_ERR:  return "Split Transaction Error";
        default:                              return "an unnamed code";
    }
}

const char* xhci_command_name(uint8_t trb_type)
{
    switch (trb_type) {
        case TRB_TYPE_ENABLE_SLOT:        return "Enable Slot";
        case TRB_TYPE_DISABLE_SLOT:       return "Disable Slot";
        case TRB_TYPE_ADDRESS_DEVICE:     return "Address Device";
        case TRB_TYPE_CONFIGURE_ENDPOINT: return "Configure Endpoint";
        case TRB_TYPE_EVALUATE_CONTEXT:   return "Evaluate Context";
        case TRB_TYPE_RESET_ENDPOINT:     return "Reset Endpoint";
        case TRB_TYPE_STOP_ENDPOINT:      return "Stop Endpoint";
        case TRB_TYPE_SET_TR_DEQUEUE:     return "Set TR Dequeue Pointer";
        case TRB_TYPE_NO_OP_CMD:          return "No Op";
        default:                          return "an unnamed command";
    }
}

void xhci_command_init(xhci_controller_t* ctrl)
{
    if (!ctrl) {
        return;
    }
    spinlock_init(&ctrl->pending_lock);
    memset(ctrl->pending_cmds, 0, sizeof(ctrl->pending_cmds));
}

/*
 * Which command a completion is answering.
 *
 * The Command Completion Event carries the physical address of the Command TRB
 * (xHCI 1.2 Section 6.4.2.2), and that TRB lives at a known offset in a ring
 * this controller owns. So the answer is arithmetic, and it is exact: an
 * address outside this ring is not this controller's command, full stop.
 *
 * What it replaces was a linear search of a table shared by every controller
 * on the machine, keyed on an address, followed by a SECOND search of the
 * device table trying to work out which device the answer belonged to.
 */
static int cmd_index_for(const xhci_controller_t* ctrl, uint64_t trb_phys)
{
    uint64_t base = ctrl->command_ring.trbs_phys;
    if (base == 0 || trb_phys < base) {
        return -1;
    }
    uint64_t offset = trb_phys - base;
    if (offset % sizeof(xhci_trb_t)) {
        return -1;
    }
    uint64_t index = offset / sizeof(xhci_trb_t);
    if (index >= ctrl->command_ring.num_trbs) {
        return -1;
    }
    return (int)index;
}

static int post_command(xhci_controller_t* ctrl, xhci_trb_t* trb,
                        uint8_t slot_id, xhci_device_slot_t* owner)
{
    uint8_t trb_type = (uint8_t)TRB_GET_TYPE(trb->control);

    spin_lock(&ctrl->pending_lock);

    uint64_t trb_phys = xhci_ring_enqueue(&ctrl->command_ring, trb);
    if (trb_phys == 0) {
        spin_unlock(&ctrl->pending_lock);
        kprintf("[xHCI %s] the command ring is full — %s for slot %u was not "
                "sent\n", ctrl->name, xhci_command_name(trb_type), slot_id);
        return -1;
    }

    int index = cmd_index_for(ctrl, trb_phys);
    if (index < 0) {
        /* The ring handed back an address that is not in the ring. Nothing
         * sensible follows from that, and it must not be silent. */
        spin_unlock(&ctrl->pending_lock);
        kprintf("[xHCI %s] the command ring produced an address outside "
                "itself (0x%llx)\n", ctrl->name,
                (unsigned long long)trb_phys);
        return -1;
    }

    if (ctrl->pending_cmds[index].state == XHCI_CMD_POSTED) {
        /*
         * The ring has come all the way round onto a command the controller
         * has still not answered. That is the controller being two hundred and
         * fifty-five commands behind, which is not a queue — it is a stopped
         * controller, and overwriting the TRB would hide it.
         */
        spin_unlock(&ctrl->pending_lock);
        kprintf("[xHCI %s] the command ring wrapped onto %s, still unanswered "
                "— the controller has stopped taking commands\n", ctrl->name,
                xhci_command_name(ctrl->pending_cmds[index].trb_type));
        return -1;
    }

    /* The clock the stuck-ring test runs on starts when the ring stops being
     * empty. Otherwise a controller idle for a minute looks overdue the
     * instant it is given its first command. */
    if (ctrl->last_cmd_answer == 0) {
        ctrl->last_cmd_answer = rdtsc();
    }

    ctrl->pending_cmds[index].owner       = owner;
    ctrl->pending_cmds[index].owner_epoch = xhci_slot_epoch(owner);
    ctrl->pending_cmds[index].posted_at   = rdtsc();
    ctrl->pending_cmds[index].slot_id     = slot_id;
    ctrl->pending_cmds[index].trb_type    = trb_type;
    ctrl->pending_cmds[index].state       = XHCI_CMD_POSTED;

    spin_unlock(&ctrl->pending_lock);

    __sync_synchronize();
    ctrl->doorbells->doorbells[0].doorbell = 0;

    return 0;
}

/*
 * Enable Slot names the kind of slot it wants (xHCI 1.2 Section 6.4.3.2, bits
 * 20:16), and the kind comes from the Supported Protocol capability that owns
 * the port — which this driver already reads and used to throw away. Zero is
 * right for USB 2 and USB 3 on every controller seen so far, and is exactly
 * the sort of "right everywhere I looked" that stops being right on somebody
 * else's machine.
 */
int xhci_post_enable_slot_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                              uint8_t slot_type)
{
    if (!ctrl || !ctrl->running) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_ENABLE_SLOT) |
                  (((uint32_t)slot_type & 0x1Fu) << 16);

    return post_command(ctrl, &trb, 0, owner);
}

int xhci_post_disable_slot_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                               uint8_t slot_id)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_DISABLE_SLOT) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

int xhci_post_address_device_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint64_t input_ctx_phys)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = input_ctx_phys;
    trb.control = TRB_SET_TYPE(TRB_TYPE_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

int xhci_post_configure_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                     uint8_t slot_id, uint64_t input_ctx_phys)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = input_ctx_phys;
    trb.control = TRB_SET_TYPE(TRB_TYPE_CONFIGURE_ENDPOINT) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

int xhci_post_evaluate_context_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                   uint8_t slot_id, uint64_t input_ctx_phys)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = input_ctx_phys;
    trb.control = TRB_SET_TYPE(TRB_TYPE_EVALUATE_CONTEXT) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

int xhci_post_reset_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint8_t dci)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots ||
        dci == 0 || dci > 31) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_RESET_ENDPOINT) |
                  ((uint32_t)dci << 16) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

int xhci_post_set_tr_dequeue_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint8_t dci,
                                 uint64_t dequeue_ptr_with_dcs)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots ||
        dci == 0 || dci > 31) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = dequeue_ptr_with_dcs;
    trb.control = TRB_SET_TYPE(TRB_TYPE_SET_TR_DEQUEUE) |
                  ((uint32_t)dci << 16) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

/* Which commands are steps of enumeration.
 *
 * Every command completion used to be handed to the enumeration state machine,
 * which was harmless only while enumeration was the sole thing issuing
 * commands. It is not any more: an endpoint reset on a device that is already
 * running would arrive as an unexplained completion for a slot in a settled
 * state, and a state machine driven by events it did not ask for is a state
 * machine that will eventually take the wrong branch. */
static bool cmd_is_enumeration_step(uint8_t trb_type,
                                    const xhci_device_slot_t* owner)
{
    /*
     * Once a device is configured, enumeration is over, and every command
     * posted against it belongs to whoever is driving it.
     *
     * A hub sets its own Hub bit with a Configure Endpoint after it has come
     * up; a disk clears a halted bulk pipe with a Reset Endpoint and a Set TR
     * Dequeue every time it is refused something. Handing those answers to the
     * state machine put "slot 1 answered a step it was not on" on the screen
     * for each of them — a line whose whole purpose is to report an answer
     * arriving before the question was written down, printed for something
     * that is neither, on the one screen that has to stay readable.
     */
    if (!owner || __atomic_load_n(&owner->state, __ATOMIC_ACQUIRE) ==
                      ENUM_STATE_CONFIGURED) {
        return false;
    }

    switch (trb_type) {
        case TRB_TYPE_ENABLE_SLOT:
        case TRB_TYPE_ADDRESS_DEVICE:
        case TRB_TYPE_CONFIGURE_ENDPOINT:
        case TRB_TYPE_EVALUATE_CONTEXT:
        case TRB_TYPE_RESET_ENDPOINT:
        case TRB_TYPE_SET_TR_DEQUEUE:
            return true;
        default:
            return false;
    }
}

void xhci_handle_command_completion(xhci_controller_t* ctrl, xhci_trb_t* event)
{
    if (!ctrl || !event) {
        return;
    }

    uint64_t trb_phys        = event->parameter;
    uint8_t  completion_code = (event->status >> 24) & 0xFF;
    uint8_t  event_slot_id   = (event->control >> 24) & 0xFF;

    int index = cmd_index_for(ctrl, trb_phys);
    if (index < 0) {
        /*
         * An answer to a question this controller was never asked.
         *
         * Said out loud, not into a debug build: this is a completion that
         * advances nothing, so whatever was waiting on it waits until a
         * watchdog gives up — a device that never enumerates, with no line in
         * the log to say why.
         */
        kprintf("[xHCI %s] a completion names TRB 0x%llx, which is not on this "
                "controller's command ring (slot %u, %s)\n",
                ctrl->name, (unsigned long long)trb_phys, event_slot_id,
                xhci_completion_name(completion_code));
        return;
    }

    /* That TRB and everything queued ahead of it are done with — the command
     * ring is executed strictly in order (Section 4.6.1), which is the same
     * property that makes one unanswered command block the rest of it. */
    xhci_ring_reclaim_to(&ctrl->command_ring, trb_phys);

    spin_lock(&ctrl->pending_lock);
    xhci_pending_cmd_t entry = ctrl->pending_cmds[index];
    if (entry.state != XHCI_CMD_POSTED) {
        spin_unlock(&ctrl->pending_lock);
        /* Command Ring Stopped is the controller acknowledging an abort, and
         * it names the TRB it had reached rather than one still outstanding.
         * Everything else here is a second answer to a settled question. */
        if (completion_code != TRB_COMPLETION_CMD_RING_STOPPED) {
            kprintf("[xHCI %s] a second answer arrived for a command already "
                    "settled (slot %u, %s)\n", ctrl->name, event_slot_id,
                    xhci_completion_name(completion_code));
        }
        return;
    }
    /* Released before the state machine runs. The controller has finished with
     * this TRB and with the input context it named, which is exactly what
     * whoever is trying to take the device down needs to know. */
    ctrl->pending_cmds[index].state = XHCI_CMD_FREE;
    ctrl->pending_cmds[index].owner = NULL;
    ctrl->last_cmd_answer = rdtsc();
    ctrl->cmd_nudges      = 0;
    spin_unlock(&ctrl->pending_lock);

    /*
     * How long that took, when it took long enough to matter.
     *
     * Silent for the ordinary case — a command answered in microseconds is not
     * news — and the one fact that separates "this controller is stuck" from
     * "this controller is slower than somebody's timeout" on a machine that
     * can only be read by photographing its screen.
     */
    uint32_t took = (uint32_t)cpu_tsc_to_ms(rdtsc() - entry.posted_at);
    if (took >= XHCI_CMD_SLOW_MS) {
        kprintf("[xHCI %s] %s on slot %u took %u ms\n", ctrl->name,
                xhci_command_name(entry.trb_type), event_slot_id, took);
    }

    /*
     * The answer goes to the device that asked, and to no other.
     *
     * The slot is named by the command, not looked for afterwards. Enable Slot
     * is the reason that matters: it carries no slot id on the way out, so the
     * old code answered it into "the first device in the table that looks like
     * it is waiting for one" — which is a guess, and with four devices coming
     * up at once on a live board it is four guesses in a row.
     */
    if (!xhci_slot_still_is(entry.owner, entry.owner_epoch)) {
        /* The device left while its command was in flight. Not an error — an
         * unplug during enumeration is an ordinary thing — but it is worth one
         * line, because it is also what a mis-delivered answer looks like. */
        kprintf("[xHCI %s] %s answered for slot %u after the device had gone "
                "(%s)\n", ctrl->name, xhci_command_name(entry.trb_type),
                event_slot_id, xhci_completion_name(completion_code));
        return;
    }

    if (completion_code != TRB_COMPLETION_SUCCESS) {
        /*
         * Named by the command, not by the event.
         *
         * A Command Ring Stopped event carries the slot id of wherever the
         * controller had got to, which is not the slot of the command being
         * answered — measured: two commands belonging to ports 10 and 12 were
         * both reported against slot 3, which is a third device that had
         * nothing to do with either of them.
         */
        kprintf("[xHCI %s] %s on slot %u was refused: %s (code %u)\n",
                ctrl->name, xhci_command_name(entry.trb_type),
                entry.slot_id ? entry.slot_id : event_slot_id,
                xhci_completion_name(completion_code), completion_code);
    }

    if (cmd_is_enumeration_step(entry.trb_type, entry.owner)) {
        xhci_enum_advance_state(ctrl, entry.owner, event_slot_id,
                                completion_code);
    }
}

int xhci_command_wait_idle(xhci_controller_t* ctrl, uint32_t timeout_ms)
{
    if (!ctrl) {
        return -1;
    }

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);

    for (;;) {
        xhci_process_events();

        if (xhci_command_outstanding(ctrl) == 0) {
            return 0;
        }
        if ((int64_t)(rdtsc() - deadline) >= 0) {
            return -1;
        }
        cpu_pause();
    }
}

bool xhci_command_pending_for(xhci_controller_t* ctrl,
                              const xhci_device_slot_t* slot)
{
    if (!ctrl || !slot) {
        return false;
    }

    spin_lock(&ctrl->pending_lock);
    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        if (ctrl->pending_cmds[i].state == XHCI_CMD_POSTED &&
            ctrl->pending_cmds[i].owner == slot) {
            spin_unlock(&ctrl->pending_lock);
            return true;
        }
    }
    spin_unlock(&ctrl->pending_lock);
    return false;
}

unsigned xhci_command_outstanding(xhci_controller_t* ctrl)
{
    if (!ctrl) {
        return 0;
    }

    unsigned count = 0;
    spin_lock(&ctrl->pending_lock);
    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        if (ctrl->pending_cmds[i].state == XHCI_CMD_POSTED) {
            count++;
        }
    }
    spin_unlock(&ctrl->pending_lock);
    return count;
}

bool xhci_command_oldest_for(xhci_controller_t* ctrl,
                             const xhci_device_slot_t* slot,
                             uint8_t* out_trb_type, uint32_t* out_age_ms)
{
    if (!ctrl || !slot) {
        return false;
    }

    uint64_t now = rdtsc();
    uint64_t oldest = 0;
    uint8_t  type = 0;
    bool     found = false;

    spin_lock(&ctrl->pending_lock);
    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        if (ctrl->pending_cmds[i].state != XHCI_CMD_POSTED ||
            ctrl->pending_cmds[i].owner != slot) {
            continue;
        }
        uint64_t age = now - ctrl->pending_cmds[i].posted_at;
        if (!found || age > oldest) {
            oldest = age;
            type   = ctrl->pending_cmds[i].trb_type;
            found  = true;
        }
    }
    spin_unlock(&ctrl->pending_lock);

    if (found) {
        if (out_trb_type) *out_trb_type = type;
        if (out_age_ms)   *out_age_ms   = (uint32_t)cpu_tsc_to_ms(oldest);
    }
    return found;
}

/*
 * Abandoning a command the controller will not answer.
 *
 * There is exactly one way to do this and it is not "stop remembering it".
 * xHCI 1.2 Section 4.6.1.2: software sets CRCR.CA, waits for the controller to
 * stop the ring, and re-publishes the ring pointer. Until it has stopped, the
 * controller is entitled to read that TRB and everything the TRB points at —
 * and what an Address Device TRB points at is an Input Context that the device
 * teardown is about to hand back to the page allocator. Forgetting a command
 * instead of aborting it therefore does not lose a device; it lets the
 * controller DMA into somebody else's memory some time later.
 *
 * A controller that has stopped answering commands has stopped being useful,
 * so every device with a command in flight is released here. That is honest:
 * they were going nowhere.
 */
static void xhci_command_ring_abort(xhci_controller_t* ctrl)
{
    volatile uint32_t* crcr_lo = (volatile uint32_t*)&ctrl->op_regs->crcr;

    /* The pointer half of CRCR reads as zero, so this writes the abort bit and
     * nothing else — which is what the specification asks for. */
    ctrl->op_regs->crcr = ctrl->op_regs->crcr | XHCI_CRCR_CA;

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(XHCI_CMD_ABORT_TIMEOUT_MS);
    bool stopped = false;
    while ((int64_t)(rdtsc() - deadline) < 0) {
        /* The abort answers with events; draining is what lets the Command
         * Ring Stopped event through and keeps the ring from filling. */
        xhci_process_events();
        if ((*crcr_lo & XHCI_CRCR_CRR) == 0) {
            stopped = true;
            break;
        }
        cpu_pause();
    }

    if (!stopped) {
        kprintf("[xHCI %s] the command ring would not stop when asked to "
                "(CRCR=0x%08x USBSTS=0x%08x) — this controller is out of "
                "service\n", ctrl->name, *crcr_lo, ctrl->op_regs->usbsts);
        ctrl->running     = false;
        ctrl->error_state = true;
    }

    /* Everything that was in flight is gone with the ring. Release the devices
     * that were waiting on it before the ring is re-published, so that nothing
     * is left believing an answer is still coming. */
    spin_lock(&ctrl->pending_lock);
    xhci_device_slot_t* orphans[XHCI_CMD_RING_TRBS];
    unsigned orphan_count = 0;
    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        if (ctrl->pending_cmds[i].state != XHCI_CMD_POSTED) {
            continue;
        }
        xhci_device_slot_t* owner = ctrl->pending_cmds[i].owner;
        ctrl->pending_cmds[i].state = XHCI_CMD_FREE;
        ctrl->pending_cmds[i].owner = NULL;
        if (owner && orphan_count < XHCI_CMD_RING_TRBS) {
            bool already = false;
            for (unsigned k = 0; k < orphan_count; k++) {
                if (orphans[k] == owner) { already = true; break; }
            }
            if (!already) {
                orphans[orphan_count++] = owner;
            }
        }
    }

    /* Start the ring over. The controller's own dequeue pointer is reloaded
     * from CRCR, so software's enqueue position and cycle state have to go
     * back to where the controller will be looking. */
    /* Nothing is owed any more, so the stuck-ring clock starts fresh with
     * whatever is posted next rather than counting the abort against it. */
    ctrl->last_cmd_answer = 0;
    ctrl->cmd_nudges      = 0;

    ctrl->command_ring.enqueue_idx = 0;
    ctrl->command_ring.dequeue_idx = 0;
    ctrl->command_ring.cycle_state = 1;
    if (ctrl->command_ring.trbs) {
        memset(ctrl->command_ring.trbs, 0,
               (size_t)ctrl->command_ring.num_trbs * sizeof(xhci_trb_t));
        xhci_trb_t* link = &ctrl->command_ring.trbs[ctrl->command_ring.num_trbs - 1];
        link->parameter = ctrl->command_ring.trbs_phys;
        link->status    = 0;
        link->control   = TRB_SET_TYPE(TRB_TYPE_LINK) | TRB_TC;
    }
    spin_unlock(&ctrl->pending_lock);

    __sync_synchronize();
    ctrl->op_regs->crcr = ctrl->command_ring.trbs_phys | XHCI_CRCR_RCS;

    /*
     * What the CONTROLLER made of each of them, before they are taken apart.
     *
     * The Output Slot Context is written by the controller and never by
     * software, so its Slot State is the controller's own account: Disabled
     * means the Address Device never took effect, Addressed or Default mean it
     * did and the answer went missing. Those are opposite faults with opposite
     * fixes, nothing else on the machine distinguishes them, and until now the
     * one line that would have said which never printed here — the enumeration
     * watchdog would have said it, and this pass retires the slots first, so
     * the watchdog correctly skips them and the report went with them.
     */
    for (unsigned k = 0; k < orphan_count; k++) {
        xhci_device_slot_t* s = orphans[k];
        kprintf("[xHCI %s]   port %u (slot %u): controller says slot %s, "
                "address %u; PORTSC 0x%08x; the port was %s\n",
                ctrl->name, s->port_num, s->slot_id,
                xhci_slot_state_name(xhci_slot_context_state(s)),
                xhci_slot_context_address(s),
                xhci_get_port_status(ctrl, s->port_num),
                xhci_port_reset_kind_name(s->reset_kind));
    }
    xhci_hold_screen();

    for (unsigned k = 0; k < orphan_count; k++) {
        xhci_slot_retire(ctrl, orphans[k]);
    }

    kprintf("[xHCI %s] command ring restarted; %u device(s) released\n",
            ctrl->name, orphan_count);
    xhci_hold_screen();
}

/*
 * Is the command ring stuck — as opposed to merely slower than somebody hoped?
 *
 * The controller works through the ring in order, so a completion for any
 * command is proof that every command posted before it has been answered too.
 * The question is therefore about the RING, not about one command: has this
 * controller answered anything at all recently?
 *
 * Asking it the other way round — "has this command been outstanding for five
 * seconds" — cost two devices on a live board. Four Address Devices went out
 * together, the first crossed the budget, the ring was aborted, and one of the
 * commands that abort killed then completed with Success. It had not hung. It
 * was late.
 */
void xhci_check_command_timeouts(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->running) {
        return;
    }

    uint64_t now    = rdtsc();
    uint64_t budget = cpu_ms_to_tsc(XHCI_CMD_TIMEOUT_MS);

    spin_lock(&ctrl->pending_lock);

    unsigned outstanding = 0;
    uint64_t oldest_at   = now;
    uint8_t  oldest_type = 0;
    uint8_t  oldest_slot = 0;

    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        if (ctrl->pending_cmds[i].state != XHCI_CMD_POSTED) {
            continue;
        }
        outstanding++;
        if ((int64_t)(ctrl->pending_cmds[i].posted_at - oldest_at) < 0) {
            oldest_at   = ctrl->pending_cmds[i].posted_at;
            oldest_type = ctrl->pending_cmds[i].trb_type;
            oldest_slot = ctrl->pending_cmds[i].slot_id;
        }
    }

    if (outstanding == 0) {
        /* Nothing owed. Let the clock start fresh with the next command
         * rather than counting the idle time against it. */
        ctrl->last_cmd_answer = 0;
        spin_unlock(&ctrl->pending_lock);
        return;
    }

    bool stuck = (int64_t)(now - ctrl->last_cmd_answer) > (int64_t)budget;
    uint32_t silent = (uint32_t)cpu_tsc_to_ms(now - ctrl->last_cmd_answer);
    uint32_t oldest_ms = (uint32_t)cpu_tsc_to_ms(now - oldest_at);
    spin_unlock(&ctrl->pending_lock);

    if (!stuck) {
        return;
    }

    /*
     * The cheap remedy first.
     *
     * A doorbell is how software says "there is work on the ring", and a
     * controller that did not act on one is indistinguishable, from the
     * outside, from a controller that has hung. Ringing it again is one
     * register write, cannot corrupt anything, and does nothing at all if the
     * controller was simply busy. Aborting the ring destroys every command in
     * flight — measured on a live board: it killed an Address Device that then
     * completed with Success — so it is what happens after the cheap thing has
     * been tried, not instead of it.
     */
    if (ctrl->cmd_nudges < XHCI_CMD_NUDGES) {
        ctrl->cmd_nudges++;
        xhci_ring_t* er = &ctrl->event_ring;
        uint32_t at_dequeue = er->trbs ? er->trbs[er->dequeue_idx].control : 0;

        kprintf("[xHCI %s] nothing answered for %u ms with %u command(s) "
                "outstanding (oldest %s on slot %u, %u ms) — ringing the "
                "doorbell again [%u/%u]\n",
                ctrl->name, silent, outstanding,
                xhci_command_name(oldest_type), oldest_slot, oldest_ms,
                ctrl->cmd_nudges, XHCI_CMD_NUDGES);

        /*
         * And which of the two possible faults it is.
         *
         * The controller has executed commands whose completions never
         * arrived — measured on a live board, three devices reading Addressed
         * with addresses assigned while this driver was still waiting to be
         * told. That leaves exactly two possibilities and this line separates
         * them:
         *
         *   the TRB at the dequeue position carries the cycle bit we expect
         *     ⇒ the events ARE on the ring and nobody is reading them;
         *   it does not
         *     ⇒ the controller executed the command and posted nothing.
         *
         * The skipped-drain count answers the same question from the other
         * end: a drain that keeps finding somebody else already draining is a
         * ring nobody is actually finishing.
         */
        kprintf("[xHCI %s]   CRCR=0x%08x USBSTS=0x%08x ERDP=0x%08x "
                "IMAN=0x%08x | command ring at %u | event ring at %u "
                "expecting cycle %u, TRB there 0x%08x | %u drain(s) skipped\n",
                ctrl->name,
                (uint32_t)ctrl->op_regs->crcr, ctrl->op_regs->usbsts,
                (uint32_t)ctrl->runtime_regs->interrupters[0].erdp,
                ctrl->runtime_regs->interrupters[0].iman,
                ctrl->command_ring.enqueue_idx,
                er->dequeue_idx, er->cycle_state, at_dequeue,
                __atomic_load_n(&ctrl->drain_skips, __ATOMIC_RELAXED));
        xhci_hold_screen();

        __sync_synchronize();
        ctrl->doorbells->doorbells[0].doorbell = 0;

        /* Give it the same budget again to answer the nudge. */
        ctrl->last_cmd_answer = now;
        return;
    }

    /*
     * It has been told three times and answered nothing. That is a stopped
     * controller, and the ring has to be taken away from it.
     */
    kprintf("[xHCI %s] still nothing after %u doorbell(s) — the command ring "
            "has stopped and is being taken back\n",
            ctrl->name, XHCI_CMD_NUDGES);

    ctrl->cmd_nudges = 0;
    xhci_command_ring_abort(ctrl);
}
