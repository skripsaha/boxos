#include "tagfs_volume.h"
#include "boardroom.h"
#include "touch.h"
#include "logbook.h"
#include "../deed/deed.h"
#include "../settle/settle.h"
#include "volume_lay.h"

/*
 * ‼ THE HOUSE: EVERY VOLUME THE MACHINE HOLDS.
 *
 * The home volume — the one this kernel was booted from, as the loader's pass
 * names it — is number 0 and always here. Every other BoxOS volume met on any
 * medium, in any seat, is taken in beside it the first time it is met,
 * numbered after the last, and mounted by its own service pass as any volume
 * is (tagfs.c). None is let go of this boot: a volume whose medium leaves
 * keeps its number and its place and waits for its medium (mount/away.h), and
 * a medium that comes back with it brings back the same volume — by its UUID,
 * in whichever seat. So a number names one volume for the whole boot, and the
 * list only grows: it is walked with no lock, each volume begun and its number
 * written before it is published by the `next` of the one before it.
 */
static TagFSVolume  s_home;
static TagFSVolume* s_last  = &s_home;
static uint32_t     s_count = 1;
static TagFSLock    s_house_lock = TAGFS_LOCK_INIT("house", LOCK_RANK_LEAF);

/* The boot mount tried: before it, a medium arriving belongs to the boot that
 * is still happening, for every volume (TagFSBootMountSettled). */
static volatile uint32_t s_boot_settled;

/* A medium seated since the last search, or a search one of whose media would
 * not be read and a seat answering again since: the media are searched again
 * for volumes the house does not hold (HouseSurvey). */
static volatile uint32_t s_survey_owed;
static volatile uint32_t s_survey_refused;
static volatile uint32_t s_surveying;

TagFSVolume* TagFSHome(void)
{
    return &s_home;
}

TagFSVolume* TagFSVolumeNext(TagFSVolume* vol)
{
    return vol ? __atomic_load_n(&vol->next, __ATOMIC_ACQUIRE) : &s_home;
}

uint32_t TagFSVolumeNumber(const TagFSVolume* vol)
{
    return vol->number;
}

bool TagFSVolumeMounted(const TagFSVolume* vol)
{
    return vol->state.initialized;
}

void TagFSVolumeUuid(const TagFSVolume* vol, uint8_t out[16])
{
    if (vol->mount.uuid_known)
        memcpy(out, vol->mount.uuid, 16);
    else
        memset(out, 0, 16);
}

void TagFSVolumeFactsOf(TagFSVolume* vol, TagFSVolumeFacts* out)
{
    memset(out, 0, sizeof(*out));
    out->number = vol->number;
    out->home   = vol == &s_home;
    if (vol->mount.uuid_known)
        memcpy(out->uuid, vol->mount.uuid, 16);
    uint8_t seat = vol->mount.seat;
    if (seat != BOARDROOM_NO_SEAT) {
        const char* name = BoardroomSeatName(seat);
        size_t      n    = name ? strnlen(name, sizeof(out->medium) - 1u) : 0;
        if (n)
            memcpy(out->medium, name, n);
        out->removable = BoardroomSeatKind(seat) == BOARD_USB;
    }
    out->away = vol->state.initialized && TagFSMediumLeft(vol);
    if (!tagfs_enter(vol))
        return;
    out->mounted   = true;
    out->read_only = TagFSIsUnderGlass(vol) || TagFSReadOnly(vol);
    out->bytes     = vol->state.volume_bytes;
    out->files     = vol->state.total_files;
    if (BlockMapDerivedUp(vol))
        out->free_bytes = AllotmentFreeUnpromised(vol) * (uint64_t)VOLUME_BLOCK_BYTES;
    tagfs_leave(vol);
}

uint32_t TagFSVolumesOn(uint8_t seat, uint32_t* numbers, uint64_t* grounds, uint32_t max)
{
    uint32_t seating = BoardroomSeatSeating(seat), found = 0;
    if (seating == 0)
        return 0;
    for (TagFSVolume* vol = TagFSVolumeNext(NULL); vol; vol = TagFSVolumeNext(vol)) {
        if (!vol->state.initialized || vol->mount.seat != seat || vol->mount.seating != seating)
            continue;
        if (found < max) {
            numbers[found] = vol->number;
            grounds[found] = vol->mount.base;
        }
        found++;
    }
    return found;
}

uint64_t TagFSHouseId(const TagFSVolume* vol, uint64_t fid)
{
    return fid ? ((uint64_t)vol->number << TAGFS_VOLUME_FILE_BITS) | fid : 0;
}

TagFSVolume* TagFSVolumeOfId(uint64_t id, uint64_t* fid)
{
    *fid = id & TAGFS_VOLUME_FILES_MOST;
    if (*fid == 0 || (id & TAGFS_PASSPORT_BIT))
        return NULL;
    TagFSVolume* vol = TagFSVolumeNumbered((uint32_t)(id >> TAGFS_VOLUME_FILE_BITS));
    return vol && vol->state.initialized ? vol : NULL;
}

TagFSVolume* TagFSVolumeNumbered(uint32_t number)
{
    for (TagFSVolume* vol = &s_home; vol; vol = TagFSVolumeNext(vol))
        if (vol->number == number)
            return vol;
    return NULL;
}

TagFSVolume* TagFSVolumeOfUuid(const uint8_t uuid[16])
{
    for (TagFSVolume* vol = &s_home; vol; vol = TagFSVolumeNext(vol))
        if (vol->mount.uuid_known && memcmp(vol->mount.uuid, uuid, 16) == 0)
            return vol;
    return NULL;
}

TagFSVolume* TagFSVolumeTakeIn(const uint8_t uuid[16])
{
    TagFSVolume* vol = kmalloc(sizeof(*vol));
    if (!vol)
        return NULL;
    TagFSVolumeBegin(vol);
    memcpy(vol->mount.uuid, uuid, 16);
    vol->mount.uuid_known         = true;
    vol->mount.boot_mount_settled = __atomic_load_n(&s_boot_settled, __ATOMIC_ACQUIRE);

    TagFSLockTake(&s_house_lock);
    if (s_count > TAGFS_VOLUMES_MOST) {
        TagFSLockDrop(&s_house_lock);
        kfree(vol);
        return NULL;
    }
    vol->number = s_count++;
    __atomic_store_n(&s_last->next, vol, __ATOMIC_RELEASE);
    s_last = vol;
    TagFSLockDrop(&s_house_lock);
    return vol;
}

void TagFSHouseBegin(void)
{
    TagFSVolumeBegin(&s_home);
}

/*
 * A volume met on `seat` at `base` that calls itself what volume `known` does
 * while `known` stands elsewhere: a copy — a medium written from another
 * block for block. Two volumes of one name are one too many: the copy is not
 * taken in, and said once a search, as what it is.
 */
static void HouseSeesCopy(TagFSVolume* known, uint8_t seat, uint64_t base)
{
    if (!known->state.initialized || (known->mount.seat == seat && known->mount.base == base))
        return;
    char text[33];
    DeedUuidText(known->mount.uuid, text);
    kprintf("[TagFS] seat %u carries a copy of volume %u of the house (%s, standing on seat %u) — the "
            "same volume twice is not taken in; the copy is left as it is\n",
            seat, known->number, text, known->mount.seat);
}

/*
 * Every BoxOS volume on every seated medium the house does not hold yet: taken
 * in, its arrival owed — its own service pass mounts it. The home volume
 * first: until it stands on a volume of its own, any volume met is the one it
 * may stand on, and none is taken in beside it. One search at a time; a medium
 * that would not be read leaves the search owed again when a seat answers.
 */
static void HouseSurvey(void)
{
    if (!s_home.mount.uuid_known || __atomic_exchange_n(&s_surveying, 1u, __ATOMIC_ACQ_REL))
        return;
    __atomic_store_n(&s_survey_owed, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_survey_refused, 0u, __ATOMIC_RELEASE);

    for (uint8_t seat = 0; seat < BoardroomSeatCount(); seat++) {
        if (!BoardroomSeatOccupied(seat))
            continue;
        MediumGround ground[GROUND_MAX_PER_MEDIUM];
        bool         refused = false;
        uint8_t      claimed = GroundSurvey(seat, ground, GROUND_MAX_PER_MEDIUM, &refused);
        if (refused)
            __atomic_store_n(&s_survey_refused, 1u, __ATOMIC_RELEASE);

        for (uint8_t g = 0; g < claimed; g++) {
            DeedCopy head;
            error_t  at_head = DeedReadHead(seat, &ground[g], &head);
            if (at_head != OK && DeedReadTailAlone(seat, &ground[g], &head) != OK) {
                if (at_head == ERR_IO)
                    __atomic_store_n(&s_survey_refused, 1u, __ATOMIC_RELEASE);
                continue;
            }
            uint8_t uuid[16];
            memcpy(uuid, head.head.uuid, 16);
            DeedRelease(&head);
            TagFSVolume* known = TagFSVolumeOfUuid(uuid);
            if (known) {
                HouseSeesCopy(known, seat, ground[g].start_sector);
                continue;
            }

            TagFSVolume* vol = TagFSVolumeTakeIn(uuid);
            if (!vol) {
                kprintf("[TagFS] seat %u carries a volume this machine does not hold, and the house "
                        "could not take it in — no memory for it, or every number given; its medium's "
                        "next arrival asks again\n", seat);
                continue;
            }
            kprintf("[TagFS] seat %u carries a volume this machine did not hold — taken in as volume %u "
                    "of the house\n", seat, vol->number);
            __atomic_store_n(&vol->mount.arrival_owed, 1u, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&s_surveying, 0u, __ATOMIC_RELEASE);
}

/*
 * Listening for the medium, instead of being told about it.
 *
 * The Boardroom used to call in here by name, which meant a room full of disks
 * had to know that somebody keeps a filesystem on one of them. Now it says
 * seat:taken and seat:emptied and this listens — so the dependency points the
 * way round it should, and anything else that cares about media arriving can
 * care without either side being changed. Every volume of the house hears
 * each: a medium seated may be any volume's coming back, or one the house
 * does not hold; a seat emptied may be any volume's leaving.
 *
 * ‼ AND NOTHING IS DONE ON THE PUBLISHER'S STACK. seat:taken used to mount the
 * volume right here, inside the room's own pass and Touch's delivery of it —
 * and the mount then said volume:mounted, and the autostart that listens for
 * THAT read programs off the medium: two publications deep, four and a half
 * kilobytes of the room's and Touch's frames under the first read, before the
 * medium's own driver and an interrupt landed on what was left. The TouchWatch
 * contract says what belongs on such a stack — note the event and let the loop
 * pick it up — and that is all these do: an arrival is a debt
 * (mount.arrival_owed, and the house's search) attended by the service pass
 * on its own stack (TagFSServiceIfPending), and a seat answering again only
 * knocks the cycle.
 */
static TouchWatch *g_seat_taken_watch   = NULL;
static TouchWatch *g_seat_emptied_watch = NULL;
static TouchWatch *g_seat_ready_watch   = NULL;

static void tagfs_on_seat_taken(TouchTag tag, const void *payload,
                                uint32_t plen, uint32_t source_pid, void *ctx)
{
    (void)tag; (void)payload; (void)plen; (void)source_pid; (void)ctx;
    for (TagFSVolume* vol = &s_home; vol; vol = TagFSVolumeNext(vol))
        __atomic_store_n(&vol->mount.arrival_owed, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_survey_owed, 1u, __ATOMIC_RELEASE);
}

static void tagfs_on_seat_emptied(TouchTag tag, const void *payload,
                                  uint32_t plen, uint32_t source_pid, void *ctx)
{
    (void)tag; (void)payload; (void)plen; (void)source_pid; (void)ctx;
    for (TagFSVolume* vol = &s_home; vol; vol = TagFSVolumeNext(vol))
        TagFSNoteMediumGone(vol);
}

/* A seat answers again: a refused cycle looks again, its count moved with the
 * bell, and a search one of whose media would not be read is owed again. A
 * mount owed is not asked here: the service pass compares the room's count
 * with the one its last attempt ended at (the note over mount.mount_owed) —
 * the announcement of a bell rung while that attempt read comes after the
 * attempt, and is no event of its debt. */
static void tagfs_on_seat_ready(TouchTag tag, const void *payload,
                                uint32_t plen, uint32_t source_pid, void *ctx)
{
    (void)tag; (void)source_pid; (void)ctx;
    const BoardroomSeatEvent *ev = payload;
    if (plen < sizeof(*ev))
        return;
    if (__atomic_load_n(&s_survey_refused, __ATOMIC_ACQUIRE))
        __atomic_store_n(&s_survey_owed, 1u, __ATOMIC_RELEASE);
    for (TagFSVolume* vol = &s_home; vol; vol = TagFSVolumeNext(vol)) {
        if (vol->state.initialized && ev->first == vol->mount.seat) {
            SettleKnock();
            return;
        }
    }
}

void TagFSWatchSeats(void)
{
    if (g_seat_taken_watch) {
        return;
    }

    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;

    TouchLogbookResolve("seat:taken", &full, &bare);
    g_seat_taken_watch = TouchWatchSet(full, tagfs_on_seat_taken, NULL);

    TouchLogbookResolve("seat:emptied", &full, &bare);
    g_seat_emptied_watch = TouchWatchSet(full, tagfs_on_seat_emptied, NULL);

    TouchLogbookResolve("seat:ready", &full, &bare);
    g_seat_ready_watch = TouchWatchSet(full, tagfs_on_seat_ready, NULL);

    if (!g_seat_taken_watch || !g_seat_emptied_watch || !g_seat_ready_watch) {
        kprintf("[TagFS] could not listen for media arriving — a volume that "
                "turns up later will not be noticed\n");
    }
}

void TagFSBootMountSettled(void)
{
    __atomic_store_n(&s_boot_settled, 1u, __ATOMIC_RELEASE);
    for (TagFSVolume* vol = &s_home; vol; vol = TagFSVolumeNext(vol))
        __atomic_store_n(&vol->mount.boot_mount_settled, 1u, __ATOMIC_RELEASE);
}

void TagFSAttendArrival(void)
{
    HouseSurvey();
    for (TagFSVolume* vol = &s_home; vol; vol = TagFSVolumeNext(vol))
        TagFSVolumeAttend(vol);
}

void TagFSHouseLook(void)
{
    __atomic_store_n(&s_survey_owed, 1u, __ATOMIC_RELEASE);
}

bool TagFSHouseLooking(void)
{
    return __atomic_load_n(&s_survey_owed, __ATOMIC_ACQUIRE) || __atomic_load_n(&s_surveying, __ATOMIC_ACQUIRE);
}

void TagFSServiceIfPending(void)
{
    if (__atomic_load_n(&s_survey_owed, __ATOMIC_ACQUIRE) && __atomic_load_n(&s_boot_settled, __ATOMIC_ACQUIRE))
        HouseSurvey();
    for (TagFSVolume* vol = &s_home; vol; vol = TagFSVolumeNext(vol))
        TagFSVolumeService(vol);
    VolumeLayServe();
}

void TagFSMemoryCameBack(void)
{
    for (TagFSVolume* vol = &s_home; vol; vol = TagFSVolumeNext(vol))
        TagFSVolumeMemoryBack(vol);
}
