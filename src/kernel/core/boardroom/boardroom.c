#include "boardroom.h"
#include "klib.h"
#include "ahci.h"
#include "ahci_sync.h"
#include "ata.h"
#include "xhci.h"
#include "xhci_enumeration.h"
#include "xhci_msd.h"
#include "xhci_hub.h"

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

uint8_t BoardroomFindVolume(BoardroomProbe probe, void* ctx)
{
    if (!probe) {
        return BOARDROOM_NO_SEAT;
    }

    uint8_t chosen = BOARDROOM_NO_SEAT;
    unsigned found = 0;

    for (BoardSeat* s = g_seats; s; s = s->next) {
        if (!probe(ctx, s->number)) {
            continue;
        }
        found++;

        if (chosen == BOARDROOM_NO_SEAT) {
            chosen = s->number;
            continue;
        }

        /* A second volume. The removable one wins, and both are named — a
         * machine that boots from a stick while carrying an old volume on an
         * internal disk should not quietly mount the wrong decade. */
        BoardSeat* c = seat_find(chosen);
        if (!c->removable && s->removable) {
            chosen = s->number;
        }
    }

    if (found > 1) {
        kprintf("[Boardroom] a volume was found on %u seats:\n", found);
        for (BoardSeat* s = g_seats; s; s = s->next) {
            if (probe(ctx, s->number)) {
                kprintf("[Boardroom]   seat %u: %s%s\n", s->number, s->name,
                        s->number == chosen ? "  <- using this one" : "");
            }
        }
        kprintf("[Boardroom] the removable medium wins, because this kernel "
                "cannot ask the firmware which one it booted from\n");
    }

    return chosen;
}
