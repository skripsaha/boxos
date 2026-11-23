/*
 * Hardware Deck — Manifest-native handlers (Phase 8 complete).
 *
 * Every prefix-chain handler in hardware_deck.c has a corresponding op here,
 * but the ABI is freed from the 192-byte cargo cult: bytes flow through
 * Crates of arbitrary size, fixed inputs through op->params.
 *
 * Param/Crate layouts are documented inline next to each handler so the
 * userspace builder can be regenerated mechanically.
 */

#include "klib.h"
#include "klib_logring.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "hardware_deck.h"
#include "crate_io.h"
#include "process.h"
#include "screen.h"
#include "crossing.h"      /* the display holding the glass lands the start */
#include "emergency_screen.h"
#include "pit.h"
#include "rtc.h"
#include "keyboard.h"
#include "tagfs.h"
#include "boardroom.h"
#include "system_halt.h"
#include "xhci.h"
#include "xhci_port.h"
#include "xhci_enumeration.h"
#include "touch.h"
#include "kring.h"
#include "kernel_config.h"

/* =========================================================================
 *  Screen — the glass, lent to the one program that draws (screen.h)
 *
 *  HW_SCREEN_CLAIM    out_crate: ScreenDescription (boxos_screen.h)
 *  HW_SCREEN_RELEASE  nothing in, nothing out
 *
 *  SYSTEM only: whoever holds the screen holds every pixel of every program
 *  and the kernel's own lines with them.
 * ========================================================================= */

static int HwScreenClaim(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(ScreenDescription)) return ERR_BUFFER_TOO_SMALL;

    ScreenDescription d;
    bool taken = false;
    error_t rc = ScreenClaim(ctx->proc, &d, &taken);
    if (rc != OK) return rc;
    if (crate_write(out, ctx, &d, sizeof(d)) != OK) {
        /* A claim that cannot be told is no claim — but only this one goes
         * back: a holder asking again keeps what it already held. */
        if (taken) (void)ScreenRelease(ctx->proc);
        return ERR_INVALID_ADDRESS;
    }
    /* The display holding the glass is the start come up: its crossing is
     * marked landed (crossing.c), with nothing held here. */
    if (taken) CrossingLand();
    return OK;
}

static int HwScreenRelease(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    return ScreenRelease(ctx->proc);
}

/* =========================================================================
 *  Timer
 * ========================================================================= */

static int HwTimerGetTicks(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    uint64_t ticks = pit_get_ticks();
    if (crate_write(out, ctx, &ticks, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwTimerGetMs(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    /* Use the dedicated monotonic uptime counter (advanced per-tick by
     * 1_000_000/freq µs) instead of deriving from `ticks * 1000 / freq`,
     * which is NOT monotonic when the scheduler reprograms the PIT under
     * load. The old derivation produced backwards-jumps causing S1's
     * "elapsed=0xFFFFFFFFFFFF…" underflow. */
    uint64_t ms = pit_get_uptime_ms();
    if (crate_write(out, ctx, &ms, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwTimerGetFreq(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_BUFFER_TOO_SMALL;

    uint32_t freq = pit_get_frequency();
    if (crate_write(out, ctx, &freq, sizeof(uint32_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  RTC
 * ========================================================================= */

static int HwRtcGetUnix64(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    uint64_t secs = rtc_get_unix64();
    if (crate_write(out, ctx, &secs, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_RTC_GET_TIME  out_crate: BoxTime (20 bytes packed) */
static int HwRtcGetTime(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(BoxTime)) return ERR_BUFFER_TOO_SMALL;

    BoxTime t;
    rtc_get_boxtime(&t);
    if (crate_write(out, ctx, &t, sizeof(BoxTime)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_RTC_GET_UPTIME  out_crate: u64 ns */
static int HwRtcGetUptime(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    uint64_t ns = rtc_get_uptime_ns();
    if (crate_write(out, ctx, &ns, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  ‼ WHAT THIS DECK DELIBERATELY NO LONGER OFFERS
 *
 *  Fourteen operations were withdrawn. A deck answers for the hardware with
 *  NAMED operations the kernel understands and can carry through properly:
 *  hw.usb.reset is the whole eleven-step repair of a controller, hw.disk.info
 *  is the drive as the driver knows it, hw.screen.claim lends the glass to the
 *  program that draws. None of the fourteen was one of those.
 *
 *  hw.port.inb/outb/inw/outw/inl/outl were the `in` and `out` instructions
 *      with a name put on them — the screwdriver taken out of the lock, with
 *      which every other lock in the house can be worked. The port
 *      was assembled from two parameter bytes, so all 65536 of them were
 *      reachable, and not one of the six ever looked at the address it was
 *      handed. One call rewrote BAR0 of an xHCI controller, which the driver
 *      reads exactly once at bring-up and never again (xhci.c): the silicon
 *      then decodes a new window while the driver keeps writing to the old
 *      one, and the USB volume is simply gone with nothing in the log to say
 *      so. One call cleared the NMI mask through bit 7 of port 0x70 — a
 *      register the RTC never reads back and therefore tracks only as a
 *      variable of its own (rtc.c), so the kernel would go on believing NMI
 *      was armed. Three calls were the chipset reset sequence on 0xCF9. A
 *      fixed list of permitted ports could not have helped: five of the
 *      twenty ranges this kernel drives — BMIDE from BAR4, and SMI_CMD,
 *      PM1a/PM1b, PM_TMR, GPE0/GPE1 from the FADT — are not known until the
 *      machine is running and differ from board to board.
 *
 *  hw.irq.register/unregister/enable/disable/isr/irr/eoi were the
 *      same hole one storey up. The check they shared refused line 0 and line
 *      2 and nothing else, so every other line could be masked from outside:
 *      the keyboard on 1, the Wire on 4, the disk on 14 and 15 — or somebody
 *      else's EOI could be sent through the middle of their handler. Two of
 *      the seven, register and unregister, never had a handler at all: they were names
 *      in a header and nothing else, and the registry had never heard of them.
 *
 *  hw.cpu.halt could not be made honest. A Manifest arrives through
 *      SYSCALL with IF clear — SFMASK clears it (amp/per_core.c) and nothing
 *      between there and a handler raises it again (idt.c says so of the
 *      dispatch itself) — so the bare `hlt` this op ran was a halt with
 *      interrupts masked: on a single-core machine the kernel stopped there
 *      until an NMI. What it was reaching for exists already and is woken by a
 *      real event rather than a guess: system.turn_in (SYSTEM_OP_TURN_IN,
 *      boxos_decks.h) lays a strand down until something addressed to it moves
 *      a cursor, and the machine as a whole goes quiet through Turn In and the
 *      Bell without anybody having to ask it to.
 * ========================================================================= */

/* =========================================================================
 *  Disk: the volumes' media committed. The disks and their tables are
 *  hardware_disks.c's.
 * ========================================================================= */

/* HW_DISK_FLUSH  params:[u8 is_master]
 *
 * The ABI still carries the legacy 0/1 master/slave flag, but the only durable
 * thing it can mean today is "commit the TagFS volumes" — every mounted
 * volume of the house, each one's medium, which are the devices any commit
 * really cares about; the first refusal is the answer. tagfs_flush_cache()
 * resolves where a volume lives at the storage layer, so this stays correct
 * on every (ATA, AHCI, USB) × (boot port) combination. The parameter
 * itself names nothing this op can act on and is read only to keep the wire
 * shape.
 *
 * UTILITY, not NONE. It takes no file and names nothing belonging to the
 * caller: it makes the WHOLE medium commit, and every program on the machine
 * pays for the pause. A program that wants its own writing to be durable has
 * the operation that was built for it — storage.anchor (OP_AUTH_APP) waits
 * for a Settle cycle and this flush together and then tells the file's watchers —
 * so nothing an app can legitimately want is behind this level.
 */
static int HwDiskFlush(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    (void)op->params[0];
    error_t first = OK;
    for (TagFSVolume* vol = TagFSVolumeNext(NULL); vol; vol = TagFSVolumeNext(vol)) {
        if (!TagFSVolumeMounted(vol))
            continue;
        error_t rc = tagfs_flush_cache(vol);
        if (rc != OK && first == OK)
            first = rc;
    }
    return first;
}


/* =========================================================================
 *  System power (reboot / shutdown — noreturn)
 * ========================================================================= */

/* kprintf, not debug_printf: this is the last line either op says before
 * system_halt()'s one-way reset/shutdown ladder runs, and debug_printf
 * compiles to nothing outside a CONFIG_DEBUG_ENABLED build (klib.h) — on
 * real hardware there is no such build, so "who asked for this" would
 * never reach the log at all. */
static int HwSystemReboot(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    kprintf("[HardwareDeck] reboot requested by PID %u\n",
            (ctx && ctx->proc) ? ctx->proc->pid : 0);
    system_halt(true);
    return OK; /* unreachable */
}

static int HwSystemShutdown(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                            const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    kprintf("[HardwareDeck] shutdown requested by PID %u\n",
            (ctx && ctx->proc) ? ctx->proc->pid : 0);
    system_halt(false);
    return OK; /* unreachable */
}

/* =========================================================================
 *  USB (xHCI control surface)
 * ========================================================================= */

/*
 * ‼ WHAT THIS SURFACE DELIBERATELY NO LONGER OFFERS
 *
 * `hw.usb.init` and `hw.usb.stop` are gone. They were not operations.
 * They were two of the driver's own boot steps, exposed by name to anything
 * carrying the system tag.
 *
 *   init  ran xhci_init() a second time. That begins with a memset of the whole
 *         device-slot table — every live device on the machine, including the
 *         medium the filesystem is mounted from, silently becomes idle — and
 *         then finds the SAME PCI function again and brings it up a second time
 *         into a second controller structure: a second MMIO mapping, a second
 *         set of rings and DCBAA (the first leaks), a reset that drops the bus,
 *         and MSI moved to the neighbouring vector so interrupts arrive at the
 *         new structure while the old one is drained for ever by every "for
 *         each controller" loop in the driver.
 *
 *   stop  returned OK and did nothing at all, and said so in its own comment.
 *         There is no xhci_stop, and halting the controller a machine reads its
 *         volume through is not an operation anybody wants offered by name.
 *
 * What replaced them is one honest thing: hw.usb.reset now means "put this
 * controller back in service", which is the whole eleven-step repair the driver
 * already implements — not the single step it used to run.
 */

/*
 * Every USB op below names the controller it is about, as its first parameter.
 *
 * A machine has as many USB controllers as it has: one on the chipset where
 * the sockets on the case are, and very often another on a graphics card. Which
 * of them a bus walk reaches first is decided by topology — a PCIe bridge sits
 * at device 1 and the chipset controller at device 0x14, so a depth-first walk
 * meets the graphics card first. "The controller" names nothing.
 *
 * Controllers are numbered from zero in the order they were brought up, and an
 * index past the last one is refused. That is also how a caller learns how many
 * there are: ask, and be told no.
 */
static xhci_controller_t *usb_named_controller(const ManifestOp *op)
{
    if (op->param_size < 1) return NULL;
    return xhci_controller_at(op->params[0]);
}

/*
 * HW_USB_RESET  params:[u8 controller] — put this controller back in service.
 *
 * The whole repair, not the first step of it. What this used to be was a bare
 * xhci_reset: the controller was halted and cleared, and then nothing — its
 * registers left at zero, this driver's own `running` and `initialized` still
 * claiming it was healthy, `error_state` still clear so the automatic repair
 * would never come for it, and every device slot still naming a device the
 * silicon had just forgotten. One system-authorised call and the machine's USB
 * was dead until it was switched off and on, with no line anywhere saying so.
 *
 * A door, not a screwdriver from the lock.
 */
static int HwUsbReset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    xhci_controller_t *c = usb_named_controller(op);
    if (!c) return ERR_NOT_INITIALIZED;

    kprintf("[HardwareDeck] PID %u asked for the controller on %s to be put "
            "back in service\n",
            (ctx && ctx->proc) ? ctx->proc->pid : 0, c->name);

    return xhci_put_back_in_service(c) ? OK : ERR_INTERNAL;
}

static int HwUsbStart(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    xhci_controller_t *c = usb_named_controller(op);
    if (!c) return ERR_NOT_INITIALIZED;
    return xhci_start(c) == 0 ? OK : ERR_INTERNAL;
}

/* HW_USB_PORT_STATUS  params:[u8 controller, u8 port]  out_crate: u32 portsc */
static int HwUsbPortStatus(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[1];
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->initialized) return ERR_NOT_INITIALIZED;
    if (port == 0 || port > c->max_ports) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_BUFFER_TOO_SMALL;
    uint32_t portsc = xhci_get_port_status(c, port);

    if (crate_write(out, ctx, &portsc, sizeof(uint32_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_USB_PORT_RESET  params:[u8 controller, u8 port] */
static int HwUsbPortReset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[1];

    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->initialized)            return ERR_NOT_INITIALIZED;
    if (port == 0 || port > c->max_ports) return ERR_INVALID_ARGUMENT;
    if (!xhci_port_has_device(c, port))   return ERR_DEVICE_NOT_READY;

    /* Starting the reset is the whole of the operation. The port announces
     * its own completion through a port-status change, and the enumeration
     * state machine is what listens for it — so this returns as soon as the
     * reset is in flight rather than holding a caller for the tens of
     * milliseconds the hardware takes. A port that needed no reset (a USB 3
     * link that trained itself) reports success without touching it. */
    int rc = xhci_port_begin_reset(c, port, NULL);
    return rc >= 0 ? OK : ERR_INTERNAL;
}

/* HW_USB_PORT_QUERY  params:[u8 controller]
 * out_crate:[u8 max_ports][u8 max_slots][u8 irq][u8 polling][u8 controllers]
 *
 * The last byte is how many controllers there are, so a caller that wants to
 * walk them all learns the number from the first one it asks rather than by
 * counting refusals. */
static int HwUsbPortQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->initialized) return ERR_NOT_INITIALIZED;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 5) return ERR_BUFFER_TOO_SMALL;
    uint8_t blob[5];
    blob[0] = c->max_ports;
    blob[1] = c->max_slots;
    blob[2] = c->irq_line;
    blob[3] = c->use_polling ? 1 : 0;
    blob[4] = xhci_controller_count();
    if (crate_write(out, ctx, blob, 5) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_USB_ENUM_DEVICE  params:[u8 controller, u8 port] */
static int HwUsbEnumDevice(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[1];
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->running) return ERR_NOT_INITIALIZED;

    int rc = xhci_enumerate_device(c, port);
    if (rc == 0)        return OK;
    if (rc == -2 || rc == -3) return ERR_INVALID_ARGUMENT;
    return ERR_INTERNAL;
}

/* HW_USB_GET_INFO  params:[u8 slot_id]  out_crate:[u8 slot][u8 port][u8 state] */
static int HwUsbGetInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t slot_id = op->params[1];
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->running) return ERR_NOT_INITIALIZED;
    xhci_device_slot_t *slot = xhci_get_device_slot(c, slot_id);
    if (!slot) return ERR_DEVICE_NOT_READY;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 3) return ERR_BUFFER_TOO_SMALL;
    uint8_t blob[3];
    blob[0] = slot->slot_id;
    blob[1] = slot->port_num;
    blob[2] = slot->state;
    if (crate_write(out, ctx, blob, 3) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* ── proving the door, on the machine it has to hold ──────────────────────────
 *
 * A boot self-test rather than a unit test, for the reason the Boardroom's and
 * the ring's already give: the thing being tested is a conversation with the
 * silicon in front of it. Whether a controller comes back from a reset, whether
 * the devices on it are found again, and whether the keyboard a person is
 * typing on survives the whole of it are facts about THIS machine and cannot be
 * established anywhere else.
 *
 * ‼ IT IS THE OP THAT IS PROVED, NOT THE DRIVER BEHIND IT. The handler is
 * reached the way the dispatcher reaches it — looked up in the registry by
 * op_kind, called with a ManifestOp built the way a caller builds one. A test
 * that called xhci_put_back_in_service directly would prove the driver and say
 * nothing about the surface, and the surface is what had two ops on it that
 * broke the machine.
 *
 * WHEN, and it is a fact rather than a clock: the first pass through the idle
 * or guide loop on which this machine has a controller in service AND a volume
 * mounted. Before that there is nothing to lose and the proof would prove
 * nothing; there is no moment to wait out and no deadline to expire.
 *
 * Gated behind USBRECOVER=on. This costs the machine every USB device it has,
 * once, and is not something a shipped build does to itself.
 */
#if CONFIG_USB_RECOVER_PROOF
void HardwareDeckUsbRecoverProof(void)
{
    static volatile uint32_t done = 0;

    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != 0)   return;
    if (xhci_controller_count() == 0)                    return;
    if (tagfs_get_seat(TagFSHome()) == BOARDROOM_NO_SEAT)           return;
    if (__atomic_exchange_n(&done, 1u, __ATOMIC_ACQUIRE) != 0) return;

    kprintf("[USB RECOVER TEST] begin — a controller in service and a volume "
            "on seat %u\n", tagfs_get_seat(TagFSHome()));

    const OpRegistration *reg =
        OpRegistryLookup(OP_KIND(HARDWARE_DECK_ID, HW_USB_RESET));
    if (!reg || !reg->handler) {
        kprintf("[USB RECOVER TEST] FAILED: hw.usb.reset is not registered\n");
        return;
    }
    if (reg->security_mask != OP_AUTH_SYSTEM) {
        kprintf("[USB RECOVER TEST] FAIL: hw.usb.reset is not system-only\n");
    } else {
        kprintf("[USB RECOVER TEST] PASS: hw.usb.reset is registered, "
                "system-only\n");
    }

    /* One op, one parameter: which controller. Built the way a caller builds
     * one, packed so `params` really does begin where the header ends. */
    struct __packed {
        ManifestOp head;
        uint8_t    params[1];
    } req;
    req.head.op_kind    = OP_KIND(HARDWARE_DECK_ID, HW_USB_RESET);
    req.head.flags      = 0;
    req.head.in_crate   = CRATE_INDEX_NONE;
    req.head.out_crate  = CRATE_INDEX_NONE;
    req.head.param_size = 1;
    req.params[0]       = 0;                    /* the first controller */

    OpContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    int rc = reg->handler(&req.head, NULL, 0, &ctx);

    if (rc == OK) {
        kprintf("[USB RECOVER TEST] %[S]PASSED%[D]: the controller was put back "
                "in service through hw.usb.reset\n");
    } else {
        kprintf("[USB RECOVER TEST] %[R]FAILED%[D]: hw.usb.reset answered %d\n",
                rc);
    }
}
#endif /* CONFIG_USB_RECOVER_PROOF */

/* =========================================================================
 *  Debug print — serial output from userspace via kernel kprintf
 *  HW_DEBUG_PRINT  in_crate: NUL-terminated string (max 256 bytes)
 *
 *  APP, not NONE. What this writes goes into the same log ring the kernel
 *  writes its own account into, and that ring is what the Wire reads out of
 *  COM1 and what `logsave` writes down — on a board it is the only report
 *  that comes back. Every boxlib program can reach it, because boxlib's own
 *  refusal paths speak through kdbg_print and the plainest program on the
 *  volume carries `app`; nothing below that has any business in the kernel's
 *  record of itself, and a frozen process (AUTH_TAG_STOPPED) now cannot write
 *  there at all, which at NONE it could — auth_level_permits answers a NONE
 *  op before it ever looks at the stopped bit.
 * ========================================================================= */

static int HwDebugPrint(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *c = &crates[op->in_crate];
    uint64_t bytes = c->size < 256 ? c->size : 256;
    if (bytes == 0) return ERR_INVALID_ARGUMENT;

    char buf[257];
    if (crate_read(c, ctx, buf, bytes) != OK) return ERR_INVALID_ADDRESS;
    buf[bytes] = '\0';

    uint32_t pid = (ctx && ctx->proc) ? ctx->proc->pid : 0;
    kprintf("[%u] %s\n", pid, buf);
    return OK;
}

/* =========================================================================
 *  Log ring — what this kernel has said, asked for from userspace
 *
 *  HW_LOG_READ  params:    [u64 from]  — ring position to read from
 *                          [u32 flags] — optional; HW_LOG_READ_TONES
 *               out_crate: [u64 oldest][u64 written][u64 copied][bytes...]
 *                          and, with HW_LOG_READ_TONES, the Tone of every
 *                          byte copied straight after them [tones...]
 *
 *  Self-describing on purpose: `oldest` says where the ring actually begins
 *  now, so a reader the writers overtook learns the size of its gap instead
 *  of splicing two ends of the log together and believing the seam.
 *
 *  Bounded to a page per call. LogRingRead copies with interrupts off, and
 *  the length of that window is the only price the rest of the machine pays
 *  for being asked what it said; a reader that wants more asks again.
 * ========================================================================= */

/* One reader for both banks: the log this boot is still writing, and the one
 * the boot before it left behind. They differ only in which function supplies
 * the bytes, and a second copy of the crate arithmetic would be a second place
 * for the header offsets to drift. */
typedef uint64_t (*LogSource)(uint64_t, void *, uint64_t, uint64_t *, uint64_t *);

static int HwLogReadFrom(LogSource source, const ManifestOp *op, Crate *crates,
                         const OpContext *ctx)
{
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8)                return ERR_INVALID_ARGUMENT;

    uint64_t from = 0;
    for (unsigned i = 0; i < 8; i++) {
        from |= (uint64_t)op->params[i] << (i * 8);
    }

    Crate *out = &crates[op->out_crate];
    if (out->capacity < HW_LOG_READ_HEADER) return ERR_BUFFER_TOO_SMALL;

    uint64_t room = out->capacity - HW_LOG_READ_HEADER;
    if (room > HW_LOG_READ_CHUNK) room = HW_LOG_READ_CHUNK;

    uint8_t *kp = crate_out_alloc(out, HW_LOG_READ_HEADER + room);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint64_t oldest = 0, written = 0;
    uint64_t copied = source(from, kp + HW_LOG_READ_HEADER, room,
                             &oldest, &written);

    memcpy(kp,      &oldest,  sizeof(uint64_t));
    memcpy(kp + 8,  &written, sizeof(uint64_t));
    memcpy(kp + 16, &copied,  sizeof(uint64_t));

    int crc = crate_out_commit(out, ctx, kp, HW_LOG_READ_HEADER + copied);
    crate_buf_free(kp);
    return (crc == OK) ? OK : ERR_INVALID_ADDRESS;
}

/* What the run before this one said. A run with no carry-over window armed
 * (klib_logring.h, LogKeepInit) says so, rather than answering with an empty
 * log that reads exactly like a machine that never spoke; an empty answer
 * from a run that HAS the window means nothing came through the last reset,
 * which is a different fact and one the caller is left to report. */
static int HwLogPrevious(const ManifestOp *op, Crate *crates,
                         uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!LogKeepWindow(NULL, NULL)) return ERR_UNSUPPORTED;
    return HwLogReadFrom(LogKeepPreviousRead, op, crates, ctx);
}

/* The same read with each byte's Tone after the bytes: half the room for
 * each. The previous boot's bank keeps no tones, so only this run's ring
 * answers this form. */
static int HwLogReadTones(const ManifestOp *op, Crate *crates, const OpContext *ctx)
{
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint64_t from = 0;
    memcpy(&from, op->params, sizeof(from));

    Crate *out = &crates[op->out_crate];
    if (out->capacity < HW_LOG_READ_HEADER + 2u) return ERR_BUFFER_TOO_SMALL;

    uint64_t room = (out->capacity - HW_LOG_READ_HEADER) / 2u;
    if (room > HW_LOG_READ_CHUNK) room = HW_LOG_READ_CHUNK;

    uint8_t *kp = crate_out_alloc(out, HW_LOG_READ_HEADER + 2u * room);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint8_t *bytes = kp + HW_LOG_READ_HEADER;
    uint8_t *tones = bytes + room;
    uint64_t oldest = 0, written = 0;
    uint64_t copied = LogRingReadTones(from, bytes, tones, room, &oldest, &written);
    if (copied < room) memmove(bytes + copied, tones, (size_t)copied);

    memcpy(kp,      &oldest,  sizeof(uint64_t));
    memcpy(kp + 8,  &written, sizeof(uint64_t));
    memcpy(kp + 16, &copied,  sizeof(uint64_t));

    int crc = crate_out_commit(out, ctx, kp, HW_LOG_READ_HEADER + 2u * copied);
    crate_buf_free(kp);
    return (crc == OK) ? OK : ERR_INVALID_ADDRESS;
}

/* What this run has said so far. Every kernel keeps it: the ring is the
 * serial line's source. */
static int HwLogRead(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    uint32_t flags = 0;
    if (op->param_size >= 12) memcpy(&flags, op->params + 8, sizeof(flags));
    if (flags & ~HW_LOG_READ_TONES) return ERR_INVALID_ARGUMENT;
    if (flags & HW_LOG_READ_TONES) return HwLogReadTones(op, crates, ctx);
    return HwLogReadFrom(LogRingRead, op, crates, ctx);
}

/* =========================================================================
 *  HW_LOG_SAY  in_crate:  the bytes, 1..HW_LOG_SAY_MAX
 *              out_crate: optional, [u64 start][u64 end] — where they went
 *
 *  A program's run of text into the ring, whole: the display program says
 *  what it draws, in the order it draws it, so the wire and `logsave` read
 *  what the glass showed; a program with no display says its text here
 *  itself, and the Emergency Screen shows it. One rule for both.
 *
 *  The bytes are copied in BEFORE the console lock — user memory is never
 *  read under it — and the run goes into the ring under it, so it never lands
 *  inside a kernel line. Tone PLAIN.
 *
 *  Who speaks is the kernel's to say, not the speaker's. The screen's holder
 *  says what the glass shows, unmarked: it is the glass. Anyone else says its
 *  own lines, and every line it starts carries "[pid] ", as hw.debug.print's
 *  do — the ring is the machine's account, and a program that could say
 *  "[HALT] Storage sync complete" unmarked could write the kernel's part of
 *  it. APP, as hw.debug.print: a frozen program does not write here either.
 * ========================================================================= */

static int HwLogSay(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *in = &crates[op->in_crate];
    if (in->size == 0)              return ERR_INVALID_ARGUMENT;
    if (in->size > HW_LOG_SAY_MAX)  return ERR_OUT_OF_RANGE;
    uint32_t n = (uint32_t)in->size;

    char     mark[16];
    uint32_t mark_len = 0;
    if (!ScreenHeldBy(ctx->proc->cabin))
        mark_len = (uint32_t)ksnprintf(mark, sizeof(mark), "[%u] ", ctx->proc->pid);

    char *bytes = kmalloc(n);
    if (!bytes) return ERR_NO_MEMORY;
    if (crate_read(in, ctx, bytes, n) != OK) {
        kfree(bytes);
        return ERR_INVALID_ADDRESS;
    }

    /* Room for a mark at every line the run may start, made before the lock;
     * whether its first byte starts a line is known only under it. */
    char *marked = NULL;
    if (mark_len) {
        uint32_t lines = 1;
        for (uint32_t i = 0; i + 1 < n; i++)
            if (bytes[i] == '\n') lines++;
        marked = kmalloc((size_t)n + (size_t)lines * mark_len);
        if (!marked) {
            kfree(bytes);
            return ERR_NO_MEMORY;
        }
    }

    uint64_t at[2];
    console_lock_acquire();
    const char *run     = bytes;
    uint32_t    run_len = n;
    if (marked) {
        uint64_t written    = LogRingWritten();
        bool     line_start = written == 0 || LogRingByteAt(written - 1) == '\n';
        uint32_t k          = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (line_start) {
                memcpy(marked + k, mark, mark_len);
                k += mark_len;
            }
            marked[k++] = bytes[i];
            line_start  = bytes[i] == '\n';
        }
        run     = marked;
        run_len = k;
    }
    LogRingSay(run, run_len, LOG_TONE_SAID, &at[0], &at[1]);
    EmergencyScreenFollow();
    console_lock_release();
    LogRingBellKnock();
    kfree(bytes);
    if (marked) kfree(marked);

    if (op->out_crate != CRATE_INDEX_NONE &&
        crate_write(&crates[op->out_crate], ctx, at, sizeof(at)) != OK)
        return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  HW_LOG_BELL  params:    [u64 from] — the position read to
 *               out_crate: [u8 news]  — 1: an utterance past `from` is already
 *                                       there, nothing was hung; 0: hung
 *
 *  The screen's holder, about to sleep, asks to be rung at the end of the
 *  next utterance past what it has read (klib_logring.h). Only the holder:
 *  there is one bell, and the ring rings the glass's hand.
 * ========================================================================= */

static int HwLogBell(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint64_t from = 0;
    memcpy(&from, op->params, sizeof(from));
    bool news = false;
    error_t rc = ScreenBellHang(ctx->proc->cabin, ctx->proc->pid,
                                ctx->proc->generation, from, &news);
    if (rc != OK) return rc;
    uint8_t said = news ? 1u : 0u;
    if (crate_write(&crates[op->out_crate], ctx, &said, 1) != OK)
        return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  Registration
 * ========================================================================= */

error_t HardwareDeckRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        /* Timer + RTC: read-only telemetry, anyone can use. */
        { HW_TIMER_GET_TICKS,    HwTimerGetTicks,    OP_AUTH_NONE,   "hw.timer.ticks"   },
        { HW_TIMER_GET_MS,       HwTimerGetMs,       OP_AUTH_NONE,   "hw.timer.ms"      },
        { HW_TIMER_GET_FREQ,     HwTimerGetFreq,     OP_AUTH_NONE,   "hw.timer.freq"    },
        { HW_RTC_GET_TIME,       HwRtcGetTime,       OP_AUTH_NONE,   "hw.rtc.time"      },
        { HW_RTC_GET_UNIX64,     HwRtcGetUnix64,     OP_AUTH_NONE,   "hw.rtc.unix64"    },
        { HW_RTC_GET_UPTIME,     HwRtcGetUptime,     OP_AUTH_NONE,   "hw.rtc.uptime"    },
        /* Disk: flush commits the whole medium on one caller's say-so — see
         * HwDiskFlush for why that is a utility's act and not an app's. The
         * disks and their tables any program may see; a change of a table is
         * the person's hands, a box that asks change-disks (hardware_disks.c). */
        { HW_DISK_FLUSH,         HwDiskFlush,        OP_AUTH_UTILITY,"hw.disk.flush"    },
        { HW_DISKS_LIST,         HwDisksList,        OP_AUTH_APP,    "hw.disks.list"    },
        { HW_DISK_LAY,           HwDiskLay,          OP_AUTH_APP,    "hw.disk.lay"      },
        { HW_DISK_ADD,           HwDiskAdd,          OP_AUTH_APP,    "hw.disk.add"      },
        { HW_DISK_REMOVE,        HwDiskRemove,       OP_AUTH_APP,    "hw.disk.remove"   },
        { HW_DISK_MEND,          HwDiskMend,         OP_AUTH_APP,    "hw.disk.mend"     },
        /* Screen: the glass is lent to the system program that draws. */
        { HW_SCREEN_CLAIM,       HwScreenClaim,      OP_AUTH_SYSTEM, "hw.screen.claim"  },
        { HW_SCREEN_RELEASE,     HwScreenRelease,    OP_AUTH_SYSTEM, "hw.screen.release"},
        /* System power: only system-tagged processes can reboot/shutdown. */
        { HW_SYSTEM_REBOOT,      HwSystemReboot,     OP_AUTH_SYSTEM, "hw.system.reboot"  },
        { HW_SYSTEM_SHUTDOWN,    HwSystemShutdown,   OP_AUTH_SYSTEM, "hw.system.shutdown"},
        { HW_DEBUG_PRINT,        HwDebugPrint,       OP_AUTH_APP,    "hw.debug.print"    },
        { HW_LOG_READ,           HwLogRead,          OP_AUTH_UTILITY,"hw.log.read"       },
        { HW_LOG_PREVIOUS,       HwLogPrevious,      OP_AUTH_UTILITY,"hw.log.previous"   },
        { HW_LOG_SAY,            HwLogSay,           OP_AUTH_APP,    "hw.log.say"        },
        { HW_LOG_BELL,           HwLogBell,          OP_AUTH_SYSTEM, "hw.log.bell"       },
        /* USB: hardware control, system+. Nothing here can leave a controller
         * halfway through anything — see the note above HwUsbReset for the two
         * that could and are gone. */
        { HW_USB_RESET,          HwUsbReset,         OP_AUTH_SYSTEM, "hw.usb.reset"     },
        { HW_USB_START,          HwUsbStart,         OP_AUTH_SYSTEM, "hw.usb.start"     },
        { HW_USB_PORT_STATUS,    HwUsbPortStatus,    OP_AUTH_SYSTEM, "hw.usb.port.status"},
        { HW_USB_PORT_RESET,     HwUsbPortReset,     OP_AUTH_SYSTEM, "hw.usb.port.reset"},
        { HW_USB_PORT_QUERY,     HwUsbPortQuery,     OP_AUTH_SYSTEM, "hw.usb.port.query"},
        { HW_USB_ENUM_DEVICE,    HwUsbEnumDevice,    OP_AUTH_SYSTEM, "hw.usb.enum"      },
        { HW_USB_GET_INFO,       HwUsbGetInfo,       OP_AUTH_SYSTEM, "hw.usb.info"      },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_HARDWARE, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[HardwareDeck] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[HardwareDeck] registered %zu named operations\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
