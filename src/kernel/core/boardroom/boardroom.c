#include "boardroom.h"
#include "touch.h"
#include "boarding.h"
#include "klib.h"
#include "ahci.h"
#include "ahci_sync.h"
#include "ahci_async.h"
#include "ata.h"
#include "xhci.h"
#include "xhci_enumeration.h"
#include "xhci_msd.h"
#include "xhci_hub.h"
#include "xhci_interrupt.h"
#include "storage_completion.h"
#include "amp.h"
#include "pmm.h"
#include "vmm.h"
#include "cpu_calibrate.h"
#include "atomics.h"

/* A seat is one medium. The list is built at init and grows when media arrive;
 * nothing here is sized in advance, because the number of disks a machine has
 * is the machine's business. */
typedef struct BoardSeat {
    struct BoardSeat* next;
    uint8_t   number;
    BoardKind kind;
    uint8_t   index;            /* AHCI port, ATA drive, or USB unit */
    bool      removable;
    char      name[48];
} BoardSeat;

static BoardSeat* g_seats = NULL;
static uint8_t    g_seat_count = 0;
static bool       g_initialised = false;

/* The largest run of sectors handed to a controller at once. Both disk paths
 * take a 16-bit count, and the USB path splits internally anyway; keeping the
 * split here means every seat behaves the same to a caller. */
#define BOARD_MAX_RUN 64u

/* Long enough that a slow stick answering one block is not called broken,
 * short enough that a machine with a dead one still boots. */
#define BOARD_ASYNC_TEST_MS 5000u

static BoardSeat* seat_find(uint8_t number)
{
    for (BoardSeat* s = g_seats; s; s = s->next) {
        if (s->number == number) {
            return s;
        }
    }
    return NULL;
}

static bool seat_exists(BoardKind kind, uint8_t index)
{
    for (BoardSeat* s = g_seats; s; s = s->next) {
        if (s->kind == kind && s->index == index) {
            return true;
        }
    }
    return false;
}

static BoardSeat* seat_add(BoardKind kind, uint8_t index, bool removable,
                           const char* name)
{
    if (seat_exists(kind, index)) {
        return NULL;
    }

    BoardSeat* s = (BoardSeat*)kmalloc(sizeof(BoardSeat));
    if (!s) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->number    = g_seat_count++;
    s->kind      = kind;
    s->index     = index;
    s->removable = removable;
    ksnprintf(s->name, sizeof(s->name), "%s", name);

    /* Appended, so seat numbers never move under anybody. */
    BoardSeat** link = &g_seats;
    while (*link) {
        link = &(*link)->next;
    }
    *link = s;
    return s;
}

static const char* ata_slot_name(uint8_t drive)
{
    switch (drive) {
        case 0:  return "ATA primary master";
        case 1:  return "ATA primary slave";
        case 2:  return "ATA secondary master";
        case 3:  return "ATA secondary slave";
        default: return "ATA";
    }
}

/*
 * Bring in whatever USB has to offer.
 *
 * Enumeration runs on interrupts, so a stick present at power-on is still
 * being asked who it is while the rest of the kernel boots. Waiting for that
 * to finish is the difference between finding the volume and deciding there
 * isn't one — and the conversation with each disk (how large are you, are you
 * ready) happens here rather than in the interrupt handler that enumerated it,
 * because it is made of transfers that have to be waited for.
 */
static void usb_attach_visitor(void* ctx, xhci_device_slot_t* slot)
{
    if (!slot || slot->driver != XHCI_DRIVER_STORAGE) {
        return;
    }
    if (xhci_msd_slot_attached(slot)) {
        return;
    }
    xhci_msd_attach((xhci_controller_t*)ctx, slot);
}

static void seat_usb_take_attendance(void)
{
    /* Every controller, because a machine has as many as it has: the chipset
     * one where the case sockets are, and often another on a graphics card.
     * Asking only the first found the wrong silicon on a live board. */
    for (uint8_t ci = 0; ci < xhci_controller_count(); ci++) {
      xhci_controller_t* ctrl = xhci_controller_at(ci);
      if (!ctrl) continue;

    /* Hubs first, and until the bus stops changing: a disk plugged into a hub
     * is not on any port this controller has, and asking the controller what
     * is attached would find the hub and stop there. Each pass may enumerate
     * devices that turn out to be hubs themselves, so this repeats until a
     * pass finds nothing new — bounded, because a bus that keeps changing on
     * every pass is a bus with something wrong with it. */
    for (int pass = 0; pass < 8; pass++) {
        if (xhci_enum_settle(ctrl, 3000) > 0) {
            break;
        }
        if (xhci_hub_service(ctrl) == 0) {
            break;
        }
    }

    int unsettled = xhci_enum_settle(ctrl, 3000);
    if (unsettled > 0) {
        kprintf("[Boardroom] %d USB device(s) still enumerating — going on "
                "without them\n", unsettled);
    }

      xhci_enum_for_each_configured(ctrl, usb_attach_visitor, ctrl);
    }
}

void BoardroomInit(void)
{
    uint8_t seated_before = g_seat_count;

    if (!g_initialised) {
        g_seats = NULL;
        g_seat_count = 0;
        g_initialised = true;
    }

    seat_usb_take_attendance();

    /* USB first, so that a removable volume takes the lower seat number and
     * anything scanning in order meets it first. */
    uint8_t usb_units = xhci_msd_unit_count();
    for (uint8_t n = 0; n < 255 && usb_units > 0; n++) {
        if (!xhci_msd_unit_present(n)) {
            continue;
        }
        usb_units--;
        char name[48];
        ksnprintf(name, sizeof(name), "%s", xhci_msd_unit_name(n));
        seat_add(BOARD_USB, n, true, name);
    }

    if (ahci_is_initialized()) {
        uint32_t mask = ahci_get_active_port_mask();
        for (uint8_t p = 0; p < 32; p++) {
            if (!(mask & (1U << p))) {
                continue;
            }
            char name[48];
            ksnprintf(name, sizeof(name), "AHCI port %u", p);
            seat_add(BOARD_AHCI, p, false, name);
        }
    } else {
        /* The legacy channels, which only matter when AHCI is absent — a
         * controller in IDE-compatibility mode presents the same disks twice
         * otherwise, and a filesystem found on both is a filesystem found
         * once. */
        uint8_t probe[BOARDROOM_SECTOR_BYTES];
        for (uint8_t d = 0; d < 4; d++) {
            if (ata_read_sectors_retry(d, 0, 1, probe) != 0) {
                continue;
            }
            seat_add(BOARD_ATA, d, false, ata_slot_name(d));
        }
    }

    kprintf("[Boardroom] %u medium/media seated\n", g_seat_count);
    for (BoardSeat* s = g_seats; s; s = s->next) {
        kprintf("[Boardroom]   seat %u: %s%s\n", s->number, s->name,
                s->removable ? " (removable)" : "");
    }

    /* Said here, at the end, and not from seat_add: a listener that mounts on
     * this would otherwise be choosing among the seats that happened to be
     * filled first, while the rest of the room was still arriving. */
    if (g_seat_count > seated_before) {
        BoardroomSeatEvent ev = {
            g_seat_count,
            (uint8_t)(g_seat_count - seated_before),
            seated_before,
            0
        };
        TouchPublish("seat:taken", &ev, sizeof(ev));
    }
}

/*
 * A medium turned up after the room had already been called to order.
 *
 * Raised wherever the arrival was noticed, which is an interrupt handler, and
 * acted on below where waiting is allowed. The two cannot be the same place:
 * seating a medium means asking it how large it is and whether it is ready,
 * and both of those are transfers somebody has to wait for.
 */
static volatile uint32_t g_arrival_pending = 0;

void BoardroomNoteArrival(void)
{
    __atomic_store_n(&g_arrival_pending, 1u, __ATOMIC_RELEASE);
}

void BoardroomNoteDeparture(void)
{
    /* The Boardroom knows about media and not about what anybody keeps on
     * them, so it says what happened and lets whoever cares decide. */
    BoardroomSeatEvent ev = { g_seat_count, 0, BOARDROOM_NO_SEAT, 0 };
    TouchPublish("seat:emptied", &ev, sizeof(ev));
}

void BoardroomAttendIfPending(void)
{
    if (__atomic_load_n(&g_arrival_pending, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    /* One core seats what arrived and the rest go away rather than queue up
     * behind it — the same arrangement the hubs and the slot teardown use, and
     * for the same reason: this walks and mutates the seat list. */
    static volatile uint32_t busy = 0;
    if (__atomic_exchange_n(&busy, 1u, __ATOMIC_ACQUIRE) != 0) {
        return;
    }
    __atomic_store_n(&g_arrival_pending, 0u, __ATOMIC_RELEASE);

    uint8_t before = g_seat_count;

    /* Idempotent per medium: a seat that already exists is not seated twice,
     * and seat numbers never move, so anything holding one keeps it. */
    BoardroomInit();

    if (g_seat_count != before) {
        kprintf("[Boardroom] %u medium/media arrived after the room was called "
                "to order\n", (unsigned)(g_seat_count - before));
    }

    /* Whether any of it carries the filesystem this machine lives on is not
     * asked here at all any more. BoardroomInit said seat:taken on its way
     * out, and whoever that concerns was listening. */
    __atomic_store_n(&busy, 0u, __ATOMIC_RELEASE);
}

uint8_t BoardroomSeatCount(void) { return g_seat_count; }

BoardKind BoardroomSeatKind(uint8_t seat)
{
    BoardSeat* s = seat_find(seat);
    return s ? s->kind : BOARD_NONE;
}

const char* BoardroomSeatName(uint8_t seat)
{
    BoardSeat* s = seat_find(seat);
    return s ? s->name : "";
}

uint8_t BoardroomSeatIndex(uint8_t seat)
{
    BoardSeat* s = seat_find(seat);
    return s ? s->index : 0;
}

bool BoardroomSeatIsRemovable(uint8_t seat)
{
    BoardSeat* s = seat_find(seat);
    return s ? s->removable : false;
}

bool BoardroomSeatOccupied(uint8_t seat)
{
    BoardSeat* s = seat_find(seat);
    if (!s) {
        return false;
    }

    switch (s->kind) {
    /* The only medium here that can walk away while the machine runs. The
     * others are screwed to the board; a SATA disk that vanishes at runtime is
     * a fault, not a removal, and it reports itself as failing commands. */
    case BOARD_USB:  return xhci_msd_unit_present(s->index);
    case BOARD_AHCI:
    case BOARD_ATA:  return true;
    default:         return false;
    }
}

int BoardroomRead(uint8_t seat, uint64_t lba, uint32_t count, void* buffer)
{
    BoardSeat* s = seat_find(seat);
    if (!s || !buffer || count == 0) {
        return -1;
    }

    uint8_t* out = (uint8_t*)buffer;
    while (count > 0) {
        uint32_t run = (count > BOARD_MAX_RUN) ? BOARD_MAX_RUN : count;
        int rc;

        switch (s->kind) {
        case BOARD_AHCI: rc = ahci_read_sectors_sync(s->index, lba,
                                                     (uint16_t)run, out); break;
        case BOARD_ATA:  rc = ata_read_sectors_retry(s->index, lba,
                                                     (uint16_t)run, out); break;
        case BOARD_USB:  rc = xhci_msd_read(s->index, lba, run, out); break;
        default:         return -1;
        }
        if (rc != 0) {
            return rc;
        }

        out   += run * BOARDROOM_SECTOR_BYTES;
        lba   += run;
        count -= run;
    }
    return 0;
}

int BoardroomWrite(uint8_t seat, uint64_t lba, uint32_t count, const void* buffer)
{
    BoardSeat* s = seat_find(seat);
    if (!s || !buffer || count == 0) {
        return -1;
    }

    const uint8_t* in = (const uint8_t*)buffer;
    while (count > 0) {
        uint32_t run = (count > BOARD_MAX_RUN) ? BOARD_MAX_RUN : count;
        int rc;

        switch (s->kind) {
        case BOARD_AHCI: rc = ahci_write_sectors_sync(s->index, lba,
                                                      (uint16_t)run, in); break;
        case BOARD_ATA:  rc = ata_write_sectors_retry(s->index, lba,
                                                      (uint16_t)run, in); break;
        case BOARD_USB:  rc = xhci_msd_write(s->index, lba, run, in); break;
        default:         return -1;
        }
        if (rc != 0) {
            return rc;
        }

        in    += run * BOARDROOM_SECTOR_BYTES;
        lba   += run;
        count -= run;
    }
    return 0;
}

bool BoardroomSeatCanReadAsync(uint8_t seat)
{
    BoardSeat* s = seat_find(seat);
    if (!s) {
        return false;
    }

    switch (s->kind) {
    case BOARD_AHCI: return ahci_is_initialized();
    case BOARD_USB:  return xhci_msd_unit_can_read_async(s->index);
    /* The legacy channels are programmed I/O with the processor doing the
     * moving. There is no completion to be told about, so there is nothing
     * here to be asynchronous about — and saying so is better than pretending
     * otherwise and quietly doing it synchronously underneath. */
    case BOARD_ATA:
    default:         return false;
    }
}

error_t BoardroomReadAsync(uint8_t seat, uint64_t lba, uint32_t count,
                           void* dma_phys, BoardroomAsyncCb cb, void* ctx)
{
    BoardSeat* s = seat_find(seat);
    if (!s || !dma_phys || count == 0) {
        return ERR_INVALID_ARGUMENT;
    }

    switch (s->kind) {
    case BOARD_AHCI: {
        uint8_t slot;
        return ahci_submit_read_async(s->index, lba, (uint16_t)count, dma_phys,
                                      cb, ctx, &slot);
    }
    case BOARD_USB:
        return xhci_msd_read_async(s->index, lba, count, dma_phys, cb, ctx);
    default:
        return ERR_UNSUPPORTED;
    }
}

/* ── the self-test ──────────────────────────────────────────────────────── */

typedef struct {
    volatile uint8_t done;
    error_t          status;
} BoardAsyncProbe;

static void board_async_probe_done(uint8_t index, uint8_t slot,
                                   error_t status, void* ctx)
{
    (void)index; (void)slot;
    BoardAsyncProbe* p = (BoardAsyncProbe*)ctx;
    p->status = status;
    __atomic_store_n(&p->done, 1u, __ATOMIC_RELEASE);
}

void BoardroomAsyncSelfTest(uint8_t seat)
{
    if (!BoardroomSeatCanReadAsync(seat)) {
        return;
    }

    /* One filesystem block, which is the unit everything above reads in, and
     * the only shape the asynchronous path accepts. Sector 0 because every
     * medium has one and its contents are not this test's business — the test
     * is whether two ways of asking give the same answer. */
    const uint32_t sectors = 8;
    const uint32_t bytes   = sectors * BOARDROOM_SECTOR_BYTES;

    void* dma = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    uint8_t* expect = (uint8_t*)kmalloc(bytes);
    if (!dma || !expect) {
        if (dma)    pmm_free(dma, 1);
        if (expect) kfree(expect);
        kprintf("[USB ASYNC TEST] no memory to run it — not run\n");
        return;
    }
    uint8_t* got = (uint8_t*)vmm_phys_to_virt((uintptr_t)dma);

    if (BoardroomRead(seat, 0, sectors, expect) != 0) {
        kprintf("[USB ASYNC TEST] the ordinary read failed, so there is "
                "nothing to compare against — not run\n");
        pmm_free(dma, 1); kfree(expect);
        return;
    }

    BoardAsyncProbe probe = { .done = 0, .status = OK };
    error_t sub = BoardroomReadAsync(seat, 0, sectors, dma,
                                     board_async_probe_done, &probe);
    if (sub != OK) {
        kprintf("[USB ASYNC TEST] FAILED: the seat would not take the read "
                "(error %d)\n", (int)sub);
        pmm_free(dma, 1); kfree(expect);
        return;
    }

    /*
     * Turn the handle. The guide loop that would normally do it does not start
     * until the end of boot, and this runs before that — so the completion is
     * pumped here, on this core, which is the one it was posted to.
     */
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(BOARD_ASYNC_TEST_MS);
    while (!__atomic_load_n(&probe.done, __ATOMIC_ACQUIRE)) {
        xhci_process_events();
        StorageCompletionPump(amp_get_core_index());
        if ((int64_t)(rdtsc() - deadline) >= 0) {
            kprintf("[USB ASYNC TEST] FAILED: no answer in %u ms — the read was "
                    "accepted and its completion never arrived\n",
                    BOARD_ASYNC_TEST_MS);
            /* The buffers are deliberately NOT freed: the transfer was
             * accepted, so the controller may still write into that page. A
             * leaked page is a smaller fault than a device writing into one
             * somebody else has been given. */
            kfree(expect);
            return;
        }
        cpu_pause();
    }

    if (probe.status != OK) {
        kprintf("[USB ASYNC TEST] FAILED: the medium refused the read "
                "(error %d)\n", (int)probe.status);
    } else if (memcmp(got, expect, bytes) != 0) {
        uint32_t first = 0;
        while (first < bytes && got[first] == expect[first]) first++;
        kprintf("[USB ASYNC TEST] FAILED: the two reads disagree, first at "
                "byte %u (0x%02x, expected 0x%02x)\n",
                first, got[first], expect[first]);
    } else {
        kprintf("[USB ASYNC TEST] PASSED: %u bytes read without anybody "
                "standing over it, identical to the ordinary read\n", bytes);
    }

    pmm_free(dma, 1);
    kfree(expect);
}

int BoardroomFlush(uint8_t seat)
{
    BoardSeat* s = seat_find(seat);
    if (!s) {
        return -1;
    }

    switch (s->kind) {
    case BOARD_AHCI: return ahci_flush_cache_sync(s->index);
    case BOARD_ATA:  return ata_flush_cache(s->index);
    case BOARD_USB:  return xhci_msd_flush(s->index);
    default:         return -1;
    }
}

/* Sixteen bytes, printed whole. Two volumes made by the same tool on the same
 * day differ in the middle of it, so an abbreviation would not identify one. */
static void uuid_text(const uint8_t uuid[16], char out[33])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = hex[(uuid[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[uuid[i] & 0xF];
    }
    out[32] = '\0';
}

uint8_t BoardroomFindVolume(BoardroomProbe probe, void* ctx)
{
    if (!probe) {
        return BOARDROOM_NO_SEAT;
    }

    /* What the loader said, if it said anything. Read once: the answer cannot
     * change while the room is being called to order. */
    uint8_t  want[16];
    bool     have_pass = BoardingPassVolume(want);

    uint8_t  chosen      = BOARDROOM_NO_SEAT;
    uint8_t  by_identity = BOARDROOM_NO_SEAT;
    unsigned found       = 0;

    for (BoardSeat* s = g_seats; s; s = s->next) {
        uint8_t uuid[16];
        memset(uuid, 0, sizeof(uuid));

        if (!probe(ctx, s->number, uuid)) {
            continue;
        }
        found++;

        if (have_pass && memcmp(uuid, want, 16) == 0 &&
            by_identity == BOARDROOM_NO_SEAT) {
            by_identity = s->number;
        }

        if (chosen == BOARDROOM_NO_SEAT) {
            chosen = s->number;
            continue;
        }

        /* The fallback, for when nothing on the bus is the volume named on the
         * pass — or there is no pass. */
        BoardSeat* c = seat_find(chosen);
        if (c && !c->removable && s->removable) {
            chosen = s->number;
        }
    }

    /*
     * The pass wins when it can be honoured.
     *
     * Not "when there is more than one candidate": a single volume that is not
     * the one this kernel came out of is still the wrong volume, and finding
     * only one of something is not evidence that it is the right one.
     */
    bool by_rule = true;
    if (by_identity != BOARDROOM_NO_SEAT) {
        chosen  = by_identity;
        by_rule = false;
    }

    if (found > 1) {
        kprintf("[Boardroom] a volume was found on %u seats:\n", found);
        for (BoardSeat* s = g_seats; s; s = s->next) {
            uint8_t uuid[16];
            memset(uuid, 0, sizeof(uuid));
            if (!probe(ctx, s->number, uuid)) {
                continue;
            }
            char text[33];
            uuid_text(uuid, text);
            kprintf("[Boardroom]   seat %u: %s  %s%s\n", s->number, s->name,
                    text, s->number == chosen ? "  <- using this one" : "");
        }
    }

    if (found == 0) {
        return BOARDROOM_NO_SEAT;
    }

    if (!by_rule) {
        kprintf("[Boardroom] seat %u carries the volume this kernel was read "
                "out of — the loader said so on its boarding pass\n", chosen);
        kscreen_hold(1000);
        return chosen;
    }

    /*
     * The pass names a volume and the room is not carrying it. Nothing is
     * handed over.
     *
     * This used to fall through to the rule below and give back somebody
     * else's volume — and it did so having just printed, in as many words,
     * that the volume it was looking for was not there. It had the exact
     * sixteen bytes it wanted, knew they were absent, and guessed anyway.
     *
     * What made that expensive is what happens next: the mount succeeds, so
     * nothing is ever wrong enough to retry, and when the real medium finishes
     * enumerating half a second later it is ignored for the rest of the boot.
     * Measured — a machine booted from a stick mounted its own old internal
     * disk instead, silently, and went on to run programs out of it.
     *
     * So the question stops being "has the bus finished?", which nothing can
     * answer except a clock, and becomes "has THAT volume arrived?", which the
     * volume itself answers exactly. Until it does, this machine has no
     * filesystem, which is the truth. Re-seating the medium is what a person
     * does about it, and re-seating produces the arrival that ends the wait.
     */
    if (have_pass) {
        char text[33];
        uuid_text(want, text);
        kprintf("[Boardroom] the loader came from the volume %s, and no seat "
                "is carrying it\n", text);
        /* found is at least one here — found == 0 returned above — so there is
         * always somebody else's volume being declined. */
        kprintf("[Boardroom] %u volume(s) are here and none of them is this "
                "machine's — mounting nothing, and waiting for the medium to "
                "arrive\n", found);
        kscreen_hold(1000);
        return BOARDROOM_NO_SEAT;
    }
    if (found > 1) {
        kprintf("[Boardroom] falling back to the rule: the removable medium "
                "wins\n");
    }

    /* Which volume is about to be mounted, and on what grounds. Held for the
     * same reason as the rest: this is the line that explains every file the
     * machine goes on to read. */
    kscreen_hold(1000);
    return chosen;
}
