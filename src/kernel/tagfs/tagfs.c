#include "tagfs.h"
#include "boardroom.h"
#include "boarding.h"
#include "deed/deed.h"
#include "tagfs_reserved.h"
#include "tag_registry/tag_registry.h"
#include "tag_bitmap/tag_bitmap.h"
#include "file_table/file_table.h"
#include "metadata_pool/meta_pool.h"
#include "use_context.h"
#include "disk_book/disk_book.h"
#include "bcdc/bcdc.h"
#include "error.h"
#include "ahci_sync.h"
#include "ahci.h"
#include "touch.h"
#include "ata.h"
#include "amp.h"
#include "irq_defer.h"
#include "cow/cow.h"
#include "braid/braid.h"
#include "dedup/dedup.h"
#include "self_heal/self_heal.h"
#include "integrity/integrity.h"
#include "../../kernel/drivers/timer/rtc.h"
#include "../../lib/kernel/crypto.h"
#include "logbook.h"
#include "cpu_calibrate.h"

static void tagfs_auto_snapshot_before_write(uint32_t file_id) {
    if (file_id == 0)
        return;

    // Check if CoW is active for this file
    if (TagFS_CowIsActive(file_id)) {
        // Create auto-snapshot before write
        char snap_name[64];
        ksnprintf(snap_name, sizeof(snap_name), "auto-snap-%u", file_id);

        uint32_t snapshot_id;
        error_t err = TagFS_SnapshotCreate(snap_name, file_id, &snapshot_id);
        if (err != OK && err != ERR_ALREADY_EXISTS) {
            debug_printf("[TagFS] Auto-snapshot failed for file %u: %d\n", file_id, err);
        }
    }
}

static TagFSState g_state;
static WellKnownTags g_well_known;

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
 * block bitmap, the free list, the file table, the metadata pool.
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
// Open File Table — per-file write serialization
// ----------------------------------------------------------------------------

/*
 * ‼ THIS TABLE BELONGS TO THE MACHINE, NOT TO THE VOLUME.
 *
 * It used to be emptied by every tagfs_init — the lock re-initialised and the
 * buckets memset — which is three separate wrongs on a re-mount:
 *
 *   a lock another core is holding is re-initialised to "free", so the mutual
 *   exclusion it was providing is simply lost, silently;
 *
 *   every OpenFileEntry in it is dropped on the floor: they are kmalloc'd,
 *   the handles that own them still point at them, and nothing frees them;
 *
 *   and — the one that corrupts a file rather than losing memory — the next
 *   ofe_acquire for a file that already had an entry makes a SECOND one. The
 *   whole purpose of this table is that two writers to one file take the same
 *   write_lock. Two entries means two locks, which means no serialisation at
 *   all, and ofe_release survives it (it kfree's an entry it cannot find in
 *   the bucket), so there is not even a symptom.
 *
 * A mount brings up what is on the medium. Who has a file open is a fact about
 * this machine, and it is left alone.
 */
static OpenFileEntry *g_open_files[OPEN_FILE_BUCKETS];
static spinlock_t g_open_table_lock;

static uint32_t ofe_hash(uint32_t file_id)
{
    return file_id % OPEN_FILE_BUCKETS;
}

static OpenFileEntry *ofe_acquire(uint32_t file_id)
{
    spin_lock(&g_open_table_lock);
    uint32_t bucket = ofe_hash(file_id);
    OpenFileEntry *e = g_open_files[bucket];
    while (e)
    {
        if (e->file_id == file_id)
        {
            e->ref_count++;
            spin_unlock(&g_open_table_lock);
            return e;
        }
        e = e->next;
    }
    e = kmalloc(sizeof(OpenFileEntry));
    if (!e)
    {
        spin_unlock(&g_open_table_lock);
        return NULL;
    }
    e->file_id = file_id;
    e->ref_count = 1;
    spinlock_init(&e->write_lock);
    e->async_write_owner  = NULL;
    e->async_pending_head = NULL;
    spinlock_init(&e->async_token_lock);
    e->next = g_open_files[bucket];
    g_open_files[bucket] = e;
    spin_unlock(&g_open_table_lock);
    return e;
}

static void ofe_release(OpenFileEntry *ofe)
{
    if (!ofe)
        return;

    bool do_free = false;

    spin_lock(&g_open_table_lock);
    if (ofe->ref_count == 0)
    {
        uint32_t fid = ofe->file_id;
        spin_unlock(&g_open_table_lock);
        panic("[TagFS] ofe_release: ref_count already zero for file_id=%u — double-release or corruption", fid);
    }
    ofe->ref_count--;
    if (ofe->ref_count == 0)
    {
        // FIX: Remove from table FIRST while holding table_lock,
        // then RELEASE table_lock BEFORE acquiring write_lock.
        // This prevents nested spinlock deadlock.
        uint32_t bucket = ofe_hash(ofe->file_id);
        OpenFileEntry **ptr = &g_open_files[bucket];
        while (*ptr)
        {
            if (*ptr == ofe)
            {
                *ptr = ofe->next;
                break;
            }
            ptr = &(*ptr)->next;
        }
        do_free = true;
        // Note: table_lock is still held here, will be released below
    }
    spin_unlock(&g_open_table_lock);

    // Now acquire write_lock OUTSIDE of table_lock to prevent deadlock.
    // This waits for any in-flight writer to finish before freeing memory.
    if (do_free)
    {
        spin_lock(&ofe->write_lock);
        spin_unlock(&ofe->write_lock);
        kfree(ofe);
    }
}

// ----------------------------------------------------------------------------
// Bitmap bit helpers
// ----------------------------------------------------------------------------

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
 * file? A second entry for one file is two write locks for one file, which is
 * no serialisation at all — and it leaves no trace of its own, because
 * ofe_release frees an entry it cannot find in the bucket without complaining. */
static bool hold_entry_still_listed(const TagFSFileHandle *handle, uint32_t *out_refs)
{
    if (!handle || !handle->ofe) {
        return false;
    }
    bool found = false;
    spin_lock(&g_open_table_lock);
    for (OpenFileEntry *e = g_open_files[ofe_hash(handle->ofe->file_id)]; e; e = e->next) {
        if (e == handle->ofe) {
            found = true;
            if (out_refs) *out_refs = e->ref_count;
            break;
        }
    }
    spin_unlock(&g_open_table_lock);
    return found;
}

/* A file is held open from the moment the volume is first mounted, so that it
 * is a handle from the PREVIOUS mounting when the volume comes back. */
static void TagFSHoldGroundOpenFile(void)
{
    if (g_hold_handle) {
        return;
    }
    uint32_t ids[8];
    int n = tagfs_list_all_files(ids, 8);
    if (n <= 0) {
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

    uint8_t probe[16];
    int rc = tagfs_read(g_hold_handle, probe, sizeof(probe));
    kprintf("[TagFS HOLDGROUND] a read through the handle from before returned "
            "%d\n", rc);

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

static inline void bitmap_set_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static inline void bitmap_clear_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static inline bool bitmap_test_bit(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8] & (uint8_t)(1u << (bit % 8))) != 0;
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
    uint32_t run = BoardroomSeatRun(g_tagfs_seat);
    uint32_t blocks = run / 8u;
    if (blocks == 0) blocks = 1;
    if (blocks > TAGFS_READ_AHEAD_BLOCKS) blocks = TAGFS_READ_AHEAD_BLOCKS;
    kprintf("[TagFS] this medium takes %u sector(s) at a time, so neighbouring "
            "blocks are read %u at a time\n", run, blocks);
}

int tagfs_volume_read(uint64_t vlba, uint32_t count, void *buffer)
{
    return BoardroomRead(g_tagfs_seat, g_volume_base + vlba, count, buffer);
}

int tagfs_volume_write(uint64_t vlba, uint32_t count, const void *buffer)
{
    return BoardroomWrite(g_tagfs_seat, g_volume_base + vlba, count, buffer);
}

static int disk_read_sectors(uint64_t vlba, uint16_t count, void *buffer)
{
    return tagfs_volume_read(vlba, count, buffer);
}

static int disk_write_sectors(uint64_t vlba, uint16_t count, const void *buffer)
{
    return tagfs_volume_write(vlba, count, buffer);
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
    bool     took = false;

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
error_t tagfs_flush_cache(void)
{
    return (BoardroomFlush(g_tagfs_seat) == 0) ? OK : ERR_IO;
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
static spinlock_t g_read_ahead_lock;

static void ReadAheadInit(void) {
    memset(g_read_ahead_cache, 0, sizeof(g_read_ahead_cache));
    g_read_ahead_head = 0;
    g_read_ahead_last_block = 0;
    spinlock_init(&g_read_ahead_lock);
}

static int ReadAheadLookup(uint32_t block, void *buffer) {
    spin_lock(&g_read_ahead_lock);
    for (uint32_t i = 0; i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        if (g_read_ahead_cache[i].valid && g_read_ahead_cache[i].block == block) {
            memcpy(buffer, g_read_ahead_cache[i].data, TAGFS_BLOCK_SIZE);
            spin_unlock(&g_read_ahead_lock);
            return 0;
        }
    }
    spin_unlock(&g_read_ahead_lock);
    return -1;
}

/* Is this block already in hand? Asked without copying it: the reader wants to
 * know whether to go to the medium at all, not what is in the block. */
static bool ReadAheadHas(uint32_t block) {
    bool here = false;
    spin_lock(&g_read_ahead_lock);
    for (uint32_t i = 0; i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        if (g_read_ahead_cache[i].valid && g_read_ahead_cache[i].block == block) {
            here = true;
            break;
        }
    }
    spin_unlock(&g_read_ahead_lock);
    return here;
}

static void ReadAheadPrefetch(uint32_t start_block, uint32_t count) {
    if (count == 0)
        return;

    // Phase 1: determine which blocks to fetch (under lock, no I/O)
    uint32_t to_fetch[TAGFS_READ_AHEAD_BLOCKS];
    uint32_t slots[TAGFS_READ_AHEAD_BLOCKS];
    uint32_t n_fetch = 0;

    spin_lock(&g_read_ahead_lock);
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
    spin_unlock(&g_read_ahead_lock);

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
            if (disk_read_sectors(block_to_vlba(to_fetch[i]), 8, tmp) == 0) {
                spin_lock(&g_read_ahead_lock);
                memcpy(g_read_ahead_cache[slots[i]].data, tmp, TAGFS_BLOCK_SIZE);
                g_read_ahead_cache[slots[i]].block = to_fetch[i];
                g_read_ahead_cache[slots[i]].valid = true;
                spin_unlock(&g_read_ahead_lock);
            }
            i++;
            continue;
        }

        if (disk_read_sectors(block_to_vlba(to_fetch[i]),
                              (uint16_t)(len * 8u), run) == 0) {
            spin_lock(&g_read_ahead_lock);
            for (uint32_t k = 0; k < len; k++) {
                memcpy(g_read_ahead_cache[slots[i + k]].data,
                       run + k * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE);
                g_read_ahead_cache[slots[i + k]].block = to_fetch[i + k];
                g_read_ahead_cache[slots[i + k]].valid = true;
            }
            spin_unlock(&g_read_ahead_lock);
        }
        kfree(run);
        i += len;
    }
}

// Drop any cached copy of `block` so a read after a write to it never serves
// pre-write data (read-after-write consistency). Called on every block write,
// sync and async.
static void ReadAheadInvalidate(uint32_t block) {
    spin_lock(&g_read_ahead_lock);
    for (uint32_t i = 0; i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        if (g_read_ahead_cache[i].valid && g_read_ahead_cache[i].block == block)
            g_read_ahead_cache[i].valid = false;
    }
    spin_unlock(&g_read_ahead_lock);
}

void tagfs_readahead_invalidate(uint32_t block) {
    ReadAheadInvalidate(block);
}

/* Every entry, for when what they are copies of is no longer there. */
static void ReadAheadForget(void) {
    spin_lock(&g_read_ahead_lock);
    for (uint32_t i = 0; i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        g_read_ahead_cache[i].valid = false;
    }
    spin_unlock(&g_read_ahead_lock);
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

    // Check read-ahead cache first
    if (ReadAheadLookup(block, buffer) == 0) {
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

    int rc = disk_read_sectors(block_to_vlba(block), 8, buffer);
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
        rc = disk_write_sectors(block_to_vlba(block), 8, (void *)buffer);

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

static const uint8_t g_ledger_magic[8] = {
    VOLUME_LEDGER_MAGIC_0, VOLUME_LEDGER_MAGIC_1, VOLUME_LEDGER_MAGIC_2,
    VOLUME_LEDGER_MAGIC_3, VOLUME_LEDGER_MAGIC_4, VOLUME_LEDGER_MAGIC_5,
    VOLUME_LEDGER_MAGIC_6, VOLUME_LEDGER_MAGIC_7
};

// ----------------------------------------------------------------------------
// The Ledger — what is true of the volume today (volume_ledger.h)
//
// Two copies, one block apart. A read takes the newer of the two that is
// intact; a write goes to the older one with a sequence number one higher. So
// the copy that survives a torn write is never the copy being written, and
// recovery is a comparison rather than a repair.
// ----------------------------------------------------------------------------

/* Which of the two copies was written last. The next write goes to the other
 * one — this is the whole of the alternation. */
static uint32_t g_ledger_current = 0;

/* What the volume remembers of the Use Context — the comma-joined list it was
 * last told, kept beside the counters and written out with them on every
 * Ledger write. One sector is the record's whole world (ledger_read_copy takes
 * one and refuses a longer claim), so this is bounded by what a sector holds
 * past the fixed fields: a list that will not fit is not kept in part.
 *
 * One Ledger writer at a time, the whole way through. Two of them reading the
 * same seq, building two records and racing for the same older copy would
 * leave the medium with whichever landed last and memory with whichever
 * committed last — not necessarily the same one. A leaf: nothing under it
 * takes another lock. */
static spinlock_t g_ledger_lock;
static char       g_ledger_use[TAGFS_SECTOR_SIZE];
static uint16_t   g_ledger_use_len;
#define LEDGER_USE_MAX  ((size_t)TAGFS_SECTOR_SIZE - sizeof(VolumeLedger))

static uint64_t ledger_vlba(uint32_t copy)
{
    return ((uint64_t)g_state.layout.state_block + copy) * TAGFS_BLOCK_SECTORS;
}

/*
 * Read one copy. Returns true when it is present, intact, and long enough to
 * hold what this kernel knows how to read — a Ledger written by a newer kernel
 * is longer than this one expects and is read up to what is understood, which
 * is what `bytes` is for.
 */
static bool ledger_read_copy(uint32_t copy, VolumeLedger *out,
                             char *tail, uint16_t *tail_len)
{
    uint8_t buf[TAGFS_SECTOR_SIZE];
    if (disk_read_sectors(ledger_vlba(copy), 1, buf) != 0) {
        kprintf("[Ledger] copy %u could not be read off the medium\n", copy);
        return false;
    }

    VolumeLedger led;
    memcpy(&led, buf, sizeof(led));

    if (memcmp(led.magic, g_ledger_magic, sizeof(g_ledger_magic)) != 0) {
        kprintf("[Ledger] copy %u carries no ledger\n", copy);
        return false;
    }

    /* Bounded before it is summed: `bytes` came off the medium and a stated
     * length longer than the sector it lives in would sum over memory this
     * does not own. Required is what every reader needs, not what this one
     * knows: a record from before the Use Context was kept ends at the
     * counters, and refusing it would refuse every volume made before then. */
    if (led.bytes < VOLUME_LEDGER_REQUIRED_BYTES || led.bytes > TAGFS_SECTOR_SIZE) {
        kprintf("[Ledger] copy %u states a length of %u, which cannot be one\n",
                copy, led.bytes);
        return false;
    }

    uint8_t probe[TAGFS_SECTOR_SIZE];
    memcpy(probe, buf, led.bytes);
    memset(probe + __builtin_offsetof(VolumeLedger, crc32), 0, sizeof(uint32_t));
    uint32_t have = KCrc32(probe, led.bytes);
    if (have != led.crc32) {
        kprintf("[Ledger] copy %u does not match its own checksum "
                "(0x%08x against 0x%08x) — it was torn mid-write\n",
                copy, have, led.crc32);
        return false;
    }

    /* What a shorter record does not carry reads as zero, not as whatever
     * followed it on the medium. */
    if (led.bytes < sizeof(led))
        memset((uint8_t *)&led + led.bytes, 0, sizeof(led) - led.bytes);

    /* The remembered Use Context rides after the fixed fields, inside the
     * summed bytes — so it is already proven intact by the checksum above;
     * only its place in the record is checked here. */
    *tail_len = 0;
    if (led.use_context_bytes) {
        uint32_t off = led.use_context_offset;
        uint32_t len = led.use_context_bytes;
        if (off < VOLUME_LEDGER_REQUIRED_BYTES || off + len > led.bytes) {
            kprintf("[Ledger] copy %u places its Use Context at %u for %u bytes, "
                    "outside its own record — it remembers none\n", copy, off, len);
        } else {
            memcpy(tail, buf + off, len);
            *tail_len = (uint16_t)len;
        }
    }

    *out = led;
    return true;
}

/* One copy, read off the medium by the reader above and handed over as it
 * is — for a proof that what was said to the volume is what it holds. */
bool tagfs_ledger_peek(uint32_t copy, VolumeLedger *out, char *tail, uint16_t *tail_len)
{
    if (copy >= VOLUME_LEDGER_COPIES || !out || !tail || !tail_len) return false;
    return ledger_read_copy(copy, out, tail, tail_len);
}

/*
 * Take up the newer of the two.
 *
 * One unreadable copy is survivable and is said out loud, because a volume
 * running on a single Ledger has no second opinion left. Both unreadable is
 * not survivable: the counters are gone, and inventing them would hand out
 * block numbers that belong to files.
 */
static error_t ledger_load(void)
{
    VolumeLedger led[VOLUME_LEDGER_COPIES];
    bool         ok[VOLUME_LEDGER_COPIES];
    uint32_t     live = 0;

    /* Each copy's remembered context, read with it; the newest copy's is the
     * one the volume stands on. Off the stack: two sectors' worth. */
    char *tails = kmalloc((size_t)VOLUME_LEDGER_COPIES * TAGFS_SECTOR_SIZE);
    uint16_t tail_len[VOLUME_LEDGER_COPIES];
    if (!tails) return ERR_NO_MEMORY;

    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        ok[c] = ledger_read_copy(c, &led[c], tails + (size_t)c * TAGFS_SECTOR_SIZE,
                                 &tail_len[c]);
        if (ok[c]) live++;
    }

    if (live == 0) {
        kfree(tails);
        kprintf("[Ledger] neither copy is readable — this volume cannot say "
                "what is on it\n");
        return ERR_TAGFS_CORRUPTED;
    }

    uint32_t newest = 0;
    bool     have   = false;
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        if (!ok[c]) continue;
        if (!have || led[c].seq > led[newest].seq) {
            newest = c;
            have   = true;
        }
    }

    g_state.ledger   = led[newest];
    g_ledger_current = newest;

    spin_lock(&g_ledger_lock);
    g_ledger_use_len = tail_len[newest];
    memcpy(g_ledger_use, tails + (size_t)newest * TAGFS_SECTOR_SIZE, g_ledger_use_len);
    spin_unlock(&g_ledger_lock);
    kfree(tails);

    if (live < VOLUME_LEDGER_COPIES) {
        kprintf("[Ledger] running on copy %u alone (seq %llu); the other one "
                "did not survive\n",
                newest, (unsigned long long)led[newest].seq);
    }
    return OK;
}

/* The write itself; g_ledger_lock is held by the caller. */
static error_t ledger_write_locked(void)
{
    if (g_state.layout.state_blocks < VOLUME_LEDGER_COPIES) {
        return ERR_TAGFS_METADATA_ERROR;
    }

    uint32_t target = (g_ledger_current + 1) % VOLUME_LEDGER_COPIES;

    uint8_t buf[TAGFS_SECTOR_SIZE];
    memset(buf, 0, sizeof(buf));

    VolumeLedger led = g_state.ledger;
    memcpy(led.magic, g_ledger_magic, sizeof(g_ledger_magic));
    led.seq          = g_state.ledger.seq + 1;
    led.written_unix = rtc_get_unix64();
    /* The remembered Use Context follows the fixed fields and is summed with
     * them: a torn write loses the context together with the counters, never
     * one without the other. */
    led.use_context_offset = g_ledger_use_len ? (uint16_t)sizeof(VolumeLedger) : 0;
    led.use_context_bytes  = g_ledger_use_len;
    led.bytes        = (uint32_t)sizeof(VolumeLedger) + g_ledger_use_len;
    led.crc32        = 0;
    memcpy(buf, &led, sizeof(led));
    memcpy(buf + sizeof(led), g_ledger_use, g_ledger_use_len);
    led.crc32 = KCrc32(buf, led.bytes);
    memcpy(buf + __builtin_offsetof(VolumeLedger, crc32), &led.crc32, sizeof(led.crc32));

    if (disk_write_sectors(ledger_vlba(target), 1, buf) != OK) {
        debug_printf("[Ledger] copy %u would not take the write\n", target);
        return ERR_TAGFS_METADATA_ERROR;
    }

    /* Only now is the new one the current one — if the write failed, the older
     * copy is still what the volume stands on. */
    g_state.ledger   = led;
    g_ledger_current = target;
    return OK;
}

error_t tagfs_write_ledger(void)
{
    spin_lock(&g_ledger_lock);
    error_t rc = ledger_write_locked();
    spin_unlock(&g_ledger_lock);
    return rc;
}

error_t tagfs_remember_use_context(const char *list, size_t len, bool *remembered)
{
    if (!list && len) return ERR_INVALID_ARGUMENT;

    /* A list the record cannot hold is not cut short and not kept: the volume
     * forgets rather than remembering half of what was said, and says so. */
    bool fits = len <= LEDGER_USE_MAX;

    spin_lock(&g_ledger_lock);
    g_ledger_use_len = fits ? (uint16_t)len : 0;
    if (g_ledger_use_len) memcpy(g_ledger_use, list, g_ledger_use_len);
    error_t rc = ledger_write_locked();
    spin_unlock(&g_ledger_lock);

    if (remembered) *remembered = fits && rc == OK;
    return rc;
}

size_t tagfs_recall_use_context(char *buf, size_t cap)
{
    spin_lock(&g_ledger_lock);
    size_t len = g_ledger_use_len;
    if (buf && cap >= len && len) memcpy(buf, g_ledger_use, len);
    spin_unlock(&g_ledger_lock);
    return len;
}

// ----------------------------------------------------------------------------
// Free extent list
// ----------------------------------------------------------------------------

static void free_list_destroy(void)
{
    FreeExtent *cur = g_state.block_bitmap.free_list;
    while (cur)
    {
        FreeExtent *next = cur->next;
        kfree(cur);
        cur = next;
    }
    g_state.block_bitmap.free_list = NULL;
    g_state.block_bitmap.extent_count = 0;
}

static void free_list_build(void)
{
    free_list_destroy();

    uint32_t total = g_state.block_bitmap.total_blocks;
    uint8_t *bitmap = g_state.block_bitmap.bitmap;
    FreeExtent **tail = &g_state.block_bitmap.free_list;

    uint32_t i = 0;
    while (i < total)
    {
        // skip used blocks
        while (i < total)
        {
            if ((i & 7) == 0 && i + 8 <= total && bitmap[i / 8] == 0xFF)
            {
                i += 8;
                continue;
            }
            if (bitmap_test_bit(bitmap, i))
            {
                i++;
                continue;
            }
            break;
        }
        if (i >= total)
            break;

        uint32_t start = i;
        while (i < total)
        {
            if ((i & 7) == 0 && i + 8 <= total && bitmap[i / 8] == 0x00)
            {
                i += 8;
                continue;
            }
            if (!bitmap_test_bit(bitmap, i))
            {
                i++;
                continue;
            }
            break;
        }

        FreeExtent *ext = kmalloc(sizeof(FreeExtent));
        if (!ext)
        {
            debug_printf("[TagFS] free_list_build: kmalloc failed\n");
            break;
        }
        ext->start = start;
        ext->count = i - start;
        ext->next = NULL;
        *tail = ext;
        tail = &ext->next;
        g_state.block_bitmap.extent_count++;
    }
}

// ----------------------------------------------------------------------------
// System behavior tag helpers
// ----------------------------------------------------------------------------

// Check if a file has trashed or hidden tag. skip_trashed/skip_hidden control
// which behavior tags to check (allows callers to exempt explicitly-queried tags).
static bool file_has_system_behavior_tag(uint32_t file_id,
                                         bool skip_trashed, bool skip_hidden)
{
    if (!skip_trashed && !skip_hidden)
        return false;

    uint16_t trashed_id = g_wk.trashed
                              ? (uint16_t)__builtin_ctzll(g_wk.trashed)
                              : TAGFS_INVALID_TAG_ID;
    uint16_t hidden_id = g_wk.hidden
                             ? (uint16_t)__builtin_ctzll(g_wk.hidden)
                             : TAGFS_INVALID_TAG_ID;

    int count = tag_bitmap_tag_count_for_file(g_state.bitmap_index, file_id);
    if (count <= 0)
        return false;

    uint16_t *file_tags = kmalloc(sizeof(uint16_t) * (uint32_t)count);
    if (!file_tags)
        return true;

    int actual = tag_bitmap_tags_for_file(g_state.bitmap_index, file_id, file_tags, (uint32_t)count);

    bool found = false;
    for (int i = 0; i < actual; i++)
    {
        if (skip_trashed && file_tags[i] == trashed_id)
        {
            found = true;
            break;
        }
        if (skip_hidden && file_tags[i] == hidden_id)
        {
            found = true;
            break;
        }
    }

    kfree(file_tags);
    return found;
}

// ----------------------------------------------------------------------------
// tagfs_init_well_known_tags
// ----------------------------------------------------------------------------

static void register_well_known(uint64_t *field, TagRegistry *reg, const char *key)
{
    uint16_t tid = tag_registry_intern(reg, key, NULL);
    *field = (tid != TAGFS_INVALID_TAG_ID && tid < 64) ? (1ULL << tid) : 0;
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

/* True iff `key` is one of the reserved kernel-owned tag keys (TagFsReservedKeys[]).
 * String compare on the bare key, so it catches both "system" and "system:foo"
 * (callers split key at ':' before calling). Works regardless of registry/mount
 * state — the reserved vocabulary is a compile-time constant. */
bool tagfs_key_is_reserved(const char *key)
{
    if (!key) return false;
    for (size_t i = 0; i < sizeof(TagFsReservedKeys) / sizeof(TagFsReservedKeys[0]); i++)
        if (strcmp(key, TagFsReservedKeys[i]) == 0) return true;
    return false;
}

void tagfs_init_well_known_tags(void)
{
    memset(&g_well_known, 0, sizeof(g_well_known));
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry)
        return;
    TagRegistry *reg = fs->registry;

    /* trashed/hidden are membership filters for file listing (not security), so
     * they stay keyed by the registry id (1ULL<<tag_id). The 7 auth-privilege
     * tags are no longer registered here — their op-authority is the FIXED
     * cabin_t.auth_bits (auth_tags.h), independent of the registry id. */
    register_well_known(&g_wk.trashed, reg, "trashed");
    register_well_known(&g_wk.hidden, reg, "hidden");

    // Stamp the reserved vocabulary as system tags. Runs after tag_registry_load,
    // so it self-heals over whatever flags the disk carried; mark_system sets no
    // dirty flag, so this re-derivation never forces a write.
    for (size_t i = 0; i < sizeof(TagFsReservedKeys) / sizeof(TagFsReservedKeys[0]); i++) {
        uint16_t tid = tag_registry_lookup(reg, TagFsReservedKeys[i], NULL);
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
     * table itself; the write serialisation it exists to provide is what goes.
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

    kprintf("[TagFS] %u data blocks, %llu free, %llu files\n",
            g_state.layout.data_blocks,
            (unsigned long long)g_state.ledger.free_blocks,
            (unsigned long long)g_state.ledger.total_files);

    /* --- DiskBook init (CoW redirect log). Replay is deferred until AFTER
     *     CoW init + manifest restore below, so restored redirects attach to
     *     the live snapshots they belong to.
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

    // --- Restore CoW snapshots from on-disk manifest ---
    {
        uint32_t cow_sector = g_state.ledger.cow_manifest_block;
        if (cow_sector != 0) {
            CowManifest *manifest = kmalloc(sizeof(CowManifest));
            if (manifest) {
                if (tagfs_read_block(cow_sector, manifest) == OK &&
                    manifest->magic == COW_MANIFEST_MAGIC &&
                    manifest->count <= COW_MANIFEST_MAX) {
                    for (uint32_t i = 0; i < manifest->count; i++) {
                        CowSnapshotDisk *src = &manifest->entries[i];
                        CowSnapshot snap;
                        memset(&snap, 0, sizeof(snap));
                        snap.snapshot_id          = src->snapshot_id;
                        snap.parent_file_id       = src->parent_file_id;
                        snap.created_time         = src->created_time;
                        snap.disk_book_checkpoint = (uint64_t)src->disk_book_checkpoint;
                        snap.file_count           = src->file_count;
                        snap.total_size           = src->total_size;
                        snap.flags                = src->flags;
                        memcpy(snap.name, src->name, TAGFS_SNAPSHOT_NAME_LEN);
                        TagFS_CowRestoreSnapshot(&snap);
                    }
                    debug_printf("[TagFS] Restored %u CoW snapshots from disk\n",
                                 manifest->count);
                } else {
                    debug_printf("[TagFS] CoW manifest missing or invalid — starting fresh\n");
                }
                kfree(manifest);
            }
        }
    }

    // --- DiskBook replay: restore durable CoW redirects into the snapshots
    //     just loaded from the manifest. Idempotent + CRC-verified. Runs after
    //     TagFS_CowInit + manifest restore so the target snapshots exist.
    if (DiskBookIsInitialized())
    {
        DiskBookValidateAndReplay();
    }

    // --- Data Deduplication init ---
    if (TagFS_DedupInit() != OK) {
        return mount_refused("its deduplication index would not start",
                             ERR_DEDUP_NOT_INITIALIZED);
    }

    // --- Self-Healing init ---
    TagFS_SelfHealInit();

    // --- Bcdc Compression init ---
    if (BcdcInit() != OK) {
        return mount_refused("its compression dictionaries would not start",
                             ERR_NO_MEMORY);
    }

    // --- Test Framework init ---
    if (TagFS_TestsInit() != OK) {
        return mount_refused("its test framework would not start",
                             ERR_NO_MEMORY);
    }

    /*
     * ‼ THE FAILURE THAT COST A BOARD ITS FILESYSTEM, ON COMMAND.
     *
     * Every refusal from here down leaves five subsystems standing — DiskBook,
     * CoW, Dedup, Self-Heal, Bcdc and the test framework — and until
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

    // --- File Table ---
    if (file_table_init(data_block_of(g_state.layout.file_table_block), g_state.layout.file_table_blocks) != OK) {
        return mount_refused("its file table would not read",
                             ERR_FILE_TABLE_CORRUPT);
    }

    // --- Metadata Pool ---
    if (meta_pool_init(data_block_of(g_state.layout.metadata_pool_block), g_state.layout.metadata_pool_blocks) != OK) {
        return mount_refused("its metadata pool would not read",
                             ERR_METADATA_POOL_FULL);
    }

    // --- Bitmap Index ---
    g_state.bitmap_index = tag_bitmap_create(TAGFS_BITMAP_INITIAL_TAG_CAP, TAGFS_BITMAP_INITIAL_FILE_CAP);
    if (!g_state.bitmap_index) {
        return mount_refused("there is no memory for its tag index",
                             ERR_NO_MEMORY);
    }

    // --- Block Bitmap ---
    uint32_t bitmap_bytes = (g_state.layout.data_blocks + 7) / 8;
    g_state.block_bitmap.bitmap = kmalloc(bitmap_bytes);
    if (!g_state.block_bitmap.bitmap)
    {
        return mount_refused("there is no memory for its block bitmap",
                             ERR_NO_MEMORY);
    }
    memset(g_state.block_bitmap.bitmap, 0, bitmap_bytes);
    g_state.block_bitmap.total_blocks = g_state.layout.data_blocks;
    g_state.block_bitmap.free_list = NULL;
    g_state.block_bitmap.extent_count = 0;

    // Read block bitmap from disk
    uint32_t bm_sector_count = (g_state.layout.block_bitmap_blocks * TAGFS_BLOCK_SECTORS);
    uint32_t bm_buf_size = bm_sector_count * TAGFS_SECTOR_SIZE;
    uint8_t *bm_buf = kmalloc(bm_buf_size);
    if (!bm_buf)
    {
        return mount_refused("there is no memory to read its block bitmap",
                             ERR_NO_MEMORY);
    }

    /*
     * ‼ A bitmap that would not read is NOT an empty bitmap.
     *
     * This used to warn and carry on with the all-zero buffer it had just
     * allocated — and an all-zero bitmap says every block of the volume is
     * free. The next allocation then hands out block 0, which is the tag
     * registry, then the file table, then the metadata pool, and writes over
     * all three. On the following boot the pool reads back as somebody else's
     * bytes, the mount decides the volume is empty, and every file on it is
     * gone.
     *
     * Measured on the board: a flash drive whose controller dropped a command
     * mid-boot came up once with everything working, and the boot after that
     * had no files on it at all. Nothing was said either time, because the
     * warning was a debug_printf and those compile to nothing in a shipped
     * build.
     *
     * So: a volume whose bitmap cannot be read is a volume this kernel will
     * not mount. Refusing costs the machine its filesystem for that boot;
     * carrying on costs it the filesystem permanently.
     */
    if (disk_read_sectors((uint64_t)((uint64_t)g_state.layout.block_bitmap_block *
                                     TAGFS_BLOCK_SECTORS),
                          (uint16_t)bm_sector_count, bm_buf) != 0)
    {
        kfree(bm_buf);
        return mount_refused("its block bitmap would not read, and a missing "
                             "bitmap reads as a volume with nothing on it — "
                             "the next write would take everything",
                             ERR_TAGFS_CORRUPTED);
    }

    uint32_t copy_bytes = bitmap_bytes < bm_buf_size ? bitmap_bytes : bm_buf_size;
    memcpy(g_state.block_bitmap.bitmap, bm_buf, copy_bytes);
    kfree(bm_buf);

    free_list_build();

    // --- Mount-time fsck: cross-check file extents vs bitmap ---
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

            // Scan all files and mark their extents
            uint32_t files_checked = 0;
            uint32_t orphan_blocks = 0;
            uint32_t missing_blocks = 0;

            for (uint32_t fid = 1; fid < g_state.ledger.next_file_id; fid++)
            {
                uint32_t mb, mo;
                if (file_table_lookup(fid, &mb, &mo) != 0)
                    continue;

                TagFSMetadata meta;
                memset(&meta, 0, sizeof(meta));
                if (meta_pool_read(mb, mo, &meta) != 0)
                {
                    // Orphaned file_table entry — metadata is gone or corrupted.
                    // Clean up the dangling reference.
                    file_table_delete(fid);
                    continue;
                }

                if (!(meta.flags & TAGFS_FILE_ACTIVE))
                {
                    // File was marked for deletion (journal replay or incomplete delete).
                    // Complete the cleanup: remove file_table entry and free blocks.
                    debug_printf("[TagFS FSCK] File %u has ACTIVE=0 — completing cleanup\n", fid);
                    file_table_delete(fid);
                    // Don't mark extents in computed_bm → blocks will be reclaimed
                    tagfs_metadata_free(&meta);
                    continue;
                }

                for (uint16_t e = 0; e < meta.extent_count; e++)
                {
                    uint32_t start = meta.extents[e].start_block;
                    uint32_t count = meta.extents[e].block_count;
                    for (uint32_t b = start; b < start + count && b < g_state.layout.data_blocks; b++)
                    {
                        bitmap_set_bit(computed_bm, b);
                    }
                }
                files_checked++;

                // Rebuild bitmap index from disk metadata
                for (uint16_t t = 0; t < meta.tag_count; t++)
                {
                    tag_bitmap_set(g_state.bitmap_index, meta.tag_ids[t], fid);
                }

                tagfs_metadata_free(&meta);
            }

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
             * the per-subsystem init's do. */
            #define MARK_CHAIN_BLOCKS(first_block, expected_magic) do {       \
                uint32_t _cb = (first_block);                                  \
                uint32_t _hops = 0;                                            \
                while (_cb != 0 && _hops < g_state.layout.data_blocks) {                  \
                    if (_cb < g_state.layout.data_blocks)                                 \
                        bitmap_set_bit(computed_bm, _cb);                      \
                    uint8_t _bbuf[TAGFS_BLOCK_SIZE];                           \
                    if (tagfs_read_block(_cb, _bbuf) != OK) break;             \
                    uint32_t _mg, _nx;                                         \
                    memcpy(&_mg, _bbuf + 0, 4);                                \
                    memcpy(&_nx, _bbuf + 4, 4);                                \
                    if (_mg != (expected_magic)) break;                        \
                    _cb = _nx;                                                 \
                    _hops++;                                                   \
                }                                                              \
            } while (0)
            MARK_CHAIN_BLOCKS(data_block_of(g_state.layout.tag_registry_block),    TAGFS_REGISTRY_MAGIC);
            MARK_CHAIN_BLOCKS(data_block_of(g_state.layout.file_table_block),      TAGFS_FILETBL_MAGIC);
            MARK_CHAIN_BLOCKS(data_block_of(g_state.layout.metadata_pool_block),   TAGFS_MPOOL_MAGIC);
            #undef MARK_CHAIN_BLOCKS

            // Integrity-map blocks are used on disk but referenced by no file —
            // mark them so they are not treated as orphans or reused.
            IntegrityMarkMapBlocks(computed_bm, g_state.layout.data_blocks);

            // Compare computed vs on-disk bitmap
            for (uint32_t b = 0; b < g_state.layout.data_blocks; b++)
            {
                bool on_disk = bitmap_test_bit(g_state.block_bitmap.bitmap, b);
                bool computed = bitmap_test_bit(computed_bm, b);
                if (on_disk && !computed)
                    orphan_blocks++;
                if (!on_disk && computed)
                    missing_blocks++;
            }

            if (orphan_blocks > 0 || missing_blocks > 0)
            {
                debug_printf("[TagFS FSCK] Bitmap mismatch: %u orphan, %u missing — additive repair\n",
                             orphan_blocks, missing_blocks);
                /* Additive repair: union (on_disk OR computed). Never
                 * release a block on-disk says is used — the chain
                 * walk above is best-effort and a subsystem we don't
                 * yet enumerate (CoW snapshots, disk_book journal,
                 * future vDSO-style) might also legitimately own a
                 * block that on-disk bitmap correctly captured but
                 * we don't reproduce. Erring towards "keep used" is
                 * safe; the worst case is a slow, leaky allocator
                 * that fsck will eventually resolve as files come
                 * and go. The opposite (release-into-allocator a
                 * block that's still live metadata) corrupts the
                 * filesystem. */
                for (uint32_t b = 0; b < g_state.layout.data_blocks; b++) {
                    if (bitmap_test_bit(computed_bm, b))
                        bitmap_set_bit(g_state.block_bitmap.bitmap, b);
                }
                free_list_build();

                // Count actual free blocks
                uint32_t used = 0;
                for (uint32_t b = 0; b < g_state.layout.data_blocks; b++)
                {
                    if (bitmap_test_bit(g_state.block_bitmap.bitmap, b))
                        used++;
                }
                g_state.ledger.free_blocks = g_state.layout.data_blocks - used;
            }

            debug_printf("[TagFS FSCK] Checked %u files, bitmap %s\n",
                         files_checked,
                         (orphan_blocks == 0 && missing_blocks == 0) ? "OK" : "repaired");
            kfree(computed_bm);
        }
    }

    // Populate well-known tag bitmasks for O(1) checks
    tagfs_init_well_known_tags();

    // Load mirror cache so future meta reads skip disk
    meta_pool_mirror_init(g_state.ledger.next_file_id + 64);

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

    TagFSHoldGroundOpenFile();      /* nothing at all unless HOLDGROUND=on */
    return 0;
}

// ----------------------------------------------------------------------------
// tagfs_sync / tagfs_shutdown
// ----------------------------------------------------------------------------

static void tagfs_sync_inside(void);

void tagfs_sync(void)
{
    if (!tagfs_enter())
        return;
    tagfs_sync_inside();
    tagfs_leave();
}

static void tagfs_sync_inside(void)
{
    tag_registry_flush(g_state.registry);
    file_table_flush();
    meta_pool_flush();
    IntegrityFlush();
    IntegrityDrainReports();   // publish any pending bit-rot Touch events

    // Write block bitmap to disk
    uint32_t bitmap_bytes = (g_state.layout.data_blocks + 7) / 8;
    uint32_t sector_count = (g_state.layout.block_bitmap_blocks * TAGFS_BLOCK_SECTORS);
    uint32_t bm_buf_size = sector_count * TAGFS_SECTOR_SIZE;
    uint8_t *bm_buf = kmalloc(bm_buf_size);
    if (bm_buf)
    {
        memset(bm_buf, 0, bm_buf_size);
        uint32_t copy_bytes = bitmap_bytes < bm_buf_size ? bitmap_bytes : bm_buf_size;
        memcpy(bm_buf, g_state.block_bitmap.bitmap, copy_bytes);
        if (disk_write_sectors(((uint64_t)g_state.layout.block_bitmap_block * TAGFS_BLOCK_SECTORS),
                               (uint16_t)sector_count, bm_buf) != 0)
        {
            debug_printf("[TagFS] sync: failed to write block bitmap\n");
        }
        kfree(bm_buf);
    }

    tagfs_write_ledger();

    /* Drive write-cache flush.
     *
     * NCQ WRITE FPDMA QUEUED returns "received" — bytes may still be
     * in the on-disk write cache (16-256 MiB) when the command
     * completes. Without an explicit FLUSH CACHE after the sync
     * sequence, a power cut between this point and the next disk
     * write loses the superblock + bitmap + journal commit records
     * we just produced — the next mount sees a corrupted file
     * system. tagfs_flush_cache() dispatches FLUSH CACHE EXT to the
     * *probed* device (the AHCI port / ATA drive that actually holds
     * the volume — never a hardcoded port 0) and only returns when the
     * drive's write cache has reached the media.
     *
     * Cost: ~5-15 ms per call on a spinning drive, ~50 µs on NVMe
     * via SATA. tagfs_sync is called on user-initiated shutdown
     * and on metadata-pool checkpoint, both rare. */
    tagfs_flush_cache();
}

/*
 * ‼ EVERYTHING THIS VOLUME BROUGHT UP, PUT BACK DOWN — INCLUDING HALF OF IT.
 *
 * This is the piece that did not exist, and its absence is what turned one
 * transient read error into a machine with no filesystem for the rest of its
 * boot. tagfs_init brings up eleven things in order and any of them may refuse;
 * when one did, the ten before it were left standing and nothing anywhere took
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
 * the same one any more. That is why the two flushing shutdowns are told so
 * explicitly rather than being relied upon to find the medium gone.
 */
static void tagfs_clear_the_ground(void)
{
    /* The registry is about to go, and with it every number it issued. The
     * Use Context holds some of them for the scheduler; told first, so no
     * dispatch decides on a page of a book that no longer exists. */
    UseContextUnbind();

    /* What this volume remembered goes with it; the next one is read, not
     * assumed. */
    spin_lock(&g_ledger_lock);
    g_ledger_use_len = 0;
    spin_unlock(&g_ledger_lock);

    if (g_state.registry) {
        tag_registry_destroy(g_state.registry);
        kfree(g_state.registry);
        g_state.registry = NULL;
    }

    if (g_state.bitmap_index) {
        tag_bitmap_destroy(g_state.bitmap_index);
        g_state.bitmap_index = NULL;
    }

    file_table_shutdown(false);
    meta_pool_shutdown(false);

    free_list_destroy();
    if (g_state.block_bitmap.bitmap)
    {
        kfree(g_state.block_bitmap.bitmap);
        g_state.block_bitmap.bitmap = NULL;
    }
    g_state.block_bitmap.total_blocks = 0;

    TagFS_CowShutdown();
    TagFS_DedupShutdown();
    TagFS_SelfHealShutdown();
    BraidShutdown();
    BcdcShutdown();
    IntegrityShutdown();
    TagFS_TestsShutdown();
    /* Last, so everything above it has been journaled by the time it goes. */
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
 * ‼ THE MACHINE IS STOPPING, AND THAT IS A DIFFERENT THING ENTIRELY.
 *
 * What has to happen is that memory reaches the medium; what must NOT happen
 * is that memory somebody is still reading is handed back to an allocator no
 * one will ever ask again. So this writes everything back through the ordinary
 * door — with the volume still up, so the flushes below it are allowed to work
 * — and then shuts the door and stops. Nothing is freed.
 *
 * Freeing at a halt was never for the machine's benefit; it was tidiness, and
 * it is the one road on which the take-down cannot wait for anybody.
 */
void tagfs_shutdown(void)
{
    if (!g_state.initialized) {
        return;
    }

    tagfs_sync();

    __atomic_store_n(&g_door, (uint8_t)VOLUME_LEAVING, __ATOMIC_RELEASE);
    debug_printf("[TagFS] Shutdown complete\n");
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
 * The ground could not be cleared when the medium left, because somebody was
 * standing on it. Finish it here, and then let the ordinary road take the
 * volume back up.
 *
 * Called from the guide loop and from the idle loop — the two places in this
 * kernel that are allowed to wait and are reached often — and it is one atomic
 * load per half when there is nothing to do, which is every iteration but a
 * handful in the life of a machine.
 */
void TagFSServiceIfPending(void)
{
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

    /* kprintf, not debug_printf: the per-test lines inside the runner are
     * debug_printf and vanish in a normal build, so without these numbers a run
     * where every single test SKIPPED still printed "All tests PASSED". */
    kprintf("[TESTS] TagFS: %u run, %u passed, %u failed, %u skipped\n",
            stats.total_tests, stats.total_passed,
            stats.total_failed, stats.total_skipped);

    return result;
}

// ----------------------------------------------------------------------------
// tagfs_create_file
// ----------------------------------------------------------------------------

static int tagfs_create_file_inside(const char *filename, const uint16_t *tag_ids, uint16_t tag_count,
                                    uint32_t *out_file_id);

int tagfs_create_file(const char *filename, const uint16_t *tag_ids, uint16_t tag_count,
                      uint32_t *out_file_id)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_create_file_inside(filename, tag_ids, tag_count, out_file_id);
    tagfs_leave();
    return rc;
}

static int tagfs_create_file_inside(const char *filename, const uint16_t *tag_ids, uint16_t tag_count,
                                    uint32_t *out_file_id)
{
    if (!filename || !out_file_id)
        return -1;

    spin_lock(&g_state.lock);

    uint32_t file_id = g_state.ledger.next_file_id++;

    // Derive auto-label tag from filename stem (e.g. "kernel.bin" → "kernel")
    char stem[128];
    {
        const char *dot = NULL;
        size_t flen = strlen(filename);
        for (size_t i = flen; i > 0; i--)
        {
            if (filename[i - 1] == '.')
            {
                dot = filename + i - 1;
                break;
            }
        }
        size_t slen = dot ? (size_t)(dot - filename) : flen;
        if (slen >= sizeof(stem))
            slen = sizeof(stem) - 1;
        memcpy(stem, filename, slen);
        stem[slen] = '\0';
    }

    // Intern the auto-label tag (lock is already held, call intern directly)
    uint16_t auto_tag = TAGFS_INVALID_TAG_ID;
    if (stem[0] != '\0' && g_state.registry)
    {
        // Release state lock briefly to avoid lock ordering issues with registry
        spin_unlock(&g_state.lock);
        auto_tag = tag_registry_intern(g_state.registry, stem, NULL);
        // Flush registry to disk if a new tag was created (crash safety)
        if (tag_registry_is_dirty())
        {
            tag_registry_flush(g_state.registry);
        }
        spin_lock(&g_state.lock);
    }

    // Build deduplicated tag array: auto-label first, then caller's tags
    uint16_t final_count = 0;
    uint16_t *final_tags = NULL;
    {
        uint16_t max_tags = tag_count + 1;
        final_tags = kmalloc(sizeof(uint16_t) * max_tags);
        if (!final_tags)
        {
            debug_printf("[TagFS] create_file: kmalloc for final_tags failed\n");
            spin_unlock(&g_state.lock);
            return -1;
        }

        // Add auto-label tag first
        if (auto_tag != TAGFS_INVALID_TAG_ID)
        {
            final_tags[final_count++] = auto_tag;
        }

        // Add caller's tags, skipping duplicates
        for (uint16_t i = 0; i < tag_count; i++)
        {
            if (!tag_ids)
                break;
            uint16_t tid = tag_ids[i];
            bool dup = false;
            for (uint16_t j = 0; j < final_count; j++)
            {
                if (final_tags[j] == tid)
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
            {
                final_tags[final_count++] = tid;
            }
        }
    }

    // Build metadata
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.file_id = file_id;
    meta.flags = TAGFS_FILE_ACTIVE;
    meta.size = 0;
    meta.created_time = 0;
    meta.modified_time = 0;
    meta.extent_count = 0;
    meta.extents = NULL;

    size_t name_len = strlen(filename);
    meta.filename = kmalloc(name_len + 1);
    if (!meta.filename)
    {
        debug_printf("[TagFS] create_file: kmalloc for filename failed\n");
        kfree(final_tags);
        spin_unlock(&g_state.lock);
        return -1;
    }
    memcpy(meta.filename, filename, name_len + 1);

    meta.tag_count = final_count;
    meta.tag_ids = final_tags;

    // No journal for create: metadata is NEW (no previous on-disk state to protect).
    // Write ordering ensures consistency:
    //   1. meta_pool_write (append-only, crash → orphaned record, fsck cleans up)
    //   2. file_table_update (crash → table points to valid metadata, counters off → fsck fixes)
    //   3. superblock update (eventual)

    // Write to metadata pool
    uint32_t meta_block, meta_offset;
    if (meta_pool_write(&meta, &meta_block, &meta_offset) != 0)
    {
        debug_printf("[TagFS] create_file: meta_pool_write failed for file_id=%u\n", file_id);
        kfree(meta.tag_ids);
        kfree(meta.filename);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Update file table
    if (file_table_update(file_id, meta_block, meta_offset) != 0)
    {
        debug_printf("[TagFS] create_file: file_table_update failed for file_id=%u\n", file_id);
        kfree(meta.tag_ids);
        kfree(meta.filename);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Add to bitmap index (use final_count, the deduplicated count)
    for (uint16_t i = 0; i < final_count; i++)
    {
        if (meta.tag_ids[i] != TAGFS_INVALID_TAG_ID)
        {
            tag_bitmap_set(g_state.bitmap_index, meta.tag_ids[i], file_id);
        }
    }

    g_state.ledger.total_files++;

    *out_file_id = file_id;

    kfree(meta.tag_ids);
    kfree(meta.filename);

    spin_unlock(&g_state.lock);

    debug_printf("[TagFS] Created file '%s' file_id=%u\n", filename, file_id);
    return 0;
}

// Forward declaration
static void tagfs_free_blocks_internal(uint32_t start_block, uint32_t count);

// ----------------------------------------------------------------------------
// tagfs_delete_file
// ----------------------------------------------------------------------------

static int tagfs_delete_file_inside(uint32_t file_id);

int tagfs_delete_file(uint32_t file_id)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_delete_file_inside(file_id);
    tagfs_leave();
    return rc;
}

static int tagfs_delete_file_inside(uint32_t file_id)
{
    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Read metadata to get extents and tags for cleanup
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    int has_meta = meta_pool_read(meta_block, meta_offset, &meta);

    // Cleanup order: bitmap → file_table → blocks → metadata pool.
    // file_table_delete before block_free ensures that on crash,
    // fsck won't find a file referencing freed (and possibly reused) blocks.

    // Remove from bitmap index
    tag_bitmap_remove_file(g_state.bitmap_index, file_id);

    // Delete from file table (commit point: file officially gone)
    file_table_delete(file_id);

    // Free data blocks if we could read metadata
    if (has_meta == 0 && meta.extents)
    {
        for (uint16_t i = 0; i < meta.extent_count; i++)
        {
            tagfs_free_blocks_internal(meta.extents[i].start_block, meta.extents[i].block_count);
        }
        tagfs_metadata_free(&meta);
    }

    // Delete from metadata pool (last: safe to zero after file_table is gone)
    meta_pool_delete(meta_block, meta_offset);

    if (g_state.ledger.total_files > 0)
    {
        g_state.ledger.total_files--;
    }

    spin_unlock(&g_state.lock);

    debug_printf("[TagFS] Deleted file_id=%u\n", file_id);
    return 0;
}

// ----------------------------------------------------------------------------
// tagfs_rename_file
// ----------------------------------------------------------------------------

static int tagfs_rename_file_inside(uint32_t file_id, const char *new_filename);

int tagfs_rename_file(uint32_t file_id, const char *new_filename)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_rename_file_inside(file_id, new_filename);
    tagfs_leave();
    return rc;
}

static int tagfs_rename_file_inside(uint32_t file_id, const char *new_filename)
{
    if (!new_filename)
        return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
    {
        debug_printf("[TagFS] rename_file: failed to read metadata for file_id=%u\n", file_id);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Replace filename
    if (meta.filename)
        kfree(meta.filename);
    size_t len = strlen(new_filename);
    meta.filename = kmalloc(len + 1);
    if (!meta.filename)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }
    memcpy(meta.filename, new_filename, len + 1);

    uint32_t new_block, new_offset;
    if (meta_pool_write(&meta, &new_block, &new_offset) != 0)
    {
        debug_printf("[TagFS] rename_file: meta_pool_write failed\n");
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }

    file_table_update(file_id, new_block, new_offset);
    meta_pool_delete(meta_block, meta_offset);

    tagfs_metadata_free(&meta);

    spin_unlock(&g_state.lock);
    return 0;
}

// ----------------------------------------------------------------------------
// Tag operations
// ----------------------------------------------------------------------------

static int tagfs_add_tag_inside(uint32_t file_id, uint16_t tag_id);

int tagfs_add_tag(uint32_t file_id, uint16_t tag_id)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_add_tag_inside(file_id, tag_id);
    tagfs_leave();
    return rc;
}

static int tagfs_add_tag_inside(uint32_t file_id, uint16_t tag_id)
{
    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Check if tag already present
    for (uint16_t i = 0; i < meta.tag_count; i++)
    {
        if (meta.tag_ids[i] == tag_id)
        {
            tagfs_metadata_free(&meta);
            spin_unlock(&g_state.lock);
            return 0;
        }
    }

    // Grow tag list
    uint16_t new_count = meta.tag_count + 1;
    uint16_t *new_ids = kmalloc(sizeof(uint16_t) * new_count);
    if (!new_ids)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }
    if (meta.tag_ids)
    {
        memcpy(new_ids, meta.tag_ids, sizeof(uint16_t) * meta.tag_count);
    }
    new_ids[meta.tag_count] = tag_id;
    kfree(meta.tag_ids);
    meta.tag_ids = new_ids;
    meta.tag_count = new_count;

    uint32_t new_block, new_offset;
    int r = meta_pool_write(&meta, &new_block, &new_offset);
    if (r != 0)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return r;
    }
    file_table_update(file_id, new_block, new_offset);
    tag_bitmap_set(g_state.bitmap_index, tag_id, file_id);
    meta_pool_delete(meta_block, meta_offset);

    tagfs_metadata_free(&meta);
    spin_unlock(&g_state.lock);
    return 0;
}

static int tagfs_remove_tag_inside(uint32_t file_id, uint16_t tag_id);

int tagfs_remove_tag(uint32_t file_id, uint16_t tag_id)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_remove_tag_inside(file_id, tag_id);
    tagfs_leave();
    return rc;
}

static int tagfs_remove_tag_inside(uint32_t file_id, uint16_t tag_id)
{
    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Find and remove
    bool found = false;
    for (uint16_t i = 0; i < meta.tag_count; i++)
    {
        if (meta.tag_ids[i] == tag_id)
        {
            meta.tag_ids[i] = meta.tag_ids[meta.tag_count - 1];
            meta.tag_count--;
            found = true;
            break;
        }
    }

    if (!found)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }

    uint32_t new_block, new_offset;
    int r = meta_pool_write(&meta, &new_block, &new_offset);
    if (r != 0)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return r;
    }
    file_table_update(file_id, new_block, new_offset);
    tag_bitmap_clear(g_state.bitmap_index, tag_id, file_id);
    meta_pool_delete(meta_block, meta_offset);

    tagfs_metadata_free(&meta);
    spin_unlock(&g_state.lock);
    return 0;
}

static bool tagfs_has_tag_inside(uint32_t file_id, uint16_t tag_id);

bool tagfs_has_tag(uint32_t file_id, uint16_t tag_id)
{
    if (!tagfs_enter())
        return false;
    bool rc = tagfs_has_tag_inside(file_id, tag_id);
    tagfs_leave();
    return rc;
}

static bool tagfs_has_tag_inside(uint32_t file_id, uint16_t tag_id)
{
    TagBitmapIndex *idx = g_state.bitmap_index;
    if (!idx)
        return false;

    spin_lock(&idx->lock);

    if ((uint32_t)tag_id >= idx->bitmap_capacity || !idx->bitmaps[tag_id])
    {
        spin_unlock(&idx->lock);
        return false;
    }

    TagBitmap *bm = idx->bitmaps[tag_id];
    uint32_t byte_idx = file_id / 8;
    uint32_t bit_idx = file_id % 8;
    uint32_t bm_bytes = (bm->bit_count + 7) / 8;

    bool result = (byte_idx < bm_bytes) &&
                  (bm->bits[byte_idx] & (1 << bit_idx));

    spin_unlock(&idx->lock);
    return result;
}

static int tagfs_add_tag_string_inside(uint32_t file_id, const char *key, const char *value);

int tagfs_add_tag_string(uint32_t file_id, const char *key, const char *value)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_add_tag_string_inside(file_id, key, value);
    tagfs_leave();
    return rc;
}

static int tagfs_add_tag_string_inside(uint32_t file_id, const char *key, const char *value)
{
    if (!key)
        return -1;
    uint16_t tag_id = tag_registry_intern(g_state.registry, key, value);
    if (tag_id == TAGFS_INVALID_TAG_ID)
        return -1;
    // Flush registry to disk if a new tag was created (crash safety)
    if (tag_registry_is_dirty())
    {
        tag_registry_flush(g_state.registry);
    }
    return tagfs_add_tag(file_id, tag_id);
}

static int tagfs_remove_tag_string_inside(uint32_t file_id, const char *key);

int tagfs_remove_tag_string(uint32_t file_id, const char *key)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_remove_tag_string_inside(file_id, key);
    tagfs_leave();
    return rc;
}

static int tagfs_remove_tag_string_inside(uint32_t file_id, const char *key)
{
    if (!key)
        return -1;
    /* Remove by key. The registry keys tags by (key,value), so a key-only
     * registry lookup cannot identify a key:value tag — and a bare "key" tag
     * created by another file would mis-resolve here. Instead drop the file's
     * own tag(s) whose key matches, regardless of value (unset "color" drops
     * "color:red" and a bare "color" alike). */
    int count = tag_bitmap_tag_count_for_file(g_state.bitmap_index, file_id);
    if (count <= 0)
        return -1;

    uint16_t *file_tags = kmalloc(sizeof(uint16_t) * (uint32_t)count);
    if (!file_tags)
        return -1;

    int actual  = tag_bitmap_tags_for_file(g_state.bitmap_index, file_id,
                                           file_tags, (uint32_t)count);
    int removed = 0;
    for (int i = 0; i < actual; i++) {
        const char *k = tag_registry_key(g_state.registry, file_tags[i]);
        if (k && strcmp(k, key) == 0 && tagfs_remove_tag(file_id, file_tags[i]) == 0)
            removed++;
    }

    kfree(file_tags);
    return removed > 0 ? 0 : -1;
}

static bool tagfs_has_tag_string_inside(uint32_t file_id, const char *key, const char *value);

bool tagfs_has_tag_string(uint32_t file_id, const char *key, const char *value)
{
    if (!tagfs_enter())
        return false;
    bool rc = tagfs_has_tag_string_inside(file_id, key, value);
    tagfs_leave();
    return rc;
}

static bool tagfs_has_tag_string_inside(uint32_t file_id, const char *key, const char *value)
{
    if (!key)
        return false;
    uint16_t tag_id = tag_registry_lookup(g_state.registry, key, value);
    if (tag_id == TAGFS_INVALID_TAG_ID)
        return false;
    return tagfs_has_tag(file_id, tag_id);
}

// ----------------------------------------------------------------------------
// Query
// ----------------------------------------------------------------------------

static int tagfs_query_files_inside(const char *query_strings[], uint32_t count,
                                    uint32_t *out_file_ids, uint32_t max_results);

int tagfs_query_files(const char *query_strings[], uint32_t count,
                      uint32_t *out_file_ids, uint32_t max_results)
{
    if (!tagfs_enter())
        return 0;
    int rc = tagfs_query_files_inside(query_strings, count, out_file_ids, max_results);
    tagfs_leave();
    return rc;
}

static int tagfs_query_files_inside(const char *query_strings[], uint32_t count,
                                    uint32_t *out_file_ids, uint32_t max_results)
{
    if (!query_strings || count == 0 || !out_file_ids || max_results == 0)
        return 0;

    uint16_t *tag_ids = kmalloc(sizeof(uint16_t) * count);
    if (!tag_ids)
        return 0;

    TagKeyGroup **groups = kmalloc(sizeof(TagKeyGroup *) * count);
    if (!groups)
    {
        kfree(tag_ids);
        return 0;
    }

    uint32_t tag_count = 0;
    uint32_t group_count = 0;

    for (uint32_t i = 0; i < count; i++)
    {
        const char *qs = query_strings[i];
        if (!qs)
            continue;

        char key[256];
        char value[256];
        tagfs_parse_tag(qs, key, sizeof(key), value, sizeof(value));

        // Check for wildcard (value == "...")
        if (strcmp(value, "...") == 0)
        {
            TagKeyGroup *grp = tag_registry_key_group(g_state.registry, key);
            if (grp)
            {
                groups[group_count++] = grp;
            }
        }
        else
        {
            uint16_t tid = tag_registry_lookup(g_state.registry, key,
                                               value[0] ? value : NULL);
            if (tid == TAGFS_INVALID_TAG_ID)
            {
                // Tag not in registry — no files can match
                kfree(tag_ids);
                kfree(groups);
                return 0;
            }
            tag_ids[tag_count++] = tid;
        }
    }

    int result = tag_bitmap_query(g_state.bitmap_index,
                                  tag_ids, tag_count,
                                  groups, group_count,
                                  out_file_ids, max_results);

    // Post-filter: remove trashed/hidden files unless explicitly queried
    if (result > 0)
    {
        uint16_t trashed_id = g_wk.trashed
                                  ? (uint16_t)__builtin_ctzll(g_wk.trashed)
                                  : TAGFS_INVALID_TAG_ID;
        uint16_t hidden_id = g_wk.hidden
                                 ? (uint16_t)__builtin_ctzll(g_wk.hidden)
                                 : TAGFS_INVALID_TAG_ID;

        // Check if trashed/hidden were explicitly part of the query
        bool query_has_trashed = false;
        bool query_has_hidden = false;

        for (uint32_t i = 0; i < tag_count; i++)
        {
            if (tag_ids[i] == trashed_id)
                query_has_trashed = true;
            if (tag_ids[i] == hidden_id)
                query_has_hidden = true;
        }
        // Also check wildcard groups — if a group contains the tag, it's explicit
        for (uint32_t g = 0; g < group_count && (!query_has_trashed || !query_has_hidden); g++)
        {
            if (!groups[g])
                continue;
            for (uint32_t k = 0; k < groups[g]->count; k++)
            {
                if (groups[g]->tag_ids[k] == trashed_id)
                    query_has_trashed = true;
                if (groups[g]->tag_ids[k] == hidden_id)
                    query_has_hidden = true;
            }
        }

        bool skip_trashed = !query_has_trashed && trashed_id != TAGFS_INVALID_TAG_ID;
        bool skip_hidden = !query_has_hidden && hidden_id != TAGFS_INVALID_TAG_ID;

        if (skip_trashed || skip_hidden)
        {
            int filtered = 0;
            for (int i = 0; i < result; i++)
            {
                if (!file_has_system_behavior_tag(out_file_ids[i], skip_trashed, skip_hidden))
                {
                    out_file_ids[filtered++] = out_file_ids[i];
                }
            }
            result = filtered;
        }
    }

    kfree(tag_ids);
    kfree(groups);
    return result;
}

static int tagfs_list_all_files_inside(uint32_t *out_file_ids, uint32_t max_results);

uint32_t tagfs_file_ceiling(void)
{
    if (!tagfs_enter())
        return 0;
    /* Under g_state.lock for the same reason the scan bound below is: create
     * bumps next_file_id under it, and a lock-free read can see the value
     * from before a create that has already returned to its caller. */
    spin_lock(&g_state.lock);
    uint32_t ceiling = g_state.ledger.next_file_id;
    spin_unlock(&g_state.lock);
    tagfs_leave();
    return ceiling;
}

int tagfs_list_all_files(uint32_t *out_file_ids, uint32_t max_results)
{
    if (!tagfs_enter())
        return 0;
    int rc = tagfs_list_all_files_inside(out_file_ids, max_results);
    tagfs_leave();
    return rc;
}

static int tagfs_list_all_files_inside(uint32_t *out_file_ids, uint32_t max_results)
{
    if (!out_file_ids || max_results == 0)
        return 0;

    uint32_t found = 0;
    /* Snapshot the scan bound under g_state.lock. create bumps next_file_id
     * under this lock, so a lock-free read here can observe a stale pre-
     * increment value (x86-TSO store buffer / no acquire) and truncate the scan
     * below a just-created fid — hiding a file whose create() already returned
     * to the caller (the cross-core half of write_concurrent "file not found").
     * Snapshot then release so the per-fid file_table locking below stays
     * unnested. */
    spin_lock(&g_state.lock);
    uint32_t max_id = g_state.ledger.next_file_id;
    spin_unlock(&g_state.lock);

    for (uint32_t fid = 1; fid < max_id && found < max_results; fid++)
    {
        uint32_t mb, mo;
        if (file_table_lookup(fid, &mb, &mo) == 0 && mb != 0)
        {
            // Skip files with trashed/hidden system behavior tags
            if (file_has_system_behavior_tag(fid, true, true))
                continue;
            out_file_ids[found++] = fid;
        }
    }

    return (int)found;
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

    OpenFileEntry *ofe = ofe_acquire(file_id);

    TagFSFileHandle *handle = kmalloc(sizeof(TagFSFileHandle));
    if (!handle)
    {
        ofe_release(ofe);
        return NULL;
    }

    handle->file_id = file_id;
    handle->flags = flags;
    handle->offset = 0;
    /* Which mounting of the volume these extents were copied out of. A handle
     * is refused after the volume has been read again, because the blocks it
     * names then belong to whatever the volume keeps there now. */
    handle->mount_epoch = tagfs_mount_epoch();
    handle->file_size = 0;
    handle->extents = NULL;
    handle->extent_count = 0;
    handle->ofe = ofe;

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) == 0)
    {
        handle->file_size = meta.size;
        handle->extent_count = meta.extent_count;
        if (meta.extent_count > 0 && meta.extents)
        {
            handle->extents = kmalloc(sizeof(FileExtent) * meta.extent_count);
            if (handle->extents)
            {
                memcpy(handle->extents, meta.extents,
                       sizeof(FileExtent) * meta.extent_count);
            }
        }
        tagfs_metadata_free(&meta);
    }

    return handle;
}

void tagfs_close(TagFSFileHandle *handle)
{
    if (!handle)
        return;
    debug_printf("[TagFS] close: file_id=%u\n", handle->file_id);
    ofe_release(handle->ofe);
    debug_printf("[TagFS] close: ofe released\n");
    if (handle->extents)
        kfree(handle->extents);
    debug_printf("[TagFS] close: extents freed\n");
    kfree(handle);
    debug_printf("[TagFS] close: handle freed\n");
}

static int tagfs_read_inside(TagFSFileHandle *handle, void *buffer, uint64_t size);

int tagfs_read(TagFSFileHandle *handle, void *buffer, uint64_t size)
{
    if (!tagfs_enter())
        return -1;
    if (!handle_belongs_here(handle, "read")) {
        tagfs_leave();
        return -1;
    }
    int rc = tagfs_read_inside(handle, buffer, size);
    tagfs_leave();
    return rc;
}

static int tagfs_read_inside(TagFSFileHandle *handle, void *buffer, uint64_t size)
{
    if (!handle || !buffer || size == 0)
        return -1;

    if (handle->offset >= handle->file_size)
        return 0;

    uint64_t remaining = handle->file_size - handle->offset;
    if (size > remaining)
        size = remaining;

    /* Heap-alloc the per-call 4 KiB block scratch so the function
     * frame stays under -Wstack-usage=8192. tagfs_read can run on
     * per-cpu kernel stacks (16 KiB total); a 4 KiB stack frame +
     * nested IRQ frame would eat half the budget. */
    uint8_t *block_buf = (uint8_t *)kmalloc(TAGFS_BLOCK_SIZE);
    if (!block_buf) return -1;
    uint8_t *out = (uint8_t *)buffer;
    uint64_t bytes_read = 0;

    while (bytes_read < size)
    {
        uint64_t file_pos = handle->offset + bytes_read;
        uint64_t extent_start = 0;
        int found = -1;

        for (uint16_t i = 0; i < handle->extent_count; i++)
        {
            uint64_t extent_size = (uint64_t)handle->extents[i].block_count * TAGFS_BLOCK_SIZE;
            if (file_pos < extent_start + extent_size)
            {
                found = i;
                break;
            }
            extent_start += extent_size;
        }

        if (found < 0)
        {
            /* The file claims more bytes than its extents describe. Whatever
             * lies past the last extent is not a hole to be filled with zeros;
             * it is metadata that does not match the medium. */
            kprintf("[TagFS] file %u: no extent covers byte %llu of %llu — the "
                    "metadata describes more file than there are blocks\n",
                    handle->file_id, (unsigned long long)file_pos,
                    (unsigned long long)handle->file_size);
            kfree(block_buf);
            return -1;
        }

        uint64_t offset_in_extent = file_pos - extent_start;
        uint32_t block_index = (uint32_t)(offset_in_extent / TAGFS_BLOCK_SIZE);
        uint32_t offset_in_block = (uint32_t)(offset_in_extent % TAGFS_BLOCK_SIZE);
        uint32_t disk_block = handle->extents[found].start_block + block_index;

        if (read_block(disk_block, block_buf) != 0)
        {
            /*
             * A block the medium would not give up.
             *
             * This used to break out of the loop and return the byte count so
             * far, which for the first block is zero — and zero is not
             * negative, so every caller that checks for failure the usual way
             * saw a successful short read into a buffer that was still full of
             * the zeros it was allocated with.
             *
             * Measured: pull the flash drive out while the system is running,
             * and the program loader reads a process image of nothing, reports
             * that it started it, and jumps to address 0xC000 where the zeros
             * are. Both userspace processes died on their first instruction
             * with every register clear. The medium leaving is not something
             * this kernel can prevent; a read that failed reading as a read
             * that succeeded is.
             *
             * Bytes already copied stay in the caller's buffer and the handle
             * does not move: a caller told the read failed cannot use either.
             */
            kprintf("[TagFS] file %u: block %u would not read — %llu of %llu "
                    "bytes had come back before the medium stopped answering\n",
                    handle->file_id, disk_block,
                    (unsigned long long)bytes_read, (unsigned long long)size);
            kfree(block_buf);
            return -1;
        }

        uint32_t chunk = TAGFS_BLOCK_SIZE - offset_in_block;
        if (chunk > size - bytes_read)
            chunk = (uint32_t)(size - bytes_read);

        memcpy(out + bytes_read, block_buf + offset_in_block, chunk);
        bytes_read += chunk;

        /*
         * Read-ahead, and it runs when the next block is NOT already in hand.
         *
         * ‼ IT USED TO RUN AFTER EVERY BLOCK, and that is why it never read
         * more than one. Asking for the next four when three of them are
         * already cached leaves exactly one to fetch, so the window crawled
         * forward a block at a time and the medium got one command per four
         * kilobytes for the whole file — measured on a flash drive: 216
         * commands to mount and start a volume, every one of them eight
         * sectors. Waiting for the miss lets the window move a whole run, and
         * a whole run of neighbours is one command (see ReadAheadPrefetch).
         */
        if (offset_in_block == 0 && chunk == TAGFS_BLOCK_SIZE) {
            uint32_t next_block = disk_block + 1;
            uint32_t blocks_remaining_in_extent = handle->extents[found].block_count - block_index - 1;
            if (blocks_remaining_in_extent > 0 && !ReadAheadHas(next_block)) {
                ReadAheadPrefetch(next_block, blocks_remaining_in_extent < TAGFS_READ_AHEAD_BLOCKS ?
                                    blocks_remaining_in_extent : TAGFS_READ_AHEAD_BLOCKS);
            }
        }
    }

    handle->offset += bytes_read;
    kfree(block_buf);
    return (int)bytes_read;
}

static int tagfs_write_inside(TagFSFileHandle *handle, const void *buffer, uint64_t size);

int tagfs_write(TagFSFileHandle *handle, const void *buffer, uint64_t size)
{
    if (!tagfs_enter())
        return -1;
    if (!handle_belongs_here(handle, "write")) {
        tagfs_leave();
        return -1;
    }
    int rc = tagfs_write_inside(handle, buffer, size);
    tagfs_leave();
    return rc;
}

static int tagfs_write_inside(TagFSFileHandle *handle, const void *buffer, uint64_t size)
{
    if (!handle || !buffer || size == 0)
        return -1;
    if (!(handle->flags & TAGFS_HANDLE_WRITE))
        return -1;

    // Auto-snapshot before write if versioning enabled (production feature)
    tagfs_auto_snapshot_before_write(handle->file_id);

    /* Serialize writes to the same file. The plain `spin_lock` would
     * deadlock with the BMIDE-IRQ-on-BSP topology: this lock is held
     * across `write_block → ata_dma_sync`, which waits for the BMIDE
     * completion IRQ. That IRQ lands only on the BSP (legacy IO-APIC
     * GSI 14). If the BSP is the CONTENDER here, its spin_lock has
     * already CLI'd and the IRQ never delivers; the holder spins
     * forever waiting for cmd.done and the BSP spins forever waiting
     * for the lock.
     *
     * Break the cycle by trylocking with the IRQ window open between
     * attempts. Each failed trylock restores RFLAGS (re-enabling IRQs), so
     * the pending BMIDE IRQ can fire on the BSP and stamp the holder's
     * `landing` slot; the holder (spinning in ata_dma_sync) claims it, runs
     * its completion, flips cmd.done, and releases. Opening the IRQ window is
     * the load-bearing part; the irq_defer_pump below is now vestigial for
     * BMIDE (the holder self-drains via landing) but harmless — it still
     * drains this core's other deferred work. Uncontended fast path is
     * unchanged — trylock succeeds first try. */
    if (handle->ofe)
    {
        if (!spin_trylock(&handle->ofe->write_lock)) {
            uint8_t self_core = amp_get_core_index();
            while (!spin_trylock(&handle->ofe->write_lock)) {
                irq_defer_pump(self_core);
                cpu_pause();
            }
        }
    }

    const uint8_t *in = (const uint8_t *)buffer;
    uint8_t block_buf[TAGFS_BLOCK_SIZE];
    uint64_t bytes_written = 0;
    bool     medium_refused = false;
    uint32_t refused_block = 0;


    while (bytes_written < size)
    {
        uint64_t file_pos = handle->offset + bytes_written;
        uint64_t extent_start = 0;
        int found = -1;

        for (uint16_t i = 0; i < handle->extent_count; i++)
        {
            uint64_t extent_size = (uint64_t)handle->extents[i].block_count * TAGFS_BLOCK_SIZE;
            if (file_pos < extent_start + extent_size)
            {
                found = i;
                break;
            }
            extent_start += extent_size;
        }

        bool newly_allocated = false;
        if (found < 0)
        {
            // extent_start = cumulative size of all existing extents (from loop)
            // Allocate enough blocks to cover from extent_start to file_pos+1
            uint32_t blocks_needed = (uint32_t)((file_pos - extent_start) / TAGFS_BLOCK_SIZE) + 1;
            if (blocks_needed > 0xFFFF)
                blocks_needed = 0xFFFF;

            uint32_t new_start;
            if (tagfs_alloc_blocks(blocks_needed, &new_start) != 0)
            {
                break;
            }

            uint16_t new_count = handle->extent_count + 1;
            FileExtent *new_extents = kmalloc(sizeof(FileExtent) * new_count);
            if (!new_extents)
            {
                tagfs_free_blocks(new_start, blocks_needed);
                break;
            }
            if (handle->extents && handle->extent_count > 0)
            {
                memcpy(new_extents, handle->extents, sizeof(FileExtent) * handle->extent_count);
                kfree(handle->extents);
            }
            new_extents[handle->extent_count].start_block = new_start;
            new_extents[handle->extent_count].block_count = (uint16_t)blocks_needed;
            handle->extents = new_extents;
            handle->extent_count = new_count;

            found = handle->extent_count - 1;
            newly_allocated = true;
            // extent_start stays as cumulative end — math works correctly now
        }

        uint64_t offset_in_extent = file_pos - extent_start;
        uint32_t block_index = (uint32_t)(offset_in_extent / TAGFS_BLOCK_SIZE);
        uint32_t offset_in_block = (uint32_t)(offset_in_extent % TAGFS_BLOCK_SIZE);
        uint32_t disk_block = handle->extents[found].start_block + block_index;

        // CoW only applies to pre-existing blocks, not newly allocated ones.
        bool is_existing_block = !newly_allocated;
        bool needs_partial_read = (offset_in_block != 0 || (size - bytes_written) < TAGFS_BLOCK_SIZE);

        if (needs_partial_read)
        {
            if (read_block(disk_block, block_buf) != 0)
            {
                memset(block_buf, 0, TAGFS_BLOCK_SIZE);
            }
        }
        else
        {
            memset(block_buf, 0, TAGFS_BLOCK_SIZE);
        }

        uint32_t chunk = TAGFS_BLOCK_SIZE - offset_in_block;
        if (chunk > size - bytes_written)
            chunk = (uint32_t)(size - bytes_written);

        memcpy(block_buf + offset_in_block, in + bytes_written, chunk);

        // CoW: before overwriting an existing block in a CoW-enabled file,
        // redirect the write to a freshly allocated single-block copy.
        // Only applied when writing a full block at block_index==0 in an extent,
        // which covers the common case without requiring extent splits.
        uint32_t write_target = disk_block;
        if (is_existing_block && block_index == 0 && TagFS_CowIsActive(handle->file_id)) {
            uint32_t cow_block = 0;
            if (TagFS_CowBeforeWrite(handle->file_id, disk_block, &cow_block) == OK && cow_block != 0) {
                // Redirect this extent's start to the new block
                handle->extents[found].start_block = cow_block;
                write_target = cow_block;
                TagFS_CowAfterWrite(handle->file_id, disk_block, cow_block);
            }
        }

        // Dedup: for complete single-block writes on newly allocated blocks only,
        // check if identical data already resides on disk.
        // We skip dedup for partial writes (read-modify-write) since we need
        // the real data to already exist on disk for dedup to be safe.
        /* Inline dedup DISABLED during write.
         *
         * Dedup inside tagfs_write caused a block reuse race: freed dedup
         * blocks were reallocated by MetaPool CHAINING (in the meta_pool_write
         * call at the end of this function), corrupting file data.
         *
         * Dedup is still effective via TagFS_DedupRegister on close/flush
         * or as a background compaction pass.  The inline path was an
         * optimization that traded safety for space — not acceptable for
         * production. */
        bool wrote_block = false;
        if (newly_allocated && block_index == 0 &&
            offset_in_block == 0 && chunk == TAGFS_BLOCK_SIZE &&
            TagFS_DedupIsInitialized()) {
            TagFS_DedupRegister(write_target, block_buf, handle->file_id);
        }

        if (!wrote_block) {
            if (write_block(write_target, block_buf) != 0)
            {
                /* The same lie the read path told, in the direction that
                 * matters more: a block the medium refused ended the loop and
                 * the byte count so far was returned as the answer. A caller
                 * checking for failure the usual way was told the write
                 * succeeded and simply moved fewer bytes than asked.
                 *
                 * The metadata below still commits what actually landed, so
                 * the file on the medium stays consistent with itself — what
                 * changes is that the caller is told. */
                medium_refused = true;
                refused_block  = write_target;
                break;
            }
        }

        bytes_written += chunk;
    }

    handle->offset += bytes_written;
    if (handle->offset > handle->file_size)
    {
        handle->file_size = handle->offset;
    }

    // Update metadata. Two paths:
    // - Extents changed (new blocks allocated): journal for crash safety
    // - Size-only (wrote within existing extents): direct metadata write
    if (bytes_written > 0)
    {
        uint32_t meta_block, meta_offset;
        if (file_table_lookup(handle->file_id, &meta_block, &meta_offset) != 0)
        {
            if (handle->ofe)
                spin_unlock(&handle->ofe->write_lock);
            return -1;
        }

        TagFSMetadata meta;
        memset(&meta, 0, sizeof(meta));
        if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
        {
            if (handle->ofe)
                spin_unlock(&handle->ofe->write_lock);
            return -1;
        }

        meta.size = handle->file_size;
        if (meta.extents)
            kfree(meta.extents);
        meta.extent_count = handle->extent_count;
        meta.extents = kmalloc(sizeof(FileExtent) * handle->extent_count);
        if (!meta.extents)
        {
            tagfs_metadata_free(&meta);
            if (handle->ofe)
                spin_unlock(&handle->ofe->write_lock);
            return -1;
        }
        memcpy(meta.extents, handle->extents, sizeof(FileExtent) * handle->extent_count);

        uint32_t new_mb, new_mo;
        if (meta_pool_write(&meta, &new_mb, &new_mo) != 0)
        {
            tagfs_metadata_free(&meta);
            if (handle->ofe)
                spin_unlock(&handle->ofe->write_lock);
            return -1;
        }
        file_table_update(handle->file_id, new_mb, new_mo);
        meta_pool_delete(meta_block, meta_offset);
        tagfs_metadata_free(&meta);
    }

    if (handle->ofe)
        spin_unlock(&handle->ofe->write_lock);

    /* WROTE Touch fan-out is the caller's responsibility — published by
     * storage_ops.c::ObjWrite (sync path) and write_job.c::w_publish
     * (async path) using a unified 32-byte payload. Publishing here
     * with a truncated 8-byte event drifted from the async contract
     * (write_observer's payload-size check failed on every BIOS / 1-core
     * config that takes the sync path). Keep tagfs_write itself free
     * of IPC side-effects so its self-tests do not need Touch wiring. */
    if (medium_refused)
    {
        kprintf("[TagFS] file %u: block %u would not be written — %llu of %llu "
                "bytes had landed before the medium stopped answering\n",
                handle->file_id, refused_block,
                (unsigned long long)bytes_written, (unsigned long long)size);
        return -1;
    }
    return (int)bytes_written;
}

// ----------------------------------------------------------------------------
// tagfs_truncate_file — drop everything past `new_size`.
//
// The operation TagFS never had. Writes only ever GREW a file (see the
// `if (handle->offset > handle->file_size)` above), so rewriting a file
// shorter left the old tail readable behind the new content — every shorter
// rewrite in the system was silently wrong. Nothing forced the issue until
// <fstream>, where plain ios_base::out means "w" ([filebuf.members] Table
// 122) and truncation is not optional.
//
// SHRINK ONLY, and a grow request is refused rather than served: TagFS does
// not zero freshly allocated blocks, so growing here would hand back whatever
// the allocator's previous tenant left. A primitive that returns another
// file's bytes is worse than one that says no. Files grow the honest way, by
// being written.
//
// REFUSED ON A SNAPSHOTTED FILE. A block that has not been copied since the
// snapshot was taken is still the snapshot's only copy, and a CowSnapshot
// records redirects rather than an extent list of its own — so nothing here
// can tell "mine alone" from "shared with a frozen view". Freeing such a
// block would corrupt the snapshot. Refusing costs a rare failed truncate;
// guessing costs the snapshot.
//
// Returns 0, or a negative -ERR_* naming the cause.
// ----------------------------------------------------------------------------

// Caller holds the file's ofe->write_lock.
static int tagfs_truncate_locked(uint32_t file_id, uint64_t new_size)
{
    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
        return -ERR_FILE_NOT_FOUND;

    /* Read the metadata under the lock rather than trusting the handle.
     * tagfs_open snapshots extents BEFORE the lock is taken, and a writer
     * that grew the file in between would leave that snapshot short — and
     * freeing from a short list frees blocks that are still live. */
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
        return -ERR_IO;

    if (new_size > meta.size)
    {
        tagfs_metadata_free(&meta);
        return -ERR_INVALID_ARGUMENT;   /* growth is not this operation's job */
    }
    if (new_size == meta.size)
    {
        tagfs_metadata_free(&meta);
        return 0;
    }

    FileExtent *old_ex = meta.extents;
    uint16_t    old_n  = meta.extent_count;

    /* The extent prefix that still holds bytes. Ceil, because a partly filled
     * last block is still a block the file owns. */
    uint32_t keep_blocks = (uint32_t)((new_size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE);

    /* Walk the list to find the cut: `kept` extents survive, and at most one
     * of them — trim_at — straddles the boundary and keeps only trim_to of
     * its blocks. */
    uint32_t acc     = 0;
    uint16_t kept    = 0;
    uint16_t trim_at = 0xFFFFu;
    uint32_t trim_to = 0;
    for (uint16_t i = 0; i < old_n; i++)
    {
        uint32_t cnt = old_ex[i].block_count;
        if (acc >= keep_blocks)
            break;
        if (acc + cnt > keep_blocks)
        {
            trim_at = i;
            trim_to = keep_blocks - acc;
            kept    = (uint16_t)(i + 1);
            break;
        }
        acc += cnt;
        kept = (uint16_t)(i + 1);
    }

    /* Build the new list first. A failure here has touched nothing. */
    FileExtent *new_ex = NULL;
    if (kept > 0)
    {
        new_ex = kmalloc(sizeof(FileExtent) * kept);
        if (!new_ex)
        {
            tagfs_metadata_free(&meta);
            return -ERR_NO_MEMORY;
        }
        memcpy(new_ex, old_ex, sizeof(FileExtent) * kept);
        if (trim_at != 0xFFFFu)
            new_ex[trim_at].block_count = (uint16_t)trim_to;
    }

    meta.extents      = new_ex;
    meta.extent_count = kept;
    meta.size         = new_size;

    /* Commit in the same order tagfs_write does: write the new record, point
     * the file table at it, then drop the old one. A crash between the write
     * and the update leaves the file at its old length — never at a length
     * whose blocks have already been handed away. */
    uint32_t new_mb, new_mo;
    if (meta_pool_write(&meta, &new_mb, &new_mo) != 0)
    {
        tagfs_metadata_free(&meta);   /* frees new_ex */
        if (old_ex) kfree(old_ex);
        return -ERR_IO;
    }
    file_table_update(file_id, new_mb, new_mo);
    meta_pool_delete(meta_block, meta_offset);

    /* Committed: the dropped blocks are unreachable, so release them.
     * tagfs_free_blocks settles them with the dedup index on the way out —
     * a recycled block behind a live content hash is exactly how a later
     * lookup would hand one file another's bytes. Dropping them from the
     * read-ahead cache is this loop's own job, so a reallocation cannot serve
     * the old tenant's content out of memory. */
    for (uint16_t i = 0; i < old_n; i++)
    {
        uint32_t drop_from = 0;
        if (i < kept)
        {
            if (i != trim_at)
                continue;               /* wholly kept */
            drop_from = trim_to;        /* head kept, tail released */
        }
        uint32_t cnt = old_ex[i].block_count;
        if (drop_from >= cnt)
            continue;

        uint32_t first = old_ex[i].start_block + drop_from;
        uint32_t n     = cnt - drop_from;
        for (uint32_t b = 0; b < n; b++)
        {
            tagfs_readahead_invalidate(first + b);
        }
        tagfs_free_blocks(first, n);
    }

    if (old_ex) kfree(old_ex);
    tagfs_metadata_free(&meta);
    return 0;
}

static int tagfs_truncate_file_inside(uint32_t file_id, uint64_t new_size);

int tagfs_truncate_file(uint32_t file_id, uint64_t new_size)
{
    if (!tagfs_enter())
        return -ERR_INVALID_OPERATION;
    int rc = tagfs_truncate_file_inside(file_id, new_size);
    tagfs_leave();
    return rc;
}

static int tagfs_truncate_file_inside(uint32_t file_id, uint64_t new_size)
{
    if (file_id == 0)
        return -ERR_INVALID_ARGUMENT;

    /* A snapshot may still be reading the blocks this would drop. */
    if (TagFS_CowIsActive(file_id))
        return -ERR_INVALID_OPERATION;

    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    if (!handle)
        return -ERR_FILE_NOT_FOUND;

    /* The trylock-with-the-IRQ-window-open dance, for the same reason
     * tagfs_write does it: this lock is held across meta_pool_write ->
     * write_block -> ata_dma_sync, which waits on a BMIDE completion IRQ that
     * lands only on the BSP. A plain spin_lock here would deadlock the BSP
     * against the holder exactly as described above tagfs_write. */
    if (handle->ofe)
    {
        if (!spin_trylock(&handle->ofe->write_lock)) {
            uint8_t self_core = amp_get_core_index();
            while (!spin_trylock(&handle->ofe->write_lock)) {
                irq_defer_pump(self_core);
                cpu_pause();
            }
        }
    }

    int rc = tagfs_truncate_locked(file_id, new_size);

    if (handle->ofe)
        spin_unlock(&handle->ofe->write_lock);
    tagfs_close(handle);
    return rc;
}

// ----------------------------------------------------------------------------
// Block allocator
// ----------------------------------------------------------------------------

// Internal version - caller MUST hold g_state.lock
// Exported for use by meta_pool.c
int tagfs_alloc_blocks_internal(uint32_t count, uint32_t *out_start_block)
{
    FreeExtent *prev = NULL;
    FreeExtent *cur = g_state.block_bitmap.free_list;

    while (cur)
    {
        if (cur->count >= count)
        {
            *out_start_block = cur->start;

            // Mark bits used
            for (uint32_t i = 0; i < count; i++)
            {
                bitmap_set_bit(g_state.block_bitmap.bitmap, cur->start + i);
            }

            if (cur->count == count)
            {
                // Remove extent from list
                if (prev)
                    prev->next = cur->next;
                else
                    g_state.block_bitmap.free_list = cur->next;
                kfree(cur);
                g_state.block_bitmap.extent_count--;
            }
            else
            {
                cur->start += count;
                cur->count -= count;
            }

            if (g_state.ledger.free_blocks >= count)
            {
                g_state.ledger.free_blocks -= count;
            }

            return 0;
        }
        prev = cur;
        cur = cur->next;
    }

    return -1;
}

// Public version - acquires g_state.lock
static int tagfs_alloc_blocks_inside(uint32_t count, uint32_t *out_start_block);

int tagfs_alloc_blocks(uint32_t count, uint32_t *out_start_block)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_alloc_blocks_inside(count, out_start_block);
    tagfs_leave();
    return rc;
}

static int tagfs_alloc_blocks_inside(uint32_t count, uint32_t *out_start_block)
{
    if (count == 0 || !out_start_block)
        return -1;

    spin_lock(&g_state.lock);
    int result = tagfs_alloc_blocks_internal(count, out_start_block);
    spin_unlock(&g_state.lock);
    return result;
}

// Reclaim one contiguous run into the bitmap and the free list.
// Caller holds g_state.lock, and has already settled the run with the dedup
// index (see tagfs_free_blocks_internal below).
static void free_run_locked(uint32_t start_block, uint32_t count)
{
    if (count == 0)
        return;

    uint32_t end = start_block + count;

    // Clear bits
    for (uint32_t i = 0; i < count; i++)
    {
        bitmap_clear_bit(g_state.block_bitmap.bitmap, start_block + i);
    }
    g_state.ledger.free_blocks += count;

    // Insert into free list in sorted order, merging adjacent extents
    FreeExtent *prev = NULL;
    FreeExtent *cur = g_state.block_bitmap.free_list;

    while (cur && cur->start < start_block)
    {
        prev = cur;
        cur = cur->next;
    }

    // Check merge right (new extent is adjacent to cur from left)
    bool merge_right = cur && cur->start == end;
    // Check merge left (prev is adjacent to new extent from right)
    bool merge_left = prev && (prev->start + prev->count) == start_block;

    if (merge_left && merge_right)
    {
        prev->count += count + cur->count;
        prev->next = cur->next;
        kfree(cur);
        g_state.block_bitmap.extent_count--;
    }
    else if (merge_left)
    {
        prev->count += count;
    }
    else if (merge_right)
    {
        cur->start = start_block;
        cur->count += count;
    }
    else
    {
        FreeExtent *new_extent = kmalloc(sizeof(FreeExtent));
        if (new_extent)
        {
            new_extent->start = start_block;
            new_extent->count = count;
            new_extent->next = cur;
            if (prev)
                prev->next = new_extent;
            else
                g_state.block_bitmap.free_list = new_extent;
            g_state.block_bitmap.extent_count++;
        }
    }
}

// Give blocks back to the allocator. Caller MUST hold g_state.lock.
//
// This is the one door out of the allocator, so it is also where a block leaves
// the dedup index. Nothing used to walk that index on the way out: every
// deleted file left its blocks behind in it, mapping live content hashes to
// bytes that now belong to somebody else, and the index grew for the lifetime
// of the system with no pass able to shrink it.
//
// The index also gets a veto. It is the only record that a block has more than
// one owner, so a block it still counts as shared is not ours to hand back —
// the run is split around it and the rest is reclaimed.
//
// Lock order is g_state.lock -> dedup lock, established here and observed
// everywhere; TagFS_DedupAllocBlock keeps its allocation outside the dedup lock
// so the cycle cannot close from the other side.
static void tagfs_free_blocks_internal(uint32_t start_block, uint32_t count)
{
    if (count == 0)
        return;

    uint32_t run_start = start_block;
    uint32_t run_len   = 0;

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t block = start_block + i;

        bool may_reclaim = true;
        TagFS_DedupUnregister(block, &may_reclaim);

        if (may_reclaim)
        {
            if (run_len == 0)
                run_start = block;
            run_len++;
            continue;
        }

        if (run_len)
        {
            free_run_locked(run_start, run_len);
            run_len = 0;
        }
    }

    if (run_len)
        free_run_locked(run_start, run_len);
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

static int tagfs_free_blocks_inside(uint32_t start_block, uint32_t count)
{
    if (count == 0)
        return 0;

    spin_lock(&g_state.lock);
    tagfs_free_blocks_internal(start_block, count);
    spin_unlock(&g_state.lock);
    return 0;
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

static int tagfs_get_metadata_inside(uint32_t file_id, TagFSMetadata *out)
{
    if (!out)
        return -1;

    // Fast path: read from mirror (no disk I/O)
    if (meta_pool_read_cached(file_id, out) == 0)
    {
        return 0;
    }

    // Slow path: disk read
    uint32_t block, offset;
    if (file_table_lookup(file_id, &block, &offset) != 0)
        return -1;
    return meta_pool_read(block, offset, out);
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
 * a copy of a name in the caller's own buffer. The pointer does not.
 */
static uint16_t tag_id_for(const char *tag, bool make_it)
{
    if (!tag || tag[0] == '\0') {
        return TAGFS_INVALID_TAG_ID;
    }
    if (!tagfs_enter()) {
        return TAGFS_INVALID_TAG_ID;
    }

    uint16_t id = TAGFS_INVALID_TAG_ID;
    if (g_state.registry) {
        char key[256], value[256];
        tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));
        const char *v = (value[0] != '\0') ? value : NULL;
        id = make_it ? tag_registry_intern(g_state.registry, key, v)
                     : tag_registry_lookup(g_state.registry, key, v);
    }

    tagfs_leave();
    return id;
}

/* The id this volume has for a tag, or TAGFS_INVALID_TAG_ID when it has none
 * — including when there is no volume at all. */
uint16_t tagfs_tag_lookup(const char *tag)
{
    return tag_id_for(tag, false);
}

/* The same, and the volume issues an id if it does not have one yet. */
uint16_t tagfs_tag_intern(const char *tag)
{
    return tag_id_for(tag, true);
}

static bool tag_name_of(uint16_t tag_id, char *out, size_t out_size, bool with_value)
{
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (!tagfs_enter()) {
        return false;
    }

    bool said = false;
    if (g_state.registry) {
        const char *key = tag_registry_key(g_state.registry, tag_id);
        if (key) {
            if (with_value) {
                tagfs_format_tag(out, out_size, key,
                                 tag_registry_value(g_state.registry, tag_id));
            } else {
                ksnprintf(out, out_size, "%s", key);
            }
            said = true;
        }
    }

    tagfs_leave();
    return said;
}

/* The key of a tag id — "app" out of "app:editor". */
bool tagfs_tag_key(uint16_t tag_id, char *out, size_t out_size)
{
    return tag_name_of(tag_id, out, out_size, false);
}

/* And the whole of it — "app:editor". */
bool tagfs_tag_text(uint16_t tag_id, char *out, size_t out_size)
{
    return tag_name_of(tag_id, out, out_size, true);
}

/*
 * Key, value and whether the volume calls the tag its own, in one answer.
 *
 * For the one caller that has to serialise a tag rather than read it: three
 * separate questions would be three separate doors, and the answer to the
 * second could come from a different volume than the answer to the first.
 */
bool tagfs_tag_parts(uint16_t tag_id, char *key, size_t key_size,
                     char *value, size_t value_size, bool *is_system)
{
    if (key && key_size)     key[0] = '\0';
    if (value && value_size) value[0] = '\0';
    if (is_system)           *is_system = false;

    if (!tagfs_enter()) {
        return false;
    }

    bool said = false;
    if (g_state.registry) {
        const char *k = tag_registry_key(g_state.registry, tag_id);
        if (k) {
            const char *v = tag_registry_value(g_state.registry, tag_id);
            if (key && key_size)     ksnprintf(key, key_size, "%s", k);
            if (value && value_size && v) ksnprintf(value, value_size, "%s", v);
            if (is_system) *is_system = tag_registry_is_system(g_state.registry, tag_id);
            said = true;
        }
    }

    tagfs_leave();
    return said;
}

void tagfs_format_tag(char *dest, size_t dest_size, const char *key, const char *value)
{
    if (!dest || dest_size == 0 || !key)
        return;
    if (value && value[0])
    {
        ksnprintf(dest, dest_size, "%s:%s", key, value);
    }
    else
    {
        strncpy(dest, key, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

int tagfs_parse_tag(const char *tag_string, char *key, size_t key_size,
                    char *value, size_t value_size)
{
    if (!tag_string || !key || !value || key_size == 0 || value_size == 0)
        return -1;

    const char *colon = strchr(tag_string, ':');
    if (colon)
    {
        size_t klen = (size_t)(colon - tag_string);
        if (klen >= key_size)
            klen = key_size - 1;
        memcpy(key, tag_string, klen);
        key[klen] = '\0';

        strncpy(value, colon + 1, value_size - 1);
        value[value_size - 1] = '\0';
    }
    else
    {
        strncpy(key, tag_string, key_size - 1);
        key[key_size - 1] = '\0';
        value[0] = '\0';
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Defrag
// ----------------------------------------------------------------------------

static int tagfs_defrag_file_inside(uint32_t file_id, uint32_t target_block);

int tagfs_defrag_file(uint32_t file_id, uint32_t target_block)
{
    if (!tagfs_enter())
        return -1;
    int rc = tagfs_defrag_file_inside(file_id, target_block);
    tagfs_leave();
    return rc;
}

static int tagfs_defrag_file_inside(uint32_t file_id, uint32_t target_block)
{
    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
    {
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Nothing to defrag if file has 0 or 1 extent
    if (meta.extent_count <= 1)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return 0;
    }

    // Calculate total blocks needed
    uint32_t total_blocks = 0;
    for (uint16_t i = 0; i < meta.extent_count; i++)
    {
        total_blocks += meta.extents[i].block_count;
    }

    // Allocate contiguous space at target location or first available
    uint32_t new_start;
    if (target_block != 0 && target_block < g_state.layout.data_blocks) {
        // Try to allocate at specific target block
        // Check if target area is free
        bool area_free = true;
        for (uint32_t b = 0; b < total_blocks && area_free; b++) {
            if (bitmap_test_bit(g_state.block_bitmap.bitmap, target_block + b)) {
                area_free = false;
            }
        }
        
        if (area_free) {
            new_start = target_block;
            // Mark blocks as allocated
            for (uint32_t b = 0; b < total_blocks; b++) {
                bitmap_set_bit(g_state.block_bitmap.bitmap, new_start + b);
            }
            g_state.ledger.free_blocks -= total_blocks;
        } else {
            // Target not available, allocate first available
            spin_unlock(&g_state.lock);
            if (tagfs_alloc_blocks(total_blocks, &new_start) != 0)
            {
                tagfs_metadata_free(&meta);
                return -1;
            }
            spin_lock(&g_state.lock);
        }
    } else {
        // No target specified, allocate first available contiguous space
        spin_unlock(&g_state.lock);
        if (tagfs_alloc_blocks(total_blocks, &new_start) != 0)
        {
            tagfs_metadata_free(&meta);
            return -1;
        }
        spin_lock(&g_state.lock);
    }

    // Copy all data to new contiguous location
    uint8_t block_buf[TAGFS_BLOCK_SIZE];
    uint32_t dest_block = new_start;
    bool copy_failed = false;

    for (uint16_t i = 0; i < meta.extent_count && !copy_failed; i++)
    {
        for (uint16_t b = 0; b < meta.extents[i].block_count && !copy_failed; b++)
        {
            if (read_block(meta.extents[i].start_block + b, block_buf) != 0 ||
                write_block(dest_block, block_buf) != 0)
            {
                copy_failed = true;
                break;
            }
            dest_block++;
        }
    }

    if (copy_failed)
    {
        // Cleanup: free newly allocated blocks
        spin_unlock(&g_state.lock);
        tagfs_free_blocks(new_start, total_blocks);
        tagfs_metadata_free(&meta);
        return -1;
    }

    // Free old fragmented blocks
    for (uint16_t i = 0; i < meta.extent_count; i++)
    {
        for (uint16_t b = 0; b < meta.extents[i].block_count; b++)
        {
            bitmap_clear_bit(g_state.block_bitmap.bitmap,
                             meta.extents[i].start_block + b);
        }
        g_state.ledger.free_blocks += meta.extents[i].block_count;
    }

    if (meta.extents)
        kfree(meta.extents);

    // Create new single extent
    meta.extents = kmalloc(sizeof(FileExtent));
    if (!meta.extents)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }
    
    meta.extents[0].start_block = new_start;
    meta.extents[0].block_count = (uint16_t)total_blocks;
    meta.extent_count = 1;
    meta.modified_time = rtc_get_unix64();

    // Update metadata pool
    meta_pool_delete(meta_block, meta_offset);
    uint32_t new_mb, new_mo;
    if (meta_pool_write(&meta, &new_mb, &new_mo) == 0)
    {
        file_table_update(file_id, new_mb, new_mo);
    }

    tagfs_metadata_free(&meta);
    spin_unlock(&g_state.lock);
    return 0;
}

static uint32_t tagfs_get_fragmentation_score_inside(void);

uint32_t tagfs_get_fragmentation_score(void)
{
    if (!tagfs_enter())
        return 0;
    uint32_t rc = tagfs_get_fragmentation_score_inside();
    tagfs_leave();
    return rc;
}

static uint32_t tagfs_get_fragmentation_score_inside(void)
{
    uint32_t score = 0;
    uint32_t max_id = g_state.ledger.next_file_id;

    for (uint32_t fid = 1; fid < max_id; fid++)
    {
        uint32_t mb, mo;
        if (file_table_lookup(fid, &mb, &mo) != 0 || mb == 0)
            continue;

        TagFSMetadata meta;
        memset(&meta, 0, sizeof(meta));
        if (meta_pool_read(mb, mo, &meta) == 0)
        {
            if (meta.extent_count > 1)
            {
                score += (meta.extent_count - 1);
            }
            tagfs_metadata_free(&meta);
        }
    }

    return score;
}
