#include "boardroom.h"
#include "ground.h"
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
#include "baton.h"
#include "amp.h"
#include "pmm.h"
#include "vmm.h"
#include "cpu_calibrate.h"
#include "atomics.h"

typedef struct BoardSeat {
    struct BoardSeat* next;
    uint8_t   number;
    BoardKind kind;
    uint8_t   index;
    bool      removable;
    bool      occupied;
    uint32_t  seating;
    char      name[48];
} BoardSeat;

static BoardSeat* g_seats = NULL;
static uint8_t    g_seat_count = 0;
static uint32_t   g_seatings = 0;
static bool       g_initialised = false;

static spinlock_t g_seats_lock;
static bool       g_seats_lock_ready = false;

static volatile uint32_t g_in_session = 0;

static void seats_lock_init(void)
{
    if (!g_seats_lock_ready) {
        spinlock_init(&g_seats_lock);
        g_seats_lock_ready = true;
    }
}

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

static BoardSeat* seat_taken(uint8_t number)
{
    BoardSeat* s = seat_find(number);
    return (s && s->occupied) ? s : NULL;
}

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
            s = c;
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
    for (uint8_t ci = 0; ci < xhci_controller_count(); ci++) {
      xhci_controller_t* ctrl = xhci_controller_at(ci);
      if (!ctrl) continue;

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
    seats_lock_init();

    while (__atomic_exchange_n(&g_in_session, 1u, __ATOMIC_ACQUIRE) != 0) {
        cpu_pause();
    }

    uint32_t seatings_before = g_seatings;

    if (!g_initialised) {
        g_seats = NULL;
        g_seat_count = 0;
        g_initialised = true;
    }

    seat_usb_take_attendance();

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
        uint8_t probe[BOARDROOM_SECTOR_BYTES];
        for (uint8_t d = 0; d < 4; d++) {
            if (ata_read_sectors_retry(d, 0, 1, probe) != 0) {
                continue;
            }
            seat_take(BOARD_ATA, d, false, ata_slot_name(d));
        }
    }

    if (g_seatings == seatings_before) {
        __atomic_store_n(&g_in_session, 0u, __ATOMIC_RELEASE);
        return;
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

        if (s->seating <= seatings_before) {
            continue;
        }

        changed++;
        if (first == BOARDROOM_NO_SEAT) {
            first = s->number;
        }

        MediumGround ground[GROUND_MAX_PER_MEDIUM];
        uint8_t claimed = GroundSurvey(s->number, ground, GROUND_MAX_PER_MEDIUM);
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

    BoardroomSeatEvent ev = { seat_occupied_count(), changed, first, 0 };

    __atomic_store_n(&g_in_session, 0u, __ATOMIC_RELEASE);

    TouchPublish("seat:taken", &ev, sizeof(ev));
}

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
        s->occupied = false;
    }
    uint8_t seated = seat_occupied_count();
    spin_unlock(&g_seats_lock);

    if (!s) {
        return;
    }

    kprintf("[Boardroom] seat %u is empty — %s has left\n", s->number, s->name);

    BoardroomSeatEvent ev = { seated, 1, s->number, 0 };
    TouchPublish("seat:emptied", &ev, sizeof(ev));
}

void BoardroomAttendIfPending(void)
{
    if (__atomic_load_n(&g_arrival_pending, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    static volatile uint32_t busy = 0;
    if (__atomic_exchange_n(&busy, 1u, __ATOMIC_ACQUIRE) != 0) {
        return;
    }
    __atomic_store_n(&g_arrival_pending, 0u, __ATOMIC_RELEASE);

    uint32_t before = g_seatings;

    BoardroomInit();

    if (g_seatings != before) {
        kprintf("[Boardroom] %u medium/media arrived after the room was called "
                "to order\n", (unsigned)(g_seatings - before));
    }

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

static uint64_t seat_sectors_in_512(uint64_t total, uint32_t logical)
{
    if (logical == 0 || (logical % BOARDROOM_SECTOR_BYTES) != 0) {
        return 0;
    }
    return total * (uint64_t)(logical / BOARDROOM_SECTOR_BYTES);
}

uint64_t BoardroomSeatSectors(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
    if (!s) return 0;

    switch (s->kind) {
    case BOARD_USB:
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

    if (run == 0) {
        run = BOARD_RUN_FALLBACK;
    }
    if (run > 65535u) {
        run = 65535u;
    }
    return run;
}

int BoardroomRead(uint8_t seat, uint64_t lba, uint32_t count, void* buffer)
{
    BoardSeat* s = seat_taken(seat);
    if (!s || !buffer || count == 0) {
        return -1;
    }

    const uint32_t most = BoardroomSeatRun(seat);
    uint8_t* out = (uint8_t*)buffer;
    while (count > 0) {
        uint32_t run = (count > most) ? most : count;
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
    BoardSeat* s = seat_taken(seat);
    if (!s || !buffer || count == 0) {
        return -1;
    }

    const uint32_t most = BoardroomSeatRun(seat);
    const uint8_t* in = (const uint8_t*)buffer;
    while (count > 0) {
        uint32_t run = (count > most) ? most : count;
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
    BoardSeat* s = seat_taken(seat);
    if (!s) {
        return false;
    }

    switch (s->kind) {
    case BOARD_AHCI: return ahci_is_initialized();
    case BOARD_USB:  return xhci_msd_unit_can_read_async(s->index);
    case BOARD_ATA:
    default:         return false;
    }
}

error_t BoardroomReadAsync(uint8_t seat, uint64_t lba, uint32_t count,
                           void* dma_phys, BoardroomAsyncCb cb, void* ctx)
{
    BoardSeat* s = seat_taken(seat);
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


typedef struct {
    Baton    node;
    error_t  status;
    uint8_t  seat;
    void*    dma;
    uint8_t* expect;
    uint32_t bytes;
    volatile uint8_t judged;
} UnattendedRead;

static UnattendedRead g_unattended;

static void unattended_read_answered(uint8_t index, uint8_t slot,
                                     error_t status, void* ctx)
{
    (void)index; (void)slot;
    UnattendedRead* r = (UnattendedRead*)ctx;
    r->status = status;
    BatonPass(&r->node);
}

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
        return;
    }
    if (!BoardroomSeatCanReadAsync(seat)) {
        kprintf("[UNATTENDED READ] not asked: seat %u (%s) answers only a read "
                "somebody stands over\n", seat, BoardroomSeatName(seat));
        return;
    }
    if (g_amp.total_cores < 2) {
        kprintf("[UNATTENDED READ] not asked: with one core every read is "
                "attended\n");
        return;
    }

    UnattendedRead* r    = &g_unattended;
    const char*     name = BoardroomSeatName(seat);

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
                                     unattended_read_answered, r);
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

int BoardroomFlush(uint8_t seat)
{
    BoardSeat* s = seat_taken(seat);
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

static void uuid_text(const uint8_t uuid[16], char out[33])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = hex[(uuid[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[uuid[i] & 0xF];
    }
    out[32] = '\0';
}

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

    uint8_t  want[16];
    bool     have_pass = BoardingPassVolume(want);

    uint8_t  chosen      = BOARDROOM_NO_SEAT;
    uint8_t  chosen_uuid[16];
    uint8_t  by_identity = BOARDROOM_NO_SEAT;
    unsigned found       = 0;

    uint8_t first_seat = BOARDROOM_NO_SEAT;
    uint8_t first_uuid[16];

    memset(chosen_uuid, 0, sizeof(chosen_uuid));
    memset(first_uuid, 0, sizeof(first_uuid));

    for (BoardSeat* s = g_seats; s; s = s->next) {
        uint8_t uuid[16];
        memset(uuid, 0, sizeof(uuid));

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

        BoardSeat* c = seat_taken(chosen);
        if (c && !c->removable && s->removable) {
            chosen = s->number;
            memcpy(chosen_uuid, uuid, 16);
        }
    }

    bool by_rule = true;
    if (by_identity != BOARDROOM_NO_SEAT) {
        chosen  = by_identity;
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

    if (have_pass) {
        char text[33];
        uuid_text(want, text);
        kprintf("[Boardroom] the loader came from the volume %s, and no seat "
                "is carrying it\n", text);
        kprintf("[Boardroom] %u volume(s) are here and none of them is this "
                "machine's — mounting nothing, and waiting for the medium to "
                "arrive\n", found);
        return BOARDROOM_NO_SEAT;
    }
    if (found > 1) {
        kprintf("[Boardroom] falling back to the rule: the removable medium "
                "wins — seat %u\n", chosen);
    }

    if (out_uuid) {
        memcpy(out_uuid, chosen_uuid, 16);
    }
    return chosen;
}