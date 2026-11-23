#include "tagfs.h"
#include "boardroom.h"
#include "boarding.h"
#include "deed/deed.h"
#include "deed_pen.h"
#include "tagfs_reserved.h"
#include "tag_registry/tag_registry.h"
#include "file_map/file_map.h"
#include "use_context.h"
#include "use_memory/use_memory.h"
#include "volume/volume_name.h"
#include "error.h"
#include "ahci_sync.h"
#include "ahci.h"
#include "touch.h"
#include "ata.h"
#include "amp.h"
#include "braid/braid.h"
#include "integrity/integrity.h"
#include "settle_cut/settle_cut.h"
#include "plot/plot.h"
#include "scribe/scribe.h"
#include "ink/ink.h"
#include "ink/fallow.h"
#include "settle/settle.h"
#include "helm/helm.h"
#include "keep/keep.h"
#include "retire/retire.h"
#include "volume/carry.h"
#include "mount/away.h"
#include "mount/perambulation.h"
#include "mount/hold_ground.h"
#include "mount/book_mount.h"
#include "mount/errata.h"
#include "read/read.h"
#include "run/run.h"
#include "../../kernel/drivers/timer/rtc.h"
#include "logbook.h"
#include "cpu_calibrate.h"
#include "pmm.h"            /* PmmFreePagesNow — the memory a refused mount waits for */
#include "baton.h"          /* the Knock that wakes a service pass for that mount */
#include "tagfs_volume.h"

// ----------------------------------------------------------------------------
// The door — who is inside the mounted volume, and which mounting it is
// ----------------------------------------------------------------------------

/*
 * ‼ A VOLUME WAS TAKEN DOWN WHILE OTHER CORES WERE STANDING INSIDE IT.
 *
 * A medium that leaves and does not come back as it was left is re-read
 * rather than carried on from — its open book given up (mount/away.h) — and
 * that road is: take the volume down, give its memory back, mount again
 * (TagFSVolumeReturned). Nothing in it ever asked whether anybody was in
 * there. On another core there could be a read walking a file's extents, a
 * query walking the tag index, a process being asked whether it carries a tag
 * — every one of them holding a pointer into exactly what tagfs_clear_the_ground
 * is about to hand back to the allocator: the registry, the tag index, the
 * block map, the book's memory.
 *
 * ‼ THE DEPARTURE'S LATCH DOES NOT COVER IT. mount.medium_left makes every read
 * and every write refuse, which is what stops work that has not started. It
 * does nothing about a caller who is already inside a call and already
 * holding those pointers.
 *
 * So: the same arrangement the xHCI slots use, for the same reason and in the
 * same words. A caller says it is coming in and says when it leaves; the door
 * is shut before anything is taken down; and the memory goes back only once
 * the last caller is out. A take-down that finds somebody inside takes NOTHING
 * away and comes back for it later (TagFSServiceIfPending) — waiting on the
 * spot is not allowed, because the spot is the guide loop, and the guide loop
 * is where the transfer that the caller inside is waiting for gets finished.
 *
 * ‼ AND THE MOUNTING IS STAMPED, BECAUSE A FILE HANDLE OUTLIVES A MOUNT.
 * A TagFSFileHandle carries its own copy of the file's extents. Across a
 * re-read those block numbers describe whatever the volume keeps there NOW,
 * and following them is not a failure, it is a read of somebody else's file.
 * A handle from a previous mounting is refused instead.
 */
typedef enum {
    VOLUME_DOWN = 0,    /* nothing is mounted */
    VOLUME_UP,          /* mounted, and callers may come in */
    VOLUME_LEAVING      /* mounted and going: nobody new in, nothing freed yet */
} VolumeDoor;







static void attend_arrival(TagFSVolume* vol);

static bool attending_take(TagFSVolume* vol)
{
    return __atomic_exchange_n(&vol->mount.attending, 1u, __ATOMIC_ACQUIRE) == 0;
}

/* Every attending ends here: an arrival noted while the flag was held found it
 * taken and only left its debt, so it is attended now, by the holder, before
 * the flag goes. One noted after the last look is picked up by the next
 * service pass — the debt stays until somebody attends it. */
static void attending_done(TagFSVolume* vol)
{
    while (__atomic_load_n(&vol->mount.arrival_owed, __ATOMIC_ACQUIRE)) {
        attend_arrival(vol);
    }
    __atomic_store_n(&vol->mount.attending, 0u, __ATOMIC_RELEASE);
}

bool tagfs_enter(TagFSVolume* vol)
{
    /*
     * Counted first, then checked.
     *
     * The other order leaves a window: a take-down that lands between the
     * check and the count sees an empty volume and gives its memory back
     * underneath a caller who has just satisfied himself it was there.
     * Counting first closes it — either the take-down sees this caller, or
     * this caller sees the take-down. One of the two always happens.
     */
    __atomic_fetch_add(&vol->mount.inside, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&vol->mount.door, __ATOMIC_ACQUIRE) != VOLUME_UP) {
        __atomic_fetch_sub(&vol->mount.inside, 1, __ATOMIC_ACQ_REL);
        return false;
    }
    return true;
}

void tagfs_leave(TagFSVolume* vol)
{
    __atomic_fetch_sub(&vol->mount.inside, 1, __ATOMIC_ACQ_REL);
}

uint32_t tagfs_mount_epoch(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->mount.mount_epoch, __ATOMIC_ACQUIRE);
}

/* It is up — by this road or any other — so nothing is owed, and the next
 * refusal is an episode of its own. */
static void mount_settled(TagFSVolume* vol)
{
    __atomic_store_n(&vol->mount.mount_heap_owed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&vol->mount.mount_owed, 0u, __ATOMIC_RELEASE);
    vol->mount.reset_seating = 0;
}

/* Every bell the room rang up to now is heard — by an attempt, a mount or the
 * search for a volume whose book is held open, that has read all it will, or
 * by one about to be asked again: only a bell after this asks the mount again
 * (the note over mount.mount_owed). Under the one-at-a-time flag. */
static void bells_heard(TagFSVolume* vol)
{
    __atomic_store_n(&vol->mount.mount_rang, BoardroomSeatReadyGen(BOARDROOM_NO_SEAT), __ATOMIC_SEQ_CST);
}

/* The memory a mount was refused for has come back: more of the machine's
 * pages free than when it was. */
static bool mount_memory_back(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->mount.mount_heap_owed, __ATOMIC_ACQUIRE) != 0 &&
           PmmFreePagesNow() > __atomic_load_n(&vol->mount.mount_heap_level, __ATOMIC_ACQUIRE);
}

/* A mount owed is asked again: a bell rung after its last attempt ended, or the
 * memory it was refused for came back. */
static bool mount_asked_again(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->mount.mount_owed, __ATOMIC_ACQUIRE) != 0 &&
           (BoardroomSeatReadyGen(BOARDROOM_NO_SEAT) !=
                __atomic_load_n(&vol->mount.mount_rang, __ATOMIC_ACQUIRE) ||
            mount_memory_back(vol));
}

/* The drain core knocked awake for a mount owed on memory: its loop's service
 * pass (TagFSServiceIfPending) asks the mount again — the visit itself stands
 * over nothing. */
static void mount_knocked(void *ctx);

static void mount_knocked(void *ctx)
{
    TagFSVolume* vol = ctx;
    KnockOpen(&vol->mount.mount_knock);
}

void TagFSVolumeMemoryBack(TagFSVolume* vol)
{
    TagFSHeapCameBack(vol);
    if (__atomic_load_n(&vol->mount.mount_heap_owed, __ATOMIC_ACQUIRE))
        KnockOn(&vol->mount.mount_knock);
}

bool tagfs_handle_is_of_this_mount(TagFSVolume* vol, const TagFSFileHandle *handle)
{
    return handle && handle->mount_epoch == tagfs_mount_epoch(vol);
}

/* Said rather than returned, because every caller of a read is written to
 * treat a refusal as an I/O failure and would otherwise report the medium as
 * broken. What happened is that the volume was read again. */
static bool handle_belongs_here(TagFSVolume* vol, const TagFSFileHandle *handle, const char *what)
{
    if (tagfs_handle_is_of_this_mount(vol, handle)) {
        return true;
    }
    if (!handle) {
        return false;               /* not a stale handle — no handle at all */
    }
    kprintf("[TagFS] %s refused: file %llu was opened on an earlier mounting of "
            "this volume, and the volume has been read again since\n",
            what, handle ? handle->file_id : 0);
    return false;
}

// ----------------------------------------------------------------------------
// Disk I/O helpers
// ----------------------------------------------------------------------------

uint8_t tagfs_get_seat(TagFSVolume* vol) { return vol->mount.seat; }

/*
 * Recognise the volume: the superblock magic, in the sector it always lives in.
 * Handed to the Boardroom, which asks it of every medium in turn — the
 * Boardroom knows about media, and this is the only thing that knows what a
 * TagFS volume looks like.
 */

static void search_met_unread(uint8_t *unread, uint8_t seat)
{
    if (unread && *unread == BOARDROOM_NO_SEAT)
        *unread = seat;
}

/*
 * Which volume `vol` stands for. Once it has stood on one, that one and no
 * other: a volume of the house is its UUID, in whichever seat (volume/house.c).
 * Before that — the home volume, never yet mounted — the one the loader read
 * this kernel out of, as the boarding pass names it, and no other either: a
 * machine booted from a stick that mounts its old internal disk instead, when
 * the stick is slower to arrive, runs programs out of the wrong decade. Only a
 * machine with no pass stands on the first volume it can read. False when
 * there is nothing to want; *strict then false, else true.
 */
static bool volume_wants(TagFSVolume* vol, uint8_t want[16], bool* strict)
{
    if (vol->mount.uuid_known) {
        memcpy(want, vol->mount.uuid, 16);
        *strict = true;
        return true;
    }
    *strict = BoardingPassVolume(want);
    return *strict;
}

/*
 * Is there a TagFS volume in this seat, and what does it call itself?
 *
 * The magic says it is one of ours. The identity says WHICH one — and that is
 * the question: a machine carrying two TagFS volumes is not unusual, it is
 * what a machine with BoxOS installed and a BoxOS stick in a socket looks
 * like, and each is a volume of the house (volume_wants). With `any`, the
 * question is only whether a volume is there at all, whichever it is.
 *
 * The search keeps the first seat whose medium refused the reads that would
 * have answered (mount.search_unread): the mount that asked is owed, not
 * finished (mount_attempt).
 */
static bool volume_recognised(TagFSVolume* vol, uint8_t seat, uint8_t out_uuid[16], bool any_volume)
{
    uint8_t *unread = &vol->mount.search_unread;

    MediumGround ground[GROUND_MAX_PER_MEDIUM];
    bool    refused = false;
    uint8_t claimed = GroundSurvey(seat, ground, GROUND_MAX_PER_MEDIUM, &refused);
    if (refused) search_met_unread(unread, seat);
    if (claimed == 0) return false;

    /*
     * A medium may carry more than one BoxOS partition, and then "is there a
     * volume here" has more than one answer. The volume asking is the answer
     * that matters, so it is consulted here rather than only by the Boardroom
     * afterwards — the Boardroom compares one identity per seat and would
     * otherwise be handed whichever ground happened to be first in the table.
     */
    uint8_t want[16];
    bool    strict = false;
    bool    have_want = !any_volume && volume_wants(vol, want, &strict);

    uint8_t first[16];
    bool    any = false;

    for (uint8_t g = 0; g < claimed; g++) {
        DeedCopy head;
        error_t  at_head = DeedReadHead(seat, &ground[g], &head);
        if (at_head != OK) {
            /* The head is what is damaged, not the volume. Its far copy names
             * the same volume and is the reason there are two. A head the
             * medium would not give up, with no far copy read either, is a
             * ground nobody knows the volume of. */
            if (DeedReadTailAlone(seat, &ground[g], &head) != OK) {
                if (at_head == ERR_IO) search_met_unread(unread, seat);
                continue;
            }
        }

        bool wanted = have_want && memcmp(head.head.uuid, want, 16) == 0;
        if (!any) {
            memcpy(first, head.head.uuid, 16);
            any = true;
        }
        DeedRelease(&head);

        if (wanted) {
            if (out_uuid) memcpy(out_uuid, want, 16);
            return true;
        }
    }

    if (strict)
        return false;
    if (any && out_uuid) memcpy(out_uuid, first, 16);
    return any;
}

/* The Boardroom's probe of a seat for the volume `ctx` stands for. */
static bool tagfs_recognise(void *ctx, uint8_t seat, uint8_t out_uuid[16])
{
    return volume_recognised((TagFSVolume*)ctx, seat, out_uuid, false);
}


uint64_t tagfs_get_volume_base(TagFSVolume* vol) { return vol->mount.base; }

/*
 * The volume wanted is on no seat, said as what it is. The one the loader
 * came from is waited for, the other volumes the room carries declined: until
 * it arrives this machine has no volume of its own, which is the truth, and
 * re-seating the medium is what a person does about it — the arrival that
 * ends the wait. Counted only here, where nothing was found.
 */
static void volume_not_found(TagFSVolume* vol, bool by_pass)
{
    uint32_t others = 0;
    for (uint8_t seat = 0; seat < BoardroomSeatCount(); seat++)
        if (BoardroomSeatOccupied(seat) && volume_recognised(vol, seat, NULL, true))
            others++;
    if (by_pass && others) {
        uint8_t want[16];
        char    text[33];
        BoardingPassVolume(want);
        DeedUuidText(want, text);
        kprintf("[TagFS] the loader came from the volume %s, and no seat is carrying it\n", text);
        kprintf("[TagFS] %u volume(s) are here and none of them is this machine's — mounting nothing, "
                "and waiting for the medium to arrive\n", others);
        return;
    }
    kprintf("[TagFS] no volume found on any of the %u seated medium(s)\n", BoardroomSeatCount());
}

static void TagFSProbeDrive(TagFSVolume* vol)
{
    /*
     * The identity comes back with the seat, because the room had it.
     *
     * tagfs_recognise is what told the room which volume that seat carries, and
     * this used to call it a SECOND time on the chosen seat purely to be told
     * again — a full survey of that medium's partition table and another read
     * of its deed, for sixteen bytes the room was already holding.
     *
     * Into a local, and copied over only once a seat has been found: what this
     * volume calls itself is what lets it be recognised when its medium is
     * pushed back in, so it has to survive a mount that FAILS. A re-mount
     * attempted in the instant the stick left again would otherwise overwrite
     * the identity with nothing, and the volume could never be matched on its
     * return for the rest of the boot.
     */
    uint8_t uuid[16];
    bool    by_pass = !vol->mount.uuid_known && BoardingPassVolume(uuid);
    vol->mount.search_unread = BOARDROOM_NO_SEAT;
    vol->mount.seat = BoardroomFindVolume(tagfs_recognise, vol, uuid);

    if (vol->mount.seat == BOARDROOM_NO_SEAT) {
        volume_not_found(vol, by_pass);
        return;
    }
    if (by_pass)
        kprintf("[TagFS] seat %u carries the volume this kernel was read out of — the loader said so on "
                "its boarding pass\n", vol->mount.seat);

    memcpy(vol->mount.uuid, uuid, 16);
    vol->mount.uuid_known = true;
    vol->mount.seating     = BoardroomSeatSeating(vol->mount.seat);

    kprintf("[TagFS] volume on seat %u: %s\n", vol->mount.seat,
            BoardroomSeatName(vol->mount.seat));

    /*
     * And how much this medium will be asked for at a time, said out loud
     * because it is a property of the MEDIUM and not of the filesystem: a
     * legacy channel answers eight sectors — one block — because past that it
     * gives up its DMA path; a flash drive and a SATA port answer far more,
     * and a file's neighbouring blocks then move together, read or written.
     * Printing it is what turns "why is this machine slower off that disk"
     * into a sentence somebody can read off a screen.
     */
    kprintf("[TagFS] this medium hands over %u sector(s) in one go, which is %u "
            "of this volume's %u-byte block(s) — a read in order fetches that "
            "many neighbours in one command, into a window of its own handle, "
            "and a write lays that many of a file's blocks in one\n",
            BoardroomSeatRun(vol->mount.seat), TagFSRunBlocks(vol), (unsigned)TAGFS_BLOCK_SIZE);
}

static bool volume_not_here(TagFSVolume* vol);

/* A run of any length, in the volume's own sectors: the room cuts it into the
 * runs the medium takes (BoardroomSeatRun). There used to be a door with a
 * 16-bit count in front of these, and a bitmap of a volume past a terabyte
 * went through it as a read of 0 sectors. Refused while the medium is away —
 * a Settle cycle between two steps included: its next step would otherwise
 * write the volume's records onto whatever sits in the seat now — and refused
 * as what it is, ERR_DEVICE_NOT_READY, a code no medium's refusal is: a caller
 * that passes a door's positive answer on says the medium left, not that it
 * failed or that the volume is full. */
int tagfs_volume_read(TagFSVolume* vol, uint64_t vlba, uint32_t count, void *buffer)
{
    if (volume_not_here(vol))
        return (int)ERR_DEVICE_NOT_READY;
    return BoardroomRead(vol->mount.seat, vol->mount.base + vlba, count, buffer);
}

void TagFSNoteReadRefused(TagFSVolume* vol)
{
    __atomic_add_fetch(&vol->mount.reads_refused, 1u, __ATOMIC_RELAXED);
}

int tagfs_volume_write(TagFSVolume* vol, uint64_t vlba, uint32_t count, const void *buffer)
{
    if (volume_not_here(vol))
        return (int)ERR_DEVICE_NOT_READY;
    if (TagFSIsUnderGlass(vol))
        return (int)TagFSGlassRefused(vol, "the volume's sector door");
    int rc = BoardroomWrite(vol->mount.seat, vol->mount.base + vlba, count, buffer, NULL);
    if (rc == 0)
        VolumeWriteSeen(vol);
    return rc;
}

/* The seat and the base the search and the ground named, let go of: the
 * volume on them was turned away. */
static void VolumeTurnedAway(TagFSVolume* vol)
{
    vol->mount.seat    = BOARDROOM_NO_SEAT;
    vol->mount.seating = 0;
    vol->mount.base   = 0;
}

/*
 * ‼ A VOLUME TURNED AWAY IS NOT A VOLUME STOOD ON. Every refusal after a Deed
 * was read lets go of the seat and base the search and the ground named. Kept,
 * the boot survey passed the refused ground over as "the volume this machine
 * is standing on" (DeedSurveyAll), and the write door — which with no volume
 * up asks only whether the seat is occupied (volume_not_here) — let a write
 * through to it. Let go of, the room refuses a seat that is not there.
 */
static error_t VolumeGroundRefused(TagFSVolume* vol, DeedCopy *head, error_t why)
{
    DeedRelease(head);
    VolumeTurnedAway(vol);
    vol->state.block_end  = 0;
    MountBookLetGo(vol);
    TagFSUnderGlass(vol, NULL);
    return why;
}

/* What the Deed says of the volume: the grid it was laid out for, where the
 * loader finds the kernel, who the volume is and how far it runs. Each cleared first: a volume whose Deed carries no grid or no BOOT is
 * judged by what it says, never by a volume mounted before it in this boot. */
static void VolumeTakeDeed(TagFSVolume* vol, const DeedCopy *head)
{
    uint16_t bytes = 0;
    const VolumeGeometry *geo =
        (const VolumeGeometry *)DeedStamp(head, VOLUME_STAMP_GEOMETRY, &bytes);
    memset(&vol->state.geometry, 0, sizeof(vol->state.geometry));
    if (geo && bytes >= sizeof(VolumeGeometry))
        memcpy(&vol->state.geometry, geo, sizeof(vol->state.geometry));

    /* Where the loader finds the kernel, so nothing moves those blocks under
     * it (PlotGather). A Deed without the stamp names no such blocks. */
    const VolumeBoot *boot = (const VolumeBoot *)DeedStamp(head, VOLUME_STAMP_BOOT, &bytes);
    memset(&vol->state.boot, 0, sizeof(vol->state.boot));
    if (boot && bytes >= sizeof(VolumeBoot))
        memcpy(&vol->state.boot, boot, sizeof(vol->state.boot));

    memcpy(vol->state.uuid, head->head.uuid, 16);
    memset(vol->state.volume_name, 0, sizeof(vol->state.volume_name));
    const VolumeBorn *born = (const VolumeBorn *)DeedStamp(head, VOLUME_STAMP_BORN, &bytes);
    if (born && bytes >= sizeof(*born))
        memcpy(vol->state.volume_name, born->maker, sizeof(vol->state.volume_name));
    vol->state.volume_bytes = head->head.sectors * VOLUME_DEED_SECTOR_BYTES;
}

/*
 * Stand the volume on its ground.
 *
 * Everything the filesystem knows about where things are comes from here: the
 * medium's partition table says which run of sectors is ours, the Deed at the
 * head of that run says what is laid out inside it, and both are read fresh
 * every time a volume is met. Nothing is remembered between boots and nothing
 * is assumed.
 */
static error_t volume_take_ground(TagFSVolume* vol, uint8_t seat)
{
    MediumGround ground[GROUND_MAX_PER_MEDIUM];
    bool    refused = false;
    uint8_t claimed = GroundSurvey(seat, ground, GROUND_MAX_PER_MEDIUM, &refused);
    if (claimed == 0) {
        /* Said as what it is: the medium would not say, or said there is no
         * ground — only the first is a refusal worth asking about again. */
        vol->mount.ground_refused = refused;
        kprintf("[TagFS] seat %u claims no ground for BoxOS\n", seat);
        return ERR_FILE_NOT_FOUND;
    }

    uint8_t want[16];
    bool    strict    = false;
    bool    have_want = volume_wants(vol, want, &strict);

    DeedCopy head;
    bool     took          = false;
    bool     unread        = false;   /* a ground whose deeds the medium would not give up */
    uint64_t ground_run    = 0;   /* how far the partition runs, kept past `g` */

    /* Two passes rather than one with a "best so far": the volume wanted is
     * the one this volume of the house is, or the one this kernel came out of,
     * and taking a different one because it happened to be listed first is
     * how a machine mounts the stick it booted next to instead of the stick it
     * booted from. A volume that has stood on one takes that one or none. */
    for (int pass = 0; pass < 2 && !took; pass++) {
        for (uint8_t g = 0; g < claimed; g++) {
            DeedCopy candidate;
            bool from_tail = false;
            error_t at_head = DeedReadHead(seat, &ground[g], &candidate);
            if (at_head != OK) {
                if (DeedReadTailAlone(seat, &ground[g], &candidate) != OK) {
                    if (at_head == ERR_IO) unread = true;
                    continue;
                }
                from_tail = true;
            }

            /* With a volume wanted, the first walk takes only that one;
             * without one there is nothing to prefer, and the first walk
             * takes the first volume it can read. */
            bool wanted = have_want &&
                          memcmp(candidate.head.uuid, want, 16) == 0;
            if (pass == 0 && have_want && !wanted) {
                DeedRelease(&candidate);
                continue;
            }

            head          = candidate;
            vol->mount.base = ground[g].start_sector;
            ground_run    = ground[g].sectors;
            took          = true;

            if (from_tail) {
                kprintf("[TagFS] seat %u: the deed at the head of this volume "
                        "is gone; standing on the copy at the far end\n", seat);
            }

            kprintf("[TagFS] volume on seat %u stands on %s ground: sectors "
                    "%llu..%llu\n", seat, GroundOriginName(ground[g].origin),
                    (unsigned long long)ground[g].start_sector,
                    (unsigned long long)(ground[g].start_sector +
                                         ground[g].sectors - 1));
            break;
        }
        if (!have_want || strict) break;   /* nothing to prefer, or nothing else to take */
    }

    if (!took) {
        vol->mount.ground_refused = unread;
        kprintf("[TagFS] seat %u has ground claimed for BoxOS and no deed on "
                "it this kernel can read\n", seat);
        return ERR_TAGFS_CORRUPTED;
    }

    DeedDescribe(seat, &head);

    /* ‼ WHETHER IT IS A BOOK — asked before anything of the volume is read or
     * written: a Deed this kernel cannot stand on is refused untouched. Then
     * where the book is: its FRAME and the medium's grid under it
     * (book_mount.h). */
    error_t turned = DeedRefused(seat, &head);
    if (turned != OK)
        return VolumeGroundRefused(vol, &head, turned);

    VolumeTakeDeed(vol, &head);
    turned = MountBookGround(vol, seat, &head, ground_run);
    if (turned != OK)
        return VolumeGroundRefused(vol, &head, turned);

    /*
     * What the volume was laid out for, against what the medium says it is
     * made of. Three answers, and each of them is worth having:
     *
     *   they agree            — nothing to say
     *   the volume is coarser — harmless; it simply uses whole blocks of a
     *                           medium built from smaller ones
     *   the volume is finer   — it works, and every metadata write that does
     *                           not cover a whole physical block costs the
     *                           device a read, a patch and a write. That is
     *                           the case the old layout was in on every flash
     *                           device made, for years, and nothing said so.
     *
     * A medium that will not say what it is built from is not an error: it is
     * a medium that will not say, and the volume stands on it either way.
     */
    uint32_t medium_physical = BoardroomSeatPhysicalBytes(seat);
    if (medium_physical == 0) {
        kprintf("[TagFS] seat %u would not say what it is built from; the "
                "volume's %u-byte grid is taken on trust\n",
                seat, vol->state.geometry.physical_bytes);
    } else if (vol->state.geometry.physical_bytes < medium_physical) {
        kprintf("[TagFS] seat %u: this volume was laid out for %u-byte blocks "
                "and the medium is built from %u — every metadata write costs "
                "it a read and a rewrite\n",
                seat, vol->state.geometry.physical_bytes, medium_physical);
    } else if (vol->state.geometry.physical_bytes > medium_physical) {
        kprintf("[TagFS] seat %u: this volume assumed %u-byte blocks and the "
                "medium is built from %u — coarser than it needs, which costs "
                "nothing\n",
                seat, vol->state.geometry.physical_bytes, medium_physical);
    } else {
        kprintf("[TagFS] seat %u: the volume's grid and the medium's are the "
                "same %u bytes\n", seat, medium_physical);
    }

    /*
     * And whether the whole of it is present. A volume with only a head still
     * mounts — this is not the reason to refuse one — but it is a volume whose
     * far end no longer agrees, and nobody finds that out later by accident.
     *
     * ‼ Said when it AGREES, and not only when it does not.
     *
     * This check has always been here and only ever spoke on failure, so a
     * volume whose whole extent is present was indistinguishable, from the
     * screen, from a volume nobody had checked — and on a machine read by
     * photographing that screen there was no way to tell which. It also meant
     * the check could be lost entirely without anything going quiet.
     */
    DeedCopy tail;
    MediumGround stood = { .start_sector = vol->mount.base,
                           .sectors      = head.head.sectors,
                           .origin       = GROUND_FROM_MBR,
                           .entry        = 0 };
    if (head.head.role == VOLUME_DEED_ROLE_TAIL) {
        /* Already standing on the far copy — there is nothing further out to
         * ask, and asking would only re-read what is already in hand. */
        kprintf("[TagFS] seat %u: its far copy agrees — it IS the far copy, and "
                "the head this volume began with is gone\n", seat);
    } else if (DeedReadTail(seat, &stood, &head, &tail) == OK) {
        kprintf("[TagFS] seat %u: its far copy agrees — the whole volume is "
                "present\n", seat);
        DeedRelease(&tail);
    } else {
        kprintf("[TagFS] seat %u: the far end of this volume does not answer; "
                "it is mounted on one deed\n", seat);
    }

    DeedRelease(&head);
    return OK;
}

/*
 * tagfs_flush_cache — force the volatile write cache of the medium that
 * actually holds the volume out to it.
 *
 * A write command "completing" means the bytes were received into a cache the
 * device can lose; this is what makes them durable. It goes to the seat the
 * volume is on, which is the whole reason the seat exists — a flush aimed at
 * the wrong device leaves every "durable" commit sitting in the right one's
 * cache.
 */
static void volume_would_not_flush(TagFSVolume* vol, error_t why)
{
    char uuid[33];
    const char* text = "a volume that has not said its name";

    if (vol->mount.uuid_known) {
        DeedUuidText(vol->mount.uuid, uuid);
        text = uuid;
    }
    /* The buffer is sized for the one thing it holds — 32 hex digits and a
     * terminator — and the other name is a constant, so it is pointed at
     * rather than copied in. It used to be printed THROUGH that buffer, which
     * is four characters too small for it: the most important line this
     * kernel prints came out cut off mid-word. */

    /* NAMED, because a durability failure that does not say WHICH volume is a
     * line nobody can act on. Two names, and they answer different questions:
     * the seat is where the operator will find the hardware, and the UUID is
     * what the volume calls itself — the machine may carry more than one. */
    kprintf("[TagFS] the volume %s, on seat %u (%s), would not flush its write "
            "cache (error %u) — everything committed to it since the last "
            "flush that did succeed is in a cache the device loses when the "
            "power goes\n",
            text, vol->mount.seat, BoardroomSeatName(vol->mount.seat),
            (unsigned)why);
}

error_t tagfs_flush_cache(TagFSVolume* vol)
{
    /* Not while the medium is away: whatever sits in its seat now would keep
     * the ticket for writes that went to the one that left, and the open book
     * would be taken up over a cache nobody committed. */
    if (volume_not_here(vol))
        return ERR_DEVICE_NOT_READY;

    /* No volume named — none found, one refused at its Deed, one let go of
     * when its medium left. There is no medium to commit, and that is said as
     * what it is: a caller asking for its writes to be made durable is never
     * told they were when the volume they went to is gone. Seat 255 is not
     * asked — it is a chair that is not there. */
    if (vol->mount.seat == BOARDROOM_NO_SEAT)
        return ERR_DEVICE_NOT_READY;

    /* The one place a flush ticket is taken and answered (scribe.h): taken
     * before the medium is asked, so the flush it stands for was issued after
     * every write whose completion was seen before it. And kept only if the
     * seat still holds the medium the volume was mounted from once it has
     * answered: a medium seated in its place while the flush waited for the
     * port carried it out, and keeps nothing the one that left was given. */
    uint64_t ticket = VolumeFlushBegin(vol);
    int  said = BoardroomFlush(vol->mount.seat);
    bool here = !volume_not_here(vol);
    VolumeFlushEnd(vol, ticket, said == 0 && here);
    if (said == 0) {
        return here ? OK : ERR_DEVICE_NOT_READY;
    }

    /*
     * ‼ A MEDIUM THAT CAN NAME ITS REFUSAL IS HEARD BY NAME.
     *
     * Every refusal used to become ERR_IO, so a port that had left the
     * machine reached the write path wearing the word for a failing disk.
     * The two vocabularies that meet here cannot be confused. A seat whose
     * medium can name its refusal answers with an `error_t`, and every
     * error_t is positive; the media that can only say "no" answer with zero
     * or a negative code of their own (ATA_ERR_*, or a plain -1). So a
     * positive answer is the medium's own word, carried through
     * BoardroomFlush untouched, and anything else is a refusal with no more
     * detail than that.
     */
    error_t why = (said > 0) ? (error_t)said : ERR_IO;

    /* ‼ AND ONE REFUSAL THE ROOM ITSELF MAKES, WHICH IS NOT A BROKEN DISK
     * EITHER. BoardroomFlush answers -1 for a seat with nobody in it, and a
     * seat empties exactly when a medium leaves — the commonest way for that
     * to happen now being an AHCI port that retired itself. Flattened into
     * ERR_IO that reads as "this disk is failing" for the rest of the boot,
     * when the truth is "this medium is not here any more", which is the one
     * thing a caller can act on differently. The room is asked rather than
     * guessed at. */
    if (said < 0 && !BoardroomSeatOccupied(vol->mount.seat)) {
        why = ERR_DEVICE_NOT_READY;
    }

    volume_would_not_flush(vol, why);
    return why;
}

bool TagFSMediumLeft(TagFSVolume* vol) { return __atomic_load_n(&vol->mount.medium_left, __ATOMIC_ACQUIRE); }

/* The room's takings so far: the newest seating any chair carries — an empty
 * chair keeps the number of the last one who sat in it. */
static uint32_t room_seatings(void)
{
    uint32_t newest = 0;
    for (uint8_t seat = 0; seat < BoardroomSeatCount(); seat++) {
        uint32_t seating = BoardroomSeatSeating(seat);
        if (seating > newest)
            newest = seating;
    }
    return newest;
}

/* Just latched: what the volume read ahead is not the medium any more. */
static void volume_book_opens(TagFSVolume* vol, bool swapped)
{
    ReadAheadForgetAll(vol);
    AwayLeft(vol, swapped);
}

/* The book opened, once — the room's takings noted before the latch. True for
 * the caller that latched it. */
static bool volume_latch(TagFSVolume* vol, bool swapped)
{
    uint32_t none = 0;
    (void)__atomic_compare_exchange_n(&vol->mount.book_seatings, &none, room_seatings(), false,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (__atomic_exchange_n(&vol->mount.medium_left, true, __ATOMIC_ACQ_REL))
        return false;
    volume_book_opens(vol, swapped);
    return true;
}

/* The book closed, whichever way: its takings let go of before the latch, so
 * a departure that latches again right after notes its own. */
static void volume_unlatch(TagFSVolume* vol)
{
    __atomic_store_n(&vol->mount.book_seatings, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&vol->mount.medium_left, false, __ATOMIC_RELEASE);
}

/* Whether the medium in `seat`, seated as `seating`, may be the one that left:
 * seated after its book opened — or the volume's own chair, holding someone
 * other than the medium it was mounted from. */
static bool volume_may_be_back(TagFSVolume* vol, uint8_t seat, uint32_t seating)
{
    uint32_t opened = __atomic_load_n(&vol->mount.book_seatings, __ATOMIC_SEQ_CST);
    return opened == 0 || seating > opened ||
           (seat == vol->mount.seat && seating != vol->mount.seating);
}

static bool volume_medium_gone(TagFSVolume* vol)
{
    if (TagFSMediumLeft(vol)) {
        return true;
    }
    if (vol->mount.seat == BOARDROOM_NO_SEAT) {
        return false;
    }

    /*
     * Two ways for the medium to be gone, and only one of them looks like it.
     *
     * The chair is empty — the obvious one. Or the chair has somebody in it
     * who is not the medium this volume was mounted from: a stick pulled out
     * and a different one pushed in before anything read a byte. Nothing about
     * the seat number changes across that, and without the taking stamp the
     * two are indistinguishable from here — which would mean serving this
     * volume's blocks out of somebody else's medium.
     */
    if (BoardroomSeatOccupied(vol->mount.seat) &&
        BoardroomSeatSeating(vol->mount.seat) == vol->mount.seating) {
        return false;
    }

    bool swapped = BoardroomSeatOccupied(vol->mount.seat);

    /* Latched once: two cores that noticed together say it once. */
    (void)volume_latch(vol, swapped);
    return true;
}

bool TagFSMediumGone(TagFSVolume* vol)
{
    return vol->state.initialized && volume_medium_gone(vol);
}

/*
 * Is the same occupant still in that seat?
 *
 * Not "is the seat occupied": a stick pulled out and a different one pushed in
 * leaves the seat number unchanged, and the taking stamp is the only thing
 * that tells the two apart. The same question volume_medium_gone() asks of the
 * mounted volume, asked here of a seat nothing is mounted from yet.
 */
static bool medium_still_seated(uint8_t seat, uint32_t seating)
{
    return BoardroomSeatOccupied(seat) &&
           BoardroomSeatSeating(seat) == seating;
}

/* A mount under way asks the seat alone and latches nothing: its volume is
 * not up, and a departure it meets is the mount's refusal. */
static bool volume_not_here(TagFSVolume* vol)
{
    if (vol->state.initialized)
        return volume_medium_gone(vol);
    return vol->mount.seat != BOARDROOM_NO_SEAT &&
           !medium_still_seated(vol->mount.seat, vol->mount.seating);
}

#ifdef CONFIG_VOLUME_TESTS
/* The stand's turn of the next mount (mount/away.h): armed by a test, met by
 * the mount, taken down by the test. */
static volatile uint32_t g_stand_turn  = STAND_MOUNT_AS_IS;
static volatile uint32_t g_stand_nth   = 0;
static volatile uint32_t g_stand_reads = 0;   /* a mount's block reads since arming */
static volatile uint32_t g_stand_fired = 0;

void TagFSStandMountTurn(StandMountTurn turn, uint32_t nth)
{
    __atomic_store_n(&g_stand_turn, (uint32_t)STAND_MOUNT_AS_IS, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stand_reads, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_stand_fired, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_stand_nth, nth, __ATOMIC_RELAXED);
    __atomic_store_n(&g_stand_turn, (uint32_t)turn, __ATOMIC_RELEASE);
}

bool TagFSStandMountTurnFired(void)
{
    return __atomic_load_n(&g_stand_fired, __ATOMIC_ACQUIRE) != 0;
}

/* A block read, met before anything of it is asked: the mount's nth finds
 * the volume's seat emptied under it, the way a hand pulling the medium out in
 * the middle of the reads leaves it. The chair alone: what the room then says
 * about it is the room's, on another core, and no part of this read. */
void TagFSStandMountReadComes(TagFSVolume* vol)
{
    if (vol->state.initialized ||
        __atomic_load_n(&g_stand_turn, __ATOMIC_ACQUIRE) != STAND_MOUNT_SEAT_EMPTIES ||
        __atomic_add_fetch(&g_stand_reads, 1u, __ATOMIC_ACQ_REL) != g_stand_nth ||
        __atomic_exchange_n(&g_stand_fired, 1u, __ATOMIC_ACQ_REL))
        return;
    BoardroomStandSeatEmpties(vol->mount.seat);
}

/* The registry about to be read by a mount: under HEAP_REFUSES the first
 * allocation of its load, on the core reading it, is answered NULL (HEAPFAIL),
 * the way an exhausted heap answers it. */
void TagFSStandMountRegistryLoads(TagFSVolume* vol)
{
    if (vol->state.initialized ||
        __atomic_load_n(&g_stand_turn, __ATOMIC_ACQUIRE) != STAND_MOUNT_HEAP_REFUSES ||
        __atomic_exchange_n(&g_stand_fired, 1u, __ATOMIC_ACQ_REL))
        return;
    HeapFailArm(amp_get_core_index(), 1);
}

/* Every mount the medium refused a read of: its seat says it answers again,
 * and the room says it on the spot — before the attempt has ended, the way a
 * driver's recovery says it in the same breath as it answers the refused
 * command, each time it recovers one. */
static void StandMountRefusalRings(TagFSVolume* vol, uint32_t refused_before)
{
    uint8_t seat = vol->mount.seat != BOARDROOM_NO_SEAT ? vol->mount.seat : vol->mount.search_unread;
    if (__atomic_load_n(&vol->mount.reads_refused, __ATOMIC_ACQUIRE) == refused_before ||
        seat == BOARDROOM_NO_SEAT ||
        __atomic_load_n(&g_stand_turn, __ATOMIC_ACQUIRE) != STAND_MOUNT_SEAT_RINGS)
        return;
    __atomic_store_n(&g_stand_fired, 1u, __ATOMIC_RELEASE);
    BoardroomNoteSeatReady(BoardroomSeatKind(seat), BoardroomSeatIndex(seat));
    BoardroomAttendIfPending();
}

/* The mount's attempts and the seats it reset, counted for the stand. */
static volatile uint32_t g_stand_attempts = 0;
static volatile uint32_t g_stand_resets   = 0;

void TagFSStandMountCounts(uint32_t *attempts, uint32_t *resets)
{
    *attempts = __atomic_load_n(&g_stand_attempts, __ATOMIC_ACQUIRE);
    *resets   = __atomic_load_n(&g_stand_resets, __ATOMIC_ACQUIRE);
}
#endif

/*
 * The arrival owed until a bell rings after the attempt that owes it — the
 * mount, or the search for a volume whose book is held open — or the memory
 * it was refused for comes back (the note over mount.mount_owed). The attempt's
 * end is counted first (bells_heard). True when the debt is new.
 */
static bool attendance_owed(TagFSVolume* vol)
{
    return __atomic_exchange_n(&vol->mount.mount_owed, 1u, __ATOMIC_SEQ_CST) == 0;
}

/*
 * The attendance owed on `seat`, which refused, and no bell since the attempt
 * ended: its medium reset once (the note over mount.reset_seating). Back, its bell
 * moved the count past the attempt's end and the attendance goes again; not
 * back, the seat is taken as one with nobody the room can reach, and nothing is
 * owed — a medium's arrival asks.
 */
static void attendance_reset(TagFSVolume* vol, uint8_t seat)
{
    uint32_t seating = BoardroomSeatSeating(seat);
    if (BoardroomSeatReadyGen(BOARDROOM_NO_SEAT) != __atomic_load_n(&vol->mount.mount_rang, __ATOMIC_ACQUIRE) ||
        !BoardroomSeatOccupied(seat) || seating == vol->mount.reset_seating)
        return;
    vol->mount.reset_seating = seating;
#ifdef CONFIG_VOLUME_TESTS
    __atomic_add_fetch(&g_stand_resets, 1u, __ATOMIC_ACQ_REL);
#endif
    kprintf("[TagFS] seat %u refused a read and no bell rang after the attempt that met it — "
            "nobody else asks it anything, so its medium is reset once, by its driver's "
            "own recovery\n", seat);
    if (BoardroomSeatReset(seat) == OK)
        return;
    kprintf("[TagFS] seat %u did not come back from its reset — the room cannot reach its "
            "medium, so nothing is owed on it: a medium's arrival asks\n", seat);
    mount_settled(vol);
}

/*
 * One attempt at the mount, and what it leaves owed (the note over
 * mount.mount_owed). Only a medium that is there and REFUSED is asked again on a
 * seat:ready rung after the attempt — its seat reset once when none was (the
 * note over mount.reset_seating) — and a mount the machine had no memory for when
 * the memory comes back (the note over mount.mount_heap_owed): one that answered
 * and was refused for what it holds is said once; one that left while it was
 * being read comes back as an arrival of its own; and a room where no medium
 * refused and none carries this machine's volume has nothing to ask.
 * TagFSProbeDrive leaves mount.seat set only
 * when a medium answered with THIS machine's volume, and names the first seat
 * whose medium would not be read when none did (mount.search_unread).
 */
static error_t mount_attempt(TagFSVolume* vol)
{
    uint32_t refused = __atomic_load_n(&vol->mount.reads_refused, __ATOMIC_ACQUIRE);
    vol->mount.ground_refused = false;
    __atomic_store_n(&vol->mount.mount_heap_owed, 0u, __ATOMIC_RELEASE);
#ifdef CONFIG_VOLUME_TESTS
    __atomic_add_fetch(&g_stand_attempts, 1u, __ATOMIC_ACQ_REL);
#endif
    error_t rc = tagfs_init(vol);
    if (rc == OK || rc == ERR_ALREADY_INITIALIZED) {
        mount_settled(vol);
        return rc;
    }
#ifdef CONFIG_VOLUME_TESTS
    StandMountRefusalRings(vol, refused);
#endif
    bells_heard(vol);

    bool    found = vol->mount.seat != BOARDROOM_NO_SEAT;
    uint8_t seat  = found ? vol->mount.seat : vol->mount.search_unread;
    if (seat == BOARDROOM_NO_SEAT)
        return rc;

    if (found && !medium_still_seated(seat, vol->mount.seating)) {
        kprintf("[TagFS] the medium in seat %u left while its volume was being read — nothing "
                "is mounted, and its next arrival is met the same way\n", seat);
        mount_settled(vol);
        return rc;
    }

    if (rc == ERR_NO_MEMORY) {
        __atomic_store_n(&vol->mount.mount_heap_level, (uint64_t)PmmFreePagesNow(), __ATOMIC_RELEASE);
        __atomic_store_n(&vol->mount.mount_heap_owed, 1u, __ATOMIC_RELEASE);
        if (attendance_owed(vol))
            kprintf("[TagFS] the machine had no memory to mount the volume in seat %u (said "
                    "above) — the mount is asked again when its memory comes back\n", seat);
        return rc;
    }

    bool answered = found && !vol->mount.ground_refused &&
                    __atomic_load_n(&vol->mount.reads_refused, __ATOMIC_ACQUIRE) == refused;
    if (answered) {
        kprintf("[TagFS] the medium in seat %u answered every read and its volume was "
                "refused for what it holds (said above) — reading it again would refuse it "
                "again, so the machine asks only when a medium arrives\n", seat);
        mount_settled(vol);
        return rc;
    }
    if (attendance_owed(vol)) {
        if (found)
            kprintf("[TagFS] the medium in seat %u refused a read while its volume was being "
                    "read — the mount is asked again when the seat says it answers again "
                    "(seat:ready)\n", seat);
        else
            kprintf("[TagFS] the medium in seat %u would not be read while the room was "
                    "searched for this machine's volume — the mount is asked again when a "
                    "seat says it answers again (seat:ready)\n", seat);
    }
    attendance_reset(vol, seat);
    return rc;
}

/*
 * And what happens when it comes back — the event the open book waits for
 * (mount/away.h).
 *
 * The latch above holds the volume as it was: nothing is taken down when the
 * medium leaves, because "out of the machine" is a state that ends — a stick
 * reseated, a controller that stopped itself and was brought back — and a
 * medium that comes back as it was left still holds everything memory holds
 * about it, the changes no cycle settled among them. What decides is read off
 * the medium, never assumed:
 *
 *   it is the SAME VOLUME — the identity in its Deed, in whichever seat it
 *   turned up in, because a socket that now has something in it is not
 *   evidence about what; but only in a seat taken after the book opened (the
 *   note over mount.book_seatings), because a medium that never left is not its
 *   return — and its Deed still says what the held one says;
 *
 *   and it is AS THIS KERNEL LEFT IT — the last stitch this kernel sewed
 *   still where it was sewn, nothing sewn past it, and every block no stitch
 *   settled read back as it was written (Perambulation, mount/
 *   perambulation.h). Another BoxOS kernel that sewed a stitch on it moved its
 *   ribbon. A host that changed its bytes and left the book alone moved
 *   nothing, and is not seen: the line that takes the book up says so.
 *
 * Both holding, the book is taken up where it stands. Anything else gives it
 * up: every waiter is answered, the in-memory picture is dropped WITHOUT
 * being written back, and the volume is mounted from the medium as it now is.
 * Another volume in the seat the book was held for gives it up as well.
 *
 * Returns true when the volume is up again, by either road.
 */
static bool tagfs_abandon(TagFSVolume* vol);

/* Everything a mount brought up, put back down — including a mount that only
 * got halfway. See the note on the definition; it is the piece whose absence
 * made one failed mount permanent. */
static void tagfs_clear_the_ground(TagFSVolume* vol);

/* Where the ground of volume `uuid` begins on `seat`, and whether its Deed
 * lays it out as the held volume is laid out (AwayDeedSame). A seat whose
 * medium would not be read is named to the search (mount.search_unread): not
 * finding the volume there says nothing about whether it is there. */
static bool volume_ground_on(TagFSVolume* vol, uint8_t seat, const uint8_t uuid[16], uint64_t *out_base,
                             bool *out_same)
{
    MediumGround ground[GROUND_MAX_PER_MEDIUM];
    bool    refused = false;
    uint8_t claimed = GroundSurvey(seat, ground, GROUND_MAX_PER_MEDIUM, &refused);
    if (refused) search_met_unread(&vol->mount.search_unread, seat);

    for (uint8_t g = 0; g < claimed; g++) {
        DeedCopy head;
        error_t  at_head = DeedReadHead(seat, &ground[g], &head);
        if (at_head != OK && DeedReadTailAlone(seat, &ground[g], &head) != OK) {
            if (at_head == ERR_IO) search_met_unread(&vol->mount.search_unread, seat);
            continue;
        }
        bool ours = memcmp(head.head.uuid, uuid, 16) == 0;
        *out_same = AwayDeedSame(vol, &head);
        DeedRelease(&head);
        if (ours) {
            *out_base = ground[g].start_sector;
            return true;
        }
    }
    return false;
}

/* The book taken up: the volume stands in the seat it came back to. The seat
 * is named before the latch is let go of, so whoever reads the latch clear
 * reads the seat too. And the medium is a seating of its own: one that left
 * refusing writes as protected — a stick with its switch on — may come back
 * with the switch off, so whether it refuses is asked again at its first
 * refused write (TagFSWriteRefused), not carried over from the one that left. */
static void volume_book_taken_up(TagFSVolume* vol, uint8_t seat, uint32_t seating, uint64_t base)
{
    vol->mount.seat    = seat;
    vol->mount.seating = seating;
    vol->mount.base   = base;
    InkReadOnlyLift(vol);
    volume_unlatch(vol);
    mount_settled(vol);                /* a search owed on seat:ready has its answer */
    AwayTakeUp(vol, seat);
    TagFSHoldGroundResumed(vol);       /* nothing at all unless HOLDGROUND=on */
}

/* The book given up: the volume let go of and mounted from the medium as it
 * now is — or, with somebody still inside it, both finished by the service
 * pass, in that order. */
static bool volume_read_again(TagFSVolume* vol, uint8_t seat, uint32_t seating)
{
    if (!tagfs_abandon(vol))
        return false;

    if (mount_attempt(vol) == OK) {
        /* Said on every road to the home volume mounted: a context this
         * machine holds is bound to the numbers of the volume that is up now
         * and told to it; a machine holding none takes up what the volume
         * remembers. Another volume of the house carries its memory of a
         * context and lives by the home volume's. */
        if (vol == TagFSHome())
            UseContextRecall();
        return true;
    }

    /* A medium that left again while it was being read is not a volume that
     * would not mount: nothing is wrong with it, and its return will be
     * announced like any other — said by the attempt when it left part way
     * through reading the volume, and here when it was gone before the room
     * could find the volume on it. One STILL THERE that would not give up a
     * volume is the other fact, and nothing asks again unless this says so. */
    if (!medium_still_seated(seat, seating)) {
        if (vol->mount.seat == BOARDROOM_NO_SEAT)
            kprintf("[TagFS] the medium left again before its volume could be found — "
                    "nothing is mounted, and its next arrival is met the same way\n");
        return false;
    }
    kprintf("[TagFS] the volume came back and would not mount — its book was given up, "
            "and why is said above\n");
    return false;
}

/*
 * ‼ WHICH OCCUPANT OF THE SEAT WAS JUDGED, REMEMBERED BEFORE ANYTHING WAS READ.
 *
 * Judging takes reads, and a hand can pull the medium out again while they
 * happen. What follows either takes a book up, throws a mount away or reads a
 * medium, and doing any of it on behalf of something that has already left is
 * how a departure gets reported as a filesystem that would not mount.
 * Measured on the board: three of fifty-five, every one of them a second pull
 * mid-read.
 */
static bool volume_book_decided(TagFSVolume* vol, uint8_t seat, uint32_t seating, uint64_t base,
                                AwayVerdict verdict)
{
    if (!medium_still_seated(seat, seating)) {
        kprintf("[TagFS] the medium in seat %u left again before it could be read — the "
                "book stays open, and its next arrival is met the same way\n", seat);
        return false;
    }
    if (verdict == AWAY_AS_LEFT) {
        volume_book_taken_up(vol, seat, seating, base);
        return true;
    }
    AwayGiveUp(vol, verdict, seat);
    return volume_read_again(vol, seat, seating);
}

static bool TagFSVolumeReturned(TagFSVolume* vol)
{
    if (!vol->mount.medium_left || !vol->mount.uuid_known) {
        return false;
    }

    bool found = false;
    vol->mount.search_unread = BOARDROOM_NO_SEAT;
    for (uint8_t seat = 0; seat < BoardroomSeatCount() && !found; seat++) {
        if (!BoardroomSeatOccupied(seat)) {
            continue;
        }
        uint32_t seating = BoardroomSeatSeating(seat);
        uint64_t base    = 0;
        bool     same    = false;
        if (!volume_may_be_back(vol, seat, seating) ||
            !volume_ground_on(vol, seat, vol->mount.uuid, &base, &same))
            continue;

        /*
         * ‼ A JUDGE ONE OF WHOSE READS THE MEDIUM REFUSED IS NO JUDGE.
         *
         * A block of the book refused while it was walked is a ribbon that
         * seems to end early, or a slot that seems another's — the book given
         * up for one command a flash drive dropped. So any refusal of this
         * seat while it was judged leaves the book open, waiting for the seat
         * to answer again. The bell a read after the refused one rang says
         * nothing of the refused one: the wait counts from the search's end,
         * as a mount's does.
         */
        uint32_t    refusals = BoardroomSeatRefusals(seat);
        AwayVerdict verdict  = Perambulation(vol, seat, base, same);
        if (BoardroomSeatRefusals(seat) == refusals)
            return volume_book_decided(vol, seat, seating, base, verdict);
        vol->mount.search_unread = seat;
        found           = true;
    }

    /* The volume is nowhere in the room: another volume in the seat the book
     * was held for ends it all the same — another occupant of it, never the
     * medium the volume was mounted from. */
    uint8_t seat = vol->mount.seat;
    if (!found && seat != BOARDROOM_NO_SEAT && BoardroomSeatOccupied(seat)) {
        uint32_t seating = BoardroomSeatSeating(seat);
        if (seating != vol->mount.seating && volume_recognised(vol, seat, NULL, true))
            return volume_book_decided(vol, seat, seating, 0, AWAY_SEAT_TAKEN);
    }

    /* Nowhere in what the room would say. A medium that would not be read may
     * be carrying it: the book stays open, and the room is searched again when
     * a seat says it answers again after this search — the arrival that
     * brought it in has been spent, and nothing else would ask. */
    bells_heard(vol);
    if (vol->mount.search_unread != BOARDROOM_NO_SEAT) {
        if (attendance_owed(vol))
            kprintf("[TagFS] the medium in seat %u would not be read while the room was "
                    "searched for the volume whose book is held open — it is searched again "
                    "when a seat says it answers again (seat:ready)\n", vol->mount.search_unread);
        attendance_reset(vol, vol->mount.search_unread);
    }
    return false;
}

#ifdef CONFIG_VOLUME_TESTS
void TagFSStandMediumLeaves(TagFSVolume* vol)
{
    (void)volume_latch(vol, false);
}

AwayVerdict TagFSStandMediumReturns(TagFSVolume* vol)
{
    AwayVerdict verdict = Perambulation(vol, vol->mount.seat, vol->mount.base, true);
    if (verdict == AWAY_AS_LEFT) {
        volume_book_taken_up(vol, vol->mount.seat, vol->mount.seating, vol->mount.base);
        return verdict;
    }
    AwayGiveUp(vol, verdict, vol->mount.seat);
    volume_unlatch(vol);
    SettleSeatReady(vol);
    return verdict;
}

/* The runner's own hold on the door let go of, and the volume let go of the
 * way a book given up lets it go — under the one-at-a-time flag, writing
 * nothing. True when it is down. */
bool TagFSStandLetGo(TagFSVolume* vol)
{
    tagfs_leave(vol);
    while (!attending_take(vol))
        cpu_pause();
    bool down = tagfs_abandon(vol);
    attending_done(vol);
    return down;
}

/* And counted back in, a volume up or not: the runner lets go of it once more
 * when it is done. */
void TagFSStandHoldAgain(TagFSVolume* vol)
{
    __atomic_fetch_add(&vol->mount.inside, 1, __ATOMIC_ACQ_REL);
}

/* No arrival owed, nobody attending, and no owed mount waiting to be asked. */
bool TagFSStandMountIdle(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->mount.arrival_owed, __ATOMIC_ACQUIRE) == 0 &&
           __atomic_load_n(&vol->mount.attending, __ATOMIC_ACQUIRE) == 0 && !mount_asked_again(vol);
}

bool TagFSStandMountOwed(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->mount.mount_owed, __ATOMIC_ACQUIRE) != 0;
}
#endif

/*
 * The medium under this volume may have just left. Ask, and latch if it has.
 *
 * TagFSMediumGone() already knows how to decide and how to say so; what was
 * missing was anybody asking at the moment it became true rather than at the
 * next read, which on a machine where nothing reads for a second is a second
 * during which the seat can come to hold a different medium entirely.
 */
void TagFSNoteMediumGone(TagFSVolume* vol)
{
    if (TagFSMediumGone(vol)) {
        TagFSHoldGroundTake(vol);      /* nothing at all unless HOLDGROUND=on */
    }
}

/*
 * A medium arrived. Is there a filesystem for this machine on it?
 *
 * Two ways in, and they are genuinely different questions. If a volume was
 * mounted and its medium left, the only right answer is the one above: that
 * volume and no other. If nothing was ever mounted — a machine that came up
 * with no filesystem at all — then a medium arriving is the first chance it
 * has had, and the whole of the ordinary mount runs, because a mount that has
 * not happened yet is not a special case of anything.
 */

/* Whether the medium a put-away volume lies on is seated anew since it was
 * put away: the seat carrying it now taken after the seating it was put away
 * from. Every taking of a chair gets the next number, so a medium pulled and
 * put back — in the same seat or another — is a seating of its own. */
static bool volume_seated_anew(TagFSVolume* vol)
{
    uint8_t uuid[16];
    uint8_t seat = BoardroomFindVolume(tagfs_recognise, vol, uuid);
    return seat != BOARDROOM_NO_SEAT && BoardroomSeatSeating(seat) != vol->mount.away_seating;
}

/* The body. Whoever calls it holds the one-at-a-time flag above, and this look
 * covers every arrival noted before it. */
static void attend_arrival(TagFSVolume* vol)
{
    __atomic_store_n(&vol->mount.arrival_owed, 0u, __ATOMIC_SEQ_CST);

    if (vol->state.initialized) {
        /* A volume on its way out is not judged again: its book was decided,
         * and the pass that finishes taking it down attends the room as an
         * arrival of its own (TagFSServiceIfPending). Judged again, a book
         * already given up — its waiters told their changes were lost — could
         * be taken up under them. */
        if (__atomic_load_n(&vol->mount.door, __ATOMIC_ACQUIRE) == VOLUME_UP)
            TagFSVolumeReturned(vol);
        return;
    }

    /* Put away by the person's word: taken up by no arrival of the medium it
     * was put away from — only by that medium seated anew. */
    if (__atomic_load_n(&vol->mount.put_away, __ATOMIC_ACQUIRE)) {
        if (!volume_seated_anew(vol))
            return;
        __atomic_store_n(&vol->mount.put_away, 0u, __ATOMIC_RELEASE);
        kprintf("[TagFS] volume %u, put away, is seated anew — taken up again\n", vol->number);
    }

    /* Never mounted. tagfs_init only marks itself done at the very end, so a
     * first attempt that found nothing left everything exactly as it was. */
    if (mount_attempt(vol) != OK)
        return;

    if (vol != TagFSHome()) {
        kprintf("[TagFS] volume %u of the house is mounted from seat %u\n", vol->number, vol->mount.seat);
        return;
    }
    /* Said on every road to the home volume mounted: the Use Context binds
     * its numbers here, and the person's word stands over the volume's
     * memory; and what waits for the machine's volume hears it. */
    UseContextRecall();
    kprintf("[TagFS] a medium arrived carrying a volume, and this machine "
            "had none — mounted from seat %u\n", vol->mount.seat);
    TouchPublish("volume:mounted", &vol->mount.seat, sizeof(vol->mount.seat));
}

void TagFSVolumeAttend(TagFSVolume* vol)
{
    if (__atomic_load_n(&vol->mount.boot_mount_settled, __ATOMIC_ACQUIRE) == 0) {
        return;
    }
    if (!attending_take(vol)) {
        /* Somebody is at it, and attends this before letting go. */
        __atomic_store_n(&vol->mount.arrival_owed, 1u, __ATOMIC_RELEASE);
        return;
    }
    attend_arrival(vol);
    attending_done(vol);
}

void tagfs_readahead_invalidate(TagFSVolume* vol, uint64_t block)
{
    ReadAheadForgetBlock(vol, block);
}

/*
 * A block a record names → a sector of the MEDIUM.
 *
 * The two coordinate systems meet here and nowhere else. What a file's
 * record and the volume's book carry is a block of the volume, counted from
 * 0 (TagFSState); and the volume sits on its medium at the base. Every road a
 * block takes to its seat asks here: a block's doors below, a run's (run/
 * run.c), the storage ops nobody stands over, and Braid, which addresses
 * other media that have no ground of their own.
 */
uint64_t tagfs_block_to_sector(TagFSVolume* vol, uint64_t block)
{
    return vol->mount.base + block * TAGFS_BLOCK_SECTORS;
}

/*
 * A single block's doors, beside the state they read, and light: they are on
 * the deepest way a request takes to the medium. A run of blocks goes through
 * run/run.c.
 *
 * Through the Braid the volume is assembled in, when it is (TagFSState.braid)
 * and it takes the block, else the volume's own medium. A read's bytes are
 * checked against the digest the book holds for the block (integrity/
 * integrity.h), and a mismatch is its answer (ERR_CHECKSUM), never thrown
 * away: the block is named and published. A write that landed is noted
 * (VolumeWriteSeen), and no read-ahead window hands out what the block held. A refusal the medium's departure explains is ERR_DEVICE_NOT_READY, in
 * a read and in a write: the caller holds nothing a broken disk would make it
 * hold. A sector the medium says it cannot read is ERR_BAD_SECTOR, and no
 * refused read: it is the medium's answer about what the block holds, for the
 * reader to judge (the mount's by where the block lies), and no seat:ready
 * will change it.
 *
 * The departure is asked the way the volume's other doors ask it
 * (volume_not_here): a mounted volume's is latched and opens its book; a
 * mount's latches nothing — no volume is up to hold a book for — and is a
 * read the medium refused, which is what it is to the mount that met it.
 */
error_t tagfs_read_block(TagFSVolume* vol, uint64_t block, void *buffer)
{
    if (!buffer)
        return ERR_NULL_POINTER;
#ifdef CONFIG_VOLUME_TESTS
    TagFSStandMountReadComes(vol);
#endif
    if (volume_not_here(vol)) {
        TagFSNoteReadRefused(vol);
        return ERR_DEVICE_NOT_READY;
    }

    bool got = vol->state.braid &&
               BraidReadBlock(vol->state.braid, tagfs_block_to_sector(vol, block), buffer, NULL) == OK;
    int  rc  = got ? 0 : BoardroomRead(vol->mount.seat, tagfs_block_to_sector(vol, block),
                                       TAGFS_BLOCK_SECTORS, buffer);
    if (rc == (int)ERR_BAD_SECTOR)
        return ERR_BAD_SECTOR;
    if (rc != 0) {
        TagFSNoteReadRefused(vol);
        return volume_not_here(vol) ? ERR_DEVICE_NOT_READY : ERR_READ_FAILED;
    }
    return IntegrityVerify(vol, block, buffer) ? OK : ERR_CHECKSUM;
}

error_t tagfs_write_block(TagFSVolume* vol, uint64_t block, const void *buffer)
{
    if (!buffer)
        return ERR_NULL_POINTER;
    if (volume_not_here(vol))
        return ERR_DEVICE_NOT_READY;
    if (TagFSIsUnderGlass(vol))
        return TagFSGlassRefused(vol, "the block door");

    bool took = vol->state.braid &&
                BraidWriteBlock(vol->state.braid, tagfs_block_to_sector(vol, block), buffer, NULL) == OK;
    if (!took && BoardroomWrite(vol->mount.seat, tagfs_block_to_sector(vol, block), TAGFS_BLOCK_SECTORS,
                                buffer, NULL) != 0)
        return volume_not_here(vol) ? ERR_DEVICE_NOT_READY : ERR_WRITE_FAILED;
    VolumeWriteSeen(vol);
    ReadAheadForgetBlock(vol, block);
    return OK;
}

// ----------------------------------------------------------------------------
// The reserved vocabulary
// ----------------------------------------------------------------------------

// The reserved-key vocabulary: the words a running program's cabin is given its
// authority by (auth_tags.h). A registry tag carries TAGFS_TAG_FLAG_SYSTEM iff
// it is the bare (value=NULL) entry for one of these keys; a value-bearing tag
// like "system:foo" is a distinct entry and stays a user tag. None of these is
// a file's role: that is its record's, and a file wearing the word system is
// only a file its user called so. The image builder lays them first, at the
// first ids in this order, and they are re-stamped at every mount below. Built
// from the same tagfs_reserved.h source as the builder's seed and the
// authority bits, so the lists cannot drift.
static const char *const TagFsReservedKeys[] = {
#define X(id, key) key,
    TAGFS_RESERVED_KEYS(X)
#undef X
};
_Static_assert(sizeof(TagFsReservedKeys) / sizeof(TagFsReservedKeys[0]) == TAGFS_RESERVED_COUNT,
               "reserved-key drift");

/* True iff the key_len bytes at `key` are one of the reserved kernel-owned tag
 * keys (TagFsReservedKeys[]). A key, not a tag's text, so it catches both
 * "system" and "system:foo" once the caller has spelled the tag. Works
 * regardless of registry/mount state — the reserved vocabulary is a
 * compile-time constant. */
bool tagfs_key_is_reserved(const char *key, size_t key_len)
{
    if (!key) return false;
    for (size_t i = 0; i < sizeof(TagFsReservedKeys) / sizeof(TagFsReservedKeys[0]); i++)
        if (TagSpanIs(TagFsReservedKeys[i], key, key_len)) return true;
    return false;
}

/* The reserved vocabulary stamped as the system's tags at every mount. Every
 * reserved tag is a tag like any other to a question: none is filtered, none
 * is kept by number. The keys' op-authority is the FIXED cabin_t.auth_bits
 * (auth_tags.h), independent of the registry id. */
static void tagfs_stamp_reserved_tags(TagFSVolume* vol)
{
    TagFSState *fs = &vol->state;
    if (!fs || !fs->registry)
        return;
    TagRegistry *reg = fs->registry;

    // Stamp the reserved vocabulary as system tags. Runs after the book's TAGS
    // are read, so it self-heals over whatever flags the disk carried; mark_system sets no
    // dirty flag, so this re-derivation never forces a write.
    for (size_t i = 0; i < sizeof(TagFsReservedKeys) / sizeof(TagFsReservedKeys[0]); i++) {
        TagSpelling reserved = TagSpellingOfKey(TagFsReservedKeys[i]);
        uint32_t    tid      = TagRegistryLookup(reg, TagOwnerPerson(), &reserved);
        if (tid != TAGFS_INVALID_TAG_ID)
            tag_registry_mark_system(reg, tid);
    }
}

// ----------------------------------------------------------------------------
// tagfs_init
// ----------------------------------------------------------------------------

/*
 * This volume did not mount, and here is the sentence that says so.
 *
 * ‼ TWO THINGS, AND NEITHER OF THEM USED TO HAPPEN.
 *
 * It is SAID — out loud, with kprintf, naming the step. Four of the fatal
 * refusals below reported with debug_printf, which compiles to nothing in a
 * shipped build, so on the owner's board a volume that would not mount looked
 * exactly like one that had: the room seated the medium, the deed was read,
 * "6009 data blocks, 410 free, 58 files" was printed, and then nothing. The
 * shell answered `Unknown command` to every name on a volume it could see.
 *
 * And the ground is CLEARED. Everything this attempt brought up goes back down,
 * so the next attempt starts from a true blank slate rather than from the
 * wreckage of this one — which is what made the first failure permanent.
 */
static error_t mount_refused(TagFSVolume* vol, const char *what, error_t why)
{
    kprintf("[TagFS] this volume is NOT mounted: %s (error %d)\n",
            what, (int)why);
    tagfs_clear_the_ground(vol);
    return why;
}

error_t tagfs_init(TagFSVolume* vol) {
    if (vol->state.initialized) {
        debug_printf("[TagFS] Already initialized\n");
        return ERR_ALREADY_INITIALIZED;
    }

    debug_printf("[TagFS] Initializing...\n");

    /*
     * ‼ NOTHING IS CLEARED HERE, AND THAT IS DELIBERATE.
     *
     * The obvious place to guarantee "this attempt starts from a blank slate"
     * is the top of this function. It was written that way first, and then
     * measured: with MOUNTFAIL=on the scenario stayed GREEN with the line
     * taken out, because every failure below already clears at the site
     * (mount_refused). A guard that cannot be shown to do anything is the same
     * kind of thing as the ring-full check this driver's sibling carried for
     * years — unreachable, unfalsifiable, and believed.
     *
     * So the guarantee lives in ONE place, where the failure is, and it is
     * provable: take the clearing out of mount_refused and the second mount
     * dies at TagFS_TestsInit exactly as the board did at TagFS_CowInit.
     *
     * The departure's latch is the one thing let go of here, and it is not
     * the ground: it is the open book of a MOUNTED volume (mount/away.h), and
     * no volume is up. One left standing would be inherited by the volume
     * about to come up, and every read of it refused from the moment it was.
     */
    volume_unlatch(vol);

    /* Which seat carries this machine's volume — asked of the whole room
     * before a sector of the volume is read. */
    TagFSProbeDrive(vol);

    /*
     * ‼ 0xFF IS NOT A SEAT, AND IT USED TO BE READ FROM ANYWAY.
     *
     * The survey above answers BOARDROOM_NO_SEAT when nothing on the bus
     * carries this machine's volume — and it has already said so, naming how
     * many media it asked. That sentinel was then carried straight into
     * volume_take_ground, which handed it to GroundSurvey, which asked the
     * Boardroom for sector zero of a chair that does not exist. The room
     * refused, as it must, and the machine printed two more lines blaming a
     * seat 255 that was never in it:
     *
     *   [Ground] seat 255: could not be read at all
     *   [TagFS] seat 255 claims no ground for BoxOS
     *
     * Both are false, and on the machine this matters on they are the last
     * thing on the screen — so the one true sentence, printed one line
     * earlier, is the one that scrolls away. A sentinel taken for a value is
     * an invented answer, and an invented answer is worse than none.
     *
     * The refusal is the same one volume_take_ground gave, so nothing above
     * this sees a different outcome: the boot stands a shell in for the
     * volume it has not got, and the arrival of a medium runs this again.
     */
    if (vol->mount.seat == BOARDROOM_NO_SEAT) {
        return ERR_FILE_NOT_FOUND;
    }

    /* The heap as the mount finds it; what it keeps is said once it stands. */
    size_t heap_before_mount = KHeapLiveBytes();

    /*
     * ‼ THREE LINES USED TO STAND HERE, AND THEY BELONGED TO THE MACHINE.
     *
     *     spinlock_init(&vol->state.lock);
     *     spinlock_init(&helm.open_table_lock);
     *     memset(helm.open_files, 0, sizeof(helm.open_files));
     *
     * They are correct exactly once, at the first mount, and they are what
     * static storage already guarantees — spinlock_init writes two zeros into
     * something that starts as zeros. On the SECOND mount, which is what a
     * medium leaving and coming back produces, they are three bugs: a lock
     * another core is holding is declared free, and the open-file table is
     * emptied under the handles that own its entries. See the note over the
     * table itself (helm/helm.c); the one Helm per file it exists to provide
     * is what goes.
     */

    /* --- Stand on the ground, and read the deed that describes it --- */
    error_t ground_rc = volume_take_ground(vol, vol->mount.seat);
    if (ground_rc != OK) {
        return ground_rc;
    }

    // --- Test Framework init: the tests run on the home volume's mount ---
    if (vol == TagFSHome() && TagFS_TestsInit() != OK) {
        return mount_refused(vol, "its test framework would not start",
                             ERR_NO_MEMORY);
    }

    /*
     * ‼ THE FAILURE THAT COST A BOARD ITS FILESYSTEM, ON COMMAND.
     *
     * Every refusal from here down leaves the test framework standing — and
     * once a snapshot layer beside it — and until
     * tagfs_clear_the_ground existed nothing took them down. The NEXT mount
     * then died at TagFS_CowInit with ERR_ALREADY_INITIALIZED, and went on
     * dying there for the rest of the boot, saying nothing. Nothing in QEMU
     * ever fails a mount, so the whole path was unreachable from the desk and
     * this repair would have been green by construction.
     *
     * `make MOUNTFAIL=on` fails the FIRST mount here, once, with all of them
     * up: the worst case, and the exact shape the board was in. What must then
     * happen is the whole of the fix — the failure is said out loud, the ground
     * is cleared, and the catch-up mount storage_deck_init makes straight
     * afterwards succeeds and reaches "data-integrity verify-on-read active".
     *
     * ‼ It is a REPRODUCTION, not a bug. Take the clear-the-ground out of the
     * top of this function and this key leaves the machine exactly as the board
     * was: a seated medium, a described deed, a printed file count, and a shell
     * that cannot find one of those files.
     */
#if CONFIG_TAGFS_MOUNT_FAIL_ONCE
    {
        static bool tripped = false;
        if (!tripped) {
            tripped = true;
            return mount_refused(vol, "MOUNTFAIL=on asked this one to fail",
                                 ERR_TAGFS_CORRUPTED);
        }
    }
#endif

    /*
     * ‼ AND THE SAME FAILURE ON A RE-MOUNT, WHICH IS A DIFFERENT MACHINE.
     *
     * MOUNTFAIL fails the BOOT mount, and the boot road has a catch-up attempt
     * built into it (storage_deck_init makes one straight afterwards). A mount
     * that fails on a RETURN has no such thing: mounting is driven by
     * arrivals, and a medium that is already seated does not arrive again. So
     * `make RETURNFAIL=on` fails the first re-mount once, ONCE, with the
     * medium left exactly where it is — as a read the medium refused, so the
     * only way back from there is the machine asking again by itself, when
     * the seat says it answers again (seat:ready, the note over
     * mount.mount_owed). A refusal no command answers is the seat's to reset once
     * (the note over mount.reset_seating), and the stick's transport reset is the
     * bell the mount is asked again on — QEMU's stick refuses nothing of its
     * own, and this is the road a real one that refused once comes back by.
     *
     * Not the first mount: the stamp counts the ones that finished, so this
     * trips on the first mount after a volume has already been up.
     */
#if CONFIG_TAGFS_RETURN_FAIL_ONCE
    {
        static bool tripped = false;
        if (!tripped && tagfs_mount_epoch(vol) >= 1) {
            tripped = true;
            TagFSNoteReadRefused(vol);
            return mount_refused(vol, "RETURNFAIL=on asked this re-mount to fail",
                                 ERR_TAGFS_CORRUPTED);
        }
    }
#endif

    /* --- What the volume's book says (mount/book_mount.h). */
    const char *what = NULL;
    error_t     read = MountReadBook(vol, &what);
    if (read != OK) {
        mount_refused(vol, what, read);
        /* A book turned away for what it holds is not a volume stood on, as
         * one turned away at its Deed is not (VolumeGroundRefused). */
        if (read == ERR_TAGFS_CORRUPTED || read == ERR_UNSUPPORTED)
            VolumeTurnedAway(vol);
        return read;
    }

    tagfs_stamp_reserved_tags(vol);

    /* Nothing is inked, nothing settled and nothing rests: every record the
     * book holds is on the medium. And nobody waits for a cycle: a volume let
     * go of answered its waiters once the last caller was out of it
     * (tagfs_abandon), and none can wait on this one before its door opens. */
    if (SettleWaitersOwed(vol)) {
#if CONFIG_STRICT
        panic("[TagFS] DEFECT: a waiter for a cycle of a volume let go of was never answered");
#endif
        kprintf("[TagFS] DEFECT: a waiter for a cycle of a volume let go of was never answered "
                "— it is answered now, ERR_DEVICE_NOT_READY\n");
    }
    InkMountReset(vol);
    SettleMountReset(vol);

    vol->state.initialized = true;

    /*
     * ‼ AND THE DOOR IS OPEN, WHICH IS A DIFFERENT SENTENCE.
     *
     * `initialized` says the memory is up. The door says callers may come in,
     * and it is what a take-down shuts before anything is handed back. They
     * are separate because there is a state between them: mounted, going, and
     * not yet freed because somebody is still inside.
     *
     * The stamp goes up first. Anything remembered from the last mounting —
     * an open handle, an async read in flight — is answered "that was a
     * different volume" from the instant the first caller can be let in.
     */
    __atomic_add_fetch(&vol->mount.mount_epoch, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&vol->mount.door, VOLUME_UP, __ATOMIC_RELEASE);

    /* A book whose mount holds a verdict asks for the session's first cycle
     * at once: it lays the verdict down (mount/errata.h, §3.5). After the
     * cadence was reset for this mount, which forgets every demand. */
    ErrataAsk(vol);

    /*
     * ‼ THE SENTENCE THAT WAS MISSING, AND ITS ABSENCE IS THE WHOLE STORY.
     *
     * Everything printed above this line is said DURING a mount and says
     * nothing about whether one finished: the deed is described, the far copy
     * is checked, "6009 data blocks, 410 free, 58 files" is printed — and then
     * eight more things have to work. On the owner's board they did not, and
     * nothing said so, so a machine with no filesystem printed the same twelve
     * lines as a machine with one. It was read off a photograph as a healthy
     * mount, twice, and the shell answering `Unknown command` to every name was
     * hunted somewhere else entirely.
     *
     * This is the counterpart to mount_refused, and between them there is now
     * exactly one line that ends the question either way.
     */
    kprintf("[TagFS] the volume on seat %u is MOUNTED — %llu file(s)\n",
            vol->mount.seat, (unsigned long long)vol->state.total_files);
    char   name[TALLY_VOLUME_NAME_MOST + 1u];
    size_t named = VolumeNameOf(vol, name, TALLY_VOLUME_NAME_MOST);
    if (named) {
        name[named] = '\0';
        kprintf("[TagFS] volume %u of the house is called \"%s\"\n", vol->number, name);
    }

    /*
     * ‼ WHAT THIS MOUNT KEEPS ON THE KERNEL HEAP, AND FOR HOW MANY NAMES.
     *
     * A mount builds its indexes in kernel memory. They used to be sized by
     * every file id the volume ever issued rather than by the files it has —
     * the pool's mirror, the table's rows and every tag's bitmap ran to
     * next_file_id — so an id spent on a file deleted years ago cost every
     * mount its slot, and a volume could one day refuse to mount because of
     * its history and not its contents. Now each holds the live files only
     * (file_map/, tag_bitmap/). Three numbers, so that two mounts of one
     * volume a known number of dead ids apart measure what a dead id costs,
     * which is nothing (logcheck `quill`, fidhistory). The bytes are the difference of
     * KHeapLiveBytes across the mount, every road of the heap; another core
     * allocating meanwhile is in the figure too, which is why the harness
     * judges a slope over tens of thousands of ids and never one mount.
     */
    size_t heap_after_mount = KHeapLiveBytes();
    kprintf("[TagFS] this mount keeps %zu bytes of kernel heap for %llu file id(s) "
            "ever issued, %llu of them live\n",
            heap_after_mount > heap_before_mount ? heap_after_mount - heap_before_mount : 0,
            (unsigned long long)(vol->book.next_fid - 1u),
            (unsigned long long)vol->state.total_files);

    MountBookPromise(vol);

    SettleCutSayArmed();            /* nothing at all unless SETTLECUT=<point> */
    TagFSHoldGroundOpenFile(vol);      /* nothing at all unless HOLDGROUND=on */
    return 0;
}

/*
 * ‼ EVERYTHING THIS VOLUME BROUGHT UP, PUT BACK DOWN — INCLUDING HALF OF IT.
 *
 * This is the piece that did not exist, and its absence is what turned one
 * transient read error into a machine with no filesystem for the rest of its
 * boot. tagfs_init brought up eight things in order and any of them could refuse;
 * when one did, the seven before it were left standing and nothing anywhere took
 * them down — tagfs_teardown begins with `if (!vol->state.initialized) return`,
 * and a mount that failed never set that flag.
 *
 * The next attempt then met a CoW layer that was already up, and TagFS_CowInit
 * answered ERR_ALREADY_INITIALIZED to that. tagfs_init treated it as fatal and
 * said so with debug_printf, which compiles to nothing. So every later mount
 * died at the same line, silently, for ever: the room seated the medium, the
 * deed was read, "58 files" was printed, and the shell could not find one of
 * them. Measured on the owner's board, twice, in photographs.
 *
 * Every shutdown below already asks whether the thing it takes down is up, so
 * calling all of them against a mount that got three steps in is exactly as
 * correct as calling them against one that finished.
 *
 * ‼ NOT ONE BYTE IS WRITTEN. What is held in memory describes the volume as it
 * was BEFORE whatever went wrong, and the medium under the head may not even be
 * the same one any more. Only a Settle cycle writes the volume's records, and
 * every shutdown below only lets go of memory — the ground resting in Fallow
 * included, which the next mount reads afresh.
 */
static void tagfs_clear_the_ground(TagFSVolume* vol)
{
    /* The registry is about to go, and with it every number it issued. The
     * Use Context holds some; told first — the door drained, so nothing binds
     * one after — its verdicts are stale before the book is freed. */
    UseContextUnbind();

    /* What this volume remembered goes with it — the Use Context's memory of
     * it too; the next one is read, not assumed. */
    UseMemoryLetGo(vol);
    VolumeNameLetGo(vol);

    if (vol->state.registry) {
        tag_registry_destroy(vol->state.registry);
        kfree(vol->state.registry);
        vol->state.registry = NULL;
    }

    if (vol->state.bitmap_index) {
        tag_bitmap_destroy(vol->state.bitmap_index);
        vol->state.bitmap_index = NULL;
    }

    InkDropAll(vol);
    FileMapDestroy(vol->state.files);
    vol->state.files = NULL;

    AllotmentDropAll(vol);

    if (vol->state.braid) {
        BraidShutdown(vol->state.braid);
        kfree(vol->state.braid);
        vol->state.braid = NULL;
    }
    IntegrityRotEnd(vol);
    MountBookLetGo(vol);
    TagFSUnderGlass(vol, NULL);
    if (vol == TagFSHome())
        TagFS_TestsShutdown();

    memset(vol->state.uuid, 0, sizeof(vol->state.uuid));
    memset(vol->state.volume_name, 0, sizeof(vol->state.volume_name));
    vol->state.volume_bytes = 0;
    vol->state.initialized = false;
}

/*
 * The medium went away underneath this volume. Let go of everything held about
 * it WITHOUT WRITING A BYTE, so that whatever comes back is read rather than
 * assumed: what is in memory describes the volume as it was before it left,
 * and the medium under the head may not even be the same one.
 *
 * ‼ IN TWO STEPS, BECAUSE SOMEBODY MAY BE INSIDE.
 *
 * First the door is shut, which stops anybody new. Then, only if the volume is
 * empty of callers, the ground is cleared. It is NOT waited for here: this
 * runs where the departure was noticed, which is the guide loop or the idle
 * loop, and the caller still inside may be waiting for a transfer that the
 * very same loop is the one to finish — waiting here would be waiting for
 * ourselves. Returns false when it has not been done, and TagFSServiceIfPending
 * comes back for it.
 */
static bool tagfs_abandon(TagFSVolume* vol)
{
    if (!vol->state.initialized) {
        return true;                        /* nothing to let go of */
    }

    /* Shut before counting, and only once: a second pass through here must not
     * announce a departure that is already under way. */
    if (__atomic_exchange_n(&vol->mount.door, (uint8_t)VOLUME_LEAVING,
                            __ATOMIC_ACQ_REL) == VOLUME_UP) {
        __atomic_store_n(&vol->mount.abandon_pending, 1u, __ATOMIC_RELEASE);
    }

    uint32_t inside = __atomic_load_n(&vol->mount.inside, __ATOMIC_ACQUIRE);
    if (inside != 0) {
        /*
         * Said once per departure, not once per attempt — the service pass
         * asks again every time round the loop, and a line per iteration is a
         * log nobody can read.
         */
        if (!vol->mount.leaving_said) {
            vol->mount.leaving_said = true;
            kprintf("[TagFS] the volume is on its way out and %u caller(s) are "
                    "still inside it — nothing of it is let go until they are "
                    "out\n", inside);
        }
        return false;
    }

    /* Nobody is inside, and nobody can come in: every waiter for a cycle of
     * this volume is linked by now, and is answered before the volume goes —
     * one that linked after its book was given up among them, which the
     * give-up's own answer could not reach, and which no mount after this
     * would ever answer if it failed. */
    SettleWaitersAnswerAll(vol, ERR_DEVICE_NOT_READY);
    tagfs_clear_the_ground(vol);

    __atomic_store_n(&vol->mount.door, (uint8_t)VOLUME_DOWN, __ATOMIC_RELEASE);
    __atomic_store_n(&vol->mount.abandon_pending, 0u, __ATOMIC_RELEASE);
    if (vol->mount.leaving_said) {
        vol->mount.leaving_said = false;
        kprintf("[TagFS] the last caller is out and the old volume has been let "
                "go of\n");
    }

    volume_unlatch(vol);
    vol->mount.seat    = BOARDROOM_NO_SEAT;
    vol->mount.seating = 0;
    return true;
}

/*
 * ‼ THE MACHINE IS STOPPING, AND ITS LAST CYCLE HAS ALREADY RUN.
 *
 * The halt runs the volume's last Settle cycle itself, before interrupts go
 * off (TagFSSettleForHalt, system_halt.c): after that a committer could be a
 * core the halt has stopped, and nothing may stand over the medium then. What
 * is left here is to shut the door and to say what the last cycle answered —
 * and how many changes no cycle settled, inked after it by a change begun
 * before the halt shut the volume (TagFSShutForHalt) that a drain giving up
 * did not wait for, or left by a cycle that was refused, which the medium was
 * not given. Nothing is freed: freeing at
 * a halt was never for the machine's benefit, and memory somebody is still
 * reading must not go back to an allocator nobody will ask again.
 */
error_t tagfs_shutdown(TagFSVolume* vol)
{
    if (!vol->state.initialized) {
        return OK;                      /* nothing mounted, nothing owed */
    }

    uint64_t after  = 0;
    error_t  answer = SettleHaltAnswer(vol, &after);
    __atomic_store_n(&vol->mount.door, (uint8_t)VOLUME_LEAVING, __ATOMIC_RELEASE);
    if (after) {
        kprintf("[TagFS] %llu change(s) no Settle cycle settled before the machine stopped — "
                "the medium was not given them\n", (unsigned long long)after);
        if (answer == OK)
            answer = ERR_TAGFS_METADATA_ERROR;
    }
    return answer;
}

/*
 * A mount this machine owes itself — or the search for a volume whose book is
 * held open — asked again because a seat said it answers again after the last
 * attempt ended, or because the memory the mount was refused for came back
 * (the notes over mount.mount_owed and mount.mount_heap_owed). Asked again, the bell
 * is heard: whatever the attempt makes of it, only a bell after it asks once
 * more. Two atomic loads when nothing is owed.
 */
static void tagfs_ask_for_the_mount_again(TagFSVolume* vol)
{
    if (!mount_asked_again(vol))
        return;
    if (!attending_take(vol)) {
        return;                 /* somebody is already at it; the event stays */
    }
    if (!mount_asked_again(vol)) {
        attending_done(vol);       /* the attempt that held the flag heard it */
        return;
    }
    bool memory = mount_memory_back(vol);
    bells_heard(vol);
    if (!vol->state.initialized) {
        if (memory)
            kprintf("[TagFS] the machine has memory again — asking for the volume nobody has "
                    "mounted\n");
        else
            kprintf("[TagFS] seat %u answers again — asking for the volume nobody has "
                    "mounted\n",
                    vol->mount.seat != BOARDROOM_NO_SEAT ? vol->mount.seat : vol->mount.search_unread);
        attend_arrival(vol);
    } else if (TagFSMediumLeft(vol)) {
        kprintf("[TagFS] a seat answers again — searching the room again for the volume "
                "whose book is held open\n");
        attend_arrival(vol);
    } else {
        mount_settled(vol);        /* somebody got there first */
    }
    attending_done(vol);
}

/* An arrival the room announced (seat:taken), attended here on this pass's own
 * stack. Two atomic loads when nothing arrived. */
static void tagfs_attend_owed_arrival(TagFSVolume* vol)
{
    if (__atomic_load_n(&vol->mount.arrival_owed, __ATOMIC_ACQUIRE) == 0 ||
        __atomic_load_n(&vol->mount.boot_mount_settled, __ATOMIC_ACQUIRE) == 0) {
        return;
    }
    if (!attending_take(vol)) {
        return;                 /* the holder attends it before letting go */
    }
    attend_arrival(vol);
    attending_done(vol);
}

/*
 * The work of a Helm handed to a waiter that does it synchronously runs here
 * first: a job holds the door for its whole life, so a volume on its way out
 * is let go of only once the work inside it has been done. Then the Settle's
 * turn: a step of its cycle, or the start of one that is due (settle.h).
 *
 * And a medium the room seated is attended — the volume mounted, or its open
 * book judged — and a mount owed is asked again when a seat answers again.
 *
 * And the ground that could not be cleared when the medium left, because
 * somebody was standing on it, is finished here, and then the ordinary road
 * takes the volume back up.
 *
 * Called from the guide loop and from the idle loop — the two places in this
 * kernel that are allowed to wait and are reached often — and it is one atomic
 * load per duty when there is nothing to do, which is every iteration but a
 * handful in the life of a machine.
 */
void TagFSVolumeService(TagFSVolume* vol)
{
    HelmRunReady(vol);
    SettleServiceIfPending(vol);
    KeepServe(vol);
    RetireServe(vol);
    CarryServe(vol);
    tagfs_attend_owed_arrival(vol);
    tagfs_ask_for_the_mount_again(vol);

    if (__atomic_load_n(&vol->mount.abandon_pending, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    TagFSHoldGroundRelease(vol);       /* nothing at all unless HOLDGROUND=on */

    if (__atomic_load_n(&vol->mount.inside, __ATOMIC_ACQUIRE) != 0) {
        return;                     /* still somebody in there */
    }

    /* Under the same flag as the other road, and for its whole length: this
     * one clears the ground and then mounts, and a second core arriving in
     * between would mount on top of it. */
    if (!attending_take(vol)) {
        return;
    }

    /* A volume put away stays in the house, shown by the name the person gave
     * it: held across the letting go, which forgets everything read, and
     * given back — the next mount reads it from the book afresh. */
    UseText* named = vol->mount.putting ? VolumeNameHeld(vol) : NULL;
    if (tagfs_abandon(vol)) {
        /* And now it is an ordinary arrival: nothing is mounted, and there may
         * be a medium in the room carrying the volume this machine had. That
         * road already knows how to check it is the same volume and to say
         * what it did, so it is not written a second time here — a volume put
         * away it leaves let go of. */
        VolumePuttingAway* putting = vol->mount.putting;
        vol->mount.putting         = NULL;
        if (putting && named)
            (void)VolumeNameRecall(vol, (const uint8_t*)named->text, named->bytes);
        attend_arrival(vol);
        TagFSHoldGroundAfterReturn(vol);   /* nothing at all unless HOLDGROUND=on */
        if (putting) {
            kprintf("[TagFS] volume %u is put away: let go of, everything of it on its medium — its medium may "
                    "be pulled out, and seated anew it is taken up again\n", vol->number);
            putting->rc = OK;
            putting->ended(putting);
        }
    }
    UseTextDrop(named);

    attending_done(vol);
}

error_t TagFSVolumePutAway(TagFSVolume* vol, VolumePuttingAway* p)
{
    if (vol == TagFSHome())
        return ERR_BUSY;
    if (!attending_take(vol))
        return ERR_WOULD_BLOCK;
    vol->mount.away_seating = vol->mount.seating;
    __atomic_store_n(&vol->mount.put_away, 1u, __ATOMIC_RELEASE);
    if (!vol->state.initialized) {
        attending_done(vol);
        kprintf("[TagFS] volume %u is put away; it was not mounted\n", vol->number);
        p->rc = OK;
        p->ended(p);
        return OK;
    }
    /* Shut, as a departure shuts it: nobody new comes in, and the service
     * pass lets it go once the last caller is out (tagfs_abandon), telling
     * `p` then. */
    vol->mount.putting = p;
    if (__atomic_exchange_n(&vol->mount.door, (uint8_t)VOLUME_LEAVING, __ATOMIC_ACQ_REL) == VOLUME_UP)
        __atomic_store_n(&vol->mount.abandon_pending, 1u, __ATOMIC_RELEASE);
    attending_done(vol);
    kprintf("[TagFS] volume %u is being put away: nobody new comes in, and it is let go of once the last caller "
            "is out\n", vol->number);
    return OK;
}

bool TagFSVolumePutAwayIs(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->mount.put_away, __ATOMIC_ACQUIRE) != 0;
}

// ----------------------------------------------------------------------------
// The next file id — the questions of which files there are live in query/
// ----------------------------------------------------------------------------

uint64_t tagfs_file_ceiling(TagFSVolume* vol)
{
    if (!tagfs_enter(vol))
        return 0;
    /* Under vol->state.lock: a create moves the book's next file id under it,
     * and a lock-free read can see the value from before a create that has
     * already returned to its caller. */
    TagFSLockTake(&vol->state.lock);
    uint64_t ceiling = vol->book.next_fid;
    TagFSLockDrop(&vol->state.lock);
    tagfs_leave(vol);
    return ceiling;
}

// ----------------------------------------------------------------------------
// File I/O (open / close / read / write)
// ----------------------------------------------------------------------------

static TagFSFileHandle *tagfs_open_inside(TagFSVolume* vol, uint64_t file_id, uint32_t flags);

TagFSFileHandle *tagfs_open(TagFSVolume* vol, uint64_t file_id, uint32_t flags)
{
    if (!tagfs_enter(vol))
        return NULL;
    TagFSFileHandle *rc = tagfs_open_inside(vol, file_id, flags);
    tagfs_leave(vol);
    return rc;
}

static TagFSFileHandle *tagfs_open_inside(TagFSVolume* vol, uint64_t file_id, uint32_t flags)
{
    /* A file the book holds locked opens for nobody (BookFileLocked). */
    if (BookFileLocked(vol, file_id))
        return NULL;

    /* An open that cannot join its file's Helm is refused (W-NULLOFE): a
     * handle with no entry would change the file beside its holder instead of
     * after it. */
    OpenFileEntry *ofe = ofe_acquire(vol, file_id);
    if (!ofe)
        return NULL;

    TagFSFileHandle *handle = kmalloc(sizeof(TagFSFileHandle));
    if (!handle)
    {
        ofe_release(vol, ofe);
        return NULL;
    }
    memset(handle, 0, sizeof(*handle));
    handle->vol     = vol;
    handle->file_id = file_id;
    handle->flags   = flags;
    handle->ofe     = ofe;
    /* Which mounting of the volume the record it holds was taken from. A
     * handle is refused after the volume has been read again, because the
     * blocks it names then belong to whatever the volume keeps there now. */
    handle->mount_epoch = tagfs_mount_epoch(vol);
    HelmViewTake(vol, handle);

    /*
     * ‼ AN OPEN HOLDS WHAT IT NAMES, OR IS REFUSED (MP-D16). The record is the
     * map's — the authority every read stands on, read with no I/O — and a
     * file whose record is not there is not handed out as an empty file. A
     * handle opened to read holds the record itself, a reference and never a
     * copy of its runs: a record is never changed once built — a change
     * builds the next — so what the handle walks stays as the open found it.
     * A handle opened only to write holds none at all (W-STALE): its work
     * loads the record under the file's Helm, and one from before the Helm was
     * its own would be stale.
     */
    bool        reads = (flags & TAGFS_HANDLE_READ) || !(flags & TAGFS_HANDLE_WRITE);
    FileRecord *rec   = FileMapRecordHold(vol->state.files, file_id, NULL);
    /* A file still arriving opens for the carry laying it alone. */
    if (rec && (rec->flags & TAGFS_FILE_ARRIVING) && !(flags & TAGFS_HANDLE_ARRIVAL))
    {
        FileRecordRelease(rec);
        rec = NULL;
    }
    if (!rec)
    {
        tagfs_close(vol, handle);
        return NULL;
    }
    handle->file_size = rec->size;
    if (reads)
        handle->rec = rec;
    else
        FileRecordRelease(rec);
    return handle;
}

void tagfs_close(TagFSVolume* vol, TagFSFileHandle *handle)
{
    if (!handle)
        return;
    HelmViewLetGo(vol, handle);
    ofe_release(vol, handle->ofe);
    ReadAheadLetGo(vol, handle);
    FileRecordRelease(handle->rec);
    kfree(handle);
}

error_t TagFSReadInto(TagFSVolume* vol, TagFSFileHandle *handle, const ReadSink *sink, uint64_t *out_read)
{
    if (!out_read)
        return ERR_INVALID_ARGUMENT;
    *out_read = 0;
    if (!sink || (!sink->kernel && !sink->vmm))
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = handle_belongs_here(vol, handle, "read")
                     ? TagFSReadStream(vol, handle, sink, out_read)
                     : ERR_TAGFS_INVALID_HANDLE;
    tagfs_leave(vol);
    return rc;
}

error_t tagfs_read(TagFSVolume* vol, TagFSFileHandle *handle, void *buffer, uint64_t size, uint64_t *out_read)
{
    ReadSink sink = { .kernel = buffer, .bytes = size };
    if (!buffer) {
        if (out_read)
            *out_read = 0;
        return ERR_INVALID_ARGUMENT;
    }
    return TagFSReadInto(vol, handle, &sink, out_read);
}

error_t tagfs_write(TagFSVolume* vol, TagFSFileHandle *handle, const PlotSource *src, uint64_t *out_written)
{
    if (!out_written)
        return ERR_INVALID_ARGUMENT;
    *out_written = 0;
    if (!src || src->bytes == 0)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t    rc = ERR_TAGFS_INVALID_HANDLE;
    HelmWaiter here;
    if (handle_belongs_here(vol, handle, "write")) {
        rc = HelmTakeNow(vol, handle, &here);
        if (rc == OK) {
            rc = HelmWrite(vol, handle, src, out_written);
            HelmLetGo(vol, handle->ofe, &here);
            TagFSDataCostWrite(vol);
        }
    }
    tagfs_leave(vol);
    return rc;
}

error_t tagfs_span_file(TagFSVolume* vol, uint64_t file_id, uint64_t size)
{
    if (file_id == 0)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t          rc     = ERR_FILE_NOT_FOUND;
    TagFSFileHandle *handle = tagfs_open(vol, file_id, TAGFS_HANDLE_WRITE);
    HelmWaiter       here;
    if (handle) {
        rc = HelmTakeNow(vol, handle, &here);
        if (rc == OK) {
            uint64_t was = 0;
            do {
                rc = HelmSpan(vol, handle, size, &was);
            } while (HelmRoomCameBack(vol, &rc));
            HelmLetGo(vol, handle->ofe, &here);
        }
        tagfs_close(vol, handle);
    }
    tagfs_leave(vol);
    return rc;
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static int tagfs_get_metadata_inside(TagFSVolume* vol, uint64_t file_id, TagFSMetadata *out);

int tagfs_get_metadata(TagFSVolume* vol, uint64_t file_id, TagFSMetadata *out)
{
    if (!tagfs_enter(vol))
        return -1;
    int rc = tagfs_get_metadata_inside(vol, file_id, out);
    tagfs_leave(vol);
    return rc;
}

/* The file's record, out of the map of live files — the authority every read
 * stands on, holding every live file's newest record, so no road leads to the
 * medium. Its header — name, tags, flags, size, times — and none of its runs:
 * no caller of this reads them, and a file's runs are walked in its record
 * (file_map/file_record.h), never copied out. -1 for a file the map does not
 * hold — nor one still arriving, which is nobody's yet — or no memory for the
 * copy. */
static int tagfs_get_metadata_inside(TagFSVolume* vol, uint64_t file_id, TagFSMetadata *out)
{
    if (!out)
        return -1;
    FileRecord *rec = FileMapRecordHold(vol->state.files, file_id, NULL);
    if (rec && (rec->flags & TAGFS_FILE_ARRIVING)) {
        FileRecordRelease(rec);
        return -1;
    }
    if (!rec)
        return -1;
    int copied = FileRecordCopyOut(rec, out);
    FileRecordRelease(rec);
    return copied;
}

void tagfs_metadata_free(TagFSMetadata *meta)
{
    if (meta->filename) { kfree(meta->filename); meta->filename = NULL; }
    if (meta->tag_ids)  { kfree(meta->tag_ids);  meta->tag_ids  = NULL; }
    if (meta->extents)  { kfree(meta->extents);  meta->extents  = NULL; }
    if (meta->shown_to) { kfree(meta->shown_to); meta->shown_to = NULL; }
    meta->tag_count    = 0;
    meta->extent_count = 0;
    meta->shown_count  = 0;
}

TagFSState *tagfs_get_state(void)
{
    return &TagFSHome()->state;
}

// ----------------------------------------------------------------------------
// Tags by name — without handing the registry out
// ----------------------------------------------------------------------------

/*
 * ‼ THE REGISTRY USED TO LEAVE THE FILESYSTEM, AND IT IS FREED ON A RE-MOUNT.
 *
 * Seven files outside TagFS reached tagfs_get_state()->registry, checked it
 * for NULL, and then used it: Touch resolving an occurrence, Bay and Brook
 * naming a channel, a process being asked whether it carries a tag, a cabin
 * being given its tags, use-context, autostart. Between that check and the use
 * of it, a volume whose medium had left could be dropped and its registry
 * handed back to the allocator. Touch resolves a tag on every publish, so this
 * was not a corner of the machine — it was its busiest road.
 *
 * All of them wanted one of four things, and all four are here now, each
 * taking the door for the length of the answer. What leaves TagFS is an id, or
 * a copy of a name in the caller's own buffer — or spans into the registry for
 * a caller that holds the door itself (TagFSTagSpelling).
 */
uint32_t TagFSLookupSpelling(TagFSVolume* vol, const TagSpelling *tag)
{
    if (!tagfs_enter(vol))
        return TAGFS_INVALID_TAG_ID;
    uint32_t id = TagRegistryLookup(vol->state.registry, TagOwnerPerson(), tag);
    tagfs_leave(vol);
    return id;
}

error_t TagFSInternSpelling(TagFSVolume* vol, const TagSpelling *tag, uint32_t *out_id)
{
    return TagFSInternOwned(vol, TagOwnerPerson(), tag, out_id);
}

error_t TagFSInternOwned(TagFSVolume* vol, TagOwner owner, const TagSpelling *tag, uint32_t *out_id)
{
    *out_id = TAGFS_INVALID_TAG_ID;
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = TagRegistryIntern(vol->state.registry, owner, tag, out_id);
    if (rc == OK && owner.kind == TAG_OWNER_PERSON)   /* inside the door: bound before this book can go */
        UseContextBindTag(tag, *out_id);
    tagfs_leave(vol);
    return rc;
}

/* The id this volume has for a tag's text, or TAGFS_INVALID_TAG_ID when it has
 * none — including when the text is no tag, and when there is no volume. */
uint32_t tagfs_tag_lookup(TagFSVolume* vol, const char *tag)
{
    TagSpelling spelled;
    if (!tag || TagSpell(tag, strlen(tag), &spelled) != OK)
        return TAGFS_INVALID_TAG_ID;
    return TagFSLookupSpelling(vol, &spelled);
}

/* The same, and the volume issues an id if it does not have one yet. */
error_t tagfs_tag_intern(TagFSVolume* vol, const char *tag, uint32_t *out_id)
{
    TagSpelling spelled;
    *out_id = TAGFS_INVALID_TAG_ID;
    if (!tag)
        return ERR_TAGFS_INVALID_TAG;
    error_t rc = TagSpell(tag, strlen(tag), &spelled);
    if (rc != OK)
        return rc;
    return TagFSInternSpelling(vol, &spelled, out_id);
}

bool TagFSTagSpelling(TagFSVolume* vol, uint32_t tag_id, TagSpelling *out, bool *is_system, TagOwner *owner)
{
    return vol->state.registry && TagRegistrySpelling(vol->state.registry, tag_id, out, is_system, owner);
}

size_t tagfs_tag_text(TagFSVolume* vol, uint32_t tag_id, char *out, size_t out_size)
{
    if (out && out_size)
        out[0] = '\0';
    if (!tagfs_enter(vol))
        return 0;

    TagSpelling tag;
    size_t      len = 0;
    if (TagFSTagSpelling(vol, tag_id, &tag, NULL, NULL)) {
        len = TagSpellingTextLength(&tag);
        if (out && len < out_size)
            TagSpellingText(&tag, out);
    }
    tagfs_leave(vol);
    return len;
}

bool TagFSHoldsProgram(TagFSVolume* vol, uint64_t owner)
{
    if (!vol || owner == 0 || !tagfs_enter(vol))
        return false;
    bool holds = FileMapProgramFiles(vol->state.files, owner, NULL, 0, NULL) > 0;
    tagfs_leave(vol);
    return holds;
}

uint64_t TagFSProgramOf(TagFSVolume* vol, uint64_t fid)
{
    if (fid == 0 || !tagfs_enter(vol))
        return 0;
    FileRecord* rec   = FileMapRecordHold(vol->state.files, fid, NULL);
    uint64_t    owner = rec ? (rec->passport ? rec->passport : fid) : 0;
    FileRecordRelease(rec);
    tagfs_leave(vol);
    return owner;
}

uint64_t TagFSProgramCard(TagFSVolume* vol, uint64_t owner)
{
    uint64_t card = 0;
    if (owner && tagfs_enter(vol)) {
        FileMapProgramFiles(vol->state.files, owner, NULL, 0, &card);
        tagfs_leave(vol);
    }
    return card;
}

uint64_t TagOwnerWord(TagFSVolume* vol, TagOwner owner)
{
    if (owner.kind != TAG_OWNER_PROGRAM)
        return 0;
    uint64_t card = TagFSProgramCard(vol, owner.id);
    return card ? card : owner.id;
}

uint32_t TagFSProgramFiles(TagFSVolume* vol, uint64_t owner, uint64_t* out, uint32_t room)
{
    if ((room && !out) || !tagfs_enter(vol))
        return 0;
    uint32_t n = FileMapProgramFiles(vol->state.files, owner, out, room, NULL);
    tagfs_leave(vol);
    return n;
}

error_t TagFSPassportLay(TagFSVolume* vol, uint64_t fid, uint64_t passport)
{
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    InkReceipt receipt;
    error_t    rc = InkPassport(vol, fid, passport, &receipt);
    tagfs_leave(vol);
    return rc;
}

bool TagFSTagIsPersons(uint32_t tag_id)
{
    TagFSVolume* vol = TagFSHome();   /* the machine's language */
    if (!tagfs_enter(vol))
        return false;
    TagSpelling tag;
    TagOwner    owner;
    bool persons = TagFSTagSpelling(vol, tag_id, &tag, NULL, &owner) && owner.kind == TAG_OWNER_PERSON;
    tagfs_leave(vol);
    return persons;
}

bool TagFSTagMatches(uint32_t tag_id, const TagSpelling *pattern)
{
    TagFSVolume* vol = TagFSHome();   /* the machine's language */
    if (!tagfs_enter(vol))
        return false;
    TagSpelling tag;
    TagOwner    owner;
    bool matches = TagFSTagSpelling(vol, tag_id, &tag, NULL, &owner) && owner.kind == TAG_OWNER_PERSON &&
                   TagPatternMatches(pattern, &tag);
    tagfs_leave(vol);
    return matches;
}

// ----------------------------------------------------------------------------
// Defrag
// ----------------------------------------------------------------------------

error_t tagfs_defrag_file(TagFSVolume* vol, uint64_t file_id)
{
    if (!tagfs_enter(vol))
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t          rc     = ERR_FILE_NOT_FOUND;
    TagFSFileHandle *handle = tagfs_open(vol, file_id, TAGFS_HANDLE_WRITE);
    HelmWaiter       here;
    if (handle) {
        rc = HelmTakeNow(vol, handle, &here);
        if (rc == OK) {
            do {
                rc = HelmDefrag(vol, handle);
            } while (HelmRoomCameBack(vol, &rc));
            HelmLetGo(vol, handle->ofe, &here);
        }
        tagfs_close(vol, handle);
    }
    tagfs_leave(vol);
    return rc;
}

/* The volume's mount part as it begins: what was not nothing. */
void MountVolumeBegin(TagFSVolume* vol)
{
    vol->state = (TagFSState){ .lock = TAGFS_LOCK_INIT("state", LOCK_RANK_STATE) };
    vol->mount.mount_knock = (Knock){ .baton = { .next = NULL, .run = mount_knocked, .ctx = vol,
                                          .one_way = 0 },
                               .raised = 0 };
    vol->mount.seat = BOARDROOM_NO_SEAT;
    vol->mount.search_unread = BOARDROOM_NO_SEAT;
}
