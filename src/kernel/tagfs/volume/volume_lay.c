#include "volume_lay.h"
#include "tagfs_volume.h"
#include "../ink/ink.h"
#include "../file_map/file_map.h"
#include "../deed/deed.h"
#include "../mount/ground_grow.h"
#include "disk_table.h"
#include "boardroom.h"
#include "book_bind.h"
#include "random.h"
#include "rtc.h"
#include "atomics.h"

/* The first Tally's seq, and slot 0's — as every maker lays them (§16.2 п.23). */
#define LAY_TALLY_SEQ  1u
#define LAY_SLOT_SEQ   2u
/* Blanked a run at a time, the front of the ground before the book. */
#define LAY_BLANK_BLOCKS  16u
/* The files the system keeps, looked up a page of the home volume's at a time. */
#define LAY_LIST_PAGE  64u

static const char* const s_reserved[] = {
#define X(id, key) key,
    TAGFS_RESERVED_KEYS(X)
#undef X
};

static VolumeLaying* volatile s_laying;
static volatile uint32_t      s_busy;
static volatile uint32_t      s_serving;

// ----------------------------------------------------------------------------
// The ground
// ----------------------------------------------------------------------------

static bool VolumeLayStillThere(const VolumeLaying* l)
{
    return BoardroomSeatOccupied(l->seat) && BoardroomSeatSeating(l->seat) == l->seating;
}

/* What a refused write of the laying answers. */
static error_t VolumeLayRefusal(const VolumeLaying* l, const char* what)
{
    if (!VolumeLayStillThere(l)) {
        kprintf("[TagFS] seat %u: the disk left while %s was laid — the partition holds no volume\n", l->seat, what);
        return ERR_DEVICE_NOT_READY;
    }
    if (BoardroomSeatRefusesWrites(l->seat)) {
        kprintf("[TagFS] seat %u: the medium refuses writes — it is protected, and no volume is laid on it\n",
                l->seat);
        return ERR_PERMISSION_DENIED;
    }
    kprintf("[TagFS] seat %u: the medium refused the write of %s — the partition holds no volume\n", l->seat, what);
    return ERR_WRITE_FAILED;
}

/* The binder's blocks and the laying's own onto the ground: the volume's
 * block `block`, the seat asked first. */
static int VolumeLaySink(void* ctx, uint64_t block, const void* data, uint64_t bytes)
{
    const VolumeLaying* l = ctx;
    if (!VolumeLayStillThere(l))
        return -1;
    return BoardroomWrite(l->seat, l->ground.start_sector + block * VOLUME_BLOCK_SECTORS,
                          (uint32_t)(bytes / BOARDROOM_SECTOR_BYTES), data, NULL) == 0 ? 0 : -1;
}

static error_t VolumeLayFlush(const VolumeLaying* l, const char* what)
{
    if (!VolumeLayStillThere(l) || BoardroomFlush(l->seat) != 0)
        return VolumeLayRefusal(l, what);
    return OK;
}

/* The partition asked for, held to what a laying may stand on: *base and
 * *sectors its ground in the room's sectors. */
static error_t VolumeLayGround(VolumeLaying* l)
{
    DiskTable t;
    error_t   rc = DiskTableRead(l->seat, &t);
    if (rc != OK)
        return rc;
    if (t.seating != l->seating || t.state == DISK_TABLE_REFUSED) {
        rc = t.state == DISK_TABLE_REFUSED ? ERR_DEVICE_NOT_READY : ERR_OBJECT_NOT_FOUND;
        DiskTableLetGo(&t);
        return rc;
    }
    const DiskPart* part = NULL;
    for (uint32_t i = 0; i < t.part_count; i++)
        if (t.parts[i].entry == l->entry)
            part = &t.parts[i];
    uint64_t ratio = t.block_bytes / BOARDROOM_SECTOR_BYTES;
    if (!part)
        rc = ERR_OUT_OF_RANGE;
    else if (part->kind != DISK_PART_BOXOS)
        rc = ERR_INVALID_ARGUMENT;
    if (rc == OK) {
        l->ground = (MediumGround){ .start_sector = part->first * ratio,
                                    .sectors      = (part->last - part->first + 1u) * ratio,
                                    .origin       = t.state == DISK_TABLE_MBR ? GROUND_FROM_MBR : GROUND_FROM_GPT,
                                    .entry        = (uint8_t)l->entry };
        uint32_t numbers[8];
        uint64_t grounds[8];
        uint32_t standing = TagFSVolumesOn(l->seat, numbers, grounds, 8);
        for (uint32_t i = 0; i < standing && i < 8; i++)
            if (grounds[i] == l->ground.start_sector) {
                kprintf("[TagFS] seat %u: volume %u of the house stands on entry %u — no volume is laid over "
                        "it\n", l->seat, numbers[i], l->entry);
                rc = ERR_BUSY;
            }
    }
    DiskTableLetGo(&t);
    if (rc == OK && l->name_len && !TallyNameSpelled(l->name, l->name_len))
        rc = ERR_INVALID_ARGUMENT;
    return rc;
}

// ----------------------------------------------------------------------------
// The book and the Deed
// ----------------------------------------------------------------------------

/* The leaves of a volume that holds no file yet: the reserved words at their
 * ids, as every maker lays them, and the name a person gave it (§1.4). */
static error_t VolumeLayLeaves(const VolumeLaying* l, BindLeaves* leaves)
{
    for (uint32_t id = 0; id < TAGFS_RESERVED_COUNT; id++) {
        TallyTag t    = { .flags = 0, .key = (const uint8_t*)s_reserved[id], .key_len = strlen(s_reserved[id]),
                          .value = NULL, .value_len = 0 };
        uint8_t* body = BindBody(leaves, 30u + t.key_len);
        if (!body || BindAdd(leaves, TALLY_TAGS, (TallyKey){ id, 0 }, body, TallyTagPut(body, &t), 1) != 0)
            return ERR_NO_MEMORY;
    }
    if (l->name_len) {
        uint8_t* body = BindBody(leaves, 10u + l->name_len);
        if (!body || BindAdd(leaves, TALLY_USE, (TallyKey){ 1, 0 }, body,
                             TallyUsePut(body, l->name, l->name_len), 1) != 0)
            return ERR_NO_MEMORY;
    }
    return OK;
}

/* One copy of the Deed — GEOMETRY · FRAME · BORN · BOOT, as every maker
 * stamps one, the BOOT empty until the kernel is laid — into `block`. */
static void VolumeLayDeed(const VolumeLaying* l, uint8_t* block, const VolumeFrame* frame, uint64_t blocks,
                          uint32_t role)
{
    VolumeGeometry geo  = VolumeGeometryLaid();
    VolumeBorn     born;
    VolumeBoot     boot;
    DeedPen        pen;
    memset(&born, 0, sizeof(born));
    memset(&boot, 0, sizeof(boot));
    born.created_unix = l->made_at;
    memcpy(born.maker, "boxos kernel", 12);
    DeedPenOpen(&pen, block, DEED_PEN_BYTES);
    (void)DeedPenSay(&pen, VOLUME_STAMP_GEOMETRY, &geo, sizeof(geo));
    (void)DeedPenSay(&pen, VOLUME_STAMP_FRAME, frame, sizeof(*frame));
    (void)DeedPenSay(&pen, VOLUME_STAMP_BORN, &born, sizeof(born));
    (void)DeedPenSay(&pen, VOLUME_STAMP_BOOT, &boot, sizeof(boot));
    DeedPenSetUuid(&pen, l->uuid);
    DeedPenSetGround(&pen, blocks * VOLUME_BLOCK_SECTORS, (blocks - 1u) * VOLUME_BLOCK_SECTORS);
    DeedPenSetRole(&pen, role);
    DeedPenSeal(&pen);
}

/* The book and the Deed on the ground, in a maker's order (volume_lay.h). */
static error_t VolumeLayWrite(VolumeLaying* l, const VolumeFrame* frame, uint64_t blocks, const BindPlan* plan)
{
    const BindSink sink   = { VolumeLaySink, l };
    uint64_t       failed = 0;
    uint8_t*       blank  = kmalloc(LAY_BLANK_BLOCKS * VOLUME_BLOCK_BYTES);
    uint8_t*       deed   = kmalloc(DEED_PEN_BYTES);
    error_t        rc     = blank && deed ? OK : ERR_NO_MEMORY;
    if (rc == OK) {
        memset(blank, 0, LAY_BLANK_BLOCKS * VOLUME_BLOCK_BYTES);
        if (VolumeLaySink(l, 0, blank, VOLUME_BLOCK_BYTES) != 0 ||
            VolumeLaySink(l, blocks - 1u, blank, VOLUME_BLOCK_BYTES) != 0)
            rc = VolumeLayRefusal(l, "the blanking of a Deed's blocks");
    }
    if (rc == OK)
        rc = VolumeLayFlush(l, "the Deed's blocks blanked");
    for (uint64_t b = 1; rc == OK && b < frame->data_block; b += LAY_BLANK_BLOCKS) {
        uint64_t n = frame->data_block - b < LAY_BLANK_BLOCKS ? frame->data_block - b : LAY_BLANK_BLOCKS;
        if (VolumeLaySink(l, b, blank, n * VOLUME_BLOCK_BYTES) != 0)
            rc = VolumeLayRefusal(l, "the front of the ground");
    }
    if (rc == OK) {
        const SpineHead   head   = { LAY_TALLY_SEQ, 0, 0, l->made_at * 1000000000ull };
        const SpineCounts counts = { 1u, TAGFS_RESERVED_COUNT, 1u };
        int               bound  = BindTally(&sink, plan, l->uuid, LAY_TALLY_SEQ, &failed);
        if (bound == BIND_OK)
            bound = BindSpine(&sink, plan, frame->spine_block, frame->spine_stride, l->uuid, &head, &counts,
                              LAY_SLOT_SEQ, &failed);
        rc = bound == BIND_OK ? OK : bound == BIND_NO_MEMORY ? ERR_NO_MEMORY : VolumeLayRefusal(l, "the book");
    }
    if (rc == OK)
        rc = VolumeLayFlush(l, "the book before its Deed");
    if (rc == OK) {
        VolumeLayDeed(l, deed, frame, blocks, VOLUME_DEED_ROLE_HEAD);
        if (VolumeLaySink(l, 0, deed, DEED_PEN_BYTES) != 0)
            rc = VolumeLayRefusal(l, "the Deed");
    }
    if (rc == OK) {
        VolumeLayDeed(l, deed, frame, blocks, VOLUME_DEED_ROLE_TAIL);
        if (VolumeLaySink(l, blocks - 1u, deed, DEED_PEN_BYTES) != 0)
            rc = VolumeLayRefusal(l, "the Deed's far copy");
    }
    if (rc == OK)
        rc = VolumeLayFlush(l, "the Deed");
    kfree(blank);
    kfree(deed);
    return rc;
}

/* A new identity, the book laid out for it in memory — refused before a byte
 * is written when the ground cannot hold it — and written. */
static error_t VolumeLayBook(VolumeLaying* l)
{
    const uint32_t grain  = VolumeGeometryLaid().grain_bytes / VOLUME_BLOCK_BYTES;
    VolumeFrame    frame  = VolumeFrameLaid(grain);
    uint64_t       blocks = l->ground.sectors / VOLUME_BLOCK_SECTORS;
    BindLeaves     leaves;
    BindPlan       plan;
    memset(&leaves, 0, sizeof(leaves));
    memset(&plan, 0, sizeof(plan));
    leaves.grain = grain;
    leaves.grid  = 1;
    leaves.seq   = LAY_TALLY_SEQ;
    RandomGuid(l->uuid);
    l->made_at = rtc_get_unix64();

    error_t rc = blocks > frame.data_block + 1u ? VolumeLayLeaves(l, &leaves) : ERR_TAGFS_NO_SPACE;
    if (rc == OK) {
        int laid = QuireLay(&leaves, frame.data_block, blocks - 1u, &plan);
        rc = laid == BIND_OK ? OK : laid == BIND_NO_ROOM ? ERR_TAGFS_NO_SPACE : ERR_NO_MEMORY;
    }
    if (rc == ERR_TAGFS_NO_SPACE)
        kprintf("[TagFS] seat %u: entry %u is %llu block(s) — too small for a volume's book\n", l->seat, l->entry,
                (unsigned long long)blocks);
    if (rc == OK)
        rc = VolumeLayWrite(l, &frame, blocks, &plan);
    if (rc == OK) {
        char text[33];
        DeedUuidText(l->uuid, text);
        kprintf("[TagFS] seat %u: a volume is laid on entry %u — %s, %llu block(s), its book bound and its Deed at "
                "both ends; the house is to take it in\n", l->seat, l->entry, text, (unsigned long long)blocks);
    }
    BindPlanFree(&plan);
    BindLeavesFree(&leaves);
    return rc;
}

error_t VolumeLayBegin(VolumeLaying* l)
{
    if (__atomic_exchange_n(&s_busy, 1u, __ATOMIC_ACQ_REL))
        return ERR_WOULD_BLOCK;
    l->rc     = OK;
    l->number = 0;
    l->roles  = 0;
    l->vol    = NULL;
    l->items  = NULL;
    error_t rc = VolumeLayGround(l);
    if (rc == OK)
        rc = VolumeLayBook(l);
    if (rc != OK) {
        __atomic_store_n(&s_busy, 0u, __ATOMIC_RELEASE);
        return rc;
    }
    l->step = VOLUME_LAY_TAKEN;
    __atomic_store_n(&s_laying, l, __ATOMIC_RELEASE);
    TagFSHouseLook();
    return OK;
}

// ----------------------------------------------------------------------------
// After the book: taken in, the system given, the kernel named, settled
// ----------------------------------------------------------------------------

/* The laying over: the answer given and the next one let in. */
static void VolumeLayEnd(VolumeLaying* l, error_t rc)
{
    if (l->rc == OK)
        l->rc = rc;
    kfree(l->items);
    l->items = NULL;
    if (l->rc == OK)
        kprintf("[TagFS] volume %u is laid: %u file(s) the system keeps, the kernel named in its Deed, all of it "
                "settled\n", l->number, l->roles);
    else
        kprintf("[TagFS] the volume laid on seat %u was not given the system (error %u) — it holds what was laid "
                "before\n", l->seat, (unsigned)l->rc);
    __atomic_store_n(&s_laying, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&s_busy, 0u, __ATOMIC_RELEASE);
    l->ended(l);
}

/* The files the home volume keeps in a role, the kernel's first: into
 * l->items, how many in *count. ERR_FILE_NOT_FOUND for a home volume that
 * keeps no kernel. Inside the home volume's door. */
static error_t VolumeLayRoleFiles(VolumeLaying* l, TagFSVolume* home, uint32_t* count)
{
    uint64_t  ids[LAY_LIST_PAGE];
    uint32_t  room = 0, n = 0, kernels = 0;
    TagFSPage page;
    *count = 0;
    for (uint32_t pass = 0; pass < 2u; pass++) {
        uint64_t from = 1;
        do {
            if (TagFSListPage(home, from, ids, LAY_LIST_PAGE, false, &page) != OK)
                return ERR_DEVICE_NOT_READY;
            for (uint32_t i = 0; i < page.delivered; i++) {
                FileRecord* rec  = FileMapRecordHold(home->state.files, ids[i], NULL);
                uint32_t    role = rec ? rec->flags & TAGFS_FILE_ROLE_MASK : 0u;
                FileRecordRelease(rec);
                if (!role || (pass == 0) != (role == TAGFS_FILE_ROLE_KERNEL))
                    continue;
                if (n == room) {
                    room = room ? room * 2u : 32u;
                    CarryItem* grown = krealloc(l->items, room * sizeof(CarryItem));
                    if (!grown)
                        return ERR_NO_MEMORY;
                    l->items = grown;
                }
                l->items[n++] = (CarryItem){ .from = home, .fid = ids[i], .arrived = 0 };
                kernels += pass == 0;
            }
            from = page.next;
        } while (from);
    }
    *count = n;
    return kernels ? OK : ERR_FILE_NOT_FOUND;
}

static void VolumeLayCarried(Carry* c)
{
    VolumeLaying* l = container_of(c, VolumeLaying, carry);
    l->roles        = c->done;
    l->rc           = c->rc;
    __atomic_store_n(&l->step, VOLUME_LAY_BOOT, __ATOMIC_RELEASE);
}

/* The house took the volume in and mounted it: the maker's carry begins. A
 * volume the house will not mount is said, and the laying ends. */
static void VolumeLayTaken(VolumeLaying* l)
{
    TagFSVolume* vol = TagFSVolumeOfUuid(l->uuid);
    if (!vol || !TagFSVolumeMounted(vol)) {
        bool coming = vol ? __atomic_load_n(&vol->mount.arrival_owed, __ATOMIC_ACQUIRE) ||
                            __atomic_load_n(&vol->mount.mount_owed, __ATOMIC_ACQUIRE)
                          : TagFSHouseLooking();
        if (!coming) {
            kprintf("[TagFS] the house did not %s the volume laid on seat %u\n", vol ? "mount" : "take in",
                    l->seat);
            VolumeLayEnd(l, ERR_TAGFS_NOT_INITIALIZED);
        }
        return;
    }
    l->vol    = vol;
    l->number = TagFSVolumeNumber(vol);
    TagFSVolume* home  = TagFSHome();
    uint32_t     count = 0;
    error_t      rc    = tagfs_enter(home) ? OK : ERR_DEVICE_NOT_READY;
    if (rc == OK) {
        rc = VolumeLayRoleFiles(l, home, &count);
        tagfs_leave(home);
    }
    if (rc != OK) {
        kprintf("[TagFS] the home volume keeps no kernel to lay on volume %u (error %u)\n", l->number,
                (unsigned)rc);
        VolumeLayEnd(l, rc);
        return;
    }
    memset(&l->carry, 0, sizeof(l->carry));
    l->carry.to      = vol;
    l->carry.items   = l->items;
    l->carry.count   = count;
    l->carry.move    = false;
    l->carry.maker   = true;
    l->carry.warrant = INK_WARRANTED;
    l->carry.ended   = VolumeLayCarried;
    l->step          = VOLUME_LAY_ROLES;
    rc = CarryBegin(&l->carry);
    if (rc != OK)
        VolumeLayEnd(l, rc);
}

/* The Deed amended to name the run of the kernel the carry laid, far copy
 * then head, and the volume's own account of it with it. */
static error_t VolumeLayBootNamed(VolumeLaying* l)
{
    TagFSVolume* vol    = l->vol;
    uint64_t     kernel = l->items[0].arrived;
    FileSpot     spot;
    VolumeBoot   boot;
    memset(&boot, 0, sizeof(boot));
    if (!tagfs_enter(vol))
        return ERR_DEVICE_NOT_READY;
    FileRecord* rec    = FileMapRecordHold(vol->state.files, kernel, NULL);
    uint64_t    blocks = rec ? (rec->size + VOLUME_BLOCK_BYTES - 1u) / VOLUME_BLOCK_BYTES : 0;
    bool        whole  = rec && blocks && FileRecordFind(rec, 0, &spot) && spot.kind == STRIP_DATA &&
                         1u + (uint64_t)spot.left >= blocks && spot.block + blocks <= UINT32_MAX;
    if (whole) {
        boot.kernel_block  = (uint32_t)spot.block;
        boot.kernel_blocks = (uint32_t)blocks;
        boot.kernel_bytes  = (uint32_t)rec->size;
    }
    FileRecordRelease(rec);
    tagfs_leave(vol);
    if (!whole) {
        kprintf("[TagFS] the kernel laid on volume %u is not one run — no loader could be pointed at it\n",
                l->number);
        return ERR_TAGFS_EXTENT_ERROR;
    }

    DeedCopy head;
    DeedPen  pen;
    error_t  rc = DeedReadHead(l->seat, &l->ground, &head);
    if (rc != OK)
        return rc;
    uint8_t* keep = kmalloc(head.raw_bytes);
    uint16_t bytes = 0;
    VolumeBoot* stamp = NULL;
    if (keep) {
        memcpy(keep, head.raw, head.raw_bytes);
        if (DeedPenTake(&pen, head.raw, head.raw_bytes) == 0)
            stamp = DeedPenFind(&pen, VOLUME_STAMP_BOOT, &bytes);
    }
    rc = !keep ? ERR_NO_MEMORY : !stamp || bytes < sizeof(VolumeBoot) ? ERR_METADATA_CORRUPTED : OK;
    if (rc == OK) {
        memcpy(stamp, &boot, sizeof(boot));
        if (!tagfs_enter(vol)) {
            rc = ERR_DEVICE_NOT_READY;
        } else {
            bool written = DeedsAmendedWritten(vol, l->seat, &head, &pen, keep, head.head.sectors,
                                               head.head.tail_sector, "the kernel its maker laid");
            if (written) {
                TagFSLockTake(&vol->state.lock);
                vol->state.boot = boot;
                TagFSLockDrop(&vol->state.lock);
            }
            tagfs_leave(vol);
            rc = written ? OK : ERR_WRITE_FAILED;
        }
    }
    kfree(keep);
    DeedRelease(&head);
    if (rc == OK)
        kprintf("[TagFS] volume %u: its Deed names the kernel laid on it — %u block(s) from block %u\n", l->number,
                boot.kernel_blocks, boot.kernel_block);
    return rc;
}

static void VolumeLaySettled(SettleWaiter* w, error_t rc)
{
    VolumeLaying* l = container_of(w, VolumeLaying, wait);
    l->came         = rc;
    __atomic_store_n(&l->step, VOLUME_LAY_SETTLED, __ATOMIC_RELEASE);
}

/* The carry ended: the kernel named, and a cycle asked to settle all of it. */
static void VolumeLayBoot(VolumeLaying* l)
{
    if (l->rc != OK) {
        VolumeLayEnd(l, l->rc);
        return;
    }
    error_t rc = VolumeLayBootNamed(l);
    if (rc != OK) {
        VolumeLayEnd(l, rc);
        return;
    }
    if (!tagfs_enter(l->vol)) {
        VolumeLayEnd(l, ERR_DEVICE_NOT_READY);
        return;
    }
    uint64_t gen = InkedGen(l->vol);
    error_t  now = SettleWaitNow(l->vol, gen);
    l->came      = now;
    __atomic_store_n(&l->step, now == ERR_WOULD_BLOCK ? VOLUME_LAY_SETTLING : VOLUME_LAY_SETTLED,
                     __ATOMIC_RELEASE);
    if (now == ERR_WOULD_BLOCK)
        SettleWaitLink(l->vol, &l->wait, gen, VolumeLaySettled);
    tagfs_leave(l->vol);
}

void VolumeLayServe(void)
{
    VolumeLaying* l = __atomic_load_n(&s_laying, __ATOMIC_ACQUIRE);
    if (!l || __atomic_exchange_n(&s_serving, 1u, __ATOMIC_ACQ_REL))
        return;
    switch (__atomic_load_n(&l->step, __ATOMIC_ACQUIRE)) {
    case VOLUME_LAY_TAKEN:   VolumeLayTaken(l); break;
    case VOLUME_LAY_BOOT:    VolumeLayBoot(l); break;
    case VOLUME_LAY_SETTLED: VolumeLayEnd(l, l->came); break;
    default:                 break;     /* the carry's turns, or the cycle's: theirs to move it on */
    }
    __atomic_store_n(&s_serving, 0u, __ATOMIC_RELEASE);
}
