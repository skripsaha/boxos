#include "tagfs.h"
#include "boardroom.h"
#include "boarding.h"
#include "deed/deed.h"
#include "deed_pen.h"
#include "tagfs_reserved.h"
#include "tag_registry/tag_registry.h"
#include "file_map/file_map.h"
#include "file_table/file_table.h"
#include "metadata_pool/meta_pool.h"
#include "use_context.h"
#include "disk_book/disk_book.h"
#include "error.h"
#include "ahci_sync.h"
#include "ahci.h"
#include "touch.h"
#include "ata.h"
#include "amp.h"
#include "cow/cow.h"
#include "braid/braid.h"
#include "integrity/integrity.h"
#include "settle_cut/settle_cut.h"
#include "plot/plot.h"
#include "scribe/scribe.h"
#include "ink/ink.h"
#include "ink/fallow.h"
#include "settle/settle.h"
#include "helm/helm.h"
#include "../../kernel/drivers/timer/rtc.h"
#include "logbook.h"
#include "cpu_calibrate.h"

static TagFSState g_state = { .lock = TAGFS_LOCK_INIT("state", LOCK_RANK_STATE) };
static WellKnownTags g_well_known = { TAGFS_INVALID_TAG_ID, TAGFS_INVALID_TAG_ID };

// Accessor function for well-known tags (tagfs_get_state already defined below)
WellKnownTags *tagfs_get_well_known_tags(void) { return &g_well_known; }

// Internal macro for accessing well-known tags
#define g_wk (*tagfs_get_well_known_tags())

// ----------------------------------------------------------------------------
// The door — who is inside the mounted volume, and which mounting it is
// ----------------------------------------------------------------------------

/*
 * ‼ A VOLUME WAS TAKEN DOWN WHILE OTHER CORES WERE STANDING INSIDE IT.
 *
 * A medium that leaves and comes back is re-read rather than carried on from,
 * and that road is: take the volume down, give its memory back, mount again
 * (TagFSVolumeReturned). Nothing in it ever asked whether anybody was in
 * there. On another core there could be a read walking a file's extents, a
 * query walking the tag index, a process being asked whether it carries a tag
 * — every one of them holding a pointer into exactly what tagfs_clear_the_ground
 * is about to hand back to the allocator: the registry, the tag index, the
 * block map, the file table, the metadata pool.
 *
 * ‼ THE ONE-WAY LATCH DOES NOT COVER IT. g_medium_left makes every read and
 * every write refuse, which is what stops work that has not started. It does
 * nothing about a caller who is already inside a call and already holding
 * those pointers.
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

static volatile uint8_t  g_door        = VOLUME_DOWN;
static volatile uint32_t g_inside      = 0;
static volatile uint32_t g_mount_epoch = 0;

/* A volume that was shut but not yet let go of, because somebody was inside.
 * Raised where the departure is noticed, lowered by the pass that finishes it
 * (TagFSServiceIfPending) — the same arrangement the retiring xHCI slots use,
 * and for the same reason: the place that notices cannot wait. */
static volatile uint32_t g_abandon_pending = 0;
static bool              g_leaving_said    = false;

/*
 * ‼ A MOUNT THAT DID NOT FINISH, AND NOTHING THAT WOULD EVER ASK AGAIN.
 *
 * Mounting is driven by ARRIVALS: a medium is seated, `seat:taken` is
 * published, and this file takes the volume up. That is the only thing that
 * ever asks. So a mount that begins and fails leaves the machine with no
 * filesystem until the NEXT arrival — and if the medium is sitting in its
 * socket, answering, there will not be one. The stick is right there and the
 * machine will not look at it again for the rest of the boot.
 *
 * Measured on the owner's board, 2026-08-30: three returns out of fifty-five
 * failed their mount, and all three recovered ONLY because the hand at the
 * machine kept replugging. Each of those failures was the medium leaving again
 * mid-read, which does produce another arrival — but a read that fails for any
 * other reason, on a medium that stays put, does not.
 *
 * So the mount is asked again. Not on a clock deciding anything: the medium is
 * in the room (the room says so), the volume is ours (its deed says so), and
 * the read failed (the medium says so). Asking again is the only answer to a
 * question whose answer changed under us, and it is spaced because nothing
 * about the hardware can change faster than that. It is bounded, and when the
 * budget is out it SAYS SO and stops — an arrival still gets the ordinary road.
 */
#define TAGFS_MOUNT_RETRY_MS    750u
#define TAGFS_MOUNT_RETRY_TRIES 8u

static volatile uint32_t g_mount_owed  = 0;
static uint64_t          g_mount_due   = 0;
static uint32_t          g_mount_tries = 0;

/*
 * ‼ ONE CORE AT A TIME MOUNTS OR UNMOUNTS, AND THE REST GO AWAY.
 *
 * Attending to a medium used to reach this file down one road only — the
 * Boardroom being called to order, which is already one-at-a-time — so nothing
 * here needed a flag of its own. The service pass is a SECOND road, taken by
 * the idle loop and by every K-Core, and without this two of them can be
 * inside tagfs_init at the same moment: two mounts of one volume, each
 * allocating what the other is about to overwrite.
 *
 * Whoever gets here first does the work; the others have nothing to add, so
 * they leave rather than queue up behind it — the same arrangement the hubs
 * and the slot service use.
 */
static volatile uint32_t g_attending = 0;

static bool attending_take(void)
{
    return __atomic_exchange_n(&g_attending, 1u, __ATOMIC_ACQUIRE) == 0;
}

static void attending_done(void)
{
    __atomic_store_n(&g_attending, 0u, __ATOMIC_RELEASE);
}

bool tagfs_enter(void)
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
    __atomic_fetch_add(&g_inside, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&g_door, __ATOMIC_ACQUIRE) != VOLUME_UP) {
        __atomic_fetch_sub(&g_inside, 1, __ATOMIC_ACQ_REL);
        return false;
    }
    return true;
}

void tagfs_leave(void)
{
    __atomic_fetch_sub(&g_inside, 1, __ATOMIC_ACQ_REL);
}

uint32_t tagfs_mount_epoch(void)
{
    return __atomic_load_n(&g_mount_epoch, __ATOMIC_ACQUIRE);
}

/* This machine believes there is a volume in the room and has not got it up.
 * Ask again in a moment; see the note over TAGFS_MOUNT_RETRY_MS. */
static void mount_owed(void)
{
    g_mount_due = rdtsc() + cpu_ms_to_tsc(TAGFS_MOUNT_RETRY_MS);
    __atomic_store_n(&g_mount_owed, 1u, __ATOMIC_RELEASE);
}


/* It is up — by this road or any other — so nothing is owed. */
static void mount_settled(void)
{
    g_mount_tries = 0;
    __atomic_store_n(&g_mount_owed, 0u, __ATOMIC_RELEASE);
}

bool tagfs_handle_is_of_this_mount(const TagFSFileHandle *handle)
{
    return handle && handle->mount_epoch == tagfs_mount_epoch();
}

/* Said rather than returned, because every caller of a read is written to
 * treat a refusal as an I/O failure and would otherwise report the medium as
 * broken. What happened is that the volume was read again. */
static bool handle_belongs_here(const TagFSFileHandle *handle, const char *what)
{
    if (tagfs_handle_is_of_this_mount(handle)) {
        return true;
    }
    if (!handle) {
        return false;               /* not a stale handle — no handle at all */
    }
    kprintf("[TagFS] %s refused: file %u was opened on an earlier mounting of "
            "this volume, and the volume has been read again since\n",
            what, handle ? handle->file_id : 0);
    return false;
}

// ----------------------------------------------------------------------------
// HOLDGROUND=on — a caller who is inside the volume when its medium leaves
// ----------------------------------------------------------------------------

/*
 * ‼ A REPRODUCTION, NOT A FEATURE. Compiled in by `make HOLDGROUND=on` and by
 * nothing else.
 *
 * The window this whole barrier exists for is microseconds wide and QEMU never
 * lands in it: on a desk the volume comes back while the machine is idle, so
 * there is nobody inside to be protected and every check about the barrier
 * would be green by construction. This puts somebody there — at the exact
 * moment the medium is noticed gone, one caller steps into the volume and
 * stays for two seconds, which is the situation a re-mount has to survive.
 *
 * It also holds a file open ACROSS the departure, which is the other half:
 * a handle carries its own copy of the file's extents, and the table of open
 * files used to be emptied out from under it by the next mount.
 *
 * Three facts come out of it, and each is the failure of a different line:
 *
 *   the take-down says it is waiting and then that the last caller is out
 *          — take the "somebody is inside" refusal out of tagfs_abandon;
 *   a read through the old handle is refused by name
 *          — take the mounting stamp off the handle;
 *   the entry that the old handle owns is STILL IN the open-file table
 *          — put the memset back into tagfs_init.
 */
#if CONFIG_TAGFS_HOLD_GROUND

#define TAGFS_HOLD_GROUND_MS 2000u

static bool             g_hold_inside  = false;
static uint64_t         g_hold_until   = 0;
static TagFSFileHandle *g_hold_handle  = NULL;

/* Is the entry this handle owns still the one the table hands out for its
 * file? A second entry for one file is a second Helm for one file, which is
 * two holders of its extents at once — and it leaves no trace of its own. */
static bool hold_entry_still_listed(const TagFSFileHandle *handle, uint32_t *out_refs)
{
    return handle && ofe_listed(handle->ofe, out_refs);
}

/* A file is held open from the moment the volume is first mounted, so that it
 * is a handle from the PREVIOUS mounting when the volume comes back. */
static void TagFSHoldGroundOpenFile(void)
{
    if (g_hold_handle) {
        return;
    }
    uint32_t  ids[1];
    TagFSPage page;
    if (TagFSListPage(1, ids, 1, false, &page) != OK || page.delivered == 0) {
        kprintf("[TagFS HOLDGROUND] this volume has no file to hold open\n");
        return;
    }
    g_hold_handle = tagfs_open(ids[0], 0);
    kprintf("[TagFS HOLDGROUND] holding file %u open across whatever happens "
            "to the medium\n", ids[0]);
}

/* And a caller inside the volume at the moment its medium is noticed gone. */
static void TagFSHoldGroundTake(void)
{
    if (g_hold_inside) {
        return;
    }
    if (!tagfs_enter()) {
        return;                     /* already shut — nothing to demonstrate */
    }
    g_hold_inside = true;
    g_hold_until  = rdtsc() + cpu_ms_to_tsc(TAGFS_HOLD_GROUND_MS);
    kprintf("[TagFS HOLDGROUND] a caller was inside the volume when its medium "
            "left, and does not step out until the volume is back and at "
            "least %u ms have passed\n", TAGFS_HOLD_GROUND_MS);
}

/* Reached only once a return is waiting to be finished, which is what makes
 * this deterministic: the caller is inside when the volume comes back, every
 * time, rather than whenever a clock happens to land. */
static void TagFSHoldGroundRelease(void)
{
    if (!g_hold_inside || (int64_t)(rdtsc() - g_hold_until) < 0) {
        return;
    }
    g_hold_inside = false;
    tagfs_leave();
    kprintf("[TagFS HOLDGROUND] the caller has stepped out of the old volume\n");
}

/* And what the volume looks like to what was held across it. */
static void TagFSHoldGroundAfterReturn(void)
{
    if (!g_hold_handle || !g_state.initialized) {
        return;
    }

    uint8_t  probe[16];
    uint64_t got = 0;
    error_t  rc  = tagfs_read(g_hold_handle, probe, sizeof(probe), &got);
    kprintf("[TagFS HOLDGROUND] a read through the handle from before answered "
            "%u\n", (unsigned)rc);

    uint32_t refs = 0;
    bool listed = hold_entry_still_listed(g_hold_handle, &refs);
    kprintf("[TagFS HOLDGROUND] the open-file entry that handle owns is %s the "
            "table, with %u reference(s)\n",
            listed ? "still in" : "NO LONGER in", refs);

    TagFSFileHandle *again = tagfs_open(g_hold_handle->file_id, 0);
    if (again) {
        refs = 0;
        listed = hold_entry_still_listed(g_hold_handle, &refs);
        kprintf("[TagFS HOLDGROUND] opening the same file again %s the same "
                "entry, now with %u reference(s)\n",
                (again->ofe == g_hold_handle->ofe) ? "found" : "made a SECOND",
                refs);
        tagfs_close(again);
    }

    tagfs_close(g_hold_handle);
    g_hold_handle = NULL;
}

#else
#define TagFSHoldGroundOpenFile()    ((void)0)
#define TagFSHoldGroundTake()        ((void)0)
#define TagFSHoldGroundRelease()     ((void)0)
#define TagFSHoldGroundAfterReturn() ((void)0)
#endif  /* CONFIG_TAGFS_HOLD_GROUND */

/* A bit of the mount's roll call map (tagfs_init): set where something owns
 * the block. */
static inline void bitmap_set_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

// ----------------------------------------------------------------------------
// Disk I/O helpers
// ----------------------------------------------------------------------------

/*
 * Which medium the volume is on.
 *
 * This used to be two numbers and a question asked at every call site: an ATA
 * drive index, an AHCI port, and "is AHCI initialised?" repeated eight times
 * over. It is now one seat number in the Boardroom, which knows what kind of
 * medium sits in it. The filesystem's business is sectors; whose sectors they
 * are is somebody else's.
 */
static uint8_t g_tagfs_seat = BOARDROOM_NO_SEAT;

/*
 * WHICH occupant of that seat this volume is.
 *
 * A seat can be taken again, which is what lets a stick that was pulled out
 * come home to the chair it left. The cost of that is exactly this: "there is
 * a medium in seat 3" stopped being enough to know it is the SAME medium, so
 * the taking is stamped here at the moment the volume is mounted and compared
 * afterwards. A chair that changed hands while nobody was reading from it is a
 * medium that left, whatever is sitting in it now.
 *
 * Before seats could be reused this was covered by accident — a returning
 * medium always got a NEW seat number, so the number itself carried the
 * answer. It also meant the room grew by one chair per unplug for ever, and a
 * medium that got its old unit number back was never announced at all.
 */
static uint32_t g_tagfs_seating = 0;

uint8_t tagfs_get_seat(void) { return g_tagfs_seat; }

/*
 * Recognise the volume: the superblock magic, in the sector it always lives in.
 * Handed to the Boardroom, which asks it of every medium in turn — the
 * Boardroom knows about media, and this is the only thing that knows what a
 * TagFS volume looks like.
 */
/*
 * Is there a TagFS volume in this seat, and what does it call itself?
 *
 * The magic says it is one of ours. The identity says WHICH one — and that is
 * the question, because the loader read this kernel out of one particular
 * volume and wrote its identity on the boarding pass. A machine carrying two
 * TagFS volumes is not unusual: it is what a machine with BoxOS installed and
 * a BoxOS stick in a socket looks like.
 */
static bool tagfs_recognise(void *ctx, uint8_t seat, uint8_t out_uuid[16])
{
    (void)ctx;

    MediumGround ground[GROUND_MAX_PER_MEDIUM];
    uint8_t claimed = GroundSurvey(seat, ground, GROUND_MAX_PER_MEDIUM);
    if (claimed == 0) return false;

    /*
     * A medium may carry more than one BoxOS partition, and then "is there a
     * volume here" has more than one answer. The one this kernel came out of
     * is the answer that matters, so the pass is consulted here rather than
     * only by the Boardroom afterwards — the Boardroom compares one identity
     * per seat and would otherwise be handed whichever ground happened to be
     * first in the table.
     */
    uint8_t want[16];
    bool    have_pass = BoardingPassVolume(want);

    uint8_t first[16];
    bool    any = false;

    for (uint8_t g = 0; g < claimed; g++) {
        DeedCopy head;
        if (DeedReadHead(seat, &ground[g], &head) != OK) {
            /* The head is what is damaged, not the volume. Its far copy names
             * the same volume and is the reason there are two. */
            if (DeedReadTailAlone(seat, &ground[g], &head) != OK) continue;
        }

        bool wanted = have_pass && memcmp(head.head.uuid, want, 16) == 0;
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

    if (any && out_uuid) memcpy(out_uuid, first, 16);
    return any;
}

/*
 * Which volume this is, as opposed to which seat it happens to be in.
 *
 * A seat is a socket and a volume is a filesystem, and on a machine you can
 * unplug things from, the second moves between the first. The identity is the
 * one the superblock carries and the loader writes on its boarding pass, so it
 * survives the medium being pulled out and put back in a different socket —
 * which is exactly the case where a seat number proves nothing.
 */
static uint8_t g_volume_uuid[16];
static bool    g_volume_uuid_known = false;

/*
 * Where this volume's ground begins on the medium it is sitting on.
 *
 * The one absolute address TagFS holds, and it is not written down anywhere
 * inside the volume: it is discovered from the medium's partition table every
 * time the volume is met. Zero until a volume has been taken up, which is
 * safe — nothing reads sectors before then, and reading sector 0 of the medium
 * is not something a filesystem should be able to do by accident.
 */
static uint64_t g_volume_base;

uint64_t tagfs_get_volume_base(void) { return g_volume_base; }

static void TagFSProbeDrive(void)
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
    g_tagfs_seat = BoardroomFindVolume(tagfs_recognise, NULL, uuid);

    if (g_tagfs_seat == BOARDROOM_NO_SEAT) {
        kprintf("[TagFS] no volume found on any of the %u seated medium(s)\n",
                BoardroomSeatCount());
        return;
    }

    memcpy(g_volume_uuid, uuid, 16);
    g_volume_uuid_known = true;
    g_tagfs_seating     = BoardroomSeatSeating(g_tagfs_seat);

    kprintf("[TagFS] volume on seat %u: %s\n", g_tagfs_seat,
            BoardroomSeatName(g_tagfs_seat));

    /*
     * And how much this medium will be asked for at a time, said out loud
     * because it is a property of the MEDIUM and not of the filesystem.
     *
     * A legacy channel answers eight sectors — one block — because past that
     * it gives up its DMA path; a flash drive and a SATA port answer far more,
     * and their neighbours are then fetched together. Printing it is what
     * turns "why is this machine slower off that disk" into a sentence
     * somebody can read off a screen.
     */
    uint32_t run     = BoardroomSeatRun(g_tagfs_seat);
    uint32_t offered = run / (TAGFS_BLOCK_SIZE / BOARDROOM_SECTOR_BYTES);
    if (offered == 0) offered = 1;

    uint32_t asked = offered;
    if (asked > TAGFS_READ_AHEAD_BLOCKS) asked = TAGFS_READ_AHEAD_BLOCKS;

    /*
     * Two numbers and the narrower of them, each said by whoever chose it.
     *
     * The line used to read "this medium takes N sectors at a time, SO
     * neighbouring blocks are read M at a time", and nothing derived M from N:
     * the flash drive on the board answered 128 sectors, which is sixteen of
     * this volume's blocks, and the four that got printed came from the size
     * of the read-ahead cache. Two media answering 32 and 128 produced the
     * same four, so the line stopped telling them apart exactly where it
     * promised to. Which side is binding is the thing worth reading: when it
     * is the cache, the medium is being asked for less than it offered, and
     * that is the answer to "why is this machine slower off that disk".
     */
    kprintf("[TagFS] this medium hands over %u sector(s) in one go, which is %u "
            "of this volume's %u-byte block(s), and the read-ahead cache holds "
            "%u — so neighbours are fetched %u at a time, %s\n",
            run, offered, (unsigned)TAGFS_BLOCK_SIZE,
            (unsigned)TAGFS_READ_AHEAD_BLOCKS, asked,
            asked < offered
                ? "which is the cache's limit and not the medium's: it offered "
                  "more than is being taken"
                : "which is all the medium offered");
}

/* A run of any length, in the volume's own sectors: the room cuts it into the
 * runs the medium takes (BoardroomSeatRun). There used to be a door with a
 * 16-bit count in front of these, and a bitmap of a volume past a terabyte
 * went through it as a read of 0 sectors. */
int tagfs_volume_read(uint64_t vlba, uint32_t count, void *buffer)
{
    return BoardroomRead(g_tagfs_seat, g_volume_base + vlba, count, buffer);
}

int tagfs_volume_write(uint64_t vlba, uint32_t count, const void *buffer)
{
    return BoardroomWrite(g_tagfs_seat, g_volume_base + vlba, count, buffer);
}

/* A Deed occupies one block wherever it sits — the pen says how many sectors
 * that is, so the tool and the kernel cannot come to disagree about it. */
#define VOLUME_DEED_BLOCKS  1u

/* How many data blocks a bitmap of this many blocks can carry a bit for. */
static uint64_t bitmap_covers_blocks(uint32_t bitmap_blocks)
{
    return (uint64_t)bitmap_blocks * (uint64_t)TAGFS_BLOCK_SIZE * 8u;
}

/*
 * Every bit for the blocks in [from, to) must read zero.
 *
 * This is a PROOF, not a write. The bits past the data run were zeroed when
 * the volume was made and nothing has been allowed to touch them since — every
 * writer of a bit is bounded by data_blocks — so a bit that is set there is
 * the medium telling us this ground is not what the partition table says it
 * is. Reading is enough to find that out, and reading cannot destroy anything,
 * which is why growth never writes a bitmap.
 *
 * Read a block at a time: the range can be two megabytes on a volume growing
 * into a 64 GB stick, and a mount is not the place to ask a kernel heap whose
 * floor is two megabytes for all of it at once.
 */
static bool ground_ahead_is_blank(uint32_t from, uint32_t to, uint32_t *out_set)
{
    *out_set = 0;
    if (to <= from) return true;

    uint8_t *buf = (uint8_t *)kmalloc(TAGFS_BLOCK_SIZE);
    if (!buf) return false;

    uint32_t first_byte = from / 8u;
    uint32_t last_byte  = (to + 7u) / 8u;         /* exclusive */
    uint64_t base       = (uint64_t)g_state.layout.block_bitmap_block *
                          TAGFS_BLOCK_SECTORS;

    for (uint32_t off = first_byte / TAGFS_BLOCK_SIZE * TAGFS_BLOCK_SIZE;
         off < last_byte; off += TAGFS_BLOCK_SIZE) {
        if (tagfs_volume_read(base + off / TAGFS_SECTOR_SIZE,
                              TAGFS_BLOCK_SECTORS, buf) != 0) {
            /* Nothing was learned, so nothing is reported: a partial tally
             * from the blocks read before the failure would have the caller
             * saying the bitmap has blocks marked used when what happened is
             * that the medium would not answer. */
            *out_set = 0;
            kfree(buf);
            return false;
        }
        for (uint32_t b = 0; b < TAGFS_BLOCK_SIZE; b++) {
            uint32_t byte_index = off + b;
            if (byte_index < first_byte || byte_index >= last_byte) continue;
            if (buf[b] == 0) continue;
            for (int bit = 0; bit < 8; bit++) {
                uint32_t block = byte_index * 8u + (uint32_t)bit;
                if (block < from || block >= to) continue;
                if (buf[b] & (1u << bit)) (*out_set)++;
            }
        }
    }

    kfree(buf);
    return *out_set == 0;
}

/*
 * ‼ THE VOLUME TAKES THE GROUND IT WAS GIVEN.
 *
 * An image is written to a medium with dd, and the medium is nearly always
 * bigger than the image. Until now the volume simply did not use the rest: a
 * 90 MiB image on a 64 GB stick left 63 GB of it dark, forever, with nothing
 * saying so. A volume that cannot take the ground under it is a volume for
 * image files, not for a machine somebody boots.
 *
 * So on every mount the two numbers are compared — how far the partition runs
 * and how far the Deed says the volume runs — and when there is ground behind
 * it, the volume takes it. That is three numbers in the Deed and nothing else:
 * `sectors`, `tail_sector`, and the LAYOUT stamp's `total_blocks`/`data_blocks`.
 *
 * ‼ data_block DOES NOT MOVE, and that is the whole reason this is safe. A
 * block number in a file's metadata is an index into the data run; move where
 * the run starts and every number on the volume means something else. The run
 * only ever gets LONGER, at the far end, where nothing was.
 *
 * ‼ THE ORDER IS THE CRASH SAFETY, and there is nothing else protecting this.
 *
 *   1. prove the new range is blank (a read — nothing is changed)
 *   2. write the tail Deed at the NEW far end, and read it back
 *   3. write the head Deed
 *
 * The head is the commit point. Cut the power before it and the volume is
 * exactly what it was: the head still states the old length, and the new free
 * bits lie beyond the old data_blocks where nobody asks about them. Cut it
 * after, and the volume is grown, whole, with both copies agreeing. There is
 * no third state.
 *
 * ‼ ONE CORE. Growth is a write, and it runs inside a mount: the boot mount is
 * the BSP alone (arrivals are gated shut until TagFSBootMountSettled), and
 * every later mount is under attending_take(). No second core can be here.
 *
 * ‼ IT ONLY EVER GROWS. A volume that shrank would be a volume whose file
 * extents point outside it. The only comparison below is "more than it has".
 */
static void volume_take_more_ground(uint8_t seat, uint64_t ground_sectors,
                                    DeedCopy *head)
{
    uint64_t have = head->head.sectors;

    /* Not a byte of room, or not a whole block of it: nothing to take, and it
     * is said, because "checked and there is nothing" and "never checked" have
     * to be tellable apart from a screen. */
    if (ground_sectors <= have + TAGFS_BLOCK_SECTORS) {
        kprintf("[TagFS] seat %u: the volume fills the ground it was given — "
                "%llu sectors, all of it\n", seat, (unsigned long long)have);
        return;
    }

    /*
     * ‼ THE MEDIUM HAS TO SAY HOW FAR IT RUNS, OR NOTHING IS TAKEN.
     *
     * A partition entry is bounded against the medium's own length only when
     * the medium answered (ground.c). A medium that will not say leaves the
     * entry unchecked — and reading past the end of a device is recoverable,
     * while writing a Deed that permanently claims ground which does not exist
     * is not: the volume then fails its own extent check on every boot after,
     * bricked by its own commit.
     */
    uint64_t medium = BoardroomSeatSectors(seat);
    if (medium == 0) {
        kprintf("[TagFS] seat %u: there is ground behind this volume and the "
                "medium will not say how far it runs — nothing is taken, "
                "because a partition table is the only thing claiming that "
                "ground is there\n", seat);
        return;
    }
    if (g_volume_base + ground_sectors > medium) {
        kprintf("[TagFS] seat %u: the ground behind this volume ends at sector "
                "%llu and the medium ends at %llu — nothing is taken\n", seat,
                (unsigned long long)(g_volume_base + ground_sectors),
                (unsigned long long)medium);
        return;
    }

    uint32_t data_block  = g_state.layout.data_block;
    uint32_t data_now    = g_state.layout.data_blocks;
    uint64_t vol_want    = ground_sectors / TAGFS_BLOCK_SECTORS;
    if (vol_want <= (uint64_t)data_block + VOLUME_DEED_BLOCKS) return;

    uint64_t data_want = vol_want - data_block - VOLUME_DEED_BLOCKS;

    /*
     * ‼ CLAMPED BY THE BITMAP, AND THE VOLUME SAYS SO.
     *
     * The bitmap lies in front of the data run, so it cannot be made bigger
     * without moving data_block — see above. mkfs lays it down for a growth
     * ceiling, and this is the volume meeting that ceiling: it takes what its
     * bitmap can account for and leaves the rest, rather than handing out
     * blocks it has nowhere to record.
     */
    uint64_t cover = bitmap_covers_blocks(g_state.layout.block_bitmap_blocks);
    bool clamped = false;
    if (data_want > cover) {
        data_want = cover;
        vol_want  = (uint64_t)data_block + data_want + VOLUME_DEED_BLOCKS;
        clamped   = true;
    }

    if (data_want <= (uint64_t)data_now) {
        kprintf("[TagFS] seat %u: the volume fills what its bitmap can account "
                "for — %u data blocks\n", seat, data_now);
        return;
    }
    /* The layout counts blocks in 32 bits; a volume that outgrew that is a
     * volume this format cannot describe, and it stops here rather than
     * wrapping. */
    if (vol_want > 0xFFFFFFFFull || data_want > 0xFFFFFFFFull) return;

    uint64_t new_sectors = vol_want * TAGFS_BLOCK_SECTORS;
    uint64_t new_tail    = (vol_want - VOLUME_DEED_BLOCKS) * TAGFS_BLOCK_SECTORS;
    uint64_t old_tail    = head->head.tail_sector;

    kprintf("[TagFS] seat %u: this volume runs %llu sectors and the ground "
            "under it runs %llu — it takes the rest\n", seat,
            (unsigned long long)have, (unsigned long long)ground_sectors);
    if (clamped) {
        kprintf("[TagFS] seat %u: its block bitmap accounts for %llu blocks, so "
                "it takes %llu of them and leaves %llu sectors unclaimed — "
                "moving the bitmap would move every block number on it\n",
                seat, (unsigned long long)cover, (unsigned long long)data_want,
                (unsigned long long)(ground_sectors - new_sectors));
    }

    /* --- 1. The new ground must be blank ------------------------------- */
    uint32_t set = 0;
    if (!ground_ahead_is_blank(data_now, (uint32_t)data_want, &set)) {
        if (set) {
            kprintf("[TagFS] seat %u: the bitmap already has %u block(s) marked "
                    "used past the end of this volume — that ground is not "
                    "blank and this volume does not take it\n", seat, set);
        } else {
            kprintf("[TagFS] seat %u: the bitmap past the end of this volume "
                    "would not read — nothing is taken\n", seat);
        }
        return;
    }

    /* --- 2. The Deed, amended ------------------------------------------ */
    uint8_t *keep = (uint8_t *)kmalloc(head->raw_bytes);
    if (!keep) return;
    memcpy(keep, head->raw, head->raw_bytes);

    DeedPen pen;
    if (DeedPenTake(&pen, head->raw, head->raw_bytes) != 0) { kfree(keep); return; }

    uint16_t lay_bytes = 0;
    void *live = DeedPenFind(&pen, VOLUME_STAMP_LAYOUT, &lay_bytes);
    if (!live || lay_bytes < sizeof(VolumeLayout)) { kfree(keep); return; }

    /*
     * ‼ TWO FIELDS IN HERE ARE COPIED FORWARD AND MEAN NOTHING.
     *
     * `file_table_blocks` and `metadata_pool_blocks` are carried out of the
     * Deed at mount and put back unchanged here, and both chains have been
     * lengthening underneath them since the volume was made. Nothing reads
     * either one any more — the mount walks both chains to their end — so what
     * is re-committed is a stale number, not a wrong decision.
     *
     * They are written rather than zeroed because the stamp is shared: an
     * older kernel reading this Deed finds the fields where it expects them,
     * and a 1 it once believed is no worse than the 0 it would find instead.
     * Stating the truth would mean asking the two subsystems how long their
     * chains are at this instant and committing that into a record whose whole
     * point is that it does not change — which is the fault this repair
     * removed, put back in a different field.
     */
    VolumeLayout grown;
    memcpy(&grown, &g_state.layout, sizeof(grown));
    grown.total_blocks = vol_want;
    grown.data_blocks  = (uint32_t)data_want;
    /* Only the fields this build knows: a stamp longer than the struct keeps
     * whatever a later mkfs put after them. */
    memcpy(live, &grown, sizeof(grown));

    DeedPenSetGround(&pen, new_sectors, new_tail);

    /* --- The far copy first, and it must read back ---------------------- */
    DeedPenSetRole(&pen, VOLUME_DEED_ROLE_TAIL);
    DeedPenSeal(&pen);

    bool far_end_took_it =
        tagfs_volume_write(new_tail, DEED_PEN_SECTORS, head->raw) == 0 &&
        BoardroomFlush(seat) == 0;

    if (far_end_took_it) {
        /* Read it back off the medium. This is the one live proof that the
         * ground the partition table claims really is there and answers — a
         * table can lie, a device that returns the bytes cannot. */
        MediumGround grown_ground = { .start_sector = g_volume_base,
                                      .sectors      = new_sectors,
                                      .origin       = GROUND_FROM_MBR,
                                      .entry        = 0 };
        DeedCopy proof;
        DeedCopy asif = *head;
        asif.head.sectors     = new_sectors;
        asif.head.tail_sector = new_tail;
        if (DeedReadTail(seat, &grown_ground, &asif, &proof) == OK) {
            DeedRelease(&proof);
        } else {
            far_end_took_it = false;
        }
    }

    if (!far_end_took_it) {
        memcpy(head->raw, keep, head->raw_bytes);
        kfree(keep);
        kprintf("[TagFS] seat %u: the new far end at sector %llu would not take "
                "its deed and give it back — the volume is left exactly as it "
                "was\n", seat, (unsigned long long)new_tail);
        return;
    }

    /* --- 3. The head. Before this line the volume is old; after it, new. -- */
    DeedPenSetRole(&pen, VOLUME_DEED_ROLE_HEAD);
    DeedPenSeal(&pen);

    if (tagfs_volume_write(0, DEED_PEN_SECTORS, head->raw) != 0 ||
        BoardroomFlush(seat) != 0) {
        memcpy(head->raw, keep, head->raw_bytes);
        kfree(keep);
        kprintf("[TagFS] seat %u: the deed at the head would not be written — "
                "the volume stands at its old size and will try again on the "
                "next mount\n", seat);
        return;
    }
    kfree(keep);

    /* Committed. Everything from here on is bookkeeping that costs a number if
     * it is interrupted, never a block. */
    memcpy(&g_state.layout, &grown, sizeof(grown));
    head->head.sectors     = new_sectors;
    head->head.tail_sector = new_tail;
    /* The Ledger's free count is NOT touched here, and that is the whole
     * reason growth needs no repair after an interruption: the count is
     * derived from the bitmap at the end of every mount (see "THE FREE COUNT
     * IS DERIVED"), so ground taken shows up in it by itself and ground taken
     * twice cannot be counted twice. It used to be added here and written, and
     * a growth that ran again after its head deed was rolled back left a
     * volume claiming more free blocks than it had blocks. */
    kprintf("[TagFS] seat %u: the volume now runs %llu sectors; its data run is "
            "%u blocks, %u more than it had, and its far copy has moved to "
            "sector %llu\n", seat, (unsigned long long)new_sectors,
            g_state.layout.data_blocks, (uint32_t)(data_want - data_now),
            (unsigned long long)new_tail);

    /*
     * ‼ THE OLD FAR COPY IS ERASED, AND THAT IS SAFETY AND NOT TIDINESS.
     *
     * The block the old tail sat in is inside the data run now and marked
     * free, so it will be written over by the first file that lands on it.
     * Until then it is a checksum-valid Deed that states the OLD length and
     * the OLD layout — and DeedReadTailAlone finds a tail by reading the last
     * block of the ground it is given. Image this volume onto a partition that
     * happens to end there, lose the head, and the machine mounts on a stale
     * deed believing an old data run: block numbers handed out over files.
     */
    uint8_t *blank = (uint8_t *)kmalloc(DEED_PEN_BYTES);
    if (blank) {
        memset(blank, 0, DEED_PEN_BYTES);
        if (tagfs_volume_write(old_tail, DEED_PEN_SECTORS, blank) == 0) {
            kprintf("[TagFS] seat %u: the deed that used to sit at sector %llu "
                    "is inside the data run now and has been erased\n",
                    seat, (unsigned long long)old_tail);
        } else {
            kprintf("[TagFS] seat %u: the deed that used to sit at sector %llu "
                    "is inside the data run now and would not erase — it will "
                    "be written over by the first file that lands there\n",
                    seat, (unsigned long long)old_tail);
        }
        kfree(blank);
    }
    /* The one flush on this path whose answer is deliberately dropped, and it
     * stays dropped. The volume was committed at the head-deed write above;
     * everything since is bookkeeping whose own failure has already been said
     * out loud two lines up, and which the next file to land on that block
     * undoes anyway. There is no claim here for a refused flush to falsify. */
    (void)BoardroomFlush(seat);
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
static error_t volume_take_ground(uint8_t seat)
{
    MediumGround ground[GROUND_MAX_PER_MEDIUM];
    uint8_t claimed = GroundSurvey(seat, ground, GROUND_MAX_PER_MEDIUM);
    if (claimed == 0) {
        kprintf("[TagFS] seat %u claims no ground for BoxOS\n", seat);
        return ERR_FILE_NOT_FOUND;
    }

    uint8_t want[16];
    bool    have_pass = BoardingPassVolume(want);

    DeedCopy head;
    bool     took          = false;
    uint64_t ground_run    = 0;   /* how far the partition runs, kept past `g` */

    /* Two passes rather than one with a "best so far": the volume named on the
     * pass is the one this kernel came out of, and taking a different one
     * because it happened to be listed first is how a machine mounts the
     * stick it booted next to instead of the stick it booted from. */
    for (int pass = 0; pass < 2 && !took; pass++) {
        for (uint8_t g = 0; g < claimed; g++) {
            DeedCopy candidate;
            bool from_tail = false;
            if (DeedReadHead(seat, &ground[g], &candidate) != OK) {
                if (DeedReadTailAlone(seat, &ground[g], &candidate) != OK) continue;
                from_tail = true;
            }

            bool wanted = have_pass &&
                          memcmp(candidate.head.uuid, want, 16) == 0;
            if (pass == 0 && !wanted) {
                DeedRelease(&candidate);
                continue;
            }

            head          = candidate;
            g_volume_base = ground[g].start_sector;
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
        if (!have_pass) break;   /* nothing to prefer; the second pass IS the first */
    }

    if (!took) {
        kprintf("[TagFS] seat %u has ground claimed for BoxOS and no deed on "
                "it this kernel can read\n", seat);
        return ERR_TAGFS_CORRUPTED;
    }

    DeedDescribe(seat, &head);

    uint16_t bytes = 0;
    const VolumeLayout *layout =
        (const VolumeLayout *)DeedStamp(&head, VOLUME_STAMP_LAYOUT, &bytes);
    if (!layout || bytes < sizeof(VolumeLayout)) {
        kprintf("[TagFS] the deed on seat %u does not say where anything is — "
                "it carries no layout this kernel understands\n", seat);
        DeedRelease(&head);
        return ERR_TAGFS_CORRUPTED;
    }
    memcpy(&g_state.layout, layout, sizeof(g_state.layout));

    const VolumeGeometry *geo =
        (const VolumeGeometry *)DeedStamp(&head, VOLUME_STAMP_GEOMETRY, &bytes);
    if (geo && bytes >= sizeof(VolumeGeometry)) {
        memcpy(&g_state.geometry, geo, sizeof(g_state.geometry));
    }

    /* Where the loader finds the kernel, so nothing moves those blocks under
     * it (PlotGather). A Deed without the stamp names no such blocks. */
    const VolumeBoot *boot =
        (const VolumeBoot *)DeedStamp(&head, VOLUME_STAMP_BOOT, &bytes);
    memset(&g_state.boot, 0, sizeof(g_state.boot));
    if (boot && bytes >= sizeof(VolumeBoot)) {
        memcpy(&g_state.boot, boot, sizeof(g_state.boot));
    }

    memcpy(g_state.uuid, head.head.uuid, 16);

    /*
     * The layout has to fit inside the volume that states it. Everything below
     * this line turns these numbers into sector addresses without checking
     * them again, so a deed that says its data run ends past its own end would
     * otherwise be read into whatever is beyond the partition.
     */
    uint64_t blocks_in_volume = head.head.sectors / TAGFS_BLOCK_SECTORS;
    if (g_state.layout.data_block == 0 ||
        g_state.layout.data_blocks == 0 ||
        (uint64_t)g_state.layout.data_block +
        (uint64_t)g_state.layout.data_blocks > blocks_in_volume ||
        (uint64_t)g_state.layout.state_block +
        (uint64_t)g_state.layout.state_blocks > blocks_in_volume ||
        g_state.layout.state_blocks < VOLUME_LEDGER_COPIES) {
        kprintf("[TagFS] the deed on seat %u lays out %u data blocks at %u "
                "inside a volume of %llu — that does not fit\n",
                seat, g_state.layout.data_blocks, g_state.layout.data_block,
                (unsigned long long)blocks_in_volume);
        DeedRelease(&head);
        return ERR_TAGFS_CORRUPTED;
    }

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
                seat, g_state.geometry.physical_bytes);
    } else if (g_state.geometry.physical_bytes < medium_physical) {
        kprintf("[TagFS] seat %u: this volume was laid out for %u-byte blocks "
                "and the medium is built from %u — every metadata write costs "
                "it a read and a rewrite\n",
                seat, g_state.geometry.physical_bytes, medium_physical);
    } else if (g_state.geometry.physical_bytes > medium_physical) {
        kprintf("[TagFS] seat %u: this volume assumed %u-byte blocks and the "
                "medium is built from %u — coarser than it needs, which costs "
                "nothing\n",
                seat, g_state.geometry.physical_bytes, medium_physical);
    } else {
        kprintf("[TagFS] seat %u: the volume's grid and the medium's are the "
                "same %u bytes\n", seat, medium_physical);
    }

    if (g_state.geometry.block_bytes != TAGFS_BLOCK_SIZE) {
        kprintf("[TagFS] the volume on seat %u was laid out in %u-byte blocks "
                "and this kernel speaks %u\n",
                seat, g_state.geometry.block_bytes, TAGFS_BLOCK_SIZE);
        DeedRelease(&head);
        return ERR_TAGFS_CORRUPTED;
    }

    /*
     * ‼ HERE, AND NOT A LINE LATER.
     *
     * Growth belongs after every refusal that could still turn this volume
     * away — a volume this kernel will not mount must not be written to — and
     * before the far-copy check below, because growing MOVES the far copy and
     * that check has to read it where it now is.
     *
     * A volume standing on its tail copy is not grown: its head is gone, and
     * the head is the commit point.
     */
    if (head.head.role == VOLUME_DEED_ROLE_HEAD) {
        volume_take_more_ground(seat, ground_run, &head);
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
    MediumGround stood = { .start_sector = g_volume_base,
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
static void volume_would_not_flush(error_t why)
{
    static const char hex[] = "0123456789abcdef";
    char uuid[33];
    const char* text = "a volume that has not said its name";

    if (g_volume_uuid_known) {
        for (int i = 0; i < 16; i++) {
            uuid[i * 2]     = hex[(g_volume_uuid[i] >> 4) & 0xF];
            uuid[i * 2 + 1] = hex[g_volume_uuid[i] & 0xF];
        }
        uuid[32] = '\0';
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
            text, g_tagfs_seat, BoardroomSeatName(g_tagfs_seat),
            (unsigned)why);
}

error_t tagfs_flush_cache(void)
{
    /* The one place a flush ticket is taken and answered (scribe.h): taken
     * before the medium is asked, so the flush it stands for was issued after
     * every write whose completion was seen before it. */
    uint64_t ticket = VolumeFlushBegin();
    int said = BoardroomFlush(g_tagfs_seat);
    VolumeFlushEnd(ticket, said == 0);
    if (said == 0) {
        return OK;
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
    if (said < 0 && !BoardroomSeatOccupied(g_tagfs_seat)) {
        why = ERR_DEVICE_NOT_READY;
    }

    volume_would_not_flush(why);
    return why;
}

/*
 * Has the medium the volume lives on left the machine?
 *
 * A removable disk is removable while the kernel is using it, and once it has
 * gone the volume on it has gone with it. Saying so once, here, is what makes
 * everything above deterministic — without it, whether a file could still be
 * read depended on whether its blocks happened to be in a four-entry
 * read-ahead cache, and a program image half of which is real and half of
 * which is missing is one the loader will happily run. Measured: it did.
 *
 * The cached bytes are not wrong. They are simply not the medium any more, and
 * a filesystem that cannot tell the difference is one that answers questions
 * about a disk that is in somebody's pocket.
 */
static bool g_medium_left = false;
static void ReadAheadForget(void);

static bool volume_medium_gone(void)
{
    if (g_medium_left) {
        return true;
    }
    if (g_tagfs_seat == BOARDROOM_NO_SEAT) {
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
    if (BoardroomSeatOccupied(g_tagfs_seat) &&
        BoardroomSeatSeating(g_tagfs_seat) == g_tagfs_seating) {
        return false;
    }

    bool swapped = BoardroomSeatOccupied(g_tagfs_seat);

    g_medium_left = true;
    ReadAheadForget();
    kprintf("[TagFS] the medium the volume lives on has %s — every read and "
            "write from here on will say so\n",
            swapped ? "been replaced by another one in the same seat"
                    : "left");
    return true;
}

/*
 * ‼ AND ONLY WHEN THERE IS SOMETHING TO ASK FOR.
 *
 * "A medium is seated" is the wrong question: a machine whose disk carries no
 * BoxOS volume at all has one seated too, and asking it eight times says
 * nothing except eight surveys and nine lines of log on every boot without a
 * filesystem.
 *
 * The right one is the seat itself. TagFSProbeDrive leaves g_tagfs_seat set
 * only when a medium in the room answered with THIS machine's volume; a mount
 * that fails past that point failed on a volume that is there, and that is the
 * only failure worth asking about again. A mount that fails because the medium
 * has gone leaves no seat, and its return will arrive on its own.
 */
static void mount_owed_if_the_volume_is_there(void)
{
    if (g_tagfs_seat != BOARDROOM_NO_SEAT) {
        mount_owed();
    }
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


/*
 * And what happens when it comes back.
 *
 * The latch above is one-way on purpose: while the medium is out of the
 * machine there is nothing that could make it safe to answer questions about
 * it. But "out of the machine" is a state that ends — a stick reseated, a
 * controller that stopped itself and was brought back — and until now it did
 * not: the volume was gone for the rest of the boot, and on a machine that
 * boots from a flash drive that is the machine gone with it.
 *
 * Coming back is not the same as something being there. Two things have to
 * hold, and both are read off the medium rather than assumed:
 *
 *   it is the SAME VOLUME — the identity in its superblock, not the seat it
 *   turned up in, because a socket that now has something in it is not
 *   evidence about what;
 *
 *   and NOTHING WROTE TO IT while it was away. A stick pulled, edited on
 *   another machine and pushed back carries a filesystem whose blocks no
 *   longer mean what this kernel's in-memory index says they mean, and
 *   carrying on over that is not a recovered volume, it is a corrupted one.
 *   The superblock still matching the copy held here is what says so.
 *
 * Returns true when the volume was taken back up.
 */
static bool tagfs_abandon(void);

/* Everything a mount brought up, put back down — including a mount that only
 * got halfway. See the note on the definition; it is the piece whose absence
 * made one failed mount permanent. */
static void tagfs_clear_the_ground(void);

static bool TagFSVolumeReturned(void)
{
    if (!g_medium_left || !g_volume_uuid_known) {
        return false;
    }

    for (uint8_t seat = 0; seat < BoardroomSeatCount(); seat++) {
        uint8_t uuid[16];
        if (!BoardroomSeatOccupied(seat)) {
            continue;
        }

        /* Reached through THIS seat, not through the one the volume used to be
         * in: that one may have nothing in it, and the read has to go where
         * the medium actually is. */
        uint8_t old_seat = g_tagfs_seat;
        g_tagfs_seat = seat;
        bool ours = tagfs_recognise(NULL, seat, uuid) &&
                    memcmp(uuid, g_volume_uuid, 16) == 0;
        g_tagfs_seat = old_seat;

        if (!ours) {
            continue;                   /* empty, or a volume that is not ours */
        }

        /*
         * ‼ WHICH OCCUPANT OF THAT SEAT THIS IS, REMEMBERED BEFORE ANYTHING IS
         * TAKEN DOWN.
         *
         * Recognising the volume took reads, and a hand can pull the medium
         * out again while they happen. Everything below either throws away a
         * mount or reads a medium, and doing either on behalf of something
         * that has already left is how a departure gets reported as a
         * filesystem that would not mount. Measured on the board: three of
         * fifty-five, every one of them a second pull mid-read.
         */
        uint32_t seating = BoardroomSeatSeating(seat);

        /*
         * Read again rather than carried on from.
         *
         * Everything held in memory about this volume was read before the
         * medium left, and while it was out of the machine anything could have
         * been done to it — including nothing, which is indistinguishable from
         * here. So the in-memory picture is dropped WITHOUT being written back
         * and the volume is mounted from the medium as it now is. Anything
         * holding a file across the departure has lost it, which is what
         * happened.
         */
        kprintf("[TagFS] the volume is back, in seat %u: %s — reading it "
                "again, because everything held about it is from before it "
                "left\n", seat, BoardroomSeatName(seat));

        if (!medium_still_seated(seat, seating)) {
            kprintf("[TagFS] the medium carrying this volume left again before "
                    "it could be read — nothing has been taken down, and its "
                    "next arrival is met the same way\n");
            return false;
        }

        if (!tagfs_abandon()) {
            /* Somebody is still inside the volume that left. Nothing of it is
             * let go while they are, and nothing is mounted on top of it: the
             * service pass finishes both, in that order. */
            return false;
        }

        if (tagfs_init() == OK) {
            /* Said on every road to a mounted volume: a context this machine
             * holds is bound to the numbers of the volume that is up now and
             * told to it; a machine holding none takes up what the volume
             * remembers. This road did not say it once, while both of the
             * others did; the asymmetry was the whole of the risk. */
            UseContextRecall();
            mount_settled();
            return true;
        }

        /*
         * Two different facts, and they used to print the same sentence.
         *
         * A medium that left again while it was being read is not a volume
         * that would not mount: nothing is wrong with it, and its return will
         * be announced like any other. A medium that is STILL THERE and would
         * not give up its volume is the other one, and nothing will ask again
         * unless this does.
         */
        if (!medium_still_seated(seat, seating)) {
            kprintf("[TagFS] the medium left again while its volume was being "
                    "read — nothing is mounted, and its next arrival is met "
                    "the same way\n");
            return false;
        }

        kprintf("[TagFS] the volume came back and would not mount\n");
        mount_owed_if_the_volume_is_there();
        return false;
    }

    return false;
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
/*
 * Until the machine has tried to mount its volume once, an arriving medium is
 * not news — it is part of the same boot, and the mount that is about to
 * happen will see it. Without this gate a K-Core running the guide loop and
 * the boot core both call tagfs_init, and the volume is mounted twice.
 * Measured: two "volume on seat 0" lines, one boot.
 */
static volatile uint32_t g_boot_mount_settled = 0;

void TagFSBootMountSettled(void)
{
    __atomic_store_n(&g_boot_mount_settled, 1u, __ATOMIC_RELEASE);
}

/*
 * The medium under this volume may have just left. Ask, and latch if it has.
 *
 * volume_medium_gone() already knows how to decide and how to say so; what was
 * missing was anybody asking at the moment it became true rather than at the
 * next read, which on a machine where nothing reads for a second is a second
 * during which the seat can come to hold a different medium entirely.
 */
void TagFSNoteMediumGone(void)
{
    if (!g_state.initialized) {
        return;
    }
    if (volume_medium_gone()) {
        TagFSHoldGroundTake();      /* nothing at all unless HOLDGROUND=on */
    }
}

/*
 * Listening for the medium, instead of being told about it.
 *
 * The Boardroom used to call in here by name, which meant a room full of disks
 * had to know that somebody keeps a filesystem on one of them. Now it says
 * seat:taken and seat:emptied and this listens — so the dependency points the
 * way round it should, and anything else that cares about media arriving can
 * care without either side being changed.
 *
 * These callbacks do real disk I/O, which the TouchWatch contract asks you not
 * to do lightly. It is safe here because of where the publisher stands: the
 * room is called to order from the guide loop and the idle loop, ordinary
 * kernel context with interrupts on, holding nothing but its own one-at-a-time
 * flag. That is exactly where mounting a volume is allowed to happen, and is
 * where this work was already being done before it was a callback.
 */
static TouchWatch *g_seat_taken_watch   = NULL;
static TouchWatch *g_seat_emptied_watch = NULL;

static void tagfs_on_seat_taken(TouchTag tag, const void *payload,
                                uint32_t plen, uint32_t source_pid, void *ctx)
{
    (void)tag; (void)payload; (void)plen; (void)source_pid; (void)ctx;
    TagFSAttendArrival();
}

static void tagfs_on_seat_emptied(TouchTag tag, const void *payload,
                                  uint32_t plen, uint32_t source_pid, void *ctx)
{
    (void)tag; (void)payload; (void)plen; (void)source_pid; (void)ctx;
    TagFSNoteMediumGone();
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

    if (!g_seat_taken_watch || !g_seat_emptied_watch) {
        kprintf("[TagFS] could not listen for media arriving — a volume that "
                "turns up later will not be noticed\n");
    }
}

/* The body. Whoever calls it holds the one-at-a-time flag above. */
static void attend_arrival(void)
{
    if (g_state.initialized) {
        TagFSVolumeReturned();
        return;
    }

    /* Never mounted. tagfs_init only marks itself done at the very end, so a
     * first attempt that found nothing left everything exactly as it was. */
    if (tagfs_init() != OK) {
        mount_owed_if_the_volume_is_there();
        return;
    }

    {
        /* The boot path does this after its own mount and this path did not,
         * so a volume that arrived late came up with the Use Context still
         * holding no numbers for its tags. The person's word, if any, stands
         * over what the arriving volume remembers. */
        UseContextRecall();
        kprintf("[TagFS] a medium arrived carrying a volume, and this machine "
                "had none — mounted from seat %u\n", g_tagfs_seat);
        TouchPublish("volume:mounted", &g_tagfs_seat, sizeof(g_tagfs_seat));
        mount_settled();
    }
}

void TagFSAttendArrival(void)
{
    if (__atomic_load_n(&g_boot_mount_settled, __ATOMIC_ACQUIRE) == 0) {
        return;
    }
    if (!attending_take()) {
        return;                 /* somebody is already at it */
    }
    attend_arrival();
    attending_done();
}

static uint64_t block_to_vlba(uint32_t block);
static int read_block(uint32_t block, void *buffer);
static int write_block(uint32_t block, const void *buffer);

// ----------------------------------------------------------------------------
// Read-Ahead Cache for Sequential Reads
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t block;
    uint8_t  data[TAGFS_BLOCK_SIZE];
    bool     valid;
} ReadAheadEntry;

static ReadAheadEntry g_read_ahead_cache[TAGFS_READ_AHEAD_BLOCKS];
static uint32_t g_read_ahead_head = 0;
static uint32_t g_read_ahead_last_block = 0;
static TagFSLock g_read_ahead_lock = TAGFS_LOCK_INIT("readahead", LOCK_RANK_LEAF);

static void ReadAheadInit(void) {
    memset(g_read_ahead_cache, 0, sizeof(g_read_ahead_cache));
    g_read_ahead_head = 0;
    g_read_ahead_last_block = 0;
    TagFSLockInit(&g_read_ahead_lock, "readahead", LOCK_RANK_LEAF);
}

static int ReadAheadLookup(uint32_t block, void *buffer) {
    TagFSLockTake(&g_read_ahead_lock);
    for (uint32_t i = 0; i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        if (g_read_ahead_cache[i].valid && g_read_ahead_cache[i].block == block) {
            memcpy(buffer, g_read_ahead_cache[i].data, TAGFS_BLOCK_SIZE);
            TagFSLockDrop(&g_read_ahead_lock);
            return 0;
        }
    }
    TagFSLockDrop(&g_read_ahead_lock);
    return -1;
}

/* Is this block already in hand? Asked without copying it: the reader wants to
 * know whether to go to the medium at all, not what is in the block. */
static bool ReadAheadHas(uint32_t block) {
    bool here = false;
    TagFSLockTake(&g_read_ahead_lock);
    for (uint32_t i = 0; i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        if (g_read_ahead_cache[i].valid && g_read_ahead_cache[i].block == block) {
            here = true;
            break;
        }
    }
    TagFSLockDrop(&g_read_ahead_lock);
    return here;
}

static void ReadAheadPrefetch(uint32_t start_block, uint32_t count) {
    if (count == 0)
        return;

    // Phase 1: determine which blocks to fetch (under lock, no I/O)
    uint32_t to_fetch[TAGFS_READ_AHEAD_BLOCKS];
    uint32_t slots[TAGFS_READ_AHEAD_BLOCKS];
    uint32_t n_fetch = 0;

    TagFSLockTake(&g_read_ahead_lock);
    for (uint32_t i = 0; i < count && i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        uint32_t block = start_block + i;
        if (block == g_read_ahead_last_block)
            continue;
        // Check cache — skip if already present
        bool already = false;
        for (uint32_t j = 0; j < TAGFS_READ_AHEAD_BLOCKS; j++) {
            if (g_read_ahead_cache[j].valid && g_read_ahead_cache[j].block == block) {
                already = true;
                break;
            }
        }
        if (already)
            continue;
        uint32_t slot = g_read_ahead_head % TAGFS_READ_AHEAD_BLOCKS;
        // Invalidate the slot now so it won't be served stale during I/O
        g_read_ahead_cache[slot].valid = false;
        to_fetch[n_fetch] = block;
        slots[n_fetch]    = slot;
        n_fetch++;
        g_read_ahead_head++;
    }
    g_read_ahead_last_block = start_block + count - 1;
    TagFSLockDrop(&g_read_ahead_lock);

    /*
     * Phase 2: the disk I/O, without holding the lock — and blocks that lie
     * NEXT TO EACH OTHER on the medium are fetched together.
     *
     * ‼ THIS LOOP ALREADY KNEW THEY WERE NEIGHBOURS AND ASKED FOR THEM ONE AT
     * A TIME ANYWAY. Four consecutive blocks were four commands: on a SATA
     * disk four round trips where one would do, and on the flash drive this
     * kernel boots from on a real board, four Bulk-Only commands — twelve USB
     * transfers, three per command — to move sixteen kilobytes that the device
     * would have handed over in one.
     *
     * How long a run may be is the MEDIUM'S answer, not a number chosen here.
     * The legacy channel says eight sectors because past that it gives up its
     * DMA path and moves the bytes with the processor, so on it this coalesces
     * nothing and that is the right answer for it; a flash drive says its
     * whole bounce buffer, and a SATA port says more still.
     */
    uint32_t run_blocks = BoardroomSeatRun(tagfs_get_seat()) / 8u;
    if (run_blocks == 0) {
        run_blocks = 1;
    }
    if (run_blocks > TAGFS_READ_AHEAD_BLOCKS) {
        run_blocks = TAGFS_READ_AHEAD_BLOCKS;
    }

    uint32_t i = 0;
    while (i < n_fetch) {
        /* How many of the blocks still to fetch are consecutive from here.
         * to_fetch[] is filled in increasing block order, so this is a walk
         * rather than a search. */
        uint32_t len = 1;
        while (i + len < n_fetch &&
               to_fetch[i + len] == to_fetch[i] + len &&
               len < run_blocks) {
            len++;
        }

        uint8_t *run = NULL;
        if (len > 1) {
            run = (uint8_t *)kmalloc(len * TAGFS_BLOCK_SIZE);
            if (!run) {
                len = 1;            /* no room to batch: one at a time still works */
            }
        }

        if (len == 1) {
            uint8_t tmp[TAGFS_BLOCK_SIZE];
            if (tagfs_volume_read(block_to_vlba(to_fetch[i]), 8, tmp) == 0) {
                TagFSLockTake(&g_read_ahead_lock);
                memcpy(g_read_ahead_cache[slots[i]].data, tmp, TAGFS_BLOCK_SIZE);
                g_read_ahead_cache[slots[i]].block = to_fetch[i];
                g_read_ahead_cache[slots[i]].valid = true;
                TagFSLockDrop(&g_read_ahead_lock);
            }
            i++;
            continue;
        }

        if (tagfs_volume_read(block_to_vlba(to_fetch[i]), len * TAGFS_BLOCK_SECTORS, run) == 0) {
            TagFSLockTake(&g_read_ahead_lock);
            for (uint32_t k = 0; k < len; k++) {
                memcpy(g_read_ahead_cache[slots[i + k]].data,
                       run + k * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE);
                g_read_ahead_cache[slots[i + k]].block = to_fetch[i + k];
                g_read_ahead_cache[slots[i + k]].valid = true;
            }
            TagFSLockDrop(&g_read_ahead_lock);
        }
        kfree(run);
        i += len;
    }
}

// Drop any cached copy of `block` so a read after a write to it never serves
// pre-write data (read-after-write consistency). Called on every block write,
// sync and async.
static void ReadAheadInvalidate(uint32_t block) {
    TagFSLockTake(&g_read_ahead_lock);
    for (uint32_t i = 0; i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        if (g_read_ahead_cache[i].valid && g_read_ahead_cache[i].block == block)
            g_read_ahead_cache[i].valid = false;
    }
    TagFSLockDrop(&g_read_ahead_lock);
}

void tagfs_readahead_invalidate(uint32_t block) {
    ReadAheadInvalidate(block);
}

/* Every entry, for when what they are copies of is no longer there. */
static void ReadAheadForget(void) {
    TagFSLockTake(&g_read_ahead_lock);
    for (uint32_t i = 0; i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        g_read_ahead_cache[i].valid = false;
    }
    TagFSLockDrop(&g_read_ahead_lock);
}

/*
 * A block the Deed names → the block TagFS allocates in.
 *
 * The Deed states where the registry, the file table and the metadata pool are
 * in blocks of the whole volume, because that is what makes a volume readable
 * by something that has not mounted it — a loader, a repair tool, another
 * kernel. TagFS itself counts from the start of the data run, because that is
 * what every file's extents on disk are counted in. This is the seam.
 */
static uint32_t data_block_of(uint32_t volume_block)
{
    return volume_block - g_state.layout.data_block;
}

/*
 * A block of the DATA RUN → a sector inside the volume.
 *
 * The two coordinate systems meet here and nowhere else. What a file's
 * metadata carries, and what the bitmap indexes, is a block of the data run
 * counted from zero; where the data run sits inside the volume is
 * `layout.data_block`, out of the Deed.
 */
static uint64_t block_to_vlba(uint32_t block)
{
    return ((uint64_t)g_state.layout.data_block + block) * TAGFS_BLOCK_SECTORS;
}

/*
 * And the same block as a sector of the MEDIUM.
 *
 * For the paths that hand an LBA straight to the Boardroom instead of going
 * through this file's door — the asynchronous storage ops, and Braid, which
 * addresses other media that have no ground of their own.
 */
uint64_t tagfs_block_to_sector(uint32_t block)
{
    return g_volume_base + block_to_vlba(block);
}

static int read_block(uint32_t block, void *buffer)
{
    if (volume_medium_gone()) {
        return -1;
    }

    /* A block the read-ahead cache holds came off the medium in a prefetch's
     * run, which checks nothing, so it is verified here, where it is handed
     * out, exactly as a block read on its own is. Checked only on a miss,
     * verify-on-read would cover one block of every run a file is read in —
     * and a file's blocks are read in runs once its extents merge (CW-D8). */
    if (ReadAheadLookup(block, buffer) == 0) {
        (void)IntegrityVerify(block, buffer);
        return 0;
    }

    // Route through Braid when it has active disks (provides redundancy/checksumming).
    // Pass the physical LBA so Braid operates in sector-space, not TagFS block-space.
    if (BraidIsHealthy()) {
        error_t braid_result = BraidReadBlock(tagfs_block_to_sector(block), buffer, NULL);
        if (braid_result == OK)
            return 0;
        // Braid failed — fall through to direct disk read as recovery path
    }

    int rc = tagfs_volume_read(block_to_vlba(block), 8, buffer);
    if (rc == 0)
        (void)IntegrityVerify(block, buffer);   // detect silent bit-rot on read
    return rc;
}

static int write_block(uint32_t block, const void *buffer)
{
    if (volume_medium_gone()) {
        return -1;
    }

    int rc = -1;

    // Route through Braid when it has active disks (provides redundancy).
    // Pass the physical LBA so Braid operates in sector-space, not TagFS block-space.
    if (BraidIsHealthy()) {
        if (BraidWriteBlock(tagfs_block_to_sector(block), buffer, NULL) == OK)
            rc = 0;
        // Braid failed — fall through to direct disk write as recovery path
    }
    if (rc != 0)
        rc = tagfs_volume_write(block_to_vlba(block), 8, (void *)buffer);

    // Drop any stale read-ahead copy + record the new integrity digest.
    if (rc == 0) {
        ReadAheadInvalidate(block);
        IntegrityUpdate(block, buffer);
    }
    return rc;
}

error_t tagfs_read_block(uint32_t block, void *buffer) {
    if (!buffer)
        return ERR_NULL_POINTER;
    
    error_t err = read_block(block, buffer);
    if (err != OK)
        return ERR_READ_FAILED;
    
    return OK;
}

error_t tagfs_write_block(uint32_t block, const void *buffer) {
    if (!buffer)
        return ERR_NULL_POINTER;

    error_t err = write_block(block, buffer);
    if (err != OK)
        return ERR_WRITE_FAILED;

    return OK;
}

/* What the Ledger SAID when it was read, kept so the derived count the mount
 * takes from the block map can tell "the medium carries a wrong number" from
 * "something already put it right in memory this mount". The mount's roll call
 * corrects the count in RAM and does not write it, so without this the same
 * wrong number came back off the medium on every boot for ever. */
static uint64_t g_ledger_free_as_read;

// ----------------------------------------------------------------------------
// tagfs_init_well_known_tags
// ----------------------------------------------------------------------------

static uint16_t register_well_known(TagRegistry *reg, const char *key)
{
    TagSpelling tag = TagSpellingOfKey(key);
    uint16_t    tid;
    return TagRegistryIntern(reg, &tag, &tid) == OK ? tid : TAGFS_INVALID_TAG_ID;
}

// The reserved-key vocabulary. A tag is "system" iff it is the bare (value=NULL)
// registry entry for one of these keys; a value-bearing tag like "system:foo" is
// a distinct entry and stays a user tag. Mirrors the format-time intern order
// (deterministic ids 0-11) and is re-stamped at every mount below. Built from
// the same tagfs_reserved.h source as the format-seed, so the list cannot drift.
static const char *const TagFsReservedKeys[] = {
#define X(k) k,
    TAGFS_RESERVED_KEYS(X)
#undef X
};
_Static_assert(sizeof(TagFsReservedKeys) / sizeof(TagFsReservedKeys[0]) == TAGFS_RESERVED_COUNT,
               "reserved-key drift");

/* The auth-privilege keys in TAGFS_AUTH_KEYS order. The fixed auth bit
 * AUTH_TAG_X (auth_tags.h) is 1u<<position here; cabin sync resolves it from the
 * key string. tagfs_init_well_known_tags asserts these are the exact prefix of
 * TagFsReservedKeys[] so the two vocabularies cannot silently diverge. */
static const char *const AuthReservedKeys[] = {
#define X(id, key) key,
    TAGFS_AUTH_KEYS(X)
#undef X
};
_Static_assert(sizeof(AuthReservedKeys) / sizeof(AuthReservedKeys[0]) == TAGFS_AUTH_COUNT,
               "auth-key drift");

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

void tagfs_init_well_known_tags(void)
{
    g_well_known.trashed = TAGFS_INVALID_TAG_ID;
    g_well_known.hidden  = TAGFS_INVALID_TAG_ID;
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry)
        return;
    TagRegistry *reg = fs->registry;

    /* trashed/hidden are membership filters for file listing (not security),
     * kept as the registry's ids. The 7 auth-privilege tags are not registered
     * here — their op-authority is the FIXED cabin_t.auth_bits (auth_tags.h),
     * independent of the registry id. */
    g_wk.trashed = register_well_known(reg, "trashed");
    g_wk.hidden  = register_well_known(reg, "hidden");

    // Stamp the reserved vocabulary as system tags. Runs after tag_registry_load,
    // so it self-heals over whatever flags the disk carried; mark_system sets no
    // dirty flag, so this re-derivation never forces a write.
    for (size_t i = 0; i < sizeof(TagFsReservedKeys) / sizeof(TagFsReservedKeys[0]); i++) {
        TagSpelling reserved = TagSpellingOfKey(TagFsReservedKeys[i]);
        uint16_t    tid      = TagRegistryLookup(reg, &reserved);
        if (tid != TAGFS_INVALID_TAG_ID)
            tag_registry_mark_system(reg, tid);
    }

    /* Vocabulary-drift guard: the fixed auth bit AUTH_TAG_X (auth_tags.h) is
     * 1u<<position in TAGFS_AUTH_KEYS, while cabin sync resolves that bit from
     * the key string. This is only coherent if the auth keys are exactly the
     * prefix of the reserved vocabulary. A mismatch means one list was
     * reordered without the other — fail closed rather than mis-grant. */
    for (size_t i = 0; i < TAGFS_AUTH_COUNT; i++) {
        if (strcmp(AuthReservedKeys[i], TagFsReservedKeys[i]) != 0)
            panic("auth/reserved vocab drift at index %u: auth='%s' reserved='%s'",
                  (unsigned)i, AuthReservedKeys[i], TagFsReservedKeys[i]);
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
static error_t mount_refused(const char *what, error_t why)
{
    kprintf("[TagFS] this volume is NOT mounted: %s (error %d)\n",
            what, (int)why);
    tagfs_clear_the_ground();
    return why;
}

/*
 * One live file the table names, held against the pool.
 *
 * ‼ WHERE THE POOL SAYS THE NEWEST RECORD IS, WINS. The pool is only appended
 * to, and the mount's walk read it in order, so the record the map holds for a
 * file is the newest of its records that ever reached the medium. A table row
 * naming somewhere else is a row whose block no cycle wrote again since —
 * rows are written when files come and go, not every time a record moves —
 * and it is corrected to the pool, in memory now and on the medium by the
 * next cycle.
 *
 * A file the pool holds no record of has nothing readable anywhere: it leaves
 * the table and the map of live files, said. The rest have their extents
 * marked in the roll call's map, when there is one, and every tag they wear
 * put into the tag index.
 */
static void mount_check_file(uint32_t fid, uint8_t *computed_bm, uint32_t *checked,
                             uint32_t *corrected, uint32_t *lost)
{
    uint32_t mb, mo;
    if (file_table_lookup(fid, &mb, &mo) != 0)
        return;

    RecordWhere where;
    FileRecord *rec = FileMapRecordHold(g_state.files, fid, &where);
    if (!rec)
    {
        /* Said with kprintf, so it survives into a shipped build: this line
         * was the silence that turned a lost file into a mystery. */
        kprintf("[TagFS FSCK] file %u is named at block %u+%u and this volume's pool "
                "holds no record of it that reads — the file is gone and its name "
                "goes with it\n", fid, mb, mo);
        (*lost)++;
        file_table_delete(fid);
        FileMapDrop(g_state.files, fid);
        return;
    }

    uint32_t pb = 0;
    if (PoolLinkBlock(where.link, &pb) && (pb != mb || where.offset != mo))
    {
        file_table_update(fid, pb, where.offset);
        (*corrected)++;
    }

    for (uint32_t e = 0; computed_bm && e < rec->extent_count; e++)
    {
        uint32_t start = rec->extents[e].start_block;
        uint32_t count = rec->extents[e].block_count;
        for (uint32_t b = start; b < start + count && b < g_state.layout.data_blocks; b++)
            bitmap_set_bit(computed_bm, b);
    }
    (*checked)++;

    for (uint32_t t = 0; t < rec->tag_count; t++)
        tag_bitmap_set(g_state.bitmap_index, rec->tags[t], fid);

    FileRecordRelease(rec);
}

/* Every live file, a batch of ids at a time out of the map of them: the walk
 * costs the files the volume holds, not every id it ever issued, and no lock
 * is held while a file is checked — which may take it out of the map. A row
 * at or past the Ledger's next_file_id names an id the volume never issued —
 * a table written ahead of its Ledger — and is left as it lies, unchecked and
 * in no index, as it always was (nextfid, step 1.6). */
static void mount_walk_files(uint8_t *computed_bm, uint32_t *checked,
                             uint32_t *corrected, uint32_t *lost)
{
    uint32_t batch[64];
    uint32_t from = 1;
    uint32_t n;
    while ((n = FileMapRows(g_state.files, from, batch, ARRAY_SIZE(batch))) > 0)
    {
        for (uint32_t i = 0; i < n; i++)
        {
            if (batch[i] >= g_state.ledger.next_file_id)
                return;
            mount_check_file(batch[i], computed_bm, checked, corrected, lost);
        }
        if (batch[n - 1] == 0xFFFFFFFFu)
            break;
        from = batch[n - 1] + 1;
    }
}

error_t tagfs_init(void) {
    if (g_state.initialized) {
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
     * dies at TagFS_CowInit exactly as the board did.
     */

    /* Detect which ATA drive (master/slave) holds the TagFS volume.
     * Must run before any disk I/O so g_tagfs_drive is correct. */
    TagFSProbeDrive();

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
    if (g_tagfs_seat == BOARDROOM_NO_SEAT) {
        return ERR_FILE_NOT_FOUND;
    }

    /* The heap as the mount finds it; what it keeps is said once it stands. */
    size_t heap_before_mount = KHeapLiveBytes();

    /*
     * ‼ THREE LINES USED TO STAND HERE, AND THEY BELONGED TO THE MACHINE.
     *
     *     spinlock_init(&g_state.lock);
     *     spinlock_init(&g_open_table_lock);
     *     memset(g_open_files, 0, sizeof(g_open_files));
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

    // Initialize read-ahead cache FIRST — read_block uses it before the rest of init
    ReadAheadInit();

    /* --- Stand on the ground, and read the deed that describes it --- */
    error_t ground_rc = volume_take_ground(g_tagfs_seat);
    if (ground_rc != OK) {
        return ground_rc;
    }

    /* --- And the Ledger beside it: what is true of the volume today --- */
    error_t ledger_rc = ledger_load();
    if (ledger_rc != OK) {
        return ledger_rc;
    }
    g_ledger_free_as_read = g_state.ledger.free_blocks;

    /* --- The DiskBook: the redirect log an earlier kernel kept for its
     *     snapshots, only ever read now. Its redirects are replayed into the
     *     snapshots the manifest names, once copy-on-write has read that.
     *
     *     Its head, the copy of its head and its records are three separate
     *     runs of the volume, in the volume's own sectors — the DiskBook is
     *     handed where they are rather than deriving them from one number,
     *     because "the backup is the next sector along" is what put a record
     *     and its only copy inside one physical block. */
    if (DiskBookInit(
            (uint64_t)g_state.layout.disk_book_block * TAGFS_BLOCK_SECTORS,
            (uint64_t)(g_state.layout.disk_book_block + 1) * TAGFS_BLOCK_SECTORS,
            (uint64_t)(g_state.layout.disk_book_block + 2) * TAGFS_BLOCK_SECTORS) != OK)
    {
        debug_printf("[TagFS] Warning: DiskBookInit failed\n");
    }

    // --- CoW Snapshots init ---
    if (TagFS_CowInit() != OK) {
        return mount_refused("its copy-on-write layer would not start",
                             ERR_COW_NOT_INITIALIZED);
    }

    /* --- The snapshots an earlier kernel left: read, held, named. A
     *     format-v1 volume takes no new one (TagFS_SnapshotRefused). */
    CowNameStanding(g_state.ledger.cow_manifest_block);

    // --- Test Framework init ---
    if (TagFS_TestsInit() != OK) {
        return mount_refused("its test framework would not start",
                             ERR_NO_MEMORY);
    }

    /*
     * ‼ THE FAILURE THAT COST A BOARD ITS FILESYSTEM, ON COMMAND.
     *
     * Every refusal from here down leaves three subsystems standing — DiskBook,
     * CoW and the test framework — and until
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
            return mount_refused("MOUNTFAIL=on asked this one to fail",
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
     * medium left exactly where it is — and the only way back from there is
     * the machine asking again by itself.
     *
     * Not the first mount: the stamp counts the ones that finished, so this
     * trips on the first mount after a volume has already been up.
     */
#if CONFIG_TAGFS_RETURN_FAIL_ONCE
    {
        static bool tripped = false;
        if (!tripped && tagfs_mount_epoch() >= 1) {
            tripped = true;
            return mount_refused("RETURNFAIL=on asked this re-mount to fail",
                                 ERR_TAGFS_CORRUPTED);
        }
    }
#endif

    // --- Tag Registry ---
    g_state.registry = kmalloc(sizeof(TagRegistry));
    if (!g_state.registry) {
        return mount_refused("there is no memory for its tag registry",
                             ERR_NO_MEMORY);
    }
    if (tag_registry_init(g_state.registry) != OK) {
        kfree(g_state.registry);
        g_state.registry = NULL;
        return mount_refused("its tag registry would not be built",
                             ERR_TAGFS_REGISTRY_FULL);
    }
    /* Same rule as the bitmap below: a registry that would not read is not an
     * empty registry. Every tag id on this volume is defined there, so a mount
     * that continues without it renames nothing and mislabels everything. */
    if (tag_registry_load(g_state.registry, data_block_of(g_state.layout.tag_registry_block)) != OK) {
        return mount_refused("its tag registry would not read", ERR_TAGFS_CORRUPTED);
    }

    /*
     * ‼ ONLY WHERE THEY START IS TAKEN FROM THE DEED. HOW LONG THEY ARE IS NOT.
     *
     * Both of these were handed layout.<thing>_blocks as well, and both of
     * those numbers are written once by mkfs and raised by nothing — while the
     * chains they describe are lengthened by ordinary use. The pool walked as
     * far as the old number allowed and stopped inside itself, and the first
     * write after that overwrote the link to the rest; the file table read its
     * later blocks off the medium and threw them away above entry 510.
     *
     * Both now walk their own chain to its end, the same ruling the free-block
     * count is already under ("THE FREE COUNT IS DERIVED, NOT REMEMBERED",
     * below). The fields stay in the Deed's layout stamp so an older reader
     * still finds everything after them, and nothing in this kernel reads
     * either one to decide anything.
     */
    // --- The map of live files: the table's rows and the pool's records ---
    g_state.files = FileMapCreate();
    if (!g_state.files) {
        return mount_refused("there is no memory for its map of live files",
                             ERR_NO_MEMORY);
    }

    // --- File Table ---
    if (file_table_init(data_block_of(g_state.layout.file_table_block)) != OK) {
        return mount_refused("its file table would not read",
                             ERR_FILE_TABLE_CORRUPT);
    }

    // --- Metadata Pool ---
    if (meta_pool_init(data_block_of(g_state.layout.metadata_pool_block)) != OK) {
        return mount_refused("its metadata pool would not read",
                             ERR_METADATA_POOL_FULL);
    }

    // --- Bitmap Index ---
    g_state.bitmap_index = tag_bitmap_create(TAGFS_BITMAP_INITIAL_TAG_CAP);
    if (!g_state.bitmap_index) {
        return mount_refused("there is no memory for its tag index",
                             ERR_NO_MEMORY);
    }

    // --- Block Map ---
    /* ‼ ONLY THE PART OF THE BITMAP THAT COVERS THE DATA RUN IS READ.
     *
     * The region a volume declares for its bitmap is laid down for the ground
     * it may one day take, not for the ground it has (create_tagfs: the growth
     * ceiling). On a 90 MiB volume that is two megabytes of region carrying
     * three kilobytes of bits. The bits past the data run are zero by
     * construction and mean nothing until the volume grows into them; when it
     * does, the block map covers them, because it is sized from data_blocks
     * and not from the region. */
    uint32_t bm_region_sectors =
        g_state.layout.block_bitmap_blocks * TAGFS_BLOCK_SECTORS;
    uint32_t bm_sector_count =
        (g_state.layout.data_blocks + TAGFS_SECTOR_SIZE * 8 - 1) / (TAGFS_SECTOR_SIZE * 8);

    /* A bitmap region too small to hold a bit for every data block is a volume
     * that cannot keep track of its own ground. It has never been possible to
     * make one — mkfs sizes the two together — so this is a deed that has been
     * damaged or written by something else, and it is not mounted. */
    if (bm_sector_count > bm_region_sectors) {
        return mount_refused("its block bitmap region is too small to hold a "
                             "bit for every block of its data run",
                             ERR_TAGFS_CORRUPTED);
    }
    if (BlockMapMount(g_state.layout.data_blocks,
                      (uint64_t)g_state.layout.block_bitmap_block * TAGFS_BLOCK_SECTORS) != OK) {
        return mount_refused("there is no memory for its block map", ERR_NO_MEMORY);
    }

    /*
     * ‼ THE POOL IS READ BEFORE THE FSCK, BECAUSE THE FSCK IS WHAT DECIDES.
     *
     * This used to run twenty lines below the fsck, and the order was the
     * whole defect: the fsck answers a file whose metadata will not read by
     * deleting its file table entry — the file is gone, permanently, and until
     * now it was gone SILENTLY, because the only line about it was a
     * debug_printf that compiles to nothing in a shipped build. Meanwhile the
     * mirror walks the pool in APPEND ORDER, so the last record it sees for a
     * file id is the newest one on the medium by construction. It knew where
     * every file's newest metadata actually was — and it learned it after the
     * decision to throw the file away had already been made.
     *
     * So the pool speaks first, and the fsck below corrects the file table
     * from it rather than merely reporting the disagreement. That is the same
     * road the free-block count already takes further down: the number that
     * can be derived wins over the number that was remembered.
     */
    if (meta_pool_mirror_init() != 0) {
        return mount_refused("its files' records could not all be held in memory",
                             ERR_NO_MEMORY);
    }

    /*
     * --- Mount-time roll call: every owner of a block, against the map ---
     *
     * The roll call's own map of the run is dense; the census by windows is
     * step 1.6. When the heap will not give it, that is said, and the walk of
     * the files still runs — it corrects the table and builds the tag index,
     * and a mount that skipped it had files no query could find.
     */
    {
        uint32_t bitmap_bytes_sz = (g_state.layout.data_blocks + 7) / 8;
        uint8_t *computed_bm = kmalloc(bitmap_bytes_sz);
        if (computed_bm)
        {
            memset(computed_bm, 0, bitmap_bytes_sz);

            // Mark reserved blocks: 0 = tag registry, 1 = file table, 2 = metadata pool
            for (uint32_t r = 0; r < 3 && r < g_state.layout.data_blocks; r++)
            {
                bitmap_set_bit(computed_bm, r);
            }
        }
        else
        {
            kprintf("[TagFS FSCK] no memory for the roll call's map of %u blocks "
                    "(%u bytes) — this mount does not hold its block map against "
                    "its files\n", g_state.layout.data_blocks, bitmap_bytes_sz);
        }

        // Scan all files and mark their extents
        uint32_t files_checked = 0;
        uint32_t entries_corrected = 0;
        uint32_t files_lost = 0;

        mount_walk_files(computed_bm, &files_checked, &entries_corrected, &files_lost);

        if (computed_bm)
        {
            /* Walk meta_pool / file_table / registry chains and mark
             * every chain block in computed_bm. Without this, chain
             * blocks (e.g. meta_pool's chain to 566) appear as
             * "orphan" — used in on-disk bitmap, missing in computed
             * — and the old fsck would silently zero them, releasing
             * still-live metadata pages to the allocator. CoW or any
             * subsequent tagfs_alloc_blocks would then claim those
             * pages and overwrite live metadata.
             *
             * Each subsystem's first block is in the superblock and
             * its chain is on disk linked via next_block. Walk it
             * here at fsck time using the same magic-check pattern
             * the per-subsystem init's do.
             *
             * ‼ THE HEAD IS WALKED WHATEVER BLOCK IT IS, AND ONLY A LINK
             * OF ZERO ENDS A CHAIN.
             *
             * This loop used to test `_cb != 0` before it did anything,
             * zero standing for "the end of the chain". Zero is also a
             * block — and the tag registry's head IS data block 0, where
             * mkfs puts it (volume_ledger.h: VOLUME_LEDGER_NO_BLOCK). So
             * the registry's walk never began: block 0 was covered by the
             * three reserved above, and every continuation the registry
             * had chained on (tag_registry_flush takes one the day its
             * records outgrow 4080 bytes) was an orphan on every mount.
             * The repair below is additive, so that cost a line and a
             * count; a roll call that gives orphans back would have freed
             * a live registry block. Measured: logcheck `census` plants a
             * registry chained to a second block and this kernel named it
             * "orphan run at data block 6014" while the host's census
             * found its owner.
             *
             * A chain ends where the block just walked says it ends,
             * next_block == 0 — the rule its loader keeps (tag_registry.c,
             * file_table.c, meta_pool.c). At most data_blocks hops: a chain
             * of distinct blocks is no longer than the run it lives in. */
            #define MARK_CHAIN_BLOCKS(first_block, expected_magic) do {       \
                uint32_t _cb = (first_block);                                  \
                for (uint32_t _hops = 0; _hops < g_state.layout.data_blocks; _hops++) { \
                    if (_cb < g_state.layout.data_blocks)                                 \
                        bitmap_set_bit(computed_bm, _cb);                      \
                    uint8_t _bbuf[TAGFS_BLOCK_SIZE];                           \
                    if (tagfs_read_block(_cb, _bbuf) != OK) break;             \
                    uint32_t _mg, _nx;                                         \
                    memcpy(&_mg, _bbuf + 0, 4);                                \
                    memcpy(&_nx, _bbuf + 4, 4);                                \
                    if (_mg != (expected_magic) || _nx == 0) break;            \
                    _cb = _nx;                                                 \
                }                                                              \
            } while (0)
            MARK_CHAIN_BLOCKS(data_block_of(g_state.layout.tag_registry_block),    TAGFS_REGISTRY_MAGIC);
            MARK_CHAIN_BLOCKS(data_block_of(g_state.layout.file_table_block),      TAGFS_FILETBL_MAGIC);
            MARK_CHAIN_BLOCKS(data_block_of(g_state.layout.metadata_pool_block),   TAGFS_MPOOL_MAGIC);
            #undef MARK_CHAIN_BLOCKS

            // Integrity-map blocks are used on disk but referenced by no file —
            // mark them so they are not treated as orphans or reused.
            IntegrityMarkMapBlocks(computed_bm, g_state.layout.data_blocks);

            /* And the same question put to copy-on-write, which owns what no
             * file extent and no chain magic can reach: the snapshot manifest
             * an earlier kernel named in the Ledger, and the blocks its
             * snapshots keep. See CowRollCall. */
            CowRollCall(computed_bm, g_state.layout.data_blocks);
        }

        /* The medium's map, read once, a block at a time, against the roll
         * call: every window summarised, every orphan and missing run named,
         * and the repair only ever adds (block_map.h). */
        BlockMapCensusCount census;
        error_t censused = BlockMapCensus(computed_bm, &census);
        kfree(computed_bm);
        if (censused == ERR_TAGFS_CORRUPTED) {
            /*
             * ‼ A bitmap that would not read is NOT an empty bitmap.
             *
             * An all-zero bitmap says every block of the volume is free, and
             * the next allocation hands out block 0, which is the tag
             * registry, then the file table, then the metadata pool. Measured
             * on the board: a flash drive whose controller dropped a command
             * mid-boot came up once with everything working, and the boot
             * after that had no files on it at all. Refusing costs the machine
             * its filesystem for that boot; carrying on costs it the
             * filesystem permanently.
             */
            return mount_refused("its block bitmap would not read, and a missing "
                                 "bitmap reads as a volume with nothing on it — "
                                 "the next write would take everything",
                                 ERR_TAGFS_CORRUPTED);
        }
        if (censused != OK) {
            return mount_refused("there is no memory to hold its block map's repair",
                                 censused);
        }
        g_state.ledger.free_blocks = census.free;

        /* Said aloud for the same reason the bitmap mismatch is: a
         * corrected entry and a lost file are both things that happened to
         * the user's files, and a shipped build has to say so. */
        if (entries_corrected || files_lost) {
            kprintf("[TagFS FSCK] %u file table entry(s) corrected from "
                    "this volume's metadata pool, %u file(s) lost\n",
                    entries_corrected, files_lost);
        }

        debug_printf("[TagFS FSCK] Checked %u files, bitmap %s\n",
                     files_checked,
                     (census.orphan == 0 && census.missing == 0) ? "OK" : "repaired");
    }

    /*
     * ‼ THE FREE COUNT IS DERIVED, NOT REMEMBERED.
     *
     * The Ledger carries a count of free blocks, and it used to be believed.
     * It is the one number on a volume that no single write makes true: it is
     * added to and taken from on every allocation, so anything that interrupts
     * a sequence of writes leaves it saying something the bitmap does not.
     *
     * MEASURED 2026-09-10: a volume that grew, had its head deed rolled back
     * to the pre-growth one — the state a power cut between the two writes of
     * a growth leaves — and then grew again on the next mount, came up
     * claiming 118764 free blocks in a data run of 73594. The bitmap knew
     * better the whole time, and the roll call above has just counted it.
     *
     * So the number is corrected in memory, and the Ledger's one writer — the
     * next Settle cycle — carries it to the medium: a mount writes no Ledger.
     */
    if (g_state.ledger.free_blocks != g_ledger_free_as_read) {
        kprintf("[TagFS] the ledger came off the medium saying %llu blocks were free and "
                "the bitmap says %llu — a block is taken in the bitmap, so that is the "
                "number kept, and the next Settle cycle writes it\n",
                (unsigned long long)g_ledger_free_as_read,
                (unsigned long long)g_state.ledger.free_blocks);
    }

    /* Said HERE and not beside the Ledger, because until the bitmap has been
     * read there is nothing to say: the Ledger's own claim is a number the
     * mount is about to check. */
    kprintf("[TagFS] %u data blocks, %llu free, %llu files\n",
            g_state.layout.data_blocks,
            (unsigned long long)g_state.ledger.free_blocks,
            (unsigned long long)g_state.ledger.total_files);

    // Populate well-known tag bitmasks for O(1) checks
    tagfs_init_well_known_tags();

    /* Nothing is inked, nothing settled and nothing rests: every record the
     * pool holds is on the medium. */
    InkMountReset();
    SettleMountReset();

    g_state.initialized = true;

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
    __atomic_add_fetch(&g_mount_epoch, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_door, VOLUME_UP, __ATOMIC_RELEASE);

    // Data-block integrity map (verify-on-read). After initialized=true so the
    // lazy first-mount allocation can use the block allocator.
    if (IntegrityInit() != OK)
        kprintf("[TagFS] integrity map unavailable - continuing without verify-on-read\n");
    else
        kprintf("[TagFS] data-integrity verify-on-read active\n");

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
            g_tagfs_seat, (unsigned long long)g_state.ledger.total_files);

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
    kprintf("[TagFS] this mount keeps %zu bytes of kernel heap for %u file id(s) "
            "ever issued, %llu of them live\n",
            heap_after_mount > heap_before_mount ? heap_after_mount - heap_before_mount : 0,
            g_state.ledger.next_file_id - 1,
            (unsigned long long)g_state.ledger.total_files);

    /*
     * ‼ AND WHAT THAT MOUNT PROMISES, SAID ONCE, IN PLAIN WORDS.
     *
     * A write's bytes reach the medium as they are written; the record that
     * names them — the file's name, size and extents — is inked in memory and
     * reaches the medium with a Settle cycle, which this step runs when
     * somebody asks and waits: `anchor`, an orderly shutdown. A promise nobody
     * is told is not a promise, and it is said here rather than in a document
     * because the document is not on the screen when the power goes.
     */
    kprintf("[TagFS] what this volume promises: a file's content reaches the "
            "medium as it is written; its name, size and extents, the table, the "
            "bitmap and the ledger are made durable by a Settle cycle — `anchor`, "
            "and an orderly shutdown\n");

    /* The Spares a delete stands on, laid by now so a volume with no free
     * block left can still give a file up (meta_pool.h, SPARE_RESERVE). A
     * volume with no room for them says so, and deletes while its tail lasts. */
    error_t spared = PoolSpareRefill(SPARE_RESERVE);
    if (spared != OK)
        kprintf("[TagFS] the Spares a delete stands on could not all be laid (error %u) — "
                "a full volume deletes only while its pool's tail has room\n",
                (unsigned)spared);

    SettleCutSayArmed();            /* nothing at all unless SETTLECUT=<point> */
    TagFSHoldGroundOpenFile();      /* nothing at all unless HOLDGROUND=on */
    return 0;
}

/*
 * ‼ EVERYTHING THIS VOLUME BROUGHT UP, PUT BACK DOWN — INCLUDING HALF OF IT.
 *
 * This is the piece that did not exist, and its absence is what turned one
 * transient read error into a machine with no filesystem for the rest of its
 * boot. tagfs_init brings up eight things in order and any of them may refuse;
 * when one did, the seven before it were left standing and nothing anywhere took
 * them down — tagfs_teardown begins with `if (!g_state.initialized) return`,
 * and a mount that failed never set that flag.
 *
 * The next attempt then met a CoW layer that was already up, and TagFS_CowInit
 * answers ERR_ALREADY_INITIALIZED to that. tagfs_init treated it as fatal and
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
 * every shutdown below only lets go of memory — the pool's tail and the ground
 * resting in Fallow included, which the next mount reads afresh.
 */
static void tagfs_clear_the_ground(void)
{
    /* The registry is about to go, and with it every number it issued. The
     * Use Context holds some of them for the scheduler; told first, so no
     * dispatch decides on a page of a book that no longer exists. */
    UseContextUnbind();

    /* What this volume remembered goes with it; the next one is read, not
     * assumed. */
    LedgerLetGo();

    if (g_state.registry) {
        tag_registry_destroy(g_state.registry);
        kfree(g_state.registry);
        g_state.registry = NULL;
    }

    if (g_state.bitmap_index) {
        tag_bitmap_destroy(g_state.bitmap_index);
        g_state.bitmap_index = NULL;
    }

    file_table_shutdown();
    meta_pool_shutdown();
    FallowDropAll();
    FileMapDestroy(g_state.files);
    g_state.files = NULL;

    BlockMapUnmount();

    TagFS_CowShutdown();
    BraidShutdown();
    IntegrityShutdown();
    TagFS_TestsShutdown();
    DiskBookShutdown();

    g_state.initialized = false;
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
static bool tagfs_abandon(void)
{
    if (!g_state.initialized) {
        return true;                        /* nothing to let go of */
    }

    /* Shut before counting, and only once: a second pass through here must not
     * announce a departure that is already under way. */
    if (__atomic_exchange_n(&g_door, (uint8_t)VOLUME_LEAVING,
                            __ATOMIC_ACQ_REL) == VOLUME_UP) {
        __atomic_store_n(&g_abandon_pending, 1u, __ATOMIC_RELEASE);
    }

    uint32_t inside = __atomic_load_n(&g_inside, __ATOMIC_ACQUIRE);
    if (inside != 0) {
        /*
         * Said once per departure, not once per attempt — the service pass
         * asks again every time round the loop, and a line per iteration is a
         * log nobody can read.
         */
        if (!g_leaving_said) {
            g_leaving_said = true;
            kprintf("[TagFS] the volume is on its way out and %u caller(s) are "
                    "still inside it — nothing of it is let go until they are "
                    "out\n", inside);
        }
        return false;
    }

    tagfs_clear_the_ground();

    __atomic_store_n(&g_door, (uint8_t)VOLUME_DOWN, __ATOMIC_RELEASE);
    __atomic_store_n(&g_abandon_pending, 0u, __ATOMIC_RELEASE);
    if (g_leaving_said) {
        g_leaving_said = false;
        kprintf("[TagFS] the last caller is out and the old volume has been let "
                "go of\n");
    }

    g_medium_left   = false;
    g_tagfs_seat    = BOARDROOM_NO_SEAT;
    g_tagfs_seating = 0;
    return true;
}

/*
 * ‼ THE MACHINE IS STOPPING, AND ITS LAST CYCLE HAS ALREADY RUN.
 *
 * The halt runs the volume's last Settle cycle itself, before interrupts go
 * off (TagFSSettleForHalt, system_halt.c): after that a committer could be a
 * core the halt has stopped, and nothing may stand over the medium then. What
 * is left here is to shut the door and to say what the last cycle answered —
 * and how many changes no cycle settled, inked after it by programs still
 * running while the halt began or left by a cycle that was refused, which the
 * medium was not given. Nothing is freed: freeing at
 * a halt was never for the machine's benefit, and memory somebody is still
 * reading must not go back to an allocator nobody will ask again.
 */
error_t tagfs_shutdown(void)
{
    if (!g_state.initialized) {
        return OK;                      /* nothing mounted, nothing owed */
    }

    uint64_t after  = 0;
    error_t  answer = SettleHaltAnswer(&after);
    __atomic_store_n(&g_door, (uint8_t)VOLUME_LEAVING, __ATOMIC_RELEASE);
    if (after) {
        kprintf("[TagFS] %llu change(s) no Settle cycle settled before the machine stopped — "
                "the medium was not given them\n", (unsigned long long)after);
        if (answer == OK)
            answer = ERR_TAGFS_METADATA_ERROR;
    }
    return answer;
}

/*
 * A mount this machine owes itself.
 *
 * Reached only when a mount was attempted and did not finish while a medium
 * sat in the room; see the note over TAGFS_MOUNT_RETRY_MS for why nothing else
 * would ever ask. One atomic load when there is nothing owed.
 */
static void tagfs_ask_for_the_mount_again(void)
{
    if (__atomic_load_n(&g_mount_owed, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    if (g_state.initialized) {
        mount_settled();                /* somebody got there first */
        return;
    }

    if ((int64_t)(rdtsc() - g_mount_due) < 0) {
        return;                         /* not yet; the hardware needs a moment */
    }

    if (g_mount_tries >= TAGFS_MOUNT_RETRY_TRIES) {
        kprintf("[TagFS] the volume in this room would not mount in %u "
                "attempts — the machine stops asking and waits for a medium to "
                "arrive\n", TAGFS_MOUNT_RETRY_TRIES);
        mount_settled();
        return;
    }

    if (!attending_take()) {
        return;                 /* somebody is already at it; no try is spent */
    }

    g_mount_tries++;
    kprintf("[TagFS] asking again for the volume nobody has mounted (attempt "
            "%u of %u)\n", g_mount_tries, TAGFS_MOUNT_RETRY_TRIES);
    attend_arrival();

    if (!g_state.initialized) {
        g_mount_due = rdtsc() + cpu_ms_to_tsc(TAGFS_MOUNT_RETRY_MS);
    }

    attending_done();
}

/*
 * The work of a Helm handed to a waiter that does it synchronously runs here
 * first: a job holds the door for its whole life, so a volume on its way out
 * is let go of only once the work inside it has been done.
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
void TagFSServiceIfPending(void)
{
    HelmRunReady();
    tagfs_ask_for_the_mount_again();

    if (__atomic_load_n(&g_abandon_pending, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    TagFSHoldGroundRelease();       /* nothing at all unless HOLDGROUND=on */

    if (__atomic_load_n(&g_inside, __ATOMIC_ACQUIRE) != 0) {
        return;                     /* still somebody in there */
    }

    /* Under the same flag as the other road, and for its whole length: this
     * one clears the ground and then mounts, and a second core arriving in
     * between would mount on top of it. */
    if (!attending_take()) {
        return;
    }

    if (tagfs_abandon()) {
        /* And now it is an ordinary arrival: nothing is mounted, and there may
         * be a medium in the room carrying the volume this machine had. That
         * road already knows how to check it is the same volume and to say
         * what it did, so it is not written a second time here. */
        attend_arrival();
        TagFSHoldGroundAfterReturn();   /* nothing at all unless HOLDGROUND=on */
    }

    attending_done();
}


// ----------------------------------------------------------------------------
// Test runner interface (called from userspace via System Deck)
// ----------------------------------------------------------------------------

static error_t TagFS_RunTests_inside(void);

error_t TagFS_RunTests(void)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = TagFS_RunTests_inside();
    tagfs_leave();
    return rc;
}

static error_t TagFS_RunTests_inside(void)
{
    TestStats stats;
    error_t result = TagFS_RunAllTests(&stats);

    /* kprintf, not debug_printf: without these numbers a run where every
     * single test SKIPPED still printed "All tests PASSED". A test held under
     * a caveat is a known defect reproduced on purpose — counted on its own,
     * neither passed nor failed nor skipped, and it does not fail the run. */
    kprintf("[TESTS] TagFS: %u run, %u passed, %u failed, %u skipped, "
            "%u held under a caveat\n",
            stats.total_tests, stats.total_passed, stats.total_failed,
            stats.total_skipped, stats.total_held);

    return result;
}

// ----------------------------------------------------------------------------
// The next file id — the questions of which files there are live in query/
// ----------------------------------------------------------------------------

uint32_t tagfs_file_ceiling(void)
{
    if (!tagfs_enter())
        return 0;
    /* Under g_state.lock: create bumps next_file_id under it, and a lock-free
     * read can see the value from before a create that has already returned
     * to its caller. */
    TagFSLockTake(&g_state.lock);
    uint32_t ceiling = g_state.ledger.next_file_id;
    TagFSLockDrop(&g_state.lock);
    tagfs_leave();
    return ceiling;
}

// ----------------------------------------------------------------------------
// File I/O (open / close / read / write)
// ----------------------------------------------------------------------------

static TagFSFileHandle *tagfs_open_inside(uint32_t file_id, uint32_t flags);

TagFSFileHandle *tagfs_open(uint32_t file_id, uint32_t flags)
{
    if (!tagfs_enter())
        return NULL;
    TagFSFileHandle *rc = tagfs_open_inside(file_id, flags);
    tagfs_leave();
    return rc;
}

static TagFSFileHandle *tagfs_open_inside(uint32_t file_id, uint32_t flags)
{
    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        debug_printf("[TagFS] open: file_id=%u not found\n", file_id);
        return NULL;
    }

    /* An open that cannot join its file's Helm is refused (W-NULLOFE): a
     * handle with no entry would change the file beside its holder instead of
     * after it. */
    OpenFileEntry *ofe = ofe_acquire(file_id);
    if (!ofe)
        return NULL;

    TagFSFileHandle *handle = kmalloc(sizeof(TagFSFileHandle));
    if (!handle)
    {
        ofe_release(ofe);
        return NULL;
    }
    memset(handle, 0, sizeof(*handle));
    handle->file_id = file_id;
    handle->flags   = flags;
    handle->ofe     = ofe;
    /* Which mounting of the volume these extents were copied out of. A handle
     * is refused after the volume has been read again, because the blocks it
     * names then belong to whatever the volume keeps there now. */
    handle->mount_epoch = tagfs_mount_epoch();
    HelmViewTake(handle);

    /*
     * ‼ AN OPEN HOLDS WHAT IT NAMES, OR IS REFUSED (MP-D16). The record is the
     * map's — the authority every read stands on, read with no I/O — and a
     * file whose record is not there, or whose extents cannot be copied, is
     * not handed out as an empty file. A handle opened only to write copies no
     * extents at all (W-STALE): its work loads the record under the file's
     * Helm, and a copy from before the Helm was its own would be stale.
     */
    bool        reads = (flags & TAGFS_HANDLE_READ) || !(flags & TAGFS_HANDLE_WRITE);
    FileRecord *rec   = FileMapRecordHold(g_state.files, file_id, NULL);
    bool        held  = rec != NULL;
    if (held && reads && rec->extent_count > 0)
    {
        handle->extents = kmalloc(sizeof(FileExtent) * rec->extent_count);
        held = handle->extents != NULL;
        if (held)
        {
            memcpy(handle->extents, rec->extents, sizeof(FileExtent) * rec->extent_count);
            handle->extent_count = rec->extent_count;
        }
    }
    if (held)
        handle->file_size = rec->size;
    FileRecordRelease(rec);
    if (!held)
    {
        tagfs_close(handle);
        return NULL;
    }
    return handle;
}

void tagfs_close(TagFSFileHandle *handle)
{
    if (!handle)
        return;
    HelmViewLetGo(handle);
    ofe_release(handle->ofe);
    kfree(handle->extents);
    kfree(handle);
}

static error_t tagfs_read_inside(TagFSFileHandle *handle, void *buffer, uint64_t size,
                                 uint64_t *out_read);

error_t tagfs_read(TagFSFileHandle *handle, void *buffer, uint64_t size, uint64_t *out_read)
{
    if (!out_read)
        return ERR_INVALID_ARGUMENT;
    *out_read = 0;
    if (!buffer)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = handle_belongs_here(handle, "read")
                     ? tagfs_read_inside(handle, buffer, size, out_read)
                     : ERR_TAGFS_INVALID_HANDLE;
    tagfs_leave();
    return rc;
}

static error_t tagfs_read_inside(TagFSFileHandle *handle, void *buffer, uint64_t size,
                                 uint64_t *out_read)
{
    if (size == 0 || handle->offset >= handle->file_size)
        return OK;

    uint64_t remaining = handle->file_size - handle->offset;
    if (size > remaining)
        size = remaining;

    /* The block goes through the heap: a kernel stack has no room for 4 KiB. */
    uint8_t *block_buf = (uint8_t *)kmalloc(TAGFS_BLOCK_SIZE);
    if (!block_buf)
        return ERR_NO_MEMORY;
    uint8_t *out        = (uint8_t *)buffer;
    uint64_t bytes_read = 0;
    uint32_t disk_block = 0, left = 0;
    error_t  rc         = OK;

    while (bytes_read < size)
    {
        uint64_t file_pos        = handle->offset + bytes_read;
        uint64_t k               = file_pos / TAGFS_BLOCK_SIZE;
        uint32_t offset_in_block = (uint32_t)(file_pos % TAGFS_BLOCK_SIZE);

        if (bytes_read > 0 && left > 0) {
            disk_block++;
            left--;
        } else if (!PlotFind(handle->extents, handle->extent_count, k, &disk_block, &left)) {
            /* The file claims more bytes than its extents describe. Whatever
             * lies past the last extent is not a hole to be filled with zeros;
             * it is metadata that does not match the medium. */
            kprintf("[TagFS] file %u: no extent covers byte %llu of %llu — the "
                    "metadata describes more file than there are blocks\n",
                    handle->file_id, (unsigned long long)file_pos,
                    (unsigned long long)handle->file_size);
            rc = ERR_METADATA_CORRUPTED;
            break;
        }

        if (read_block(disk_block, block_buf) != 0)
        {
            /*
             * A block the medium would not give up is the read's answer, never
             * a shorter count: a caller that took a short read of zeros for
             * data once started a process image of nothing (the program loader,
             * a flash drive pulled while running). Bytes already copied stay in
             * the caller's buffer and the handle does not move.
             */
            kprintf("[TagFS] file %u: block %u would not read — %llu of %llu "
                    "bytes had come back before the medium stopped answering\n",
                    handle->file_id, disk_block,
                    (unsigned long long)bytes_read, (unsigned long long)size);
            rc = ERR_READ_FAILED;
            break;
        }

        uint32_t chunk = TAGFS_BLOCK_SIZE - offset_in_block;
        if (chunk > size - bytes_read)
            chunk = (uint32_t)(size - bytes_read);

        memcpy(out + bytes_read, block_buf + offset_in_block, chunk);
        bytes_read += chunk;

        /*
         * Read-ahead, and only when the next block is NOT already in hand.
         * Run after every block, it asked for four when three were cached, and
         * the window crawled a block at a time — one command per 4 KiB on a
         * flash drive. Waiting for the miss moves a whole run, and a whole run
         * of neighbours is one command (ReadAheadPrefetch).
         */
        if (offset_in_block == 0 && chunk == TAGFS_BLOCK_SIZE && left > 0 &&
            !ReadAheadHas(disk_block + 1)) {
            ReadAheadPrefetch(disk_block + 1, left < TAGFS_READ_AHEAD_BLOCKS
                                                  ? left : TAGFS_READ_AHEAD_BLOCKS);
        }
    }

    kfree(block_buf);
    if (rc != OK)
        return rc;
    handle->offset += bytes_read;
    *out_read = bytes_read;
    return OK;
}

/* A caller in the kernel takes a file's Helm only when it is free: it cannot
 * be parked behind the holder the way a program is (write_job.c), and it does
 * not spin for it. Held: said, and false. */
static bool tagfs_helm_now(TagFSFileHandle *handle, HelmWaiter *here)
{
    memset(here, 0, sizeof(*here));
    if (HelmTry(handle->ofe, here))
        return true;
    kprintf("[TagFS] file %u is being changed by the holder of its Helm — a caller in the "
            "kernel cannot be parked behind it, and is refused\n", handle->file_id);
    return false;
}

error_t tagfs_write(TagFSFileHandle *handle, const PlotSource *src, uint64_t *out_written)
{
    if (!out_written)
        return ERR_INVALID_ARGUMENT;
    *out_written = 0;
    if (!src || src->bytes == 0)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t    rc = ERR_TAGFS_INVALID_HANDLE;
    HelmWaiter here;
    if (handle_belongs_here(handle, "write")) {
        rc = ERR_BUSY;
        if (tagfs_helm_now(handle, &here)) {
            rc = HelmWrite(handle, src, out_written);
            HelmLetGo(handle->ofe, &here);
        }
    }
    tagfs_leave();
    return rc;
}

error_t tagfs_span_file(uint32_t file_id, uint64_t size)
{
    if (file_id == 0)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t          rc     = ERR_FILE_NOT_FOUND;
    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    HelmWaiter       here;
    if (handle) {
        rc = ERR_BUSY;
        if (tagfs_helm_now(handle, &here)) {
            uint64_t was = 0;
            rc = HelmSpan(handle, size, &was);
            HelmLetGo(handle->ofe, &here);
        }
        tagfs_close(handle);
    }
    tagfs_leave();
    return rc;
}

// ----------------------------------------------------------------------------
// Block allocator — the doors into the block map (block_map/)
// ----------------------------------------------------------------------------

static int tagfs_alloc_blocks_inside(uint32_t count, uint32_t *out_start_block);

int tagfs_alloc_blocks(uint32_t count, uint32_t *out_start_block)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_alloc_blocks_inside(count, out_start_block);
    tagfs_leave();
    return rc;
}

/* ‼ BEFORE ANYTHING ON THE MEDIUM NAMES IT. Every caller of this function goes
 * on to name the block somewhere that outlives this boot — a Ledger field, a
 * chain's next pointer — and the take is owed to the medium until the next
 * Settle cycle's S1 writes its sector, which comes before the name that cycle
 * carries. A crash between the two leaves a block nobody claims, never a block
 * two owners claim (block_map.h, MapOwed). */
static int tagfs_alloc_blocks_inside(uint32_t count, uint32_t *out_start_block)
{
    if (count == 0 || !out_start_block)
        return -1;
    return BlockMapTake(count, 0, out_start_block, __func__) == OK ? 0 : -1;
}

// Public version - acquires g_state.lock
static int tagfs_free_blocks_inside(uint32_t start_block, uint32_t count);

int tagfs_free_blocks(uint32_t start_block, uint32_t count)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_free_blocks_inside(start_block, count);
    tagfs_leave();
    return rc;
}

/* The sectors of the run are brought in first, with no lock held, so the give
 * under the lock is judged at once and its caller hears the answer; the edits
 * the lock made without a sector are laid on before this returns. */
static int tagfs_free_blocks_inside(uint32_t start_block, uint32_t count)
{
    if (count == 0)
        return 0;

    BlockMapFetchRun(start_block, count, __func__);
    TagFSLockTake(&g_state.lock);
    error_t given = BlockMapGiveLocked(start_block, count);
    TagFSLockDrop(&g_state.lock);
    BlockMapSettle();
    return given == OK ? 0 : -(int)given;
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static int tagfs_get_metadata_inside(uint32_t file_id, TagFSMetadata *out);

int tagfs_get_metadata(uint32_t file_id, TagFSMetadata *out)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_get_metadata_inside(file_id, out);
    tagfs_leave();
    return rc;
}

/* The file's record, out of the map of live files — the authority every read
 * stands on, holding every live file's newest record, so no road leads to the
 * medium. -1 for a file the map does not hold, or no memory for the copy. */
static int tagfs_get_metadata_inside(uint32_t file_id, TagFSMetadata *out)
{
    if (!out)
        return -1;
    FileRecord *rec = FileMapRecordHold(g_state.files, file_id, NULL);
    if (!rec)
        return -1;
    int copied = FileRecordCopyOut(rec, out);
    FileRecordRelease(rec);
    return copied;
}

TagFSState *tagfs_get_state(void)
{
    return &g_state;
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
uint16_t TagFSLookupSpelling(const TagSpelling *tag)
{
    if (!tagfs_enter())
        return TAGFS_INVALID_TAG_ID;
    uint16_t id = TagRegistryLookup(g_state.registry, tag);
    tagfs_leave();
    return id;
}

error_t TagFSInternSpelling(const TagSpelling *tag, uint16_t *out_id)
{
    *out_id = TAGFS_INVALID_TAG_ID;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = TagRegistryIntern(g_state.registry, tag, out_id);
    tagfs_leave();
    return rc;
}

/* The id this volume has for a tag's text, or TAGFS_INVALID_TAG_ID when it has
 * none — including when the text is no tag, and when there is no volume. */
uint16_t tagfs_tag_lookup(const char *tag)
{
    TagSpelling spelled;
    if (!tag || TagSpell(tag, strlen(tag), &spelled) != OK)
        return TAGFS_INVALID_TAG_ID;
    return TagFSLookupSpelling(&spelled);
}

/* The same, and the volume issues an id if it does not have one yet. */
error_t tagfs_tag_intern(const char *tag, uint16_t *out_id)
{
    TagSpelling spelled;
    *out_id = TAGFS_INVALID_TAG_ID;
    if (!tag)
        return ERR_TAGFS_INVALID_TAG;
    error_t rc = TagSpell(tag, strlen(tag), &spelled);
    if (rc != OK)
        return rc;
    return TagFSInternSpelling(&spelled, out_id);
}

bool TagFSTagSpelling(uint16_t tag_id, TagSpelling *out, bool *is_system)
{
    return g_state.registry && TagRegistrySpelling(g_state.registry, tag_id, out, is_system);
}

bool tagfs_tag_key(uint16_t tag_id, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return false;
    out[0] = '\0';
    if (!tagfs_enter())
        return false;

    TagSpelling tag;
    bool said = TagFSTagSpelling(tag_id, &tag, NULL) && tag.key_len < out_size;
    if (said) {
        memcpy(out, tag.key, tag.key_len);
        out[tag.key_len] = '\0';
    }
    tagfs_leave();
    return said;
}

size_t tagfs_tag_text(uint16_t tag_id, char *out, size_t out_size)
{
    if (out && out_size)
        out[0] = '\0';
    if (!tagfs_enter())
        return 0;

    TagSpelling tag;
    size_t      len = 0;
    if (TagFSTagSpelling(tag_id, &tag, NULL)) {
        len = TagSpellingTextLength(&tag);
        if (out && len < out_size)
            TagSpellingText(&tag, out);
    }
    tagfs_leave();
    return len;
}

bool TagFSTagMatches(uint16_t tag_id, const TagSpelling *pattern)
{
    if (!tagfs_enter())
        return false;
    TagSpelling tag;
    bool matches = TagFSTagSpelling(tag_id, &tag, NULL) && TagPatternMatches(pattern, &tag);
    tagfs_leave();
    return matches;
}

char *TagFSKeyList(const uint16_t *tag_ids, uint32_t count)
{
    if (!tagfs_enter())
        return NULL;

    TagSpelling tag;
    size_t      need = 1;
    for (uint32_t i = 0; i < count; i++)
        if (TagFSTagSpelling(tag_ids[i], &tag, NULL))
            need += tag.key_len + 1;

    char *list = kmalloc(need);
    if (list) {
        size_t at = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!TagFSTagSpelling(tag_ids[i], &tag, NULL))
                continue;
            if (at)
                list[at++] = ',';
            memcpy(list + at, tag.key, tag.key_len);
            at += tag.key_len;
        }
        list[at] = '\0';
    }
    tagfs_leave();
    return list;
}

// ----------------------------------------------------------------------------
// Defrag
// ----------------------------------------------------------------------------

error_t tagfs_defrag_file(uint32_t file_id, uint32_t target_block)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t          rc     = ERR_FILE_NOT_FOUND;
    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    HelmWaiter       here;
    if (handle) {
        rc = ERR_BUSY;
        if (tagfs_helm_now(handle, &here)) {
            rc = HelmDefrag(handle, target_block);
            HelmLetGo(handle->ofe, &here);
        }
        tagfs_close(handle);
    }
    tagfs_leave();
    return rc;
}
