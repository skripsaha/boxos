#include "boardroom.h"
#include "ground.h"
#include "touch.h"
#include "boarding.h"
#include "klib.h"
#include "ahci.h"
#include "ahci_sync.h"
#include "ahci_async.h"
#include "ata.h"
#include "ata_async.h"     /* AtaDriveReset — the channel's software reset, asked by a seat */
#include "xhci.h"
#include "xhci_enumeration.h"
#include "xhci_msd.h"
#include "xhci_hub.h"
#include "xhci_interrupt.h"
#include "baton.h"
#include "amp.h"
#include "pmm.h"
#include "vmm.h"
#include "cpu_calibrate.h"
#include "atomics.h"
#include "block_fail.h"
#include "lock_rank.h"     /* LockRankWitness — a TagFS lock held across the medium is named */

/*
 * A seat is a PLACE, and a medium is who is sitting in it.
 *
 * That used to be one thing: a seat was a medium, appended to the list when it
 * was found, and there was no way to say it had gone. Two faults came out of
 * the same missing idea. A stick pulled out left its chair behind for ever, so
 * sixteen plug cycles on a live board made sixteen chairs with nobody in
 * fifteen of them. And — far worse — a stick pushed back in got its unit
 * number handed straight back to it, landed in the chair the room already had,
 * and the room announced nothing, because the only thing it could announce was
 * that it had grown. The volume was never mounted again.
 *
 * So a chair can now be empty, and can be sat in again. The list still only
 * grows, and seat numbers still never move under anybody holding one — an
 * empty chair is not deleted, it is unoccupied, which is why a number handed
 * out once stays meaningful for as long as the machine runs.
 */
typedef struct BoardSeat {
    struct BoardSeat* next;
    uint8_t   number;
    BoardKind kind;
    uint8_t   index;            /* AHCI port, ATA drive, or USB unit */
    bool      removable;
    bool      occupied;         /* is there a medium in it at this moment */
    uint32_t  seating;          /* which taking of the room this occupant is */
    volatile uint32_t ready_gen;  /* seat:ready said of this chair, counted */
    volatile uint32_t refusals;   /* commands this chair refused, counted */
    volatile uint8_t  refused;    /* a command refused since the last one answered */
    volatile uint8_t  bell;       /* seat:ready noted, the room's pass yet to say it */
    char      name[48];
} BoardSeat;

static BoardSeat* g_seats = NULL;
static uint8_t    g_seat_count = 0;     /* chairs, whether or not anybody is in them */
static uint32_t   g_seatings = 0;       /* takings anywhere in the room, ever */
static bool       g_initialised = false;

/*
 * ‼ THE BELL: A SEAT THAT REFUSED SAYS SO WHEN IT ANSWERS AGAIN.
 *
 * Whoever a medium told "not now" waits for seat:ready instead of asking on a
 * clock — and only two places in the whole kernel ever said it: an AHCI port
 * its recovery brought back, and a USB disk that reported a state change and
 * then committed its cache. A legacy channel never said it, and neither did a
 * USB disk that refused a read, so a mount owed on either was owed for the
 * rest of the boot, and a Settle cycle refused on one waited for nothing.
 *
 * So the room keeps the fact for every chair, because every command any medium
 * answers passes through here: a command refused marks the chair, and the
 * first one the chair answers after that rings — seat:ready, once, for that
 * refusal. The drivers' own recovery points ring it too (an AHCI port back, an
 * IDE channel reset, a USB transport reset), and a ring answers the refusal
 * before it.
 *
 * Noted where the answer comes — a completion interrupt, a driver's recovery
 * under its port's lock, a read four frames deep in the integrity map — and
 * SAID by the room's own pass (BoardroomAttendIfPending), where a listener may
 * take its time: nothing about who is listening is allowed to land on the
 * stack or under the locks of whoever happened to be answered. The count
 * moves where it is noted, so a waiter that compares counts is never behind
 * the bell.
 */
static volatile uint32_t g_ready_gen = 0;   /* seat:ready noted of any chair, counted */
static volatile uint32_t g_bell_owed = 0;   /* some chair's bell is noted and not yet said */

static void BoardSeatReadyNoted(BoardSeat* s)
{
    __atomic_store_n(&s->refused, 0u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->ready_gen, 1u, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&g_ready_gen, 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->bell, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_bell_owed, 1u, __ATOMIC_RELEASE);
}

/* What the medium in `s` said to one command, handed to it while the chair's
 * occupant was `seating`: an answer that comes after the chair changed hands
 * is the old occupant's and marks nobody. One load when nothing was refused,
 * which is every command but a handful. */
static void BoardSeatAnswered(BoardSeat* s, uint32_t seating, bool answered)
{
    if (__atomic_load_n(&s->seating, __ATOMIC_ACQUIRE) != seating)
        return;
    if (!answered) {
        __atomic_add_fetch(&s->refusals, 1u, __ATOMIC_SEQ_CST);
        __atomic_store_n(&s->refused, 1u, __ATOMIC_RELEASE);
        return;
    }
    if (__atomic_load_n(&s->refused, __ATOMIC_ACQUIRE) &&
        __atomic_exchange_n(&s->refused, 0u, __ATOMIC_ACQ_REL))
        BoardSeatReadyNoted(s);
}

/*
 * The list, and the fields in it.
 *
 * Seats are never freed, so walking one is safe without holding anything —
 * a pointer into the room stays a pointer into the room. What needs the lock
 * is deciding to take a chair, because two cores that both find the same empty
 * one would both sit in it: departures come from the USB service pass and
 * arrivals from the attendance pass, and on a machine with six cores those are
 * routinely different cores at the same moment.
 *
 * ‼ Never held across disk I/O or an allocation. Every path below reads what
 * it needs, lets go, and only then talks to a medium — the alternative is the
 * one this kernel already has a debt for, and it is not going to be paid by
 * adding another.
 */
static spinlock_t g_seats_lock;
static bool       g_seats_lock_ready = false;

/*
 * The room is called to order by ONE core at a time, and the second WAITS.
 *
 * There are two doors into it. The boot walks in on the BSP; the attendance
 * pass walks in from whichever core went idle, every time something arrives on
 * a bus. Only the second door had a lock on it, so on a machine whose USB
 * settles slowly — a controller that reset itself, a port that takes ten
 * seconds to answer — the boot was still seating media when a hot-plug sent
 * another core in through the other door. Both then attached the same disk.
 *
 * Waiting rather than going away, because both callers need the room to be IN
 * ORDER when they return: a boot that skipped it has no volume, and going away
 * would trade a duplicate for a machine with no filesystem. The wait is
 * bounded by exactly the work the waiter would otherwise have done itself.
 */
static volatile uint32_t g_in_session = 0;

static void seats_lock_init(void)
{
    if (!g_seats_lock_ready) {
        spinlock_init(&g_seats_lock);
        g_seats_lock_ready = true;
    }
}

/* What a seat answers when it will not say, and the floor under every answer:
 * one filesystem block, which is the unit everything above reads in. */
#define BOARD_RUN_FALLBACK 8u

static BoardSeat* seat_find(uint8_t number)
{
    for (BoardSeat* s = g_seats; s; s = s->next) {
        if (s->number == number) {
            return s;
        }
    }
    return NULL;
}

/*
 * The chair, and only while somebody is in it.
 *
 * Every path that goes on to touch a medium comes through here rather than
 * through seat_find. The index in an empty chair is the number the controller
 * used to give the medium that left, and controllers hand those out again
 * immediately — so a caller holding a stale seat number would be sent to
 * whatever is in that unit now, and would be told nothing about it.
 */
static BoardSeat* seat_taken(uint8_t number)
{
    BoardSeat* s = seat_find(number);
    return (s && s->occupied) ? s : NULL;
}

/* The chair this medium is sitting in, or none. An empty chair that used to
 * hold it is not it: the whole point of emptying one is that the number in it
 * stops meaning anything. Caller holds the lock. */
static BoardSeat* seat_holding_locked(BoardKind kind, uint8_t index)
{
    for (BoardSeat* s = g_seats; s; s = s->next) {
        if (s->occupied && s->kind == kind && s->index == index) {
            return s;
        }
    }
    return NULL;
}

static uint8_t seat_occupied_count(void)
{
    uint8_t n = 0;
    for (BoardSeat* s = g_seats; s; s = s->next) {
        if (s->occupied) n++;
    }
    return n;
}

/*
 * Sit a medium down, and say which chair it got.
 *
 * An empty chair of the same kind is taken before a new one is added, lowest
 * first — a room that adds a chair per arrival is a room that fills up with
 * furniture on a machine somebody keeps swapping sticks on. Returns NULL when
 * this medium is already sitting somewhere, which is the ordinary answer on
 * every attendance pass after the first.
 *
 * The chair itself is allocated before the lock is taken and given back if it
 * turns out not to be needed: kmalloc under a spinlock is a wait of unbounded
 * length inside one of bounded length, and this room is walked from the USB
 * service pass.
 */
static BoardSeat* seat_take(BoardKind kind, uint8_t index, bool removable,
                            const char* name)
{
    seats_lock_init();

    BoardSeat* spare = (BoardSeat*)kmalloc(sizeof(BoardSeat));

    spin_lock(&g_seats_lock);

    if (seat_holding_locked(kind, index)) {
        spin_unlock(&g_seats_lock);
        if (spare) kfree(spare);
        return NULL;
    }

    BoardSeat* s = NULL;
    for (BoardSeat* c = g_seats; c; c = c->next) {
        if (!c->occupied && c->kind == kind) {
            s = c;                      /* the list is in number order */
            break;
        }
    }

    if (!s) {
        if (!spare) {
            spin_unlock(&g_seats_lock);
            return NULL;
        }
        s = spare;
        spare = NULL;
        memset(s, 0, sizeof(*s));
        s->number = g_seat_count++;
        s->kind   = kind;

        /* Appended, so seat numbers never move under anybody. */
        BoardSeat** link = &g_seats;
        while (*link) {
            link = &(*link)->next;
        }
        *link = s;
    }

    s->index     = index;
    s->removable = removable;
    s->occupied  = true;
    s->seating   = ++g_seatings;
    /* A refusal, and a bell not yet said, were the last occupant's. The count
     * is the chair's and goes on: a waiter holding it holds a number that can
     * only move forward. */
    __atomic_store_n(&s->refused, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->bell, 0u, __ATOMIC_RELAXED);
    ksnprintf(s->name, sizeof(s->name), "%s", name);

    spin_unlock(&g_seats_lock);

    if (spare) kfree(spare);
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

uint32_t BoardroomInit(void)
{
    /*
     * What counts as "new in this pass" is a TAKING, not a seat number.
     *
     * It used to be "seat number below the count we started with", which was
     * the same thing only while chairs could never be sat in twice. Now a
     * returning stick lands in seat 3 that has been there since boot, and the
     * seat number says nothing at all about whether anything happened.
     */
    seats_lock_init();

    while (__atomic_exchange_n(&g_in_session, 1u, __ATOMIC_ACQUIRE) != 0) {
        cpu_pause();
    }

    /* Read AFTER the gate: whoever went first may have seated something, and
     * counting from before their pass would report their work as ours. */
    uint32_t seatings_before = g_seatings;

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
        seat_take(BOARD_USB, n, true, name);
    }

    if (ahci_is_initialized()) {
        uint32_t mask = ahci_get_active_port_mask();
        for (uint8_t p = 0; p < 32; p++) {
            if (!(mask & (1U << p))) {
                continue;
            }
            char name[48];
            ksnprintf(name, sizeof(name), "AHCI port %u", p);
            seat_take(BOARD_AHCI, p, false, name);
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
            seat_take(BOARD_ATA, d, false, ata_slot_name(d));
        }
    }

    /* Nothing changed hands. The room is called to order on every arrival the
     * bus notices, and most of those are a device this room already has — so
     * saying the same thing again is how a log stops being read. */
    if (g_seatings == seatings_before) {
        __atomic_store_n(&g_in_session, 0u, __ATOMIC_RELEASE);
        return 0;
    }

    uint8_t changed = 0;
    uint8_t first   = BOARDROOM_NO_SEAT;

    kprintf("[Boardroom] %u medium/media seated\n", seat_occupied_count());
    for (BoardSeat* s = g_seats; s; s = s->next) {
        if (!s->occupied) {
            kprintf("[Boardroom]   seat %u: empty\n", s->number);
            continue;
        }

        kprintf("[Boardroom]   seat %u: %s%s\n", s->number, s->name,
                s->removable ? " (removable)" : "");

        /* And how the medium is divided, which nothing in this kernel used to
         * read. A volume was assumed to begin at one absolute sector of the
         * whole device, so BoxOS could exist in exactly one place on any
         * medium — and a disk belonging to somebody else could not be shared
         * with it at all.
         *
         * Only for the seats that changed hands in this pass. The room is
         * called to order again every time something arrives, and re-reading
         * the table of every medium already in it would say the same thing
         * again on every hot-plug. */
        if (s->seating <= seatings_before) {
            continue;
        }

        changed++;
        if (first == BOARDROOM_NO_SEAT) {
            first = s->number;
        }

        MediumGround ground[GROUND_MAX_PER_MEDIUM];
        uint8_t claimed = GroundSurvey(s->number, ground, GROUND_MAX_PER_MEDIUM, NULL);
        for (uint8_t g = 0; g < claimed; g++) {
            kprintf("[Boardroom]     ground %u: sectors %llu..%llu "
                    "(%llu MiB), from %s entry %u\n",
                    g,
                    (unsigned long long)ground[g].start_sector,
                    (unsigned long long)(ground[g].start_sector +
                                         ground[g].sectors - 1),
                    (unsigned long long)((ground[g].sectors *
                                          BOARDROOM_SECTOR_BYTES) /
                                         (1024u * 1024u)),
                    GroundOriginName(ground[g].origin), ground[g].entry);
        }
    }

    /*
     * Said here, at the end, and not from seat_take: a listener that mounts on
     * this would otherwise be choosing among the seats that happened to be
     * filled first, while the rest of the room was still arriving.
     *
     * And said whenever a chair CHANGED HANDS, which is the whole repair. It
     * used to be said only when the room GREW, and a stick pushed back into
     * the machine it was pulled out of does not make the room grow: its unit
     * number is free, it gets it back, and it sits down in the chair it left.
     * The room went on holding a volume nobody had mounted.
     */
    BoardroomSeatEvent ev = { seat_occupied_count(), changed, first, 0 };

    /* Given back BEFORE the announcement: a listener that mounts a volume on
     * this event reads sectors, and a reader must never be holding the door
     * the medium's own arrival has to come through. */
    __atomic_store_n(&g_in_session, 0u, __ATOMIC_RELEASE);

    TouchPublish("seat:taken", &ev, sizeof(ev));

    /* What THIS pass seated, and only that. Counted under the door, off the
     * seatings that carry a number above the one this pass started from. */
    return changed;
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

void BoardroomNoteDeparture(BoardKind kind, uint8_t index)
{
    seats_lock_init();

    spin_lock(&g_seats_lock);
    BoardSeat* s = seat_holding_locked(kind, index);
    if (s) {
        /* The chair stays. Only the person in it goes — which is what makes a
         * seat number something anybody can hold on to across an unplug. */
        s->occupied = false;
    }
    uint8_t seated = seat_occupied_count();
    spin_unlock(&g_seats_lock);

    if (!s) {
        /* A medium that was never seated. Ordinary: an enumeration that failed
         * before the room was told about it still comes through here. */
        return;
    }

    kprintf("[Boardroom] seat %u is empty — %s has left\n", s->number, s->name);

    /* The Boardroom knows about media and not about what anybody keeps on
     * them, so it says what happened, names the chair it happened to, and lets
     * whoever cares decide what that means. */
    BoardroomSeatEvent ev = { seated, 1, s->number, 0 };
    TouchPublish("seat:emptied", &ev, sizeof(ev));
}

#ifdef CONFIG_VOLUME_TESTS
void BoardroomStandSeatEmpties(uint8_t seat)
{
    seats_lock_init();

    spin_lock(&g_seats_lock);
    BoardSeat* s = seat_find(seat);
    if (s) {
        s->occupied = false;
    }
    spin_unlock(&g_seats_lock);
}
#endif

void BoardroomNoteSeatReady(BoardKind kind, uint8_t index)
{
    seats_lock_init();

    spin_lock(&g_seats_lock);
    BoardSeat* s = seat_holding_locked(kind, index);
    /* A medium nobody seated — one recovered before the room was first called
     * to order — has no chair to name, and nobody could be waiting on it. */
    if (s) {
        BoardSeatReadyNoted(s);
    }
    spin_unlock(&g_seats_lock);
}

uint32_t BoardroomSeatReadyGen(uint8_t seat)
{
    if (seat == BOARDROOM_NO_SEAT) {
        return __atomic_load_n(&g_ready_gen, __ATOMIC_SEQ_CST);
    }
    BoardSeat* s = seat_find(seat);
    return s ? __atomic_load_n(&s->ready_gen, __ATOMIC_SEQ_CST) : 0;
}

uint32_t BoardroomSeatRefusals(uint8_t seat)
{
    BoardSeat* s = seat_find(seat);
    return s ? __atomic_load_n(&s->refusals, __ATOMIC_SEQ_CST) : 0;
}

/* Every bell the seats noted, said: one seat:ready per chair that rang, and
 * none for a chair that has emptied since — nobody could be waiting on it. A
 * bell noted while this walks is either seen by it or leaves the room's flag
 * up for the next pass, because the chair's bell is put up before the room's.
 * One load when nothing rang. */
static void BoardRingOwed(void)
{
    if (__atomic_load_n(&g_bell_owed, __ATOMIC_ACQUIRE) == 0 ||
        __atomic_exchange_n(&g_bell_owed, 0u, __ATOMIC_ACQ_REL) == 0) {
        return;
    }
    for (BoardSeat* s = g_seats; s; s = s->next) {
        if (!__atomic_exchange_n(&s->bell, 0u, __ATOMIC_ACQ_REL) || !s->occupied) {
            continue;
        }
        BoardroomSeatEvent ev = { seat_occupied_count(), 0, s->number, 0 };
        TouchPublish("seat:ready", &ev, sizeof(ev));
    }
}

void BoardroomAttendIfPending(void)
{
    BoardRingOwed();

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

    /*
     * Idempotent per medium: a medium already sitting somewhere is not seated
     * twice, and seat numbers never move, so anything holding one keeps it.
     *
     * The count comes back from the pass, and is not worked out here from two
     * reads of g_seatings. It used to be, and the door is taken INSIDE
     * BoardroomInit, so on a cold boot the snapshot was read while the first
     * core still had the room open: the USB enumeration that raises the
     * arrival flag runs under that very door, from inside the first core's
     * pass. The second core read zero, waited, found the room already full,
     * seated nothing — and announced the first core's three media as having
     * turned up after the room was called to order. Nothing had turned up.
     * That WAS the room being called to order.
     */
    uint32_t seated_now = BoardroomInit();

    if (seated_now > 0) {
        kprintf("[Boardroom] %u medium/media took a seat while the machine was "
                "already running, so this was a hot-plug and not part of "
                "bring-up\n", (unsigned)seated_now);
    }

    /* Whether any of it carries the filesystem this machine lives on is not
     * asked here at all any more. BoardroomInit said seat:taken on its way
     * out, and whoever that concerns was listening. */
    __atomic_store_n(&busy, 0u, __ATOMIC_RELEASE);
}

uint8_t BoardroomSeatCount(void) { return g_seat_count; }

BoardKind BoardroomSeatKind(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
    return s ? s->kind : BOARD_NONE;
}

const char* BoardroomSeatName(uint8_t seat)
{
    /* An empty chair has no name. It keeps the last occupant's in the struct
     * so the departure line can say who left, but handing that out afterwards
     * would name a medium that is in somebody's pocket. */
    BoardSeat* s = seat_find(seat);
    return (s && s->occupied) ? s->name : "";
}

uint32_t BoardroomSeatPhysicalBytes(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
    if (!s) return 0;

    switch (s->kind) {
    case BOARD_USB:
        return xhci_msd_unit_physical_bytes(s->index);
    case BOARD_AHCI: {
        ahci_port_t* p = ahci_get_port_state(s->index);
        return p ? p->physical_sector_size : 0;
    }
    case BOARD_ATA:
        if (s->index < 4) {
            return g_ata_devices[s->index].physical_sector_size;
        }
        return 0;
    default:
        return 0;
    }
}

/*
 * A medium's own logical sectors, in the 512-byte ones this room speaks.
 *
 * Almost always the identity: both disk drivers refuse anything but 512-byte
 * logical sectors, and the USB path converts before it answers. It is written
 * out anyway because "the number the device gave" and "the number this room
 * means" are different quantities, and the one place they are the same is
 * exactly where nobody notices when they stop being.
 */
static uint64_t seat_sectors_in_512(uint64_t total, uint32_t logical)
{
    if (logical == 0 || (logical % BOARDROOM_SECTOR_BYTES) != 0) {
        return 0;               /* not something this room can express */
    }
    return total * (uint64_t)(logical / BOARDROOM_SECTOR_BYTES);
}

uint64_t BoardroomSeatSectors(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
    if (!s) return 0;

    switch (s->kind) {
    case BOARD_USB:
        /* Already in this room's sectors — the disk driver does the conversion
         * where it learns the geometry, because it is the one that has to
         * split a request across the device's own blocks. */
        return xhci_msd_unit_sectors(s->index);
    case BOARD_AHCI: {
        ahci_port_t* p = ahci_get_port_state(s->index);
        return p ? seat_sectors_in_512(p->total_sectors, p->logical_sector_size)
                 : 0;
    }
    case BOARD_ATA:
        if (s->index < 4) {
            return seat_sectors_in_512(g_ata_devices[s->index].total_sectors,
                                       g_ata_devices[s->index].logical_sector_size);
        }
        return 0;
    default:
        return 0;
    }
}

uint8_t BoardroomSeatIndex(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
    return s ? s->index : 0;
}

bool BoardroomSeatIsRemovable(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
    return s ? s->removable : false;
}

uint32_t BoardroomSeatSeating(uint8_t seat)
{
    BoardSeat* s = seat_find(seat);
    return s ? s->seating : 0;
}

bool BoardroomSeatOccupied(uint8_t seat)
{
    BoardSeat* s = seat_find(seat);
    if (!s || !s->occupied) {
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

uint32_t BoardroomSeatRun(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
    if (!s) {
        return BOARD_RUN_FALLBACK;
    }

    uint32_t run;
    switch (s->kind) {
    case BOARD_AHCI: run = ahci_max_run_sectors(s->index);        break;
    case BOARD_ATA:  run = ata_max_run_sectors(s->index);         break;
    case BOARD_USB:  run = xhci_msd_unit_max_run(s->index);       break;
    default:         run = BOARD_RUN_FALLBACK;                    break;
    }

    /* A driver that answers nothing gets the floor rather than a division by
     * zero or a loop that never advances. */
    if (run == 0) {
        run = BOARD_RUN_FALLBACK;
    }
    /* Both disk paths take a 16-bit count, whatever they believe about
     * themselves. The room is where that truth is kept. */
    if (run > 65535u) {
        run = 65535u;
    }
    return run;
}

#ifdef CONFIG_VOLUME_TESTS
/*
 * What a seat's driver answers once its own retries have run out, for a
 * command BLOCKFAIL refuses in its place (block_fail.h). The caller above
 * cannot tell the two apart, which is the point: every road a real refusal
 * takes out of here, this one takes too.
 *
 *   AHCI  -1 for a read or a write, after AHCI_MAX_RETRIES with the port
 *         recovered between tries; ERR_IO, as an error_t, for a flush.
 *   ATA   ATA_ERR_MAX_RETRIES for a read or a write; ATA_ERR_FLUSH_FAILED.
 *   USB   -1 for all three.
 */
static int BoardMediumRefusal(BoardKind kind, BlockFailKind what)
{
    switch (kind) {
    case BOARD_AHCI: return what == BLOCK_FAIL_FLUSH ? (int)ERR_IO : -1;
    case BOARD_ATA:  return what == BLOCK_FAIL_FLUSH ? ATA_ERR_FLUSH_FAILED
                                                     : ATA_ERR_MAX_RETRIES;
    default:         return -1;
    }
}

/* And what it answers for a read of a sector the device says it cannot read,
 * every attempt (BlockFailArmBadSectors). */
static int BoardBadSectorRefusal(BoardKind kind)
{
    switch (kind) {
    case BOARD_AHCI: return AHCI_ERR_MEDIUM;
    case BOARD_ATA:  return ATA_ERR_MEDIUM;
    case BOARD_USB:  return XHCI_MSD_ERR_MEDIUM;
    default:         return -1;
    }
}
#endif

/*
 * ‼ A SECTOR THE MEDIUM CANNOT READ IS AN ANSWER, NOT A REFUSAL.
 *
 * Every driver gives up on a read in two ways that look the same to whoever
 * only asks "did it work": the conversation broke down — the link dropped, the
 * bus timed out, the device never answered — or it went through, and the
 * device's answer was that the data of a sector it reached cannot be read: UNC
 * in the ATA error register (ATA8-ACS, READ SECTOR(S) and READ DMA EXT error
 * outputs; AHCI hands it in PxTFD), MEDIUM ERROR on a USB disk (SPC sense key
 * 3h). The first says nothing of what the medium holds and is asked again once
 * the seat answers (seat:ready); the second is what it holds, said by the only
 * one who knows — a page torn as it was written reads that way until it is
 * written again. So the room gives the second a name of its own: (int)
 * ERR_BAD_SECTOR, the error_t carried through the int as the flush carries
 * ERR_IO (tagfs_flush_cache), and every other answer stays the driver's own.
 * Each driver says it only when every one of its own attempts ended so.
 */
static int BoardReadAnswer(BoardKind kind, int rc)
{
    bool bad = (kind == BOARD_AHCI && rc == AHCI_ERR_MEDIUM) ||
               (kind == BOARD_ATA  && rc == ATA_ERR_MEDIUM)  ||
               (kind == BOARD_USB  && rc == XHCI_MSD_ERR_MEDIUM);
    return bad ? (int)ERR_BAD_SECTOR : rc;
}

/* One command's read, handed to the seat's driver: its answer. */
static int BoardSeatRead(const BoardSeat* s, uint64_t lba, uint32_t run, uint8_t* out)
{
    switch (s->kind) {
    case BOARD_AHCI: return ahci_read_sectors_sync(s->index, lba, (uint16_t)run, out);
    case BOARD_ATA:  return ata_read_sectors_retry(s->index, lba, (uint16_t)run, out);
    case BOARD_USB:  return xhci_msd_read(s->index, lba, run, out);
    default:         return -1;
    }
}

int BoardroomRead(uint8_t seat, uint64_t lba, uint32_t count, void* buffer)
{
    LockRankWitness("read", (uintptr_t)__builtin_return_address(0));
    BoardSeat* s = seat_taken(seat);
    if (!s || !buffer || count == 0) {
        return -1;
    }

    const uint32_t seating = __atomic_load_n(&s->seating, __ATOMIC_ACQUIRE);
    const uint32_t most    = BoardroomSeatRun(seat);
    uint8_t*       out     = (uint8_t*)buffer;
    int            rc      = 0;
    while (count > 0) {
        uint32_t run = (count > most) ? most : count;

#ifdef CONFIG_VOLUME_TESTS
        /* Per run, because a run is what a driver is handed: the runs before a
         * refused one reach the medium, exactly as they do when it is real. */
        bool bad = false;
        if (BlockFailReadStrikes(seat, lba, run, &bad)) {
            rc = BoardReadAnswer(s->kind, bad ? BoardBadSectorRefusal(s->kind)
                                              : BoardMediumRefusal(s->kind, BLOCK_FAIL_READ));
            break;
        }
#endif

        rc = BoardReadAnswer(s->kind, BoardSeatRead(s, lba, run, out));
        if (rc != 0) {
            break;
        }

        out   += run * BOARDROOM_SECTOR_BYTES;
        lba   += run;
        count -= run;
    }
    /* A sector the medium cannot read is the seat answering: no bell owed. */
    BoardSeatAnswered(s, seating, rc == 0 || rc == (int)ERR_BAD_SECTOR);
    return rc;
}

/* One command's write, handed to the seat's driver: its answer. */
static int BoardSeatWrite(const BoardSeat* s, uint64_t lba, uint32_t run, const uint8_t* in)
{
    switch (s->kind) {
    case BOARD_AHCI: return ahci_write_sectors_sync(s->index, lba, (uint16_t)run, in);
    case BOARD_ATA:  return ata_write_sectors_retry(s->index, lba, (uint16_t)run, in);
    case BOARD_USB:  return xhci_msd_write(s->index, lba, run, in);
    default:         return -1;
    }
}

int BoardroomWrite(uint8_t seat, uint64_t lba, uint32_t count, const void* buffer,
                   uint32_t* landed)
{
    LockRankWitness("write", (uintptr_t)__builtin_return_address(0));
    if (landed) {
        *landed = 0;
    }
    BoardSeat* s = seat_taken(seat);
    if (!s || !buffer || count == 0) {
        return -1;
    }

    const uint32_t seating = __atomic_load_n(&s->seating, __ATOMIC_ACQUIRE);
    const uint32_t most    = BoardroomSeatRun(seat);
    const uint8_t* in      = (const uint8_t*)buffer;
    uint32_t       took    = 0;
    int            rc      = 0;
    while (count > 0) {
        uint32_t run = (count > most) ? most : count;

#ifdef CONFIG_VOLUME_TESTS
        /* Refused before the driver is called, so a refused write never
         * reaches the medium — the runs before it do, and so do the sectors
         * of this one before the plan's first, the way a medium that names
         * the sector it failed at keeps them. A write nobody stands over asks
         * the same plan at its own door (BoardroomWriteAsync). */
        uint32_t keep = 0;
        if (BlockFailProtects(seat) ||
            BlockFailStrikes(seat, BLOCK_FAIL_WRITE, lba, run, &keep)) {
            rc = keep ? BoardSeatWrite(s, lba, keep, in) : 0;
            if (rc == 0) {
                took += keep;
                rc    = BoardMediumRefusal(s->kind, BLOCK_FAIL_WRITE);
            }
            break;
        }
#endif

        rc = BoardSeatWrite(s, lba, run, in);
        if (rc != 0) {
            break;
        }
        in    += run * BOARDROOM_SECTOR_BYTES;
        lba   += run;
        count -= run;
        took  += run;
    }
    if (landed) {
        *landed = took;
    }
    BoardSeatAnswered(s, seating, rc == 0);
    return rc;
}

bool BoardroomSeatRefusesWrites(uint8_t seat)
{
#ifdef CONFIG_VOLUME_TESTS
    if (BlockFailProtects(seat)) {
        return true;
    }
#endif
    BoardSeat* s = seat_taken(seat);
    if (!s) {
        return false;
    }
    switch (s->kind) {
    case BOARD_USB:  return xhci_msd_unit_write_protected(s->index);
    case BOARD_AHCI: return ahci_port_refuses_writes(s->index);
    /* The legacy channels say nothing a write-protected drive would say
     * differently from any other refusal. */
    default:         return false;
    }
}

bool BoardroomSeatAnnouncesReady(uint8_t seat)
{
    return seat_taken(seat) != NULL;
}

/*
 * ‼ A SEAT NOBODY WILL ASK AGAIN IS RESET — BY ITS DRIVER'S OWN RECOVERY.
 *
 * The bell rings on the first command a seat answers after a refusal, so a
 * caller refused by a medium error no driver recovered from, with no command
 * of its own left to send, waits for a bell nobody can ring: a mount owed on
 * a seat the machine reads nothing else from waited for the rest of the boot.
 * What that caller can ask for is what each driver already does when it finds
 * its medium lost: an IDE channel's software reset, a USB disk's transport
 * reset, an AHCI port's link reset — recovery, never the command again.
 *
 * A medium that comes back from it rings the bell here, whatever its driver
 * said on the way. One that does not is a seat the room cannot reach — a USB
 * disk that went with it, a port the reset retired, a channel with no drive
 * answering — and the caller is told so.
 */
error_t BoardroomSeatReset(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
    if (!s) {
        return ERR_DEVICE_NOT_READY;
    }

    bool back;
    switch (s->kind) {
    case BOARD_ATA:  back = AtaDriveReset(s->index);          break;
    case BOARD_AHCI: back = AhciPortReset(s->index);          break;
    case BOARD_USB:  back = XhciMsdUnitReset(s->index) == 0;  break;
    default:         back = false;                            break;
    }
    if (!back) {
        return ERR_DEVICE_NOT_READY;
    }
    BoardroomNoteSeatReady(s->kind, s->index);
    return OK;
}

/* Whether the seat answers a command nobody stands over, read or write. */
static bool seat_can_async(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
    if (!s) {
        return false;
    }

    switch (s->kind) {
    case BOARD_AHCI: return ahci_is_initialized();
    case BOARD_USB:  return xhci_msd_unit_can_async(s->index);
    /* The legacy channels are programmed I/O with the processor doing the
     * moving. There is no completion to be told about, so there is nothing
     * here to be asynchronous about — and saying so is better than pretending
     * otherwise and quietly doing it synchronously underneath. */
    case BOARD_ATA:
    default:         return false;
    }
}

bool BoardroomSeatCanReadAsync(uint8_t seat)
{
    return seat_can_async(seat);
}

bool BoardroomSeatCanWriteAsync(uint8_t seat)
{
    return seat_can_async(seat);
}

/*
 * The room's answer to a command nobody stood over: what the seat said is
 * noted against its chair (the bell, above) and handed on to whoever moored
 * the berth. Lean, as every seat's answer is — it may run in the completion
 * interrupt, so it takes no lock and allocates nothing — and it reads the
 * berth before handing it on, because from that call on the berth is its
 * owner's again.
 *
 * And under BLOCKFAIL, the answer to the sectors a struck write kept, which
 * went out as their own command: the caller hears the refusal it was going to
 * hear, with what landed before the struck sector — or the kept command's own
 * refusal, with nothing.
 */
static void BoardAsyncAnswered(uint8_t index, uint8_t slot, error_t status, void* ctx)
{
    RoadsteadBerth*   b      = (RoadsteadBerth*)ctx;
    RoadsteadAnswered theirs = b->theirs;
    void*             their  = b->theirs_ctx;
    BoardSeat*        s      = seat_find(b->seat);

#ifdef CONFIG_VOLUME_TESTS
    if (b->struck) {
        b->landed = status == OK ? b->sectors : 0;
        status    = status == OK ? ERR_IO : status;
    }
#endif
    if (s) {
        BoardSeatAnswered(s, b->seating, status == OK);
    }
    theirs(index, slot, status, their);
}

/* One command nobody stands over, read or write, handed to its seat. */
static error_t seat_async(uint8_t seat, uint64_t lba, uint32_t count, void* dma_phys,
                          BoardroomAsyncCb cb, void* ctx, RoadsteadBerth* berth, bool write)
{
    BoardSeat* s = seat_taken(seat);
    if (!s || !dma_phys || count == 0 || !cb || !berth) {
        return ERR_INVALID_ARGUMENT;
    }

    memset(berth, 0, sizeof(*berth));
    berth->lba        = lba;
    berth->sectors    = count;
    berth->dma_phys   = dma_phys;
    berth->theirs     = cb;
    berth->theirs_ctx = ctx;
    berth->seat       = seat;
    berth->seating    = __atomic_load_n(&s->seating, __ATOMIC_ACQUIRE);

    /* From here the room's answer is the one every road below calls, the
     * refusals BLOCKFAIL makes in a medium's place among them. */
    cb  = BoardAsyncAnswered;
    ctx = berth;
    berth->answered = cb;
    berth->ctx      = ctx;

#ifdef CONFIG_VOLUME_TESTS
    /* Refused the way these seats refuse a command nobody stands over: taken,
     * and then answered through the callback with ERR_IO — never by the
     * submit's own return, which a real medium error does not reach. The
     * callbacks in use only keep the status and pass it on, and a real
     * completion can already arrive before the submit returns, so answering
     * here is a road the callers already take. Nothing of it is issued, and
     * the buffer is not touched — except a write's sectors before the plan's
     * first one, which go out as their own command, as BoardroomWrite keeps
     * them, and are answered as landed (BoardStruckAnswered). A protected
     * medium refuses every write here as it does at the attended door. */
    uint32_t keep   = 0;
    bool     struck = (s->kind == BOARD_AHCI || s->kind == BOARD_USB) &&
                      ((write && BlockFailProtects(seat)) ||
                       BlockFailStrikes(seat, write ? BLOCK_FAIL_WRITE : BLOCK_FAIL_READ, lba,
                                        count, write ? &keep : NULL));
    if (struck && keep == 0) {
        cb(s->index, 0, ERR_IO, ctx);
        return OK;
    }
    if (struck) {
        berth->sectors = keep;
        berth->struck  = true;
    }
#endif

    switch (s->kind) {
    case BOARD_AHCI: return write ? ahci_submit_write_async(s->index, berth)
                                  : ahci_submit_read_async(s->index, berth);
    case BOARD_USB:  return write ? xhci_msd_write_async(s->index, berth)
                                  : xhci_msd_read_async(s->index, berth);
    default:         return ERR_UNSUPPORTED;
    }
}

error_t BoardroomReadAsync(uint8_t seat, uint64_t lba, uint32_t count,
                           void* dma_phys, BoardroomAsyncCb cb, void* ctx,
                           RoadsteadBerth* berth)
{
    return seat_async(seat, lba, count, dma_phys, cb, ctx, berth, false);
}

error_t BoardroomWriteAsync(uint8_t seat, uint64_t lba, uint32_t count,
                            void* dma_phys, BoardroomAsyncCb cb, void* ctx,
                            RoadsteadBerth* berth)
{
    return seat_async(seat, lba, count, dma_phys, cb, ctx, berth, true);
}

/* ── a read nobody stands over, proved on this machine ─────────────────── */

/*
 * The proof's record. It is a static, and it has to be: the seat keeps the
 * address of this record for as long as the read is outstanding, and a record
 * that is still outstanding cannot live on a stack frame that goes away. It
 * did, once. The proof gave up after a clock of its own, returned, and the
 * answer came in later through the completion interrupt — into whatever the
 * K-Core loop had put on that stack by then. Measured: 259 ms after the
 * give-up, on this core, at the same address.
 */
typedef struct {
    Baton    node;              /* the answer's continuation: passed by the
                                 * seat's callback, run by the pump */
    RoadsteadBerth berth;       /* where the read waits if the seat is busy */
    error_t  status;            /* what the medium said */
    uint8_t  seat;
    void*    dma;               /* the physical page the medium wrote */
    uint8_t* expect;            /* the attended read of the same block */
    uint32_t bytes;
    volatile uint8_t judged;    /* the verdict has been said */
} UnattendedRead;

static UnattendedRead g_unattended;

/* The seat's callback. Lean, because on a running machine it is called from
 * the completion interrupt: it keeps what came and passes the rest on. */
static void unattended_read_answered(uint8_t index, uint8_t slot,
                                     error_t status, void* ctx)
{
    (void)index; (void)slot;
    UnattendedRead* r = (UnattendedRead*)ctx;
    r->status = status;
    BatonPass(&r->node);
}

/* The pump half: compares, says the verdict, gives the buffers back. */
static void unattended_read_judge(void* ctx)
{
    UnattendedRead* r    = (UnattendedRead*)ctx;
    const uint8_t*  got  = (const uint8_t*)vmm_phys_to_virt((uintptr_t)r->dma);
    const char*     name = BoardroomSeatName(r->seat);

    if (r->status != OK) {
        kprintf("[UNATTENDED READ] seat %u (%s): FAILED — the medium refused "
                "the read (error %d)\n", r->seat, name, (int)r->status);
    } else if (memcmp(got, r->expect, r->bytes) != 0) {
        uint32_t first = 0;
        while (first < r->bytes && got[first] == r->expect[first]) first++;
        kprintf("[UNATTENDED READ] seat %u (%s): FAILED — the two reads "
                "disagree, first at byte %u (0x%02x, expected 0x%02x)\n",
                r->seat, name, first, got[first], r->expect[first]);
    } else {
        kprintf("[UNATTENDED READ] seat %u (%s): PASSED — %u bytes read with "
                "nobody standing over them, identical to the ordinary read\n",
                r->seat, name, r->bytes);
    }

    pmm_free(r->dma, 1);
    kfree(r->expect);
    r->dma    = NULL;
    r->expect = NULL;
    __atomic_store_n(&r->judged, 1u, __ATOMIC_RELEASE);
}

/*
 * Turn the handle under the seat by hand.
 *
 * This runs before the machine is running. The BSP has not opened interrupts
 * yet — it does that on entering its K-Core loop, after kernel_main — so the
 * completion interrupt an AHCI port raises is left waiting in the LAPIC, and a
 * USB event lands on a ring nobody is reading. What the tick and the K-Core
 * loop do once the machine is up is done here instead, on this core and in the
 * same words: bring in what the medium has answered, and run the medium's own
 * watch over a read nobody stands over — the one that gives up on a silent
 * device at the device's own patience and answers for it. The proof therefore
 * has no clock of its own. A medium that answers ends the wait; one that does
 * not is given up on by the driver that owns it, and that ends the wait too.
 */
static void seat_turn_handle(const BoardSeat* s)
{
    switch (s->kind) {
    case BOARD_AHCI:
        ahci_watchdog_scan();
        break;
    case BOARD_USB:
        xhci_process_events();
        xhci_msd_watchdog();
        break;
    default:
        break;
    }
    BatonPump(amp_get_core_index());
}

void BoardroomProveUnattendedRead(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
    if (!s) {
        return;             /* no volume under this machine: nothing to prove */
    }
    if (!BoardroomSeatCanReadAsync(seat)) {
        kprintf("[UNATTENDED READ] not asked: seat %u (%s) answers only a read "
                "somebody stands over\n", seat, BoardroomSeatName(seat));
        return;
    }
    /* The condition under which the storage deck reads this way at all
     * (storage_ops.c, ObjRead): on one core every read is attended, and a proof
     * of a path this machine will never take says nothing about this machine.
     * It is also the condition under which the AHCI watch runs. */
    if (g_amp.total_cores < 2) {
        kprintf("[UNATTENDED READ] not asked: with one core every read is "
                "attended\n");
        return;
    }

    UnattendedRead* r    = &g_unattended;
    const char*     name = BoardroomSeatName(seat);

    /* One filesystem block, which is the unit everything above reads in, and
     * the only shape the unattended path accepts. Sector 0 because every
     * medium has one and its contents are not this proof's business — the
     * question is whether two ways of asking give the same answer. */
    const uint32_t sectors = BOARD_RUN_FALLBACK;
    const uint32_t bytes   = sectors * BOARDROOM_SECTOR_BYTES;

    void*    dma    = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    uint8_t* expect = (uint8_t*)kmalloc(bytes);
    if (!dma || !expect) {
        if (dma)    pmm_free(dma, 1);
        if (expect) kfree(expect);
        kprintf("[UNATTENDED READ] seat %u (%s): not run — no memory for it\n",
                seat, name);
        return;
    }

    if (BoardroomRead(seat, 0, sectors, expect) != 0) {
        kprintf("[UNATTENDED READ] seat %u (%s): not run — the ordinary read "
                "failed, so there is nothing to compare against\n", seat, name);
        pmm_free(dma, 1);
        kfree(expect);
        return;
    }

    r->node.run  = unattended_read_judge;
    r->node.ctx  = r;
    r->node.next = NULL;
    r->status    = OK;
    r->seat      = seat;
    r->dma       = dma;
    r->expect    = expect;
    r->bytes     = bytes;
    __atomic_store_n(&r->judged, 0u, __ATOMIC_RELAXED);

    error_t sub = BoardroomReadAsync(seat, 0, sectors, dma,
                                     unattended_read_answered, r, &r->berth);
    if (sub != OK) {
        kprintf("[UNATTENDED READ] seat %u (%s): FAILED — the seat would not "
                "take the read (error %d)\n", seat, name, (int)sub);
        pmm_free(dma, 1);
        kfree(expect);
        r->dma    = NULL;
        r->expect = NULL;
        return;
    }

    while (!__atomic_load_n(&r->judged, __ATOMIC_ACQUIRE)) {
        seat_turn_handle(s);
        cpu_pause();
    }
}

/* One cache flush, handed to the seat's driver: its answer. */
static int BoardSeatFlush(const BoardSeat* s, uint8_t seat)
{
    (void)seat;
#ifdef CONFIG_VOLUME_TESTS
    /* A plan that answers with a SCSI sense is judged by the USB disk's own
     * driver, as the device's answer would be — remembered, and said. */
    uint8_t sense[3];
    if (BlockFailFlushStrikes(seat, sense)) {
        if (s->kind == BOARD_USB && (sense[0] | sense[1] | sense[2]))
            return xhci_msd_flush_answered(s->index, sense[0], sense[1], sense[2]);
        return BoardMediumRefusal(s->kind, BLOCK_FAIL_FLUSH);
    }
#endif

    switch (s->kind) {
    case BOARD_AHCI: return ahci_flush_cache_sync(s->index);
    case BOARD_ATA:  return ata_flush_cache(s->index);
    case BOARD_USB:  return xhci_msd_flush(s->index);
    default:         return -1;
    }
}

int BoardroomFlush(uint8_t seat)
{
    LockRankWitness("flush", (uintptr_t)__builtin_return_address(0));
    BoardSeat* s = seat_taken(seat);
    if (!s) {
        return -1;
    }

    const uint32_t seating = __atomic_load_n(&s->seating, __ATOMIC_ACQUIRE);
    int            rc      = BoardSeatFlush(s, seat);
    BoardSeatAnswered(s, seating, rc == 0);
    return rc;
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

/* One line of the list that is printed when more than one volume is here. */
static void board_say_volume(uint8_t seat, const uint8_t uuid[16])
{
    char text[33];
    uuid_text(uuid, text);
    kprintf("[Boardroom]   seat %u: %s  %s\n", seat, BoardroomSeatName(seat),
            text);
}

uint8_t BoardroomFindVolume(BoardroomProbe probe, void* ctx,
                            uint8_t out_uuid[16])
{
    if (out_uuid) {
        memset(out_uuid, 0, 16);
    }
    if (!probe) {
        return BOARDROOM_NO_SEAT;
    }

    /* What the loader said, if it said anything. Read once: the answer cannot
     * change while the room is being called to order. */
    uint8_t  want[16];
    bool     have_pass = BoardingPassVolume(want);

    uint8_t  chosen      = BOARDROOM_NO_SEAT;
    uint8_t  chosen_uuid[16];
    uint8_t  by_identity = BOARDROOM_NO_SEAT;
    unsigned found       = 0;

    /*
     * The first volume met, held back by one line.
     *
     * The list below is only worth printing when there is more than one, and
     * that is not known until the second turns up — by which time the first has
     * been and gone. Two locals close that, and what they replace was a SECOND
     * WALK of the room afterwards calling probe() again on every chair. probe()
     * is the filesystem: it reads a partition table and a deed off each medium.
     * Sixteen bytes were being paid for with a re-read of every disk in the
     * machine.
     */
    uint8_t first_seat = BOARDROOM_NO_SEAT;
    uint8_t first_uuid[16];

    memset(chosen_uuid, 0, sizeof(chosen_uuid));
    memset(first_uuid, 0, sizeof(first_uuid));

    for (BoardSeat* s = g_seats; s; s = s->next) {
        uint8_t uuid[16];
        memset(uuid, 0, sizeof(uuid));

        /* An empty chair carries nothing, and probing one reads through a unit
         * number that now belongs to somebody else. */
        if (!s->occupied) {
            continue;
        }
        if (!probe(ctx, s->number, uuid)) {
            continue;
        }
        found++;

        if (found == 1) {
            first_seat = s->number;
            memcpy(first_uuid, uuid, 16);
        } else {
            if (found == 2) {
                kprintf("[Boardroom] a volume is here on more than one seat:\n");
                board_say_volume(first_seat, first_uuid);
            }
            board_say_volume(s->number, uuid);
        }

        if (have_pass && memcmp(uuid, want, 16) == 0 &&
            by_identity == BOARDROOM_NO_SEAT) {
            by_identity = s->number;
        }

        if (chosen == BOARDROOM_NO_SEAT) {
            chosen = s->number;
            memcpy(chosen_uuid, uuid, 16);
            continue;
        }

        /* The fallback, for when nothing on the bus is the volume named on the
         * pass — or there is no pass. */
        BoardSeat* c = seat_taken(chosen);
        if (c && !c->removable && s->removable) {
            chosen = s->number;
            memcpy(chosen_uuid, uuid, 16);
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
        /* It was chosen BECAUSE it matched, so the pass's sixteen bytes are
         * that seat's identity — there is nothing else it could be. */
        memcpy(chosen_uuid, want, 16);
        by_rule = false;
    }

    if (found == 0) {
        return BOARDROOM_NO_SEAT;
    }

    if (!by_rule) {
        kprintf("[Boardroom] seat %u carries the volume this kernel was read "
                "out of — the loader said so on its boarding pass\n", chosen);
        if (out_uuid) {
            memcpy(out_uuid, chosen_uuid, 16);
        }
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
        return BOARDROOM_NO_SEAT;
    }
    if (found > 1) {
        kprintf("[Boardroom] falling back to the rule: the removable medium "
                "wins — seat %u\n", chosen);
    }

    /* Which volume is about to be mounted, and on what grounds — the line that
     * explains every file the machine goes on to read. It scrolls past like
     * everything else and is recoverable like everything else, from the ring
     * the kernel keeps of its own boot. */
    if (out_uuid) {
        memcpy(out_uuid, chosen_uuid, 16);
    }
    return chosen;
}
