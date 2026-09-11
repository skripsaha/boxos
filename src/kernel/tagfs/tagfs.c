#include "tagfs.h"
#include "boardroom.h"
#include "boarding.h"
#include "deed/deed.h"
#include "deed_pen.h"
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
#include "baton.h"
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

    if (TagFS_CowIsActive(file_id)) {
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

WellKnownTags *tagfs_get_well_known_tags(void) { return &g_well_known; }

#define g_wk (*tagfs_get_well_known_tags())


typedef enum {
    VOLUME_DOWN = 0,
    VOLUME_UP,
    VOLUME_LEAVING
} VolumeDoor;

static volatile uint8_t  g_door        = VOLUME_DOWN;
static volatile uint32_t g_inside      = 0;
static volatile uint32_t g_mount_epoch = 0;

static volatile uint32_t g_abandon_pending = 0;
static bool              g_leaving_said    = false;

#define TAGFS_MOUNT_RETRY_MS    750u
#define TAGFS_MOUNT_RETRY_TRIES 8u

static volatile uint32_t g_mount_owed  = 0;
static uint64_t          g_mount_due   = 0;
static uint32_t          g_mount_tries = 0;

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

static void mount_owed(void)
{
    g_mount_due = rdtsc() + cpu_ms_to_tsc(TAGFS_MOUNT_RETRY_MS);
    __atomic_store_n(&g_mount_owed, 1u, __ATOMIC_RELEASE);
}


static void mount_settled(void)
{
    g_mount_tries = 0;
    __atomic_store_n(&g_mount_owed, 0u, __ATOMIC_RELEASE);
}

bool tagfs_handle_is_of_this_mount(const TagFSFileHandle *handle)
{
    return handle && handle->mount_epoch == tagfs_mount_epoch();
}

static bool handle_belongs_here(const TagFSFileHandle *handle, const char *what)
{
    if (tagfs_handle_is_of_this_mount(handle)) {
        return true;
    }
    if (!handle) {
        return false;
    }
    kprintf("[TagFS] %s refused: file %u was opened on an earlier mounting of "
            "this volume, and the volume has been read again since\n",
            what, handle ? handle->file_id : 0);
    return false;
}


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
    }
    spin_unlock(&g_open_table_lock);

    if (do_free)
    {
        spin_lock(&ofe->write_lock);
        spin_unlock(&ofe->write_lock);
        kfree(ofe);
    }
}



#if CONFIG_TAGFS_HOLD_GROUND

#define TAGFS_HOLD_GROUND_MS 2000u

static bool             g_hold_inside  = false;
static uint64_t         g_hold_until   = 0;
static TagFSFileHandle *g_hold_handle  = NULL;

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

static void TagFSHoldGroundTake(void)
{
    if (g_hold_inside) {
        return;
    }
    if (!tagfs_enter()) {
        return;
    }
    g_hold_inside = true;
    g_hold_until  = rdtsc() + cpu_ms_to_tsc(TAGFS_HOLD_GROUND_MS);
    kprintf("[TagFS HOLDGROUND] a caller was inside the volume when its medium "
            "left, and does not step out until the volume is back and at "
            "least %u ms have passed\n", TAGFS_HOLD_GROUND_MS);
}

static void TagFSHoldGroundRelease(void)
{
    if (!g_hold_inside || (int64_t)(rdtsc() - g_hold_until) < 0) {
        return;
    }
    g_hold_inside = false;
    tagfs_leave();
    kprintf("[TagFS HOLDGROUND] the caller has stepped out of the old volume\n");
}

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
#endif

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


static uint8_t g_tagfs_seat = BOARDROOM_NO_SEAT;

static uint32_t g_tagfs_seating = 0;

uint8_t tagfs_get_seat(void) { return g_tagfs_seat; }

static bool tagfs_recognise(void *ctx, uint8_t seat, uint8_t out_uuid[16])
{
    (void)ctx;

    MediumGround ground[GROUND_MAX_PER_MEDIUM];
    uint8_t claimed = GroundSurvey(seat, ground, GROUND_MAX_PER_MEDIUM);
    if (claimed == 0) return false;

    uint8_t want[16];
    bool    have_pass = BoardingPassVolume(want);

    uint8_t first[16];
    bool    any = false;

    for (uint8_t g = 0; g < claimed; g++) {
        DeedCopy head;
        if (DeedReadHead(seat, &ground[g], &head) != OK) {
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

static uint8_t g_volume_uuid[16];
static bool    g_volume_uuid_known = false;

static uint64_t g_volume_base;

uint64_t tagfs_get_volume_base(void) { return g_volume_base; }

static void TagFSProbeDrive(void)
{
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

#define VOLUME_DEED_BLOCKS  1u

static uint64_t bitmap_covers_blocks(uint32_t bitmap_blocks)
{
    return (uint64_t)bitmap_blocks * (uint64_t)TAGFS_BLOCK_SIZE * 8u;
}

static bool ground_ahead_is_blank(uint32_t from, uint32_t to, uint32_t *out_set)
{
    *out_set = 0;
    if (to <= from) return true;

    uint8_t *buf = (uint8_t *)kmalloc(TAGFS_BLOCK_SIZE);
    if (!buf) return false;

    uint32_t first_byte = from / 8u;
    uint32_t last_byte  = (to + 7u) / 8u;
    uint64_t base       = (uint64_t)g_state.layout.block_bitmap_block *
                          TAGFS_BLOCK_SECTORS;

    for (uint32_t off = first_byte / TAGFS_BLOCK_SIZE * TAGFS_BLOCK_SIZE;
         off < last_byte; off += TAGFS_BLOCK_SIZE) {
        if (disk_read_sectors(base + off / TAGFS_SECTOR_SIZE,
                              TAGFS_BLOCK_SECTORS, buf) != 0) {
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

static void volume_take_more_ground(uint8_t seat, uint64_t ground_sectors,
                                    DeedCopy *head)
{
    uint64_t have = head->head.sectors;

    if (ground_sectors <= have + TAGFS_BLOCK_SECTORS) {
        kprintf("[TagFS] seat %u: the volume fills the ground it was given — "
                "%llu sectors, all of it\n", seat, (unsigned long long)have);
        return;
    }

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

    uint8_t *keep = (uint8_t *)kmalloc(head->raw_bytes);
    if (!keep) return;
    memcpy(keep, head->raw, head->raw_bytes);

    DeedPen pen;
    if (DeedPenTake(&pen, head->raw, head->raw_bytes) != 0) { kfree(keep); return; }

    uint16_t lay_bytes = 0;
    void *live = DeedPenFind(&pen, VOLUME_STAMP_LAYOUT, &lay_bytes);
    if (!live || lay_bytes < sizeof(VolumeLayout)) { kfree(keep); return; }

    VolumeLayout grown;
    memcpy(&grown, &g_state.layout, sizeof(grown));
    grown.total_blocks = vol_want;
    grown.data_blocks  = (uint32_t)data_want;
    memcpy(live, &grown, sizeof(grown));

    DeedPenSetGround(&pen, new_sectors, new_tail);

    DeedPenSetRole(&pen, VOLUME_DEED_ROLE_TAIL);
    DeedPenSeal(&pen);

    bool far_end_took_it =
        tagfs_volume_write(new_tail, DEED_PEN_SECTORS, head->raw) == 0 &&
        BoardroomFlush(seat) == 0;

    if (far_end_took_it) {
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

    memcpy(&g_state.layout, &grown, sizeof(grown));
    head->head.sectors     = new_sectors;
    head->head.tail_sector = new_tail;
    kprintf("[TagFS] seat %u: the volume now runs %llu sectors; its data run is "
            "%u blocks, %u more than it had, and its far copy has moved to "
            "sector %llu\n", seat, (unsigned long long)new_sectors,
            g_state.layout.data_blocks, (uint32_t)(data_want - data_now),
            (unsigned long long)new_tail);

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
    (void)BoardroomFlush(seat);
}

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
    uint64_t ground_run    = 0;

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
        if (!have_pass) break;
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

    if (head.head.role == VOLUME_DEED_ROLE_HEAD) {
        volume_take_more_ground(seat, ground_run, &head);
    }

    DeedCopy tail;
    MediumGround stood = { .start_sector = g_volume_base,
                           .sectors      = head.head.sectors,
                           .origin       = GROUND_FROM_MBR,
                           .entry        = 0 };
    if (head.head.role == VOLUME_DEED_ROLE_TAIL) {
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

error_t tagfs_flush_cache(void)
{
    return (BoardroomFlush(g_tagfs_seat) == 0) ? OK : ERR_IO;
}

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

static void mount_owed_if_the_volume_is_there(void)
{
    if (g_tagfs_seat != BOARDROOM_NO_SEAT) {
        mount_owed();
    }
}

static bool medium_still_seated(uint8_t seat, uint32_t seating)
{
    return BoardroomSeatOccupied(seat) &&
           BoardroomSeatSeating(seat) == seating;
}


static bool tagfs_abandon(void);

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

        uint8_t old_seat = g_tagfs_seat;
        g_tagfs_seat = seat;
        bool ours = tagfs_recognise(NULL, seat, uuid) &&
                    memcmp(uuid, g_volume_uuid, 16) == 0;
        g_tagfs_seat = old_seat;

        if (!ours) {
            continue;
        }

        uint32_t seating = BoardroomSeatSeating(seat);

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
            return false;
        }

        if (tagfs_init() == OK) {
            UseContextRecall();
            mount_settled();
            return true;
        }

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

static volatile uint32_t g_boot_mount_settled = 0;

void TagFSBootMountSettled(void)
{
    __atomic_store_n(&g_boot_mount_settled, 1u, __ATOMIC_RELEASE);
}

void TagFSNoteMediumGone(void)
{
    if (!g_state.initialized) {
        return;
    }
    if (volume_medium_gone()) {
        TagFSHoldGroundTake();
    }
}

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

static void attend_arrival(void)
{
    if (g_state.initialized) {
        TagFSVolumeReturned();
        return;
    }

    if (tagfs_init() != OK) {
        mount_owed_if_the_volume_is_there();
        return;
    }

    {
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
        return;
    }
    attend_arrival();
    attending_done();
}

static uint64_t block_to_vlba(uint32_t block);
static int read_block(uint32_t block, void *buffer);
static int write_block(uint32_t block, const void *buffer);


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

    uint32_t to_fetch[TAGFS_READ_AHEAD_BLOCKS];
    uint32_t slots[TAGFS_READ_AHEAD_BLOCKS];
    uint32_t n_fetch = 0;

    spin_lock(&g_read_ahead_lock);
    for (uint32_t i = 0; i < count && i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        uint32_t block = start_block + i;
        if (block == g_read_ahead_last_block)
            continue;
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
        g_read_ahead_cache[slot].valid = false;
        to_fetch[n_fetch] = block;
        slots[n_fetch]    = slot;
        n_fetch++;
        g_read_ahead_head++;
    }
    g_read_ahead_last_block = start_block + count - 1;
    spin_unlock(&g_read_ahead_lock);

    uint32_t run_blocks = BoardroomSeatRun(tagfs_get_seat()) / 8u;
    if (run_blocks == 0) {
        run_blocks = 1;
    }
    if (run_blocks > TAGFS_READ_AHEAD_BLOCKS) {
        run_blocks = TAGFS_READ_AHEAD_BLOCKS;
    }

    uint32_t i = 0;
    while (i < n_fetch) {
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
                len = 1;
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

static void ReadAheadForget(void) {
    spin_lock(&g_read_ahead_lock);
    for (uint32_t i = 0; i < TAGFS_READ_AHEAD_BLOCKS; i++) {
        g_read_ahead_cache[i].valid = false;
    }
    spin_unlock(&g_read_ahead_lock);
}

static uint32_t data_block_of(uint32_t volume_block)
{
    return volume_block - g_state.layout.data_block;
}

static uint64_t block_to_vlba(uint32_t block)
{
    return ((uint64_t)g_state.layout.data_block + block) * TAGFS_BLOCK_SECTORS;
}

uint64_t tagfs_block_to_sector(uint32_t block)
{
    return g_volume_base + block_to_vlba(block);
}

static int read_block(uint32_t block, void *buffer)
{
    if (volume_medium_gone()) {
        return -1;
    }

    if (ReadAheadLookup(block, buffer) == 0) {
        return 0;
    }

    if (BraidIsHealthy()) {
        error_t braid_result = BraidReadBlock(tagfs_block_to_sector(block), buffer, NULL);
        if (braid_result == OK)
            return 0;
    }

    int rc = disk_read_sectors(block_to_vlba(block), 8, buffer);
    if (rc == 0)
        (void)IntegrityVerify(block, buffer);
    return rc;
}

static int write_block(uint32_t block, const void *buffer)
{
    if (volume_medium_gone()) {
        return -1;
    }

    int rc = -1;

    if (BraidIsHealthy()) {
        if (BraidWriteBlock(tagfs_block_to_sector(block), buffer, NULL) == OK)
            rc = 0;
    }
    if (rc != 0)
        rc = disk_write_sectors(block_to_vlba(block), 8, (void *)buffer);

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


static uint32_t g_ledger_current = 0;

static spinlock_t g_ledger_lock;
static char       g_ledger_use[TAGFS_SECTOR_SIZE];
static uint16_t   g_ledger_use_len;
#define LEDGER_USE_MAX  ((size_t)TAGFS_SECTOR_SIZE - sizeof(VolumeLedger))

static uint64_t ledger_vlba(uint32_t copy)
{
    return ((uint64_t)g_state.layout.state_block + copy) * TAGFS_BLOCK_SECTORS;
}

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

    if (led.bytes < sizeof(led))
        memset((uint8_t *)&led + led.bytes, 0, sizeof(led) - led.bytes);

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

bool tagfs_ledger_peek(uint32_t copy, VolumeLedger *out, char *tail, uint16_t *tail_len)
{
    if (copy >= VOLUME_LEDGER_COPIES || !out || !tail || !tail_len) return false;
    return ledger_read_copy(copy, out, tail, tail_len);
}

static error_t ledger_load(void)
{
    VolumeLedger led[VOLUME_LEDGER_COPIES];
    bool         ok[VOLUME_LEDGER_COPIES];
    uint32_t     live = 0;

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

static uint64_t g_ledger_free_as_read;

static uint64_t count_free_blocks(void)
{
    uint64_t free_blocks = 0;
    for (uint32_t b = 0; b < g_state.layout.data_blocks; b++) {
        if (!bitmap_test_bit(g_state.block_bitmap.bitmap, b)) free_blocks++;
    }
    return free_blocks;
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


static void register_well_known(uint64_t *field, TagRegistry *reg, const char *key)
{
    uint16_t tid = tag_registry_intern(reg, key, NULL);
    *field = (tid != TAGFS_INVALID_TAG_ID && tid < 64) ? (1ULL << tid) : 0;
}

static const char *const TagFsReservedKeys[] = {
#define X(k) k,
    TAGFS_RESERVED_KEYS(X)
#undef X
};
_Static_assert(sizeof(TagFsReservedKeys) / sizeof(TagFsReservedKeys[0]) == TAGFS_RESERVED_COUNT,
               "reserved-key drift");

static const char *const AuthReservedKeys[] = {
#define X(id, key) key,
    TAGFS_AUTH_KEYS(X)
#undef X
};
_Static_assert(sizeof(AuthReservedKeys) / sizeof(AuthReservedKeys[0]) == TAGFS_AUTH_COUNT,
               "auth-key drift");

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

    register_well_known(&g_wk.trashed, reg, "trashed");
    register_well_known(&g_wk.hidden, reg, "hidden");

    for (size_t i = 0; i < sizeof(TagFsReservedKeys) / sizeof(TagFsReservedKeys[0]); i++) {
        uint16_t tid = tag_registry_lookup(reg, TagFsReservedKeys[i], NULL);
        if (tid != TAGFS_INVALID_TAG_ID)
            tag_registry_mark_system(reg, tid);
    }

    for (size_t i = 0; i < TAGFS_AUTH_COUNT; i++) {
        if (strcmp(AuthReservedKeys[i], TagFsReservedKeys[i]) != 0)
            panic("auth/reserved vocab drift at index %u: auth='%s' reserved='%s'",
                  (unsigned)i, AuthReservedKeys[i], TagFsReservedKeys[i]);
    }
}


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


    TagFSProbeDrive();


    ReadAheadInit();

    error_t ground_rc = volume_take_ground(g_tagfs_seat);
    if (ground_rc != OK) {
        return ground_rc;
    }

    error_t ledger_rc = ledger_load();
    if (ledger_rc != OK) {
        return ledger_rc;
    }
    g_ledger_free_as_read = g_state.ledger.free_blocks;

    if (DiskBookInit(
            (uint64_t)g_state.layout.disk_book_block * TAGFS_BLOCK_SECTORS,
            (uint64_t)(g_state.layout.disk_book_block + 1) * TAGFS_BLOCK_SECTORS,
            (uint64_t)(g_state.layout.disk_book_block + 2) * TAGFS_BLOCK_SECTORS) != OK)
    {
        debug_printf("[TagFS] Warning: DiskBookInit failed\n");
    }

    if (TagFS_CowInit() != OK) {
        return mount_refused("its copy-on-write layer would not start",
                             ERR_COW_NOT_INITIALIZED);
    }

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

    if (DiskBookIsInitialized())
    {
        DiskBookValidateAndReplay();
    }

    if (TagFS_DedupInit() != OK) {
        return mount_refused("its deduplication index would not start",
                             ERR_DEDUP_NOT_INITIALIZED);
    }

    TagFS_SelfHealInit();

    if (BcdcInit() != OK) {
        return mount_refused("its compression dictionaries would not start",
                             ERR_NO_MEMORY);
    }

    if (TagFS_TestsInit() != OK) {
        return mount_refused("its test framework would not start",
                             ERR_NO_MEMORY);
    }

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
    if (tag_registry_load(g_state.registry, data_block_of(g_state.layout.tag_registry_block)) != OK) {
        return mount_refused("its tag registry would not read", ERR_TAGFS_CORRUPTED);
    }

    if (file_table_init(data_block_of(g_state.layout.file_table_block)) != OK) {
        return mount_refused("its file table would not read",
                             ERR_FILE_TABLE_CORRUPT);
    }

    if (meta_pool_init(data_block_of(g_state.layout.metadata_pool_block)) != OK) {
        return mount_refused("its metadata pool would not read",
                             ERR_METADATA_POOL_FULL);
    }

    g_state.bitmap_index = tag_bitmap_create(TAGFS_BITMAP_INITIAL_TAG_CAP, TAGFS_BITMAP_INITIAL_FILE_CAP);
    if (!g_state.bitmap_index) {
        return mount_refused("there is no memory for its tag index",
                             ERR_NO_MEMORY);
    }

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

    uint32_t bm_region_sectors =
        g_state.layout.block_bitmap_blocks * TAGFS_BLOCK_SECTORS;
    uint32_t bm_sector_count =
        (bitmap_bytes + TAGFS_SECTOR_SIZE - 1) / TAGFS_SECTOR_SIZE;

    if (bm_sector_count > bm_region_sectors) {
        return mount_refused("its block bitmap region is too small to hold a "
                             "bit for every block of its data run",
                             ERR_TAGFS_CORRUPTED);
    }

    uint32_t bm_buf_size = bm_sector_count * TAGFS_SECTOR_SIZE;
    uint8_t *bm_buf = kmalloc(bm_buf_size);
    if (!bm_buf)
    {
        return mount_refused("there is no memory to read its block bitmap",
                             ERR_NO_MEMORY);
    }

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

    meta_pool_mirror_init(g_state.ledger.next_file_id + 64);

    {
        uint32_t bitmap_bytes_sz = (g_state.layout.data_blocks + 7) / 8;
        uint8_t *computed_bm = kmalloc(bitmap_bytes_sz);
        if (computed_bm)
        {
            memset(computed_bm, 0, bitmap_bytes_sz);

            for (uint32_t r = 0; r < 3 && r < g_state.layout.data_blocks; r++)
            {
                bitmap_set_bit(computed_bm, r);
            }

            uint32_t files_checked = 0;
            uint32_t orphan_blocks = 0;
            uint32_t missing_blocks = 0;
            uint32_t entries_corrected = 0;
            uint32_t files_lost = 0;

            for (uint32_t fid = 1; fid < g_state.ledger.next_file_id; fid++)
            {
                uint32_t mb, mo;
                if (file_table_lookup(fid, &mb, &mo) != 0)
                    continue;

                uint32_t pb, po;
                if (meta_pool_mirror_where(fid, &pb, &po) == 0 &&
                    (pb != mb || po != mo))
                {
                    kprintf("[TagFS FSCK] file %u is named at block %u+%u and "
                            "this volume's newest metadata for it is at block "
                            "%u+%u — the pool is the newer of the two, and the "
                            "table is corrected to it\n",
                            fid, mb, mo, pb, po);
                    file_table_update(fid, pb, po);
                    mb = pb;
                    mo = po;
                    entries_corrected++;
                }

                TagFSMetadata meta;
                memset(&meta, 0, sizeof(meta));
                if (meta_pool_read(mb, mo, &meta) != 0)
                {
                    kprintf("[TagFS FSCK] file %u is named at block %u+%u and "
                            "there is no readable metadata there or anywhere "
                            "else in this volume's pool — the file is gone and "
                            "its name goes with it\n", fid, mb, mo);
                    files_lost++;
                    file_table_delete(fid);
                    continue;
                }

                if (!(meta.flags & TAGFS_FILE_ACTIVE))
                {
                    debug_printf("[TagFS FSCK] File %u has ACTIVE=0 — completing cleanup\n", fid);
                    file_table_delete(fid);
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

                for (uint16_t t = 0; t < meta.tag_count; t++)
                {
                    tag_bitmap_set(g_state.bitmap_index, meta.tag_ids[t], fid);
                }

                tagfs_metadata_free(&meta);
            }

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

            IntegrityMarkMapBlocks(computed_bm, g_state.layout.data_blocks);

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
                kprintf("[TagFS FSCK] Bitmap mismatch: %u orphan, %u missing - additive repair\n",
                        orphan_blocks, missing_blocks);
                for (uint32_t b = 0; b < g_state.layout.data_blocks; b++) {
                    if (bitmap_test_bit(computed_bm, b))
                        bitmap_set_bit(g_state.block_bitmap.bitmap, b);
                }
                free_list_build();

                uint32_t used = 0;
                for (uint32_t b = 0; b < g_state.layout.data_blocks; b++)
                {
                    if (bitmap_test_bit(g_state.block_bitmap.bitmap, b))
                        used++;
                }
                g_state.ledger.free_blocks = g_state.layout.data_blocks - used;
            }

            if (entries_corrected || files_lost) {
                kprintf("[TagFS FSCK] %u file table entry(s) corrected from "
                        "this volume's metadata pool, %u file(s) lost\n",
                        entries_corrected, files_lost);
            }

            debug_printf("[TagFS FSCK] Checked %u files, bitmap %s\n",
                         files_checked,
                         (orphan_blocks == 0 && missing_blocks == 0) ? "OK" : "repaired");
            kfree(computed_bm);
        }
    }

    {
        uint64_t counted = count_free_blocks();
        g_state.ledger.free_blocks = counted;
        if (counted != g_ledger_free_as_read) {
            bool kept = (tagfs_write_ledger() == OK);
            kprintf("[TagFS] the ledger came off the medium saying %llu blocks "
                    "were free and the bitmap says %llu — a block is taken in "
                    "the bitmap, so that is the number kept%s\n",
                    (unsigned long long)g_ledger_free_as_read,
                    (unsigned long long)counted,
                    kept ? ", and it is written back"
                         : "; the medium would not take it, so it is counted "
                           "again on the next mount");
        }
    }

    kprintf("[TagFS] %u data blocks, %llu free, %llu files\n",
            g_state.layout.data_blocks,
            (unsigned long long)g_state.ledger.free_blocks,
            (unsigned long long)g_state.ledger.total_files);

    tagfs_init_well_known_tags();

    g_state.initialized = true;

    __atomic_add_fetch(&g_mount_epoch, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_door, VOLUME_UP, __ATOMIC_RELEASE);

    if (IntegrityInit() != OK)
        kprintf("[TagFS] integrity map unavailable - continuing without verify-on-read\n");
    else
        kprintf("[TagFS] data-integrity verify-on-read active\n");

    kprintf("[TagFS] the volume on seat %u is MOUNTED — %llu file(s)\n",
            g_tagfs_seat, (unsigned long long)g_state.ledger.total_files);

    kprintf("[TagFS] what this volume promises: a file's content reaches the "
            "medium as it is written; its name, the table, the bitmap and the "
            "ledger are made durable by `anchor` and by an orderly shutdown\n");

    TagFSHoldGroundOpenFile();
    return 0;
}


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

    meta_pool_flush();
    file_table_flush();
    meta_pool_flush_retires();

    IntegrityFlush();
    IntegrityDrainReports();

    uint32_t bitmap_bytes = (g_state.layout.data_blocks + 7) / 8;
    uint32_t sector_count = (bitmap_bytes + TAGFS_SECTOR_SIZE - 1) / TAGFS_SECTOR_SIZE;
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

    tagfs_flush_cache();
}

static void tagfs_clear_the_ground(void)
{
    UseContextUnbind();

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
    DiskBookShutdown();

    g_state.initialized = false;
}

static bool tagfs_abandon(void)
{
    if (!g_state.initialized) {
        return true;
    }

    if (__atomic_exchange_n(&g_door, (uint8_t)VOLUME_LEAVING,
                            __ATOMIC_ACQ_REL) == VOLUME_UP) {
        __atomic_store_n(&g_abandon_pending, 1u, __ATOMIC_RELEASE);
    }

    uint32_t inside = __atomic_load_n(&g_inside, __ATOMIC_ACQUIRE);
    if (inside != 0) {
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

void tagfs_shutdown(void)
{
    if (!g_state.initialized) {
        return;
    }

    tagfs_sync();

    __atomic_store_n(&g_door, (uint8_t)VOLUME_LEAVING, __ATOMIC_RELEASE);
    debug_printf("[TagFS] Shutdown complete\n");
}

static void tagfs_ask_for_the_mount_again(void)
{
    if (__atomic_load_n(&g_mount_owed, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    if (g_state.initialized) {
        mount_settled();
        return;
    }

    if ((int64_t)(rdtsc() - g_mount_due) < 0) {
        return;
    }

    if (g_mount_tries >= TAGFS_MOUNT_RETRY_TRIES) {
        kprintf("[TagFS] the volume in this room would not mount in %u "
                "attempts — the machine stops asking and waits for a medium to "
                "arrive\n", TAGFS_MOUNT_RETRY_TRIES);
        mount_settled();
        return;
    }

    if (!attending_take()) {
        return;
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

void TagFSServiceIfPending(void)
{
    tagfs_ask_for_the_mount_again();

    if (__atomic_load_n(&g_abandon_pending, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    TagFSHoldGroundRelease();

    if (__atomic_load_n(&g_inside, __ATOMIC_ACQUIRE) != 0) {
        return;
    }

    if (!attending_take()) {
        return;
    }

    if (tagfs_abandon()) {
        attend_arrival();
        TagFSHoldGroundAfterReturn();
    }

    attending_done();
}



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

    kprintf("[TESTS] TagFS: %u run, %u passed, %u failed, %u skipped\n",
            stats.total_tests, stats.total_passed,
            stats.total_failed, stats.total_skipped);

    return result;
}


static void tagfs_name_stem(const char *filename, char *out, size_t out_size)
{
    size_t flen = strlen(filename);
    size_t slen = flen;
    for (size_t i = flen; i > 0; i--)
    {
        if (filename[i - 1] == '.')
        {
            slen = i - 1;
            break;
        }
    }
    if (slen >= out_size)
        slen = out_size - 1;
    memcpy(out, filename, slen);
    out[slen] = '\0';
}

static uint16_t tagfs_name_tag_intern(const char *filename)
{
    char stem[128];
    tagfs_name_stem(filename, stem, sizeof(stem));
    if (stem[0] == '\0' || !g_state.registry)
        return TAGFS_INVALID_TAG_ID;
    uint16_t tag = tag_registry_intern(g_state.registry, stem, NULL);
    if (tag_registry_is_dirty())
    {
        tag_registry_flush(g_state.registry);
    }
    return tag;
}

static uint16_t tagfs_name_tag_lookup(const char *filename)
{
    char stem[128];
    tagfs_name_stem(filename, stem, sizeof(stem));
    if (stem[0] == '\0' || !g_state.registry)
        return TAGFS_INVALID_TAG_ID;
    return tag_registry_lookup(g_state.registry, stem, NULL);
}


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

    uint16_t auto_tag = tagfs_name_tag_intern(filename);

    spin_lock(&g_state.lock);

    uint32_t file_id = g_state.ledger.next_file_id++;

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

        if (auto_tag != TAGFS_INVALID_TAG_ID)
        {
            final_tags[final_count++] = auto_tag;
        }

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


    uint32_t meta_block, meta_offset;
    if (meta_pool_write(&meta, &meta_block, &meta_offset) != 0)
    {
        debug_printf("[TagFS] create_file: meta_pool_write failed for file_id=%u\n", file_id);
        kfree(meta.tag_ids);
        kfree(meta.filename);
        spin_unlock(&g_state.lock);
        return -1;
    }

    if (file_table_update(file_id, meta_block, meta_offset) != 0)
    {
        debug_printf("[TagFS] create_file: file_table_update failed for file_id=%u\n", file_id);
        kfree(meta.tag_ids);
        kfree(meta.filename);
        spin_unlock(&g_state.lock);
        return -1;
    }

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

void tagfs_free_blocks_internal(uint32_t start_block, uint32_t count);


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

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    int has_meta = meta_pool_read(meta_block, meta_offset, &meta);


    tag_bitmap_remove_file(g_state.bitmap_index, file_id);

    file_table_delete(file_id);

    if (has_meta == 0 && meta.extents)
    {
        for (uint16_t i = 0; i < meta.extent_count; i++)
        {
            tagfs_free_blocks_internal(meta.extents[i].start_block, meta.extents[i].block_count);
        }
        tagfs_metadata_free(&meta);
    }

    meta_pool_delete(meta_block, meta_offset);

    if (g_state.ledger.total_files > 0)
    {
        g_state.ledger.total_files--;
    }

    spin_unlock(&g_state.lock);

    debug_printf("[TagFS] Deleted file_id=%u\n", file_id);
    return 0;
}


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

    uint16_t new_tag = tagfs_name_tag_intern(new_filename);

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

    uint16_t old_tag = meta.filename ? tagfs_name_tag_lookup(meta.filename)
                                     : TAGFS_INVALID_TAG_ID;

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

    bool old_tag_removed = false;
    bool new_tag_added   = false;
    if (old_tag != new_tag)
    {
        if (old_tag != TAGFS_INVALID_TAG_ID)
        {
            for (uint16_t i = 0; i < meta.tag_count; i++)
            {
                if (meta.tag_ids[i] == old_tag)
                {
                    for (uint16_t j = i; j + 1 < meta.tag_count; j++)
                        meta.tag_ids[j] = meta.tag_ids[j + 1];
                    meta.tag_count--;
                    old_tag_removed = true;
                    break;
                }
            }
        }
        if (new_tag != TAGFS_INVALID_TAG_ID)
        {
            bool present = false;
            for (uint16_t i = 0; i < meta.tag_count; i++)
                if (meta.tag_ids[i] == new_tag) present = true;
            if (!present)
            {
                uint16_t *ids = kmalloc(sizeof(uint16_t) * (meta.tag_count + 1u));
                if (!ids)
                {
                    tagfs_metadata_free(&meta);
                    spin_unlock(&g_state.lock);
                    return -1;
                }
                ids[0] = new_tag;
                if (meta.tag_ids)
                    memcpy(ids + 1, meta.tag_ids, sizeof(uint16_t) * meta.tag_count);
                kfree(meta.tag_ids);
                meta.tag_ids = ids;
                meta.tag_count++;
                new_tag_added = true;
            }
        }
    }

    uint32_t new_block, new_offset;
    if (meta_pool_write(&meta, &new_block, &new_offset) != 0)
    {
        debug_printf("[TagFS] rename_file: meta_pool_write failed\n");
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }

    file_table_update(file_id, new_block, new_offset);
    if (old_tag_removed)
        tag_bitmap_clear(g_state.bitmap_index, old_tag, file_id);
    if (new_tag_added)
        tag_bitmap_set(g_state.bitmap_index, new_tag, file_id);
    meta_pool_delete(meta_block, meta_offset);

    tagfs_metadata_free(&meta);

    spin_unlock(&g_state.lock);
    return 0;
}


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

    for (uint16_t i = 0; i < meta.tag_count; i++)
    {
        if (meta.tag_ids[i] == tag_id)
        {
            tagfs_metadata_free(&meta);
            spin_unlock(&g_state.lock);
            return 0;
        }
    }

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

    if (result > 0)
    {
        uint16_t trashed_id = g_wk.trashed
                                  ? (uint16_t)__builtin_ctzll(g_wk.trashed)
                                  : TAGFS_INVALID_TAG_ID;
        uint16_t hidden_id = g_wk.hidden
                                 ? (uint16_t)__builtin_ctzll(g_wk.hidden)
                                 : TAGFS_INVALID_TAG_ID;

        bool query_has_trashed = false;
        bool query_has_hidden = false;

        for (uint32_t i = 0; i < tag_count; i++)
        {
            if (tag_ids[i] == trashed_id)
                query_has_trashed = true;
            if (tag_ids[i] == hidden_id)
                query_has_hidden = true;
        }
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
    spin_lock(&g_state.lock);
    uint32_t max_id = g_state.ledger.next_file_id;
    spin_unlock(&g_state.lock);

    for (uint32_t fid = 1; fid < max_id && found < max_results; fid++)
    {
        uint32_t mb, mo;
        if (file_table_lookup(fid, &mb, &mo) == 0 && mb != 0)
        {
            if (file_has_system_behavior_tag(fid, true, true))
                continue;
            out_file_ids[found++] = fid;
        }
    }

    return (int)found;
}


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

    tagfs_auto_snapshot_before_write(handle->file_id);

    if (handle->ofe)
    {
        if (!spin_trylock(&handle->ofe->write_lock)) {
            uint8_t self_core = amp_get_core_index();
            while (!spin_trylock(&handle->ofe->write_lock)) {
                BatonPump(self_core);
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
        }

        uint64_t offset_in_extent = file_pos - extent_start;
        uint32_t block_index = (uint32_t)(offset_in_extent / TAGFS_BLOCK_SIZE);
        uint32_t offset_in_block = (uint32_t)(offset_in_extent % TAGFS_BLOCK_SIZE);
        uint32_t disk_block = handle->extents[found].start_block + block_index;

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

        uint32_t write_target = disk_block;
        if (is_existing_block && block_index == 0 && TagFS_CowIsActive(handle->file_id)) {
            uint32_t cow_block = 0;
            if (TagFS_CowBeforeWrite(handle->file_id, disk_block, &cow_block) == OK && cow_block != 0) {
                handle->extents[found].start_block = cow_block;
                write_target = cow_block;
                TagFS_CowAfterWrite(handle->file_id, disk_block, cow_block);
            }
        }

        bool wrote_block = false;
        if (newly_allocated && block_index == 0 &&
            offset_in_block == 0 && chunk == TAGFS_BLOCK_SIZE &&
            TagFS_DedupIsInitialized()) {
            TagFS_DedupRegister(write_target, block_buf, handle->file_id);
        }

        if (!wrote_block) {
            if (write_block(write_target, block_buf) != 0)
            {
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


static int tagfs_truncate_locked(uint32_t file_id, uint64_t new_size)
{
    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0)
        return -ERR_FILE_NOT_FOUND;

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0)
        return -ERR_IO;

    if (new_size > meta.size)
    {
        tagfs_metadata_free(&meta);
        return -ERR_INVALID_ARGUMENT;
    }
    if (new_size == meta.size)
    {
        tagfs_metadata_free(&meta);
        return 0;
    }

    FileExtent *old_ex = meta.extents;
    uint16_t    old_n  = meta.extent_count;

    uint32_t keep_blocks = (uint32_t)((new_size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE);

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

    uint32_t new_mb, new_mo;
    if (meta_pool_write(&meta, &new_mb, &new_mo) != 0)
    {
        tagfs_metadata_free(&meta);
        if (old_ex) kfree(old_ex);
        return -ERR_IO;
    }
    file_table_update(file_id, new_mb, new_mo);
    meta_pool_delete(meta_block, meta_offset);

    for (uint16_t i = 0; i < old_n; i++)
    {
        uint32_t drop_from = 0;
        if (i < kept)
        {
            if (i != trim_at)
                continue;
            drop_from = trim_to;
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

    if (TagFS_CowIsActive(file_id))
        return -ERR_INVALID_OPERATION;

    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    if (!handle)
        return -ERR_FILE_NOT_FOUND;

    if (handle->ofe)
    {
        if (!spin_trylock(&handle->ofe->write_lock)) {
            uint8_t self_core = amp_get_core_index();
            while (!spin_trylock(&handle->ofe->write_lock)) {
                BatonPump(self_core);
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


int tagfs_alloc_blocks_internal(uint32_t count, uint32_t *out_start_block)
{
    FreeExtent *prev = NULL;
    FreeExtent *cur = g_state.block_bitmap.free_list;

    while (cur)
    {
        if (cur->count >= count)
        {
            *out_start_block = cur->start;

            for (uint32_t i = 0; i < count; i++)
            {
                bitmap_set_bit(g_state.block_bitmap.bitmap, cur->start + i);
            }

            if (cur->count == count)
            {
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

static void free_run_locked(uint32_t start_block, uint32_t count)
{
    if (count == 0)
        return;

    uint32_t end = start_block + count;

    for (uint32_t i = 0; i < count; i++)
    {
        bitmap_clear_bit(g_state.block_bitmap.bitmap, start_block + i);
    }
    g_state.ledger.free_blocks += count;

    FreeExtent *prev = NULL;
    FreeExtent *cur = g_state.block_bitmap.free_list;

    while (cur && cur->start < start_block)
    {
        prev = cur;
        cur = cur->next;
    }

    bool merge_right = cur && cur->start == end;
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

void tagfs_free_blocks_internal(uint32_t start_block, uint32_t count)
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

    if (meta_pool_read_cached(file_id, out) == 0)
    {
        return 0;
    }

    uint32_t block, offset;
    if (file_table_lookup(file_id, &block, &offset) != 0)
        return -1;
    return meta_pool_read(block, offset, out);
}

TagFSState *tagfs_get_state(void)
{
    return &g_state;
}


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

uint16_t tagfs_tag_lookup(const char *tag)
{
    return tag_id_for(tag, false);
}

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

bool tagfs_tag_key(uint16_t tag_id, char *out, size_t out_size)
{
    return tag_name_of(tag_id, out, out_size, false);
}

bool tagfs_tag_text(uint16_t tag_id, char *out, size_t out_size)
{
    return tag_name_of(tag_id, out, out_size, true);
}

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

    if (meta.extent_count <= 1)
    {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return 0;
    }

    uint32_t total_blocks = 0;
    for (uint16_t i = 0; i < meta.extent_count; i++)
    {
        total_blocks += meta.extents[i].block_count;
    }

    uint32_t new_start;
    if (target_block != 0 && target_block < g_state.layout.data_blocks) {
        bool area_free = true;
        for (uint32_t b = 0; b < total_blocks && area_free; b++) {
            if (bitmap_test_bit(g_state.block_bitmap.bitmap, target_block + b)) {
                area_free = false;
            }
        }

        if (area_free) {
            new_start = target_block;
            for (uint32_t b = 0; b < total_blocks; b++) {
                bitmap_set_bit(g_state.block_bitmap.bitmap, new_start + b);
            }
            g_state.ledger.free_blocks -= total_blocks;
        } else {
            spin_unlock(&g_state.lock);
            if (tagfs_alloc_blocks(total_blocks, &new_start) != 0)
            {
                tagfs_metadata_free(&meta);
                return -1;
            }
            spin_lock(&g_state.lock);
        }
    } else {
        spin_unlock(&g_state.lock);
        if (tagfs_alloc_blocks(total_blocks, &new_start) != 0)
        {
            tagfs_metadata_free(&meta);
            return -1;
        }
        spin_lock(&g_state.lock);
    }

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
        spin_unlock(&g_state.lock);
        tagfs_free_blocks(new_start, total_blocks);
        tagfs_metadata_free(&meta);
        return -1;
    }

    FileExtent* old_extents   = meta.extents;
    uint16_t    old_extent_ct = meta.extent_count;

    meta.extents = kmalloc(sizeof(FileExtent));
    if (!meta.extents)
    {
        meta.extents      = old_extents;
        meta.extent_count = old_extent_ct;
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        tagfs_free_blocks(new_start, total_blocks);
        return -1;
    }

    meta.extents[0].start_block = new_start;
    meta.extents[0].block_count = (uint16_t)total_blocks;
    meta.extent_count = 1;
    meta.modified_time = rtc_get_unix64();

    uint32_t new_mb, new_mo;
    if (meta_pool_write(&meta, &new_mb, &new_mo) != 0)
    {
        kprintf("[TagFS] file %u was copied to block %u to be defragmented "
                "and this volume would not take the metadata naming it there "
                "— the file is left exactly where it was, and the copy's "
                "ground goes back\n", file_id, new_start);
        kfree(old_extents);
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        tagfs_free_blocks(new_start, total_blocks);
        return -1;
    }

    file_table_update(file_id, new_mb, new_mo);
    meta_pool_delete(meta_block, meta_offset);

    for (uint16_t i = 0; i < old_extent_ct; i++)
    {
        for (uint16_t b = 0; b < old_extents[i].block_count; b++)
        {
            bitmap_clear_bit(g_state.block_bitmap.bitmap,
                             old_extents[i].start_block + b);
        }
        g_state.ledger.free_blocks += old_extents[i].block_count;
    }
    kfree(old_extents);

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