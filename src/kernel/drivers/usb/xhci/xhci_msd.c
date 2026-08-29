#include "xhci_msd.h"
#include "boardroom.h"
#include "xhci_endpoint.h"
#include "xhci_enumeration.h"
#include "xhci_port.h"
#include "xhci_transfer.h"
#include "xhci_command.h"
#include "xhci_interrupt.h"
#include "xhci_trb.h"
#include "usb_common.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "storage_completion.h"

/* ── Bulk-Only Transport wire format (USB MSC BOT 1.0) ──────────────────── */

#define CBW_SIGNATURE 0x43425355u       /* "USBC" */
#define CSW_SIGNATURE 0x53425355u       /* "USBS" */

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;                     /* 0x80 = device to host */
    uint8_t  lun;                       /* bits 3:0 */
    uint8_t  cb_length;                 /* bits 4:0, 1..16 */
    uint8_t  cb[16];
} __attribute__((packed)) MsdCbw;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;                    /* 0 passed, 1 failed, 2 phase error */
} __attribute__((packed)) MsdCsw;

_Static_assert(sizeof(MsdCbw) == 31, "a command wrapper is 31 bytes");
_Static_assert(sizeof(MsdCsw) == 13, "a status wrapper is 13 bytes");

#define CSW_PASSED      0
#define CSW_FAILED      1
#define CSW_PHASE_ERROR 2

/* Class requests, on the control pipe. */
#define MSD_REQ_GET_MAX_LUN 0xFE
#define MSD_REQ_BOT_RESET   0xFF

/* ── SCSI, the part of it a disk needs to answer ────────────────────────── */

#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define SCSI_WRITE_10         0x2A
#define SCSI_SYNC_CACHE_10    0x35

/*
 * The sixteen-byte forms (SBC-3).
 *
 * READ CAPACITY(10) answers with a 32-bit last block number, so it can say
 * nothing larger than two tebibytes. A device bigger than that is required to
 * answer 0xFFFFFFFF and wait to be asked again properly — and a host that
 * takes that literally decides the drive is exactly two tebibytes, hands the
 * top of it to a filesystem, and finds out later. READ CAPACITY(16) is a
 * SERVICE ACTION IN command: opcode 0x9E with the action in the low five bits
 * of byte 1.
 */
#define SCSI_SERVICE_ACTION_IN_16 0x9E
#define SCSI_SAI_READ_CAPACITY_16 0x10
#define SCSI_READ_16              0x88
#define SCSI_WRITE_16             0x8A
#define SCSI_SYNC_CACHE_16        0x91

/*
 * ‼ WHAT BOUNDS A BULK TRANSFER, AND WHY IT IS NOT A CLOCK.
 *
 * ON BULK, SILENCE MEANS THE DEVICE IS SAYING "NOT YET".
 *
 * A device that is busy answers NAK, and a NAK is not an error: it does not
 * touch the endpoint's error count, and the controller simply asks again, for
 * as long as it takes. A device that has gone, or broken, is the opposite —
 * the transaction fails, the controller retries it CErr times and then posts a
 * Transfer Event that says so. The bus reports TROUBLE as an event and reports
 * BUSY as nothing at all, so a driver that reads "nothing" as "trouble" has
 * the two exactly the wrong way round.
 *
 * That is what a five-second deadline on a transfer was doing here, and the
 * objection is not theoretical: a flash drive stops answering for seconds at a
 * time while it does its own garbage collection, and this kernel boots from
 * one. Measured — a medium held to 512 bytes a second for a single 4 KiB block
 * was declared broken and had its transport reset.
 *
 * No host stack puts a clock on the transfer itself. Linux's usb-storage says
 * so in as many words — "transfer one buffer via bulk pipe, WITHOUT TIMEOUTS"
 * — and passes MAX_SCHEDULE_TIMEOUT; the bound lives a layer up, on the whole
 * COMMAND, and it is the SCSI disk timeout (drivers/scsi/sd.h SD_TIMEOUT,
 * thirty seconds).
 *
 * So: ONE bound, on a command rather than on a stage, sized by the class
 * rather than invented here, and it is the LAST RESORT rather than the test.
 * What ends a wait in every case anybody can name is a FACT, asked every pass:
 * the device is still in the socket, its endpoint is not in an error state,
 * and the controller has not stopped. The clock is left for the one device
 * nothing else can catch — broken firmware that answers neither way and NAKs
 * for ever — and it says that is what it is.
 */
#define MSD_COMMAND_PATIENCE_MS 30000

/*
 * ‼ AND IT IS A BUDGET PER COMMAND, WHICH ONLY MEANT ANYTHING WHILE EVERY
 * COMMAND WAS THE SAME SIZE.
 *
 * Thirty seconds for the four-kilobyte block this kernel used to ask for is a
 * floor of about a hundred and thirty bytes a second. Once the room began
 * asking each medium how much it takes at a time, one command could carry four
 * blocks — and the same thirty seconds then demanded four times the rate from
 * the same device. A drive answering steadily at five hundred bytes a second
 * passed while it was asked for one block and was declared broken when it was
 * asked for four, having done nothing differently.
 *
 * So the floor is what is written down, and the budget is derived from it and
 * the size of the request. The number now says something a person can check
 * against a device's datasheet instead of being a duration that happened to
 * fit one request size.
 */
#define MSD_SLOWEST_BYTES_PER_SEC 128u

static uint32_t msd_patience_ms(uint32_t bytes)
{
    /* Callers pass what the device has to get through, which is this command
     * plus whatever it may still be finishing — see MsdJob::behind. */
    uint64_t ms = ((uint64_t)bytes * 1000u) / MSD_SLOWEST_BYTES_PER_SEC;
    if (ms < MSD_COMMAND_PATIENCE_MS) {
        ms = MSD_COMMAND_PATIENCE_MS;
    }
    if (ms > 0xFFFFFFFFull) {
        ms = 0xFFFFFFFFull;
    }
    return (uint32_t)ms;
}

/* A control transfer is a different animal, and the specification does put
 * numbers on it: a standard request with no data stage completes in 50 ms, and
 * one with data keeps its stages 500 ms apart (USB 2.0 §9.2.6.4). A second is
 * generous for every one of them. */
#define MSD_CTRL_TIMEOUT_MS   1000

/* The bounce buffer every transfer passes through, and therefore the largest
 * piece of a request that crosses the bus at once. Bigger requests are split,
 * so no caller has to know this number exists. */
#define MSD_BOUNCE_BYTES      (64u * 1024u)

/*
 * Coming up is something the device SAYS, and this waits for exactly as long
 * as it goes on saying it.
 *
 * A medium is legitimately not ready for a while after it is plugged in — a
 * disk spinning up, a card reader initialising — and SCSI has a sentence for
 * exactly that: NOT READY / LOGICAL UNIT IS IN PROCESS OF BECOMING READY. It
 * also has different sentences for "there is no medium in me" and for "I want
 * a START UNIT first", and those are not waiting matters at all.
 *
 * This used to be forty attempts and a fixed pause, which is two seconds of
 * patience for something the standard puts no bound on, and it asked the
 * device why only to look for one answer. A stick that takes three seconds to
 * come up was a stick this machine refused — and on a machine that boots from
 * one, refusing it is the whole boot.
 *
 * So the loop runs while the device says it is coming up and stops the moment
 * it says anything else. The clock below is the last resort, for a device that
 * says it is coming up for ever, and its size is the class's: Linux gives a
 * disk tens of seconds to spin up before it gives in.
 */
#define MSD_READY_PATIENCE_MS 30000
#define MSD_READY_WAIT_MS     50

/* The sense keys this driver acts on, by the names SPC gives them. */
#define SENSE_NOT_READY       0x02
#define SENSE_UNIT_ATTENTION  0x06

/* And the additional codes that turn "not ready" into an answer. */
#define ASC_NOT_READY         0x04    /* ASCQ says which kind */
#define ASCQ_BECOMING_READY   0x01
#define ASCQ_START_NEEDED     0x02
#define ASC_NO_MEDIUM         0x3A

struct MsdAsyncReq;
struct MsdJob;

typedef struct XhciMsdUnit {
    struct XhciMsdUnit* next;

    xhci_controller_t*  ctrl;
    xhci_device_slot_t* slot;

    /* Which TENANCY of that slot. A slot is a numbered place the controller
     * hands out and takes back, and the next device to arrive may be given the
     * same number — so "my slot" is not by itself an identity. The epoch is:
     * it changes when the place changes hands, and comparing it is how this
     * unit can tell its own device from its successor. */
    uint32_t epoch;

    uint8_t  number;
    uint8_t  lun;
    bool     ready;

    /*
     * Two counts, because the device and the filesystem measure in different
     * units and pretending otherwise is how a 4096-byte-sector drive gets read
     * one eighth of the way through and believed.
     *
     *   blocks       what the device has, in ITS logical blocks
     *   block_bytes  how big one of those is
     *   sectors      the same medium in the 512-byte sectors BoxOS speaks
     *   per_sector   block_bytes / 512, which is 1 on almost everything
     */
    uint64_t blocks;
    uint64_t sectors;
    uint32_t block_bytes;
    uint32_t per_sector;

    /*
     * And what the medium is really made of, which is not the same question.
     *
     * Almost every flash device today is "512e": it is ADDRESSED in 512-byte
     * logical blocks and BUILT from 4096-byte (or larger) physical ones. Reads
     * do not care. A write that does not cover a whole physical block makes
     * the device read it, patch it and write it back — twice the work and
     * twice the wear, invisibly, forever.
     *
     * SBC-4 READ CAPACITY(16) states both: byte 13 bits 3:0 give the exponent
     * N where one physical block holds 2^N logical ones, and byte 14 bits 5:0
     * with byte 15 give the lowest logical block that starts a physical one.
     * READ CAPACITY(10) has neither field, which is why asking it alone
     * leaves a kernel guessing at the geometry it is writing to.
     *
     * phys_block_bytes == 0 means the device would not say.
     */
    uint32_t phys_block_bytes;
    uint32_t lowest_aligned_lba;

    uint32_t tag;

    /* Whether this unit has already said its device left. */
    bool     said_gone;

    /*
     * How much the last command on this unit asked for.
     *
     * ‼ A DEVICE IS NOT IDLE THE INSTANT IT ANSWERS. Flash does its own
     * housekeeping after a read, and the bigger the read the longer that
     * takes — so the command AFTER a large one is made to wait by it, without
     * anything being wrong with either. Once the room began handing whole runs
     * of neighbouring blocks to a medium that accepts them, a four-kilobyte
     * command could be queued behind sixteen and be given only its own four
     * kilobytes' worth of patience. Measured: at five hundred and twelve bytes
     * a second the small command took the whole of the budget sized for it
     * alone, and was called broken — a margin exactly as wide as the driver's
     * own error, which is not a margin.
     */
    uint32_t prev_bytes;

    void*    cmd_virt;   uint64_t cmd_phys;      /* wrappers, one page */
    void*    bounce_virt;uint64_t bounce_phys;   /* data */

    /* Whose turn it is on this device. Not a lock — see msd_gate_enter. */
    volatile uint32_t busy;

    /* The job holding the turn when nobody is standing over it, and since
     * when. Only ever set for an asynchronous read: a caller that waits is its
     * own watchdog, and one that does not needs this. */
    struct MsdJob*    watched;
    volatile uint64_t watched_since;

    /* Requests that arrived while somebody else had the turn, and are not
     * standing over it. A caller that can wait does; a caller that has gone
     * away leaves its request here to be started when the turn comes free.
     * The lock covers the list and nothing else — it is held for the length of
     * two pointer assignments, which is what a spinlock is for. */
    struct MsdAsyncReq* q_head;
    struct MsdAsyncReq* q_tail;
    spinlock_t          q_lock;

    char     name[41];                  /* "usb0 VENDOR PRODUCT" and room */
} XhciMsdUnit;

static XhciMsdUnit* g_units = NULL;
static spinlock_t   g_units_lock;
static bool         g_units_lock_ready = false;

static void msd_units_lock_init(void)
{
    if (!g_units_lock_ready) {
        spinlock_init(&g_units_lock);
        g_units_lock_ready = true;
    }
}

/* ── small helpers ──────────────────────────────────────────────────────── */

static void be32_put(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void be64_put(uint8_t* p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56); p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40); p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);  p[7] = (uint8_t)v;
}

static uint64_t be64_get(const uint8_t* p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  |  (uint64_t)p[7];
}

static uint32_t be32_get(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void msd_pause_ms(uint32_t ms)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(ms);
    while ((int64_t)(rdtsc() - deadline) < 0) {
        cpu_pause();
    }
}

/* The list, walked under the lock that guards it. Callers that go on to speak
 * to the device want msd_take instead — this one is for the bookkeeping that
 * only reads a number. */
static XhciMsdUnit* msd_find_locked(uint8_t number)
{
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        if (u->number == number) {
            return u;
        }
    }
    return NULL;
}

/*
 * Take hold of a unit, and of the device underneath it.
 *
 * Finding the unit and stepping inside the slot happen under the same lock, so
 * a device that is being taken down cannot slip between the two. The step
 * inside is what keeps the endpoints, the rings and the buffers alive for as
 * long as this caller is using them: the disk is pulled out by a hand, and the
 * hand does not wait for the filesystem to finish its sentence.
 *
 * The unit's own memory is covered by the same hold, because a unit is only
 * ever released while its slot is being taken down, and that waits.
 */
static XhciMsdUnit* msd_take(uint8_t number)
{
    if (!g_units_lock_ready) {
        return NULL;
    }

    spin_lock(&g_units_lock);
    XhciMsdUnit* u = msd_find_locked(number);
    if (u && (!u->ready || !xhci_slot_enter(u->slot))) {
        u = NULL;
    }
    spin_unlock(&g_units_lock);
    return u;
}

static void msd_give_back(XhciMsdUnit* u)
{
    if (u) {
        xhci_slot_leave(u->slot);
    }
}

/*
 * ── whose turn it is on this device ─────────────────────────────────────────
 *
 * One command at a time, because Bulk-Only Transport is a strictly serial
 * conversation and every command on this unit passes through the same bounce
 * buffer. That much was always true. What was wrong was the instrument.
 *
 * This was a spinlock, held from the first transfer of a request to the last.
 * spin_lock() disables interrupts for as long as it is held (klib_lock.c: cli
 * on the way in, the saved RFLAGS restored on the way out), and what it was
 * held across is a flash transfer: hundreds of microseconds under emulation,
 * milliseconds on a real device, and for a request above 64 KiB the whole
 * multi-pass sequence of them.
 *
 * A core that cannot take an interrupt for that long:
 *   - cannot acknowledge a cross-core TLB shootdown IPI, and the core that
 *     sent it spins for that acknowledgement and PANICS on the timeout — the
 *     same real-HW deadlock spin_lock's own wait-service hook exists to avoid;
 *   - cannot take the timer tick, which is what runs this driver's command and
 *     enumeration watchdogs — so the one thing that could notice a wedged
 *     transfer is switched off for exactly the duration of the transfer;
 *   - cannot take the controller's own interrupt.
 *
 * The same fault was found and fixed in enumeration, where a ten-millisecond
 * TRSTRCY wait was being spun out under the event-ring lock (the reasoning is
 * written out in xhci_enumeration.h, ENUM_STATE_WAIT_RESET_RECOVERY). The
 * lesson was applied where it was found and nowhere else; this is the same
 * fault on the hot path of every file the machine reads.
 *
 * The SATA path never had it: ahci_read_sectors_sync takes its port lock only
 * around the register store and spins for completion with interrupts on. This
 * is the USB path being brought to the same standard.
 *
 * So: a gate rather than a lock. Taking a turn is an exchange; waiting for one
 * happens with interrupts as the caller left them, and drains the event ring
 * while it waits — which is what lets the holder's answer arrive and its turn
 * end.
 *
 * There is deliberately NO deadline on the wait for a turn. A deadline here
 * would abandon a request that was about to succeed, and it would be a
 * deadline on the wrong thing: the holder cannot hold for ever, because the
 * COMMAND it is running is bounded — by the facts msd_job_run asks every pass,
 * and behind them by MSD_COMMAND_PATIENCE_MS — so the gate is always released.
 * A long wait is worth SAYING on a board, and it is said once.
 *
 * ‼ THAT SENTENCE IS LOAD-BEARING, AND IT WAS ONCE FALSE. An asynchronous job
 * is released by msd_async_done and by nothing else, so its holder is bounded
 * only for as long as xhci_msd_watchdog goes on running — which is the guide
 * loop and the idle loop. Anything that stops those from running turns this
 * wait into a permanent one, with a slot visit held throughout.
 */
#define MSD_GATE_COMPLAIN_MS 2000

static void msd_gate_enter(XhciMsdUnit* u)
{
    if (__atomic_exchange_n(&u->busy, 1u, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    uint64_t complain_at = rdtsc() + cpu_ms_to_tsc(MSD_GATE_COMPLAIN_MS);
    bool     complained  = false;

    for (;;) {
        /* The holder is waiting for an answer off the event ring, and on a core
         * that is not the one taking the controller's interrupt this is what
         * brings that answer in. Safe from here precisely because this holds no
         * lock: the drain takes its own, and a nested call on this core is
         * refused rather than deadlocked. */
        xhci_process_events();
        cpu_pause();

        if (__atomic_exchange_n(&u->busy, 1u, __ATOMIC_ACQUIRE) == 0) {
            return;
        }

        if (!complained && (int64_t)(rdtsc() - complain_at) >= 0) {
            complained = true;
            kprintf("[USB disk %u] a second request has been waiting %u ms for "
                    "its turn on this device\n", u->number, MSD_GATE_COMPLAIN_MS);
        }
    }
}

/*
 * A job nobody is standing over, and the clock that stands over it instead.
 *
 * Armed when such a job takes the turn and disarmed when it gives it back, so
 * the window watched is exactly the window in which the device owes an answer.
 */
static void msd_watch_arm(XhciMsdUnit* u, struct MsdJob* j)
{
    __atomic_store_n(&u->watched_since, rdtsc(), __ATOMIC_RELAXED);
    __atomic_store_n(&u->watched, j, __ATOMIC_RELEASE);
}

static void msd_watch_disarm(XhciMsdUnit* u)
{
    __atomic_store_n(&u->watched, NULL, __ATOMIC_RELEASE);
}

static bool msd_gate_try_enter(XhciMsdUnit* u)
{
    return __atomic_exchange_n(&u->busy, 1u, __ATOMIC_ACQUIRE) == 0;
}

static void msd_start_queued(XhciMsdUnit* u);

/*
 * Give up the turn — and, before anybody else can take it, hand it to a
 * request that has been left waiting.
 *
 * The handover matters: a caller that went away cannot come back and try
 * again, so if the turn were simply released the queued request would sit
 * there until some unrelated command happened to end and remember it.
 */
static void msd_gate_leave(XhciMsdUnit* u)
{
    __atomic_store_n(&u->busy, 0u, __ATOMIC_RELEASE);
    msd_start_queued(u);
}

/* Lowest number nobody is using. Numbers are stable for the life of a unit, so
 * a disk that leaves and comes back does not renumber the ones beside it. */
static uint8_t msd_next_number(void)
{
    for (uint8_t n = 0; n < 255; n++) {
        if (!msd_find_locked(n)) {
            return n;
        }
    }
    return 255;
}

/* ── is the device still there ──────────────────────────────────────────── */

/*
 * The one question this driver asks before it spends anything on a device.
 *
 * Everything below — clearing a halted pipe, resetting the transport, waiting
 * for an answer — costs a budget per step, and on a live board a device that
 * has been pulled does not go quiet: it answers `Endpoint Not Enabled`, and
 * the controller holds commands for a slot that is not there any more. So the
 * recovery kept going, seconds at a time, against something that had left.
 *
 * What that cost was not obvious. A caller inside the slot holds `visitors`
 * above zero and keeps commands outstanding, and `xhci_slot_service` waits for
 * BOTH to clear before it takes the slot down — so `xhci_msd_release` never
 * ran, the unit number was never given back, the next stick got a higher one,
 * and a higher number is a chair nobody was sitting in. That is the whole of
 * "usb0..usb4 from one flash drive".
 *
 * Two facts, and neither of them is a clock:
 *   - the slot is still live AND still the tenancy this unit belongs to. A
 *     retired slot fails this, and so does one already handed to the next
 *     device to arrive;
 *   - the root port does not say the socket is empty. Only the NEGATIVE answer
 *     is worth anything here: a device behind a hub carries the ROOT port, so
 *     a healthy reading there describes the branch and says nothing about the
 *     device. `xhci_slot_retire` clears port_num, so this covers exactly the
 *     window before the port event has been drained.
 */
static bool msd_device_is_there(const XhciMsdUnit* u)
{
    if (!u || !u->slot) {
        return false;
    }
    if (!xhci_slot_still_is(u->slot, u->epoch)) {
        return false;
    }
    uint8_t port = u->slot->port_num;
    if (port != 0 && xhci_port_says_gone(u->ctrl, port)) {
        return false;
    }
    return true;
}

/* Said once per unit, because the recovery has several steps and each of them
 * asks: eight identical lines describe one departure no better than one. */
static void msd_note_gone(XhciMsdUnit* u, const char* what)
{
    if (u->said_gone) {
        return;
    }
    u->said_gone = true;
    kprintf("[USB disk %u] %s — the device has left, so it is not being "
            "asked for anything more\n", u->number, what);
}

/* ── endpoint recovery ──────────────────────────────────────────────────── */

/*
 * A stalled bulk endpoint has to be cleared on both sides of the wire: the
 * device is told with CLEAR_FEATURE(ENDPOINT_HALT) and the controller with
 * Reset Endpoint, and doing only one of them leaves the two disagreeing about
 * whether the pipe is usable. The device goes first, which is the order the
 * mass storage class specification lays out for its own error recovery.
 */
static void msd_clear_halt(XhciMsdUnit* u, uint8_t dci)
{
    if (!msd_device_is_there(u)) {
        msd_note_gone(u, "a halted pipe was to be cleared");
        return;
    }

    xhci_endpoint_t* ep = &u->slot->endpoints[dci];

    usb_setup_packet_t setup = {
        .bmRequestType = 0x02,          /* host to device, endpoint */
        .bRequest = USB_REQ_CLEAR_FEATURE,
        .wValue = USB_FEATURE_ENDPOINT_HALT,
        .wIndex = ep->addr,
        .wLength = 0
    };
    xhci_control_transfer_sync(u->ctrl, u->slot, &setup, 0, 0, false,
                               MSD_CTRL_TIMEOUT_MS);

    /* Asked again: the transfer above is where a departure is most likely to
     * be discovered, and the two commands below are the expensive half. */
    if (!msd_device_is_there(u)) {
        msd_note_gone(u, "the pipe would not clear");
        return;
    }

    xhci_ep_recover(u->ctrl, u->slot, dci);
    xhci_command_wait_idle(u->ctrl, MSD_CTRL_TIMEOUT_MS);
}

/*
 * The reset the class defines for when the two ends have lost track of each
 * other entirely — a phase error, or a status wrapper that never came. It puts
 * the device back at the start of a command and both pipes back in order.
 */
static void msd_bot_reset(XhciMsdUnit* u)
{
    /* The class reset is three control transfers and four commands. Spending
     * that on a device that has left is what kept a slot occupied for tens of
     * seconds while the room waited to seat the next one. */
    if (!msd_device_is_there(u)) {
        msd_note_gone(u, "the transport was to be reset");
        return;
    }

    kprintf("[USB disk %u] resetting the transport\n", u->number);

    usb_setup_packet_t setup = {
        .bmRequestType = 0x21,          /* host to device, class, interface */
        .bRequest = MSD_REQ_BOT_RESET,
        .wValue = 0,
        .wIndex = u->slot->interface_num,
        .wLength = 0
    };
    xhci_control_transfer_sync(u->ctrl, u->slot, &setup, 0, 0, false,
                               MSD_CTRL_TIMEOUT_MS);

    msd_clear_halt(u, u->slot->ep_bulk_in);
    msd_clear_halt(u, u->slot->ep_bulk_out);
}

/* ── one command, three transfers ───────────────────────────────────────── */

/*
 * A command is a job with a place in it, and the place is what moves.
 *
 * Bulk-Only Transport is three transfers — a command wrapper out, a data stage
 * if the command has one, a status wrapper back — and each of them is finished
 * by the controller posting an event. The old shape asked for each transfer and
 * then stood there until the answer came, which meant a core was spent for the
 * whole length of a flash read and the constitution's rule against waiting by
 * counting was broken three times per command.
 *
 * So the command remembers where it is instead. Each answered transfer decides
 * the next one, exactly the way enumeration already works in this driver: the
 * state is what the job is listening for, and nothing anywhere waits.
 *
 * Who turns the handle is a separate question from what the handle does, and
 * there are two answers:
 *
 *   - a caller that wants the sectors before it goes on (the filesystem being
 *     mounted, with nothing else to do until the block arrives) drives the job
 *     itself, in msd_job_run — draining the ring and stepping the job. It holds
 *     no lock while it does, so interrupts are served throughout;
 *
 *   - a caller that has better things to do registers the job's completion node
 *     instead. The drain posts it the moment the transfer is answered, and a
 *     K-Core steps the job from the guide loop. The node lives inside the job,
 *     so posting it cannot fail for want of a slot — a lost transfer completion
 *     would leave its owner waiting for ever.
 *
 * One machine, two drivers. The second is what lets a process reading a file
 * park instead of spin.
 */

#define MSD_PHASE_IDLE 0
#define MSD_PHASE_CBW  1
#define MSD_PHASE_DATA 2
#define MSD_PHASE_CSW  3
#define MSD_PHASE_DONE 4

/* A device is allowed to stall the status stage once and be asked again; the
 * class says so, and asking a second time is the whole of the remedy. */
#define MSD_CSW_TRIES 2

typedef struct MsdJob {
    /* First, and by value: this is how a completion reaches a K-Core without
     * an allocation standing between the two. */
    StorageCompletion node;

    XhciMsdUnit* u;

    uint8_t   cdb[16];
    uint8_t   cdb_len;
    uint64_t  data_phys;
    uint32_t  data_len;
    bool      data_in;

    uint32_t  tag;                  /* what the status wrapper must echo */
    uint8_t   phase;
    uint8_t   dci;                  /* the endpoint the current phase is on */
    uint8_t   csw_tries;
    bool      hand_to_kcore;        /* false = the caller is driving */

    uint32_t  behind;               /* bytes the device was last asked for */
    uint32_t  transferred;          /* bytes the data stage moved */
    int       status;               /* 0 carried out, 1 refused, -1 broken */
    volatile uint8_t finished;

    /* Told when the job is over, for a caller that did not stay. */
    void (*done)(void* ctx, int status, uint32_t transferred);
    void*  done_ctx;
} MsdJob;

static void msd_job_step(void* ctx);

/* Put the next transfer on the wire. The completion node goes with it only for
 * a job somebody else is driving; a job its own caller is stepping has no use
 * for the queue and does not touch it. */
static int msd_job_submit(MsdJob* j, uint8_t dci, uint64_t phys, uint32_t len)
{
    j->dci = dci;
    return xhci_ep_submit_async(j->u->ctrl, j->u->slot, dci, phys, len,
                                j->hand_to_kcore ? &j->node : NULL);
}

static void msd_job_finish(MsdJob* j, int status)
{
    j->phase  = MSD_PHASE_DONE;
    j->status = status;
    __atomic_store_n(&j->finished, 1u, __ATOMIC_RELEASE);
    if (j->done) {
        j->done(j->done_ctx, status, j->transferred);
    }
}

static void msd_job_begin(MsdJob* j)
{
    XhciMsdUnit* u = j->u;
    MsdCbw* cbw = (MsdCbw*)u->cmd_virt;

    j->tag = ++u->tag;

    /* What this one may be waiting on, and what the next one may wait on. */
    j->behind      = u->prev_bytes;
    u->prev_bytes  = j->data_len;

    /* Where the answer goes and what it runs, set once, before anything can
     * post it. The node is used only by a job somebody else is driving, but a
     * half-filled one is not worth the risk of it ever being posted. */
    j->node.run = msd_job_step;
    j->node.ctx = j;

    memset(cbw, 0, sizeof(*cbw));
    cbw->signature   = CBW_SIGNATURE;
    cbw->tag         = j->tag;
    cbw->data_length = j->data_len;
    cbw->flags       = j->data_in ? 0x80 : 0x00;
    cbw->lun         = u->lun;
    cbw->cb_length   = j->cdb_len;
    memcpy(cbw->cb, j->cdb, j->cdb_len);

    /* Written down before it is asked for, for the same reason the endpoint
     * registers its completion before the doorbell: the answer may arrive
     * inside the submission, and a job that still says IDLE when its answer
     * turns up is a job that will never be stepped. */
    j->phase = MSD_PHASE_CBW;

    if (msd_job_submit(j, u->slot->ep_bulk_out, u->cmd_phys,
                       sizeof(MsdCbw)) != 0) {
        msd_job_finish(j, -1);
    }
}

/*
 * One answered transfer, and what follows from it.
 *
 * Runs either on the caller's own stack (a job it is driving) or on a K-Core
 * out of the guide loop. Never inside the event drain, which is why the
 * recovery below is allowed to be made of control transfers: clearing a halted
 * pipe means speaking to the device, and speaking to the device is exactly what
 * the drain cannot do. The hub service in the same loop is made of the same
 * stuff for the same reason.
 */
static void msd_job_step(void* ctx)
{
    MsdJob* j = (MsdJob*)ctx;
    XhciMsdUnit* u = j->u;

    /* Asked before the answer is taken, not after. A job that is over has no
     * transfer of its own outstanding, and the endpoint it last used belongs
     * to the next command now — taking a result here would consume somebody
     * else's answer and leave them waiting for one that had already come. */
    if (j->phase == MSD_PHASE_IDLE || j->phase == MSD_PHASE_DONE) {
        return;
    }

    uint8_t  code     = 0;
    uint32_t residual = 0;
    if (!xhci_ep_take_result(u->slot, j->dci, &code, &residual)) {
        return;                     /* not answered yet */
    }

    bool ok    = (code == TRB_COMPLETION_SUCCESS);
    bool short_ok = ok || (code == TRB_COMPLETION_SHORT_PKT);

    switch (j->phase) {

    case MSD_PHASE_CBW:
        if (!ok) {
            /* The device would not even take the command. A halted pipe is
             * cleared so the next command has somewhere to go; there is
             * nothing to ask about, because nothing was asked. */
            if (code == TRB_COMPLETION_STALL) {
                msd_clear_halt(u, u->slot->ep_bulk_out);
            }
            msd_job_finish(j, -1);
            return;
        }

        if (j->data_len > 0) {
            j->phase = MSD_PHASE_DATA;
            uint8_t dci = j->data_in ? u->slot->ep_bulk_in : u->slot->ep_bulk_out;
            if (msd_job_submit(j, dci, j->data_phys, j->data_len) != 0) {
                msd_job_finish(j, -1);
            }
            return;
        }
        /* No data stage: straight to the status wrapper. */
        j->phase = MSD_PHASE_CSW;
        memset((uint8_t*)u->cmd_virt + 64, 0, sizeof(MsdCsw));
        if (msd_job_submit(j, u->slot->ep_bulk_in, u->cmd_phys + 64,
                           sizeof(MsdCsw)) != 0) {
            msd_job_finish(j, -1);
        }
        return;

    case MSD_PHASE_DATA:
        if (code == TRB_COMPLETION_STALL) {
            /* A stalled data stage is not the end of the exchange: the device
             * still owes a status wrapper, and reading it is how the driver
             * learns what went wrong. Clear the pipe and carry on to it. */
            msd_clear_halt(u, j->dci);
        } else if (!short_ok) {
            msd_bot_reset(u);
            msd_job_finish(j, -1);
            return;
        } else {
            j->transferred = (residual <= j->data_len)
                           ? (j->data_len - residual) : 0;
        }

        j->phase = MSD_PHASE_CSW;
        memset((uint8_t*)u->cmd_virt + 64, 0, sizeof(MsdCsw));
        if (msd_job_submit(j, u->slot->ep_bulk_in, u->cmd_phys + 64,
                           sizeof(MsdCsw)) != 0) {
            msd_job_finish(j, -1);
        }
        return;

    case MSD_PHASE_CSW: {
        if (code == TRB_COMPLETION_STALL && j->csw_tries < MSD_CSW_TRIES) {
            j->csw_tries++;
            msd_clear_halt(u, u->slot->ep_bulk_in);
            memset((uint8_t*)u->cmd_virt + 64, 0, sizeof(MsdCsw));
            if (msd_job_submit(j, u->slot->ep_bulk_in, u->cmd_phys + 64,
                               sizeof(MsdCsw)) != 0) {
                msd_job_finish(j, -1);
            }
            return;
        }
        if (!short_ok) {
            msd_bot_reset(u);
            msd_job_finish(j, -1);
            return;
        }

        const MsdCsw* csw = (const MsdCsw*)((uint8_t*)u->cmd_virt + 64);
        if (csw->signature != CSW_SIGNATURE || csw->tag != j->tag) {
            kprintf("[USB disk %u] status wrapper does not match the command "
                    "(signature 0x%08x, tag %u for %u)\n",
                    u->number, csw->signature, csw->tag, j->tag);
            msd_bot_reset(u);
            msd_job_finish(j, -1);
            return;
        }
        if (csw->status == CSW_PHASE_ERROR) {
            msd_bot_reset(u);
            msd_job_finish(j, -1);
            return;
        }

        msd_job_finish(j, (csw->status == CSW_PASSED) ? 0 : 1);
        return;
    }

    default:
        return;
    }
}

/*
 * Drive a job to its end on this stack, for a caller that wants the answer
 * before it goes on.
 *
 * The ring is drained here rather than waited on, because the same call has to
 * work before interrupts are routed, with them masked, and on a controller with
 * none — and because during boot there is no guide loop yet to turn the handle.
 * What it does NOT do is hold a lock while it does so.
 */
static void msd_job_run(MsdJob* j)
{
    msd_job_begin(j);

    /* The last resort, and it covers the WHOLE command rather than a stage of
     * it — see MSD_COMMAND_PATIENCE_MS. Nothing below reaches it except a
     * device that is present, whose endpoint is well, on a controller that is
     * running, and which has still not spoken. */
    const uint32_t patience = msd_patience_ms(j->data_len + j->behind);
    uint64_t give_up_at = rdtsc() + cpu_ms_to_tsc(patience);

    while (!__atomic_load_n(&j->finished, __ATOMIC_ACQUIRE)) {
        xhci_process_events();
        msd_job_step(j);

        if (__atomic_load_n(&j->finished, __ATOMIC_ACQUIRE)) {
            break;
        }

        /*
         * ── the facts, asked every pass ────────────────────────────────────
         *
         * Is it still in the socket. A device that has been pulled is not a
         * device that is answering slowly, and there is a precise answer to
         * which of the two this is. Without it a stick pulled mid-read cost
         * the whole budget here and the transport reset after it, and for all
         * of that time the slot could not be taken down and the unit number
         * could not be given back.
         */
        if (!msd_device_is_there(j->u)) {
            msd_note_gone(j->u, "an answer was owed at this stage");
            xhci_ep_abandon(j->u->ctrl, j->u->slot, j->dci);
            msd_job_finish(j, -1);
            break;
        }

        /*
         * Is the pipe well. The controller keeps the endpoint's state in the
         * Output Endpoint Context and writes it as things happen to it, so an
         * endpoint that has gone to Error or been taken away is a fact this
         * loop can read rather than a silence it has to wait out. Halted is
         * NOT one of these: a stall arrives as a Transfer Event of its own and
         * the job above knows what to do with it.
         */
        uint8_t ep_state = xhci_ep_context_state(j->u->ctrl, j->u->slot, j->dci);
        if (ep_state == XHCI_EP_STATE_ERROR ||
            ep_state == XHCI_EP_STATE_DISABLED) {
            kprintf("[USB disk %u] the pipe this command is on is %s — not "
                    "waiting for an answer that cannot come\n",
                    j->u->number, xhci_ep_state_name(ep_state));
            xhci_ep_abandon(j->u->ctrl, j->u->slot, j->dci);
            msd_job_finish(j, -1);
            break;
        }

        /* Is anybody driving. A controller that has stopped itself is not
         * going to answer this or anything else, and it says so in USBSTS —
         * which the drain above reads on every pass. */
        if (j->u->ctrl->error_state) {
            kprintf("[USB disk %u] the controller has stopped — this command "
                    "has nobody to answer it\n", j->u->number);
            msd_job_finish(j, -1);
            break;
        }

        /*
         * ── and only then the clock ────────────────────────────────────────
         *
         * Reached only by a device that is present, on a well pipe, on a
         * running controller, and still silent — which on bulk means it has
         * been saying "not yet" for half a minute. That is not a device this
         * driver can go on holding a caller for, and it is the one case no
         * register anywhere distinguishes from a healthy one.
         */
        if ((int64_t)(rdtsc() - give_up_at) >= 0) {
            kprintf("[USB disk %u] the device has been asking for more time "
                    "for %u ms at stage %u — giving up on the command\n",
                    j->u->number, patience, j->phase);
            /*
             * The host side first, and the order is not a preference.
             *
             * The transfer this gave up on is still on the endpoint's ring and
             * the controller still owns it. Resetting the transport before
             * taking it back tells the device to start a new command over a
             * pipe the controller is still walking — and leaves the endpoint
             * marked as busy for the rest of the boot, so every later read on
             * this disk is refused before it is even sent.
             */
            xhci_ep_abandon(j->u->ctrl, j->u->slot, j->dci);
            msd_bot_reset(j->u);
            msd_job_finish(j, -1);
            break;
        }
        cpu_pause();
    }
}

/*
 * Returns 0 when the device carried the command out, positive when it refused
 * it (a SCSI failure the caller may want to ask about), negative when the
 * conversation itself broke down.
 *
 * The caller holds the unit's turn; every buffer used here belongs to the unit.
 */
static int msd_command(XhciMsdUnit* u, const uint8_t* cdb, uint8_t cdb_len,
                       uint64_t data_phys, uint32_t data_len, bool data_in,
                       uint32_t* out_transferred)
{
    if (cdb_len == 0 || cdb_len > 16) {
        return -1;
    }

    /* The answer to this cannot arrive until this call returns — the drain
     * that would carry it is below this frame on the same stack. Asked before
     * anything is put on the wire, so nothing is left in flight. */
    if (xhci_drain_is_mine(u->ctrl)) {
        kprintf("[USB disk %u] a command was waited for from inside the event "
                "drain — its answer cannot arrive until this returns\n",
                u->number);
        return -1;
    }

    MsdJob job;
    memset(&job, 0, sizeof(job));
    job.u         = u;
    job.cdb_len   = cdb_len;
    job.data_phys = data_phys;
    job.data_len  = data_len;
    job.data_in   = data_in;
    memcpy(job.cdb, cdb, cdb_len);

    if (out_transferred) {
        *out_transferred = 0;
    }

    msd_job_run(&job);

    if (out_transferred) {
        *out_transferred = job.transferred;
    }
    return job.status;
}
/* Ask the device why it refused. Used for its own sake — the sense key is what
 * tells "no medium" apart from "still spinning up" apart from "broken". */
static int msd_request_sense(XhciMsdUnit* u, uint8_t* out_key, uint8_t* out_asc,
                             uint8_t* out_ascq)
{
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint32_t got = 0;

    memset(u->bounce_virt, 0, 18);
    int rc = msd_command(u, cdb, sizeof(cdb), u->bounce_phys, 18, true, &got);
    /* The qualifier is byte 13, so fourteen bytes is not enough to have it —
     * and it is the byte that separates "I am coming up" from "start me
     * first", which are opposite answers to the same question. */
    if (rc != 0 || got < 14) {
        return -1;
    }

    const uint8_t* s = (const uint8_t*)u->bounce_virt;
    if (out_key)  *out_key  = s[2] & 0x0F;
    if (out_asc)  *out_asc  = s[12];
    if (out_ascq) *out_ascq = (got > 13) ? s[13] : 0;
    return 0;
}

/* ── attach ─────────────────────────────────────────────────────────────── */

/*
 * How big it is, asked in whichever form can hold the answer.
 *
 * Ten bytes first, because every device answers it. A device whose last block
 * number does not fit in the thirty-two bits that reply has is required to say
 * 0xFFFFFFFF, which is not a size — it is the device asking to be asked again
 * with the sixteen-byte form. Believing it costs the whole of a drive above
 * two tebibytes, quietly, with a filesystem laid over the part that is not
 * there.
 */
static int msd_read_capacity(XhciMsdUnit* u)
{
    uint8_t cdb10[10] = { SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t got = 0;

    memset(u->bounce_virt, 0, 8);
    int rc = msd_command(u, cdb10, sizeof(cdb10), u->bounce_phys, 8, true, &got);
    if (rc != 0 || got < 8) {
        return -1;
    }

    const uint8_t* c = (const uint8_t*)u->bounce_virt;
    uint32_t last_lba = be32_get(c);
    uint32_t block    = be32_get(c + 4);

    if (block == 0) {
        return -1;
    }

    bool ten_sufficed = (last_lba != 0xFFFFFFFFu);
    if (ten_sufficed) {
        u->block_bytes = block;
        u->blocks      = (uint64_t)last_lba + 1;
    }

    /*
     * Sixteen bytes: opcode, service action, an eight-byte LBA that is zero
     * here, a four-byte allocation length, control.
     *
     * Asked of EVERY device, not only of one too large for the ten-byte form.
     * The capacity is the smaller half of what it answers; the other half is
     * the physical geometry, and there is nowhere else to get it. A device
     * that does not implement it says so and keeps the numbers it already
     * gave — that is a device without an answer, not a device that failed.
     */
    uint8_t cdb16[16] = {0};
    cdb16[0]  = SCSI_SERVICE_ACTION_IN_16;
    cdb16[1]  = SCSI_SAI_READ_CAPACITY_16;
    be32_put(&cdb16[10], 32);

    memset(u->bounce_virt, 0, 32);
    got = 0;
    rc = msd_command(u, cdb16, sizeof(cdb16), u->bounce_phys, 32, true, &got);

    if (rc != 0 || got < 12) {
        if (!ten_sufficed) {
            kprintf("[USB disk %u] says it is larger than a ten-byte capacity "
                    "can state and then would not answer the sixteen-byte "
                    "one\n", u->number);
            return -1;
        }
        return 0;                       /* geometry unknown; capacity stands */
    }

    uint64_t last64 = be64_get(c);
    uint32_t blk64  = be32_get(c + 8);
    if (blk64 == 0) {
        return ten_sufficed ? 0 : -1;
    }

    if (!ten_sufficed) {
        u->block_bytes = blk64;
        u->blocks      = last64 + 1;
    } else if (blk64 != u->block_bytes) {
        /* The two commands describe one medium and must agree about it. When
         * they do not, the ten-byte answer stands — every device implements
         * it — and the disagreement is said rather than averaged. */
        kprintf("[USB disk %u] answers %u-byte blocks to one capacity command "
                "and %u to the other; using %u\n",
                u->number, u->block_bytes, blk64, u->block_bytes);
    }

    /* SBC-4 READ CAPACITY(16): byte 13 bits 3:0 = LOGICAL BLOCKS PER PHYSICAL
     * BLOCK EXPONENT, byte 14 bits 5:0 (MSB) with byte 15 = LOWEST ALIGNED
     * LOGICAL BLOCK ADDRESS. Both need sixteen bytes back, not twelve. */
    if (got >= 16) {
        /* The exponent field is four bits, so it cannot exceed 15 — but the
         * block size it multiplies came off the wire too, and a device that
         * states an absurd one must not be able to overflow the shift into a
         * small number that looks reasonable. Checked against the room left,
         * not against the exponent. */
        uint32_t exponent = c[13] & 0x0Fu;
        if (u->block_bytes <= (0xFFFFFFFFu >> exponent)) {
            u->phys_block_bytes = u->block_bytes << exponent;
        }
        u->lowest_aligned_lba = ((uint32_t)(c[14] & 0x3Fu) << 8) | c[15];
    }
    return 0;
}

static int msd_wait_ready(XhciMsdUnit* u)
{
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    uint64_t give_up_at = rdtsc() + cpu_ms_to_tsc(MSD_READY_PATIENCE_MS);
    bool said_coming_up = false;

    for (;;) {
        int rc = msd_command(u, cdb, sizeof(cdb), 0, 0, false, NULL);
        if (rc == 0) {
            if (said_coming_up) {
                kprintf("[USB disk %u] ready\n", u->number);
            }
            return 0;
        }
        if (rc < 0) {
            return -1;                  /* the transport, not the medium */
        }

        /*
         * It refused, so ask it why. Everything below is the device's own
         * answer being acted on, and a device that will not say why is a
         * device this driver has nothing to wait FOR.
         */
        uint8_t key = 0, asc = 0, ascq = 0;
        if (msd_request_sense(u, &key, &asc, &ascq) != 0) {
            kprintf("[USB disk %u] refused a command and would not say why\n",
                    u->number);
            return -1;
        }

        /* A state change — a medium arriving, a reset, a power-on — is not a
         * refusal, it is the device clearing its throat. Asked again at once
         * rather than after a pause. */
        if (key == SENSE_UNIT_ATTENTION) {
            continue;
        }

        if (key != SENSE_NOT_READY) {
            kprintf("[USB disk %u] is not ready and does not say it is coming "
                    "up (sense key 0x%x, code 0x%02x/0x%02x)\n",
                    u->number, key, asc, ascq);
            return -1;
        }

        if (asc == ASC_NO_MEDIUM) {
            /* A card reader with nothing in it. Waiting changes nothing, and
             * this is the device saying so rather than a clock deciding. */
            kprintf("[USB disk %u] no medium\n", u->number);
            return -1;
        }

        if (asc != ASC_NOT_READY || ascq == ASCQ_START_NEEDED) {
            /* Either it is not ready for a reason that is not time, or it is
             * waiting to be told to start — which is a command, not a wait,
             * and this driver does not send it. Named either way, because a
             * medium refused for a reason nobody printed is a machine with no
             * filesystem and no explanation. */
            kprintf("[USB disk %u] is not ready and waiting will not change it "
                    "(code 0x%02x/0x%02x%s)\n", u->number, asc, ascq,
                    ascq == ASCQ_START_NEEDED ? " — it wants a START UNIT" : "");
            return -1;
        }

        /* NOT READY / IN PROCESS OF BECOMING READY. This is the one answer
         * that means "ask me again", and it is the only one waited on. */
        if (!said_coming_up) {
            said_coming_up = true;
            kprintf("[USB disk %u] says it is still coming up — waiting for "
                    "it to say otherwise\n", u->number);
        }

        if ((int64_t)(rdtsc() - give_up_at) >= 0) {
            kprintf("[USB disk %u] has been coming up for %u ms and has not "
                    "finished — not using it\n", u->number,
                    MSD_READY_PATIENCE_MS);
            return -1;
        }

        msd_pause_ms(MSD_READY_WAIT_MS);
    }
}

static void msd_read_identity(XhciMsdUnit* u)
{
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint32_t got = 0;

    memset(u->bounce_virt, 0, 36);
    if (msd_command(u, cdb, sizeof(cdb), u->bounce_phys, 36, true, &got) != 0 ||
        got < 36) {
        ksnprintf(u->name, sizeof(u->name), "usb%u", u->number);
        return;
    }

    /* Vendor is bytes 8..15, product 16..31, both space padded ASCII. */
    const char* id = (const char*)u->bounce_virt;
    char vendor[9], product[17];
    memcpy(vendor, id + 8, 8);   vendor[8] = '\0';
    memcpy(product, id + 16, 16); product[16] = '\0';
    for (int i = 7; i >= 0 && vendor[i] == ' '; i--)  vendor[i] = '\0';
    for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = '\0';

    ksnprintf(u->name, sizeof(u->name), "usb%u %s %s", u->number,
              vendor, product);
}

/* Everything here is a conversation with the device, and the whole point of a
 * removable disk is that it can be pulled out in the middle of one. The caller
 * below holds the device open for the length of it. */
static int msd_attach_held(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    msd_units_lock_init();

    XhciMsdUnit* u = (XhciMsdUnit*)kmalloc(sizeof(XhciMsdUnit));
    if (!u) {
        return -1;
    }
    memset(u, 0, sizeof(*u));
    spinlock_init(&u->q_lock);
    u->ctrl  = ctrl;
    u->slot  = slot;
    u->epoch = xhci_slot_epoch(slot);

    void* cmd = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    void* bounce = pmm_alloc_zero(vmm_size_to_pages(MSD_BOUNCE_BYTES),
                                  PHYS_TAG_DMA32);
    if (!cmd || !bounce) {
        if (cmd)    pmm_free(cmd, 1);
        if (bounce) pmm_free(bounce, vmm_size_to_pages(MSD_BOUNCE_BYTES));
        kfree(u);
        return -1;
    }
    u->cmd_phys    = (uint64_t)cmd;
    u->cmd_virt    = vmm_phys_to_virt((uintptr_t)cmd);
    u->bounce_phys = (uint64_t)bounce;
    u->bounce_virt = vmm_phys_to_virt((uintptr_t)bounce);

    /*
     * ‼ IS THERE ALREADY ONE OF THESE? ASKED WHERE THE ANSWER CANNOT CHANGE.
     *
     * The caller asks xhci_msd_slot_attached() first, and that walk is not
     * under this lock — so two cores can both be told "no" for the same slot
     * and both arrive here. That is not a theoretical arrangement: the room is
     * called to order from the boot on the BSP and from the attendance pass on
     * any idle core, and both of those attach whatever storage they find.
     *
     * The second unit is the damage. Only one of them is ever unlinked when
     * the device leaves — xhci_msd_release takes the FIRST match — so the
     * other keeps its unit number for the rest of the boot. The number is
     * never free again, the next stick gets a higher one, and a higher number
     * is a chair nobody was sitting in. Measured on a live board: one flash
     * drive, seats usb0, usb1 and usb2, two of which could not be read.
     *
     * So the question is asked again HERE, holding the lock that the answer
     * depends on. A second attach for a slot that already has a unit is not an
     * error — the device IS attached — it is simply nothing to do.
     */
    spin_lock(&g_units_lock);
    for (XhciMsdUnit* other = g_units; other; other = other->next) {
        if (other->slot == slot) {
            spin_unlock(&g_units_lock);
            pmm_free(cmd, 1);
            pmm_free(bounce, vmm_size_to_pages(MSD_BOUNCE_BYTES));
            kfree(u);
            return 0;               /* somebody else got here first */
        }
    }
    u->number = msd_next_number();
    u->next   = g_units;
    g_units   = u;
    spin_unlock(&g_units_lock);

    /* A fact, not a moment: the first mass-storage device this kernel has
     * configured. Nothing in a build without CTRLGIVEUP=on. */
    xhci_ctrl_giveup_proof(ctrl, slot);

    /* How many logical units. A device that does not implement the request
     * stalls it, and a stall here means exactly one — which is every flash
     * drive ever made. */
    uint8_t* lun_buf = (uint8_t*)u->bounce_virt;
    *lun_buf = 0;
    usb_setup_packet_t setup = {
        .bmRequestType = 0xA1,          /* device to host, class, interface */
        .bRequest = MSD_REQ_GET_MAX_LUN,
        .wValue = 0,
        .wIndex = slot->interface_num,
        .wLength = 1
    };
    int rc = xhci_control_transfer_sync(ctrl, slot, &setup, u->bounce_phys, 1,
                                        true, MSD_CTRL_TIMEOUT_MS);
    if (rc == TRB_COMPLETION_STALL) {
        msd_clear_halt(u, 1);
    }
    u->lun = 0;                         /* the first one, always */

    if (msd_wait_ready(u) != 0) {
        kprintf("[USB disk %u] never became ready\n", u->number);
        xhci_msd_release(slot);
        return -1;
    }

    if (msd_read_capacity(u) != 0) {
        kprintf("[USB disk %u] would not say how large it is\n", u->number);
        xhci_msd_release(slot);
        return -1;
    }

    msd_read_identity(u);

    /*
     * A device whose blocks are larger than a sector is addressed in its own
     * units and translated here, rather than refused.
     *
     * Refusing was honest while nothing did the translation, and it meant a
     * 4096-byte-sector drive was a drive BoxOS could not boot from. What it
     * needs is arithmetic and, for a write that does not land on a block
     * boundary, the read-modify-write below — which is not an optimisation to
     * be skipped but the only way to change part of a block on a device that
     * will only accept whole ones.
     *
     * What cannot be translated is a block that is not a whole number of
     * sectors, or one larger than the buffer everything moves through. Both
     * are said out loud rather than guessed at.
     */
    if (u->block_bytes < XHCI_MSD_SECTOR_BYTES ||
        (u->block_bytes % XHCI_MSD_SECTOR_BYTES) != 0 ||
        u->block_bytes > MSD_BOUNCE_BYTES) {
        kprintf("[USB disk %u] %s: %u-byte blocks, which this kernel cannot "
                "express in %u-byte sectors — not using it\n",
                u->number, u->name, u->block_bytes, XHCI_MSD_SECTOR_BYTES);
        xhci_msd_release(slot);
        return -1;
    }

    u->per_sector = u->block_bytes / XHCI_MSD_SECTOR_BYTES;
    u->sectors    = u->blocks * u->per_sector;
    u->ready      = true;

    uint64_t mib = (u->sectors * XHCI_MSD_SECTOR_BYTES) / (1024u * 1024u);
    kprintf("[USB disk %u] %s: %llu sectors, %llu MiB\n",
            u->number, u->name,
            (unsigned long long)u->sectors, (unsigned long long)mib);
    if (u->per_sector != 1) {
        kprintf("[USB disk %u] addressed in %u-byte blocks (%llu of them)\n",
                u->number, u->block_bytes, (unsigned long long)u->blocks);
    }

    /* What it is made of, when it was willing to say. Printed even when it
     * matches the logical block, because "512 logical over 4096 physical" and
     * "512 over 512" are different media and only one of them punishes a
     * misaligned write — and which one this is decides where a volume ought
     * to start on it. */
    if (u->phys_block_bytes != 0) {
        kprintf("[USB disk %u] built from %u-byte physical blocks%s\n",
                u->number, u->phys_block_bytes,
                u->lowest_aligned_lba ? ", offset from sector zero" : "");
        if (u->lowest_aligned_lba != 0) {
            kprintf("[USB disk %u] its first aligned block is logical %u — "
                    "anything laid out from zero is skewed on it\n",
                    u->number, u->lowest_aligned_lba);
        }
    } else {
        kprintf("[USB disk %u] would not say what it is built from; assuming "
                "nothing about alignment\n", u->number);
    }
    return 0;
}

int xhci_msd_attach(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    if (!ctrl || !slot || !slot->endpoints ||
        !slot->ep_bulk_in || !slot->ep_bulk_out) {
        return -1;
    }

    if (!xhci_slot_enter(slot)) {
        return -1;                      /* gone before we said hello */
    }
    int rc = msd_attach_held(ctrl, slot);
    xhci_slot_leave(slot);
    return rc;
}

bool xhci_msd_slot_attached(xhci_device_slot_t* slot)
{
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        if (u->slot == slot) {
            return true;
        }
    }
    return false;
}

void xhci_msd_release(xhci_device_slot_t* slot)
{
    if (!slot || !g_units_lock_ready) {
        return;
    }

    spin_lock(&g_units_lock);
    XhciMsdUnit** link = &g_units;
    XhciMsdUnit*  u    = NULL;
    while (*link) {
        if ((*link)->slot == slot) {
            u = *link;
            *link = u->next;
            break;
        }
        link = &(*link)->next;
    }
    spin_unlock(&g_units_lock);

    if (!u) {
        return;
    }

    if (u->ready) {
        kprintf("[USB disk %u] %s is gone\n", u->number, u->name);
    }

    /* Which unit it was. Read before the unit is freed, because it is the
     * whole of what the room is being told. */
    uint8_t number = u->number;

    if (u->cmd_phys)    pmm_free((void*)u->cmd_phys, 1);
    if (u->bounce_phys) pmm_free((void*)u->bounce_phys,
                                 vmm_size_to_pages(MSD_BOUNCE_BYTES));
    kfree(u);

    /*
     * And whoever is keeping a filesystem on it is told NOW, not at its next
     * read — and told WHICH unit left.
     *
     * A unit number is handed out again as soon as it is free, and a seat
     * holds a unit number — so a medium that leaves and another that arrives
     * before anybody touches the filesystem are, from the seat's point of
     * view, the same medium throughout. Measured: a stick pulled and pushed
     * back was never noticed to have gone at all, because nothing read from it
     * in between, and the seat went on pointing at whatever took the number.
     *
     * Naming the unit is what lets the room empty the right chair. Saying only
     * "something left" meant the room could not, so the chair stayed occupied
     * for ever — and the stick coming home found its own seat already taken by
     * its own ghost, which is silence exactly where an arrival should be.
     *
     * This runs from the service pass, which is ordinary kernel context, so
     * saying it here is allowed and is the last moment at which it is still
     * true that the number belongs to nobody.
     */
    BoardroomNoteDeparture(BOARD_USB, number);
}

/* ── sector I/O ─────────────────────────────────────────────────────────── */

uint8_t xhci_msd_unit_count(void)
{
    if (!g_units_lock_ready) return 0;
    uint8_t n = 0;
    spin_lock(&g_units_lock);
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        if (u->ready) n++;
    }
    spin_unlock(&g_units_lock);
    return n;
}

/*
 * Is there a disk here to speak to right now?
 *
 * Asked by whoever holds a seat number and needs to know whether the medium in
 * it is still there — so the answer has to be "could I start a transfer this
 * instant", not "is the bookkeeping still on the list". Those are different
 * moments: a device is marked as leaving where the unplug is noticed, and its
 * unit is unlinked later, in the pass that takes the device down. Between the
 * two, the old answer was yes.
 *
 * That gap was measurable. A filesystem above asked, was told the medium was
 * still there, and went on serving reads out of its block cache — which
 * happens above the disk driver and never asks it anything. One run in ten
 * assembled a whole program image that way and ran it.
 */
bool xhci_msd_unit_present(uint8_t unit)
{
    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return false;
    }
    msd_give_back(u);
    return true;
}

uint32_t xhci_msd_unit_max_run(uint8_t unit)
{
    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return MSD_BOUNCE_BYTES / XHCI_MSD_SECTOR_BYTES;
    }
    /* Whole device blocks, so a pass never has to split one — the same
     * arithmetic msd_rw does per pass. */
    uint32_t blocks = MSD_BOUNCE_BYTES / u->block_bytes;
    uint32_t run    = blocks * u->per_sector;
    msd_give_back(u);
    return run ? run : (MSD_BOUNCE_BYTES / XHCI_MSD_SECTOR_BYTES);
}

uint64_t xhci_msd_unit_sectors(uint8_t unit)
{
    if (!g_units_lock_ready) return 0;
    spin_lock(&g_units_lock);
    XhciMsdUnit* u = msd_find_locked(unit);
    uint64_t sectors = (u && u->ready) ? u->sectors : 0;
    spin_unlock(&g_units_lock);
    return sectors;
}

/* What this unit is built from, in bytes — the answer READ CAPACITY(16) gives
 * and almost every flash device gives differently from what it is addressed
 * in. Zero when the device would not say, which is a fact and not a failure. */
uint32_t xhci_msd_unit_physical_bytes(uint8_t unit)
{
    if (!g_units_lock_ready) return 0;
    spin_lock(&g_units_lock);
    XhciMsdUnit* u = msd_find_locked(unit);
    uint32_t bytes = (u && u->ready) ? u->phys_block_bytes : 0;
    spin_unlock(&g_units_lock);
    return bytes;
}

/* The name belongs to the unit, so it is only worth anything while the unit is
 * seated. Every caller copies it straight away, which is the only safe way to
 * use it and the only way it is used. */
const char* xhci_msd_unit_name(uint8_t unit)
{
    if (!g_units_lock_ready) return "";
    spin_lock(&g_units_lock);
    XhciMsdUnit* u = msd_find_locked(unit);
    const char* name = (u && u->ready) ? u->name : "";
    spin_unlock(&g_units_lock);
    return name;
}

/*
 * One run of device blocks, in or out of the bounce buffer.
 *
 * The command is chosen by the numbers rather than by a capability the device
 * was never asked about: ten bytes while the block number fits in the
 * thirty-two the ten-byte form has room for, sixteen when it does not. A drive
 * above two tebibytes is addressed correctly at both ends of itself, and one
 * below never sees a sixteen-byte command it might not implement.
 */
static int msd_run_blocks(XhciMsdUnit* u, uint64_t block, uint32_t nblocks,
                          uint32_t bytes, bool write)
{
    uint8_t cdb[16] = {0};
    uint8_t cdb_len;

    if (block + nblocks > 0x100000000ULL) {
        cdb[0] = write ? SCSI_WRITE_16 : SCSI_READ_16;
        be64_put(&cdb[2], block);
        be32_put(&cdb[10], nblocks);
        cdb_len = 16;
    } else {
        cdb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
        be32_put(&cdb[2], (uint32_t)block);
        cdb[7] = (uint8_t)(nblocks >> 8);
        cdb[8] = (uint8_t)nblocks;
        cdb_len = 10;
    }

    uint32_t moved = 0;
    int rc = msd_command(u, cdb, cdb_len, u->bounce_phys, bytes, !write, &moved);
    if (rc != 0 || moved != bytes) {
        uint8_t key = 0, asc = 0, ascq = 0;
        msd_request_sense(u, &key, &asc, &ascq);
        kprintf("[USB disk %u] %s of %u block(s) at %llu failed "
                "(sense key 0x%x, code 0x%02x/0x%02x)\n",
                u->number, write ? "write" : "read", nblocks,
                (unsigned long long)block, key, asc, ascq);
        return -1;
    }
    return 0;
}

/*
 * Sectors in, device blocks out.
 *
 * Everything above this speaks 512-byte sectors, which is what filesystems on
 * this kernel are laid out in. Most devices agree and the translation is the
 * identity. A device with larger blocks does not, and the difference is not
 * something a caller should have to know: a read is widened to the blocks that
 * contain it and the wanted part copied out, and a write that does not begin
 * and end on a block boundary reads the blocks it partly covers first, changes
 * the middle, and writes them back whole. There is no other way to change part
 * of a block on a device that will only accept whole ones — and doing it by
 * writing a partial block instead destroys the sectors on either side.
 */
static int msd_rw(uint8_t unit, uint64_t lba, uint32_t count,
                  void* buffer, bool write)
{
    if (!buffer || count == 0) {
        return -1;
    }

    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return -1;                      /* gone, or on its way out */
    }
    if (lba + count > u->sectors) {
        kprintf("[USB disk %u] request for sectors %llu..%llu, and it has "
                "%llu\n", unit, (unsigned long long)lba,
                (unsigned long long)(lba + count - 1),
                (unsigned long long)u->sectors);
        msd_give_back(u);
        return -1;
    }

    const uint32_t per_sector  = u->per_sector;          /* sectors per block */
    const uint32_t block_bytes = u->block_bytes;
    /* Whole blocks per pass, so a pass never has to split one. */
    const uint32_t blocks_per_pass = MSD_BOUNCE_BYTES / block_bytes;

    uint8_t* caller = (uint8_t*)buffer;
    int result = 0;

    msd_gate_enter(u);

    while (count > 0) {
        uint64_t block     = lba / per_sector;
        uint32_t head      = (uint32_t)(lba % per_sector);   /* sectors in */
        uint32_t room      = (blocks_per_pass * per_sector) - head;
        uint32_t chunk     = (count > room) ? room : count;  /* sectors */
        uint32_t nblocks   = (head + chunk + per_sector - 1) / per_sector;
        uint32_t bytes     = nblocks * block_bytes;
        uint32_t head_bytes = head * XHCI_MSD_SECTOR_BYTES;
        uint32_t chunk_bytes = chunk * XHCI_MSD_SECTOR_BYTES;

        bool whole = (head == 0) && ((chunk % per_sector) == 0);

        /* A write that does not cover the blocks it touches has to read them
         * first; a read always reads. */
        if (!write || !whole) {
            if (msd_run_blocks(u, block, nblocks, bytes, false) != 0) {
                result = -1;
                break;
            }
        }

        if (write) {
            memcpy((uint8_t*)u->bounce_virt + head_bytes, caller, chunk_bytes);
            if (msd_run_blocks(u, block, nblocks, bytes, true) != 0) {
                result = -1;
                break;
            }
        } else {
            memcpy(caller, (uint8_t*)u->bounce_virt + head_bytes, chunk_bytes);
        }

        caller += chunk_bytes;
        lba    += chunk;
        count  -= chunk;
    }

    msd_gate_leave(u);
    msd_give_back(u);
    return result;
}


/* ── a read for somebody who did not stay ───────────────────────────────── */

/*
 * The same BOT job, with the other driver on the handle.
 *
 * Everything above this — the Boardroom, the storage deck, a process reading a
 * file — has had exactly one way to get sectors off a flash drive: ask, and
 * stand there. That is what made a machine booted from a stick spend a core on
 * every block of every file, while the same machine booted from a SATA disk
 * parked the caller and got on with something else. The difference was never
 * the hardware; the USB path was simply never joined to the completion spine
 * the SATA path has used all along.
 *
 * A request that finds the device busy is LEFT here rather than refused. Its
 * owner is not standing over it and cannot be told to try again, and the layer
 * above has no way to fall back once it has committed to waiting — so a refusal
 * would surface as a failed read on a perfectly good disk.
 */
typedef struct MsdAsyncReq {
    struct MsdAsyncReq* next;
    MsdJob         job;             /* carries the never-drop completion node */
    XhciMsdUnit*   u;
    XhciMsdAsyncCb cb;
    void*          ctx;
    uint8_t        unit_number;
} MsdAsyncReq;

static void msd_async_done(void* ctx, int status, uint32_t transferred);

/*
 * Start whatever is at the head of the queue, if the turn is free.
 *
 * Written as a loop rather than as a call from the completion path: a request
 * whose submission fails finishes immediately, which frees the turn again, and
 * a chain of those would otherwise recurse as deep as the queue is long.
 */
static void msd_start_queued(XhciMsdUnit* u)
{
    for (;;) {
        spin_lock(&u->q_lock);
        MsdAsyncReq* r = u->q_head;
        if (!r) {
            spin_unlock(&u->q_lock);
            return;
        }
        spin_unlock(&u->q_lock);

        if (!msd_gate_try_enter(u)) {
            return;                 /* somebody else has it; they will hand over */
        }

        /* Unlinked only once the turn is actually held, so two cores cannot
         * both take the same request off the front. */
        spin_lock(&u->q_lock);
        r = u->q_head;
        if (!r) {
            spin_unlock(&u->q_lock);
            __atomic_store_n(&u->busy, 0u, __ATOMIC_RELEASE);
            return;
        }
        u->q_head = r->next;
        if (!u->q_head) {
            u->q_tail = NULL;
        }
        spin_unlock(&u->q_lock);

        r->next = NULL;
        msd_watch_arm(u, &r->job);
        msd_job_begin(&r->job);

        /* If it finished inside that call (a submission that could not be
         * made), the turn is free again and the next one may go now. */
        if (!__atomic_load_n(&r->job.finished, __ATOMIC_ACQUIRE)) {
            return;
        }
    }
}

/*
 * The job is over, and this runs on a K-Core.
 *
 * The order matters: the device is let go of first, so the turn is available
 * to the next request before the caller above is woken and possibly asks for
 * another block; then the request's memory goes; then the answer is given.
 */
static void msd_async_done(void* ctx, int status, uint32_t transferred)
{
    MsdAsyncReq* r = (MsdAsyncReq*)ctx;
    XhciMsdUnit* u = r->u;

    msd_watch_disarm(u);

    XhciMsdAsyncCb cb   = r->cb;
    void*          cctx = r->ctx;
    uint8_t        unit = r->unit_number;
    uint32_t       want = r->job.data_len;

    /* A device that carried the command out but moved fewer bytes than were
     * asked for has not answered the question that was put to it. */
    error_t st = (status == 0 && transferred == want) ? OK : ERR_IO;

    kfree(r);

    /* The turn, then the visit. Both belong to the request and neither may
     * outlive it. */
    msd_gate_leave(u);
    xhci_slot_leave(u->slot);

    if (cb) {
        cb(unit, 0, st, cctx);
    }
}

void xhci_msd_watchdog(void)
{
    if (!g_units_lock_ready) {
        return;
    }

    /*
     * The list is walked under its lock and the giving-up is done outside it:
     * finishing a job runs the caller's completion, and that is not something
     * to do with a spinlock held — least of all this one, which every other
     * unit operation needs.
     */
    MsdJob*      late_job  = NULL;
    XhciMsdUnit* late_unit = NULL;

    bool gone = false;

    spin_lock(&g_units_lock);
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        MsdJob* j = __atomic_load_n(&u->watched, __ATOMIC_ACQUIRE);
        if (!j || __atomic_load_n(&j->finished, __ATOMIC_ACQUIRE)) {
            continue;
        }

        /*
         * ‼ THE FACT FIRST, AND IT IS WHY THIS PASS EXISTS AT ALL.
         *
         * A caller that stayed asks this every pass of its own loop. One that
         * did not stay has nobody to ask it — so a read left on a device that
         * has been pulled used to wait out the WHOLE budget before anything
         * noticed, and for every one of those milliseconds the slot could not
         * be taken down, the unit number could not come back, and the medium
         * could not be announced as gone. With several reads outstanding that
         * is the budget over and over, one after another.
         *
         * The device having left is a fact — the slot's tenancy and the port's
         * own register — and it ends the wait now.
         */
        bool here = msd_device_is_there(u);
        uint64_t since = __atomic_load_n(&u->watched_since, __ATOMIC_RELAXED);
        if (here && (int64_t)(rdtsc() - since) <
                        (int64_t)cpu_ms_to_tsc(msd_patience_ms(j->data_len + j->behind))) {
            continue;
        }
        /* Claimed here, so a second pass on another core cannot give up on the
         * same job twice. */
        if (__atomic_exchange_n(&u->watched, NULL, __ATOMIC_ACQ_REL) != j) {
            continue;
        }
        late_job  = j;
        late_unit = u;
        gone      = !here;
        break;                  /* one per pass is plenty; the next comes round */
    }
    spin_unlock(&g_units_lock);

    if (!late_job) {
        return;
    }

    if (gone) {
        msd_note_gone(late_unit, "a read nobody was waiting on was outstanding");
        xhci_ep_abandon(late_unit->ctrl, late_unit->slot, late_job->dci);
        msd_job_finish(late_job, -1);
        return;
    }

    kprintf("[USB disk %u] a read nobody was waiting on has been asking for "
            "more time for %u ms at stage %u — giving up on the command\n",
            late_unit->number,
            msd_patience_ms(late_job->data_len + late_job->behind),
            late_job->phase);
    /* The transfer comes off the endpoint before anything else happens — see
     * the note on the same call in msd_job_run. It matters more here: this job
     * carries a completion node, and the node lives inside memory that
     * msd_job_finish is about to hand back. */
    xhci_ep_abandon(late_unit->ctrl, late_unit->slot, late_job->dci);
    msd_bot_reset(late_unit);
    msd_job_finish(late_job, -1);
}

bool xhci_msd_unit_can_read_async(uint8_t unit)
{
    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return false;
    }
    /* Nothing about the geometry rules it out: a request is refused per-request
     * if it does not sit on whole blocks, and the reads that come down here are
     * filesystem blocks, which do. */
    msd_give_back(u);
    return true;
}

error_t xhci_msd_read_async(uint8_t unit, uint64_t lba, uint32_t count,
                            void* dma_phys, XhciMsdAsyncCb cb, void* ctx)
{
    if (!dma_phys || count == 0) {
        return ERR_INVALID_ARGUMENT;
    }

    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return ERR_DEVICE_NOT_READY;
    }

    if (lba + count > u->sectors) {
        msd_give_back(u);
        return ERR_INVALID_ARGUMENT;
    }

    /*
     * Whole device blocks, starting on one. Said out loud rather than worked
     * around: widening the request would need the bounce buffer and a copy out
     * of it, and the copy is exactly what the caller is not here to do.
     */
    const uint32_t per_sector = u->per_sector;
    if ((lba % per_sector) != 0 || (count % per_sector) != 0) {
        msd_give_back(u);
        return ERR_INVALID_ARGUMENT;
    }
    uint32_t nblocks = count / per_sector;
    uint32_t bytes   = nblocks * u->block_bytes;
    if (bytes > MSD_BOUNCE_BYTES) {
        msd_give_back(u);
        return ERR_INVALID_ARGUMENT;
    }

    MsdAsyncReq* r = (MsdAsyncReq*)kmalloc(sizeof(MsdAsyncReq));
    if (!r) {
        msd_give_back(u);
        return ERR_NO_MEMORY;
    }
    memset(r, 0, sizeof(*r));
    r->u           = u;
    r->cb          = cb;
    r->ctx         = ctx;
    r->unit_number = unit;

    MsdJob* j = &r->job;
    j->u             = u;
    j->hand_to_kcore = true;
    j->data_phys     = (uint64_t)dma_phys;
    j->data_len      = bytes;
    j->data_in       = true;
    j->done          = msd_async_done;
    j->done_ctx      = r;

    uint64_t block = lba / per_sector;
    if (block + nblocks > 0x100000000ULL) {
        j->cdb[0] = SCSI_READ_16;
        be64_put(&j->cdb[2], block);
        be32_put(&j->cdb[10], nblocks);
        j->cdb_len = 16;
    } else {
        j->cdb[0] = SCSI_READ_10;
        be32_put(&j->cdb[2], (uint32_t)block);
        j->cdb[7] = (uint8_t)(nblocks >> 8);
        j->cdb[8] = (uint8_t)nblocks;
        j->cdb_len = 10;
    }

    /*
     * The visit is NOT given back here. It is what keeps the endpoints, the
     * rings and the buffers alive while the transfer is in flight, and the
     * transfer outlives this call by design — msd_async_done gives it back.
     */
    if (msd_gate_try_enter(u)) {
        msd_watch_arm(u, j);
        msd_job_begin(j);
        return OK;
    }

    spin_lock(&u->q_lock);
    if (u->q_tail) {
        u->q_tail->next = r;
    } else {
        u->q_head = r;
    }
    u->q_tail = r;
    spin_unlock(&u->q_lock);

    /* And in case the holder finished between the failed try and the link. */
    msd_start_queued(u);
    return OK;
}

int xhci_msd_read(uint8_t unit, uint64_t lba, uint32_t count, void* buffer)
{
    return msd_rw(unit, lba, count, buffer, false);
}

int xhci_msd_write(uint8_t unit, uint64_t lba, uint32_t count, const void* buffer)
{
    return msd_rw(unit, lba, count, (void*)buffer, true);
}

int xhci_msd_flush(uint8_t unit)
{
    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return -1;
    }

    /* SYNCHRONIZE CACHE with a zero block count means "all of it". A device
     * that does not implement it refuses, and a refusal here is not a failure:
     * a device with no write cache has nothing to synchronise. */
    uint8_t cdb[16] = {0};
    uint8_t cdb_len;
    if (u->blocks > 0x100000000ULL) {
        cdb[0] = SCSI_SYNC_CACHE_16;
        cdb_len = 16;
    } else {
        cdb[0] = SCSI_SYNC_CACHE_10;
        cdb_len = 10;
    }

    msd_gate_enter(u);
    int rc = msd_command(u, cdb, cdb_len, 0, 0, false, NULL);
    msd_gate_leave(u);
    msd_give_back(u);

    return (rc >= 0) ? 0 : -1;
}
