#include "volume_lay.h"
#include "volume_lay_files.h"
#include "tagfs_volume.h"
#include "tagfs_handle.h"
#include "../deed/deed.h"
#include "disk_table.h"
#include "boardroom.h"
#include "buildplate.h"
#include "random.h"
#include "rtc.h"
#include "klib.h"

/* A piece: what a turn reads off the home volume, writes, or blanks. */
#define LAY_PIECE_BLOCKS  16u
#define LAY_PIECE         (LAY_PIECE_BLOCKS * VOLUME_BLOCK_BYTES)
/* The kernel's last bytes a piece keeps in front of the next, so a plate
 * that straddles two is seen whole. */
#define LAY_PLATE_KEPT    (sizeof(BuildPlate) - 1u)

static VolumeLaying* volatile s_laying;
static volatile uint32_t      s_busy;
static volatile uint32_t      s_serving;

static void VolumeLayEnd(VolumeLaying* l, error_t rc);

// ----------------------------------------------------------------------------
// The disk
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

/* `bytes` onto the ground at the volume's block `block`, the seat asked
 * first: the binder's sink and the laying's own. */
static int VolumeLaySink(void* ctx, uint64_t block, const void* data, uint64_t bytes)
{
    const VolumeLaying* l = ctx;
    if (!VolumeLayStillThere(l))
        return -1;
    return BoardroomWrite(l->seat, l->ground.start_sector + block * VOLUME_BLOCK_SECTORS,
                          (uint32_t)(bytes / BOARDROOM_SECTOR_BYTES), data, NULL) == 0 ? 0 : -1;
}

static error_t VolumeLayWritten(VolumeLaying* l, uint64_t block, const void* data, uint64_t bytes, const char* what)
{
    return VolumeLaySink(l, block, data, bytes) == 0 ? OK : VolumeLayRefusal(l, what);
}

static error_t VolumeLayFlush(const VolumeLaying* l, const char* what)
{
    if (!VolumeLayStillThere(l) || BoardroomFlush(l->seat) != 0)
        return VolumeLayRefusal(l, what);
    return OK;
}

/* The partition asked for, held to what a laying may stand on: its ground in
 * the room's sectors into l->ground. */
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
// The turns
// ----------------------------------------------------------------------------

/* The next turn, at the home volume's next service point. */
static void VolumeLayNext(VolumeLaying* l, VolumeLayStep step)
{
    l->step = step;
    HelmReady(TagFSHome(), &l->turn);
}

/* `want` bytes of the file the pass is on, from l->offset, into `into`. */
static error_t VolumeLayPieceRead(VolumeLaying* l, VolumeLaidFile* f, uint8_t* into, uint64_t want)
{
    TagFSVolume* home = TagFSHome();
    uint64_t     got  = 0;
    if (!tagfs_enter(home))
        return ERR_DEVICE_NOT_READY;
    f->src->offset = l->offset;
    error_t rc     = tagfs_read(home, f->src, into, want, &got);
    tagfs_leave(home);
    if (rc == OK && got != want)
        rc = ERR_METADATA_CORRUPTED;    /* the handle's view holds the file it opened: its size is there */
    if (rc != OK)
        kprintf("[TagFS] '%s' could not be read off the home volume to be laid (error %u)\n", f->name,
                (unsigned)rc);
    return rc;
}

/* The kernel's bytes the last read added to those kept in front of them,
 * looked at for its plate: every place one could begin that is first seen
 * whole now, the last bytes kept for the next piece. The tag is the running
 * kernel's own, so the kernel's image carries no second copy of it. */
static void VolumeLayPlateSought(VolumeLaying* l, uint64_t got)
{
    const BuildPlate* own = BuildPlateRead();
    uint64_t          len = l->kept + got;
    for (uint64_t i = 0; i + sizeof(BuildPlate) <= len; i++) {
        if (memcmp(l->piece + i, own->Tag, BUILDPLATE_TAG_BYTES) != 0 || l->plates++)
            continue;
        BuildPlate p;
        memcpy(&p, l->piece + i, sizeof(p));
        if (p.Magic == BUILDPLATE_MAGIC) {
            l->packed.tree    = p.TreeFingerprint;
            l->packed.verdict = p.Verdict;
        }
    }
    uint32_t keep = len < LAY_PLATE_KEPT ? (uint32_t)len : (uint32_t)LAY_PLATE_KEPT;
    memmove(l->piece, l->piece + len - keep, keep);
    l->kept = keep;
}

/* The file the pass is on done with: the next one. False when it was the last. */
static bool VolumeLayFileDone(VolumeLaying* l)
{
    l->offset = 0;
    l->kept   = 0;
    return ++l->at < l->count;
}

/* A piece of a file read, folded into the shipment, looked at for the
 * kernel's plate, and the digest of each block taken — the last block's tail
 * zeros, as written. Past the last file the book is laid out with them. */
static void VolumeLayDigestTurn(VolumeLaying* l)
{
    VolumeLaidFile* f    = &l->files[l->at];
    uint64_t        want = f->size - l->offset < LAY_PIECE ? f->size - l->offset : LAY_PIECE;
    uint8_t*        into = l->piece + l->kept;
    if (l->offset == 0)
        l->packed.shipment = VolumeShipmentFold(l->packed.shipment, f->name, f->name_len + 1u);
    error_t rc = want ? VolumeLayPieceRead(l, f, into, want) : OK;
    if (rc != OK) {
        VolumeLayEnd(l, rc);
        return;
    }
    uint64_t blocks = (want + VOLUME_BLOCK_BYTES - 1u) / VOLUME_BLOCK_BYTES;
    uint64_t first  = f->start - l->frame.data_block + l->offset / VOLUME_BLOCK_BYTES;
    l->packed.shipment = VolumeShipmentFold(l->packed.shipment, into, want);
    memset(into + want, 0, blocks * VOLUME_BLOCK_BYTES - want);
    for (uint64_t b = 0; b < blocks; b++)
        l->digests[first + b] = BoxHashIntegrity(into + b * VOLUME_BLOCK_BYTES, VOLUME_BLOCK_BYTES, &l->hash);
    if (l->at == 0)
        VolumeLayPlateSought(l, want);
    l->offset += want;
    if (l->offset < f->size) {
        VolumeLayNext(l, VOLUME_LAY_DIGEST);
        return;
    }
    l->packed.shipment = VolumeShipmentFold(l->packed.shipment, "", 1);
    if (VolumeLayFileDone(l)) {
        VolumeLayNext(l, VOLUME_LAY_DIGEST);
        return;
    }
    if (l->plates != 1) {
        kprintf("[TagFS] the kernel laid carries %u plate(s) — the volume will say it knows nothing about which "
                "tree made it\n", l->plates);
        l->packed.tree    = 0;
        l->packed.verdict = BUILDPLATE_VERDICT_UNSTAMPED;
    }
    rc = VolumeLayBookPlanned(l);
    if (rc != OK) {
        VolumeLayEnd(l, rc);
        return;
    }
    l->at     = 0;
    l->offset = 0;
    VolumeLayNext(l, VOLUME_LAY_BLANK);
}

/* The Deed's blocks at both ends blanked and on the medium before anything
 * else is written — a partition that held a volume keeps its Deed until then,
 * and a Deed left standing through the writes below would name what they
 * overwrote. Then the front — the Deed's grain past it and the Spine's — a
 * run a turn. */
static void VolumeLayBlankTurn(VolumeLaying* l)
{
    error_t rc = OK;
    memset(l->piece, 0, LAY_PIECE);
    if (l->front == 0) {
        rc = VolumeLayWritten(l, 0, l->piece, VOLUME_BLOCK_BYTES, "the blanking of the Deed's blocks");
        if (rc == OK)
            rc = VolumeLayWritten(l, l->blocks - 1u, l->piece, VOLUME_BLOCK_BYTES,
                                  "the blanking of the Deed's blocks");
        if (rc == OK)
            rc = VolumeLayFlush(l, "the Deed's blocks blanked");
        l->front = 1;
    } else {
        uint64_t left = l->frame.data_block - l->front;
        uint64_t n    = left < LAY_PIECE_BLOCKS ? left : LAY_PIECE_BLOCKS;
        rc = VolumeLayWritten(l, l->front, l->piece, n * VOLUME_BLOCK_BYTES, "the front of the ground");
        l->front += n;
    }
    if (rc != OK)
        VolumeLayEnd(l, rc);
    else
        VolumeLayNext(l, l->front < l->frame.data_block ? VOLUME_LAY_BLANK : VOLUME_LAY_DATA);
}

/* A piece of a file read again, each block held to the digest the book will
 * carry for it — a file whose bytes are not the ones their digests were taken
 * of is refused — and written to its place. */
static void VolumeLayDataTurn(VolumeLaying* l)
{
    VolumeLaidFile* f    = &l->files[l->at];
    uint64_t        want = f->size - l->offset < LAY_PIECE ? f->size - l->offset : LAY_PIECE;
    error_t         rc   = want ? VolumeLayPieceRead(l, f, l->piece, want) : OK;
    uint64_t        blocks = (want + VOLUME_BLOCK_BYTES - 1u) / VOLUME_BLOCK_BYTES;
    uint64_t        first  = f->start - l->frame.data_block + l->offset / VOLUME_BLOCK_BYTES;
    if (rc == OK)
        memset(l->piece + want, 0, blocks * VOLUME_BLOCK_BYTES - want);
    for (uint64_t b = 0; rc == OK && b < blocks; b++)
        if (BoxHashIntegrity(l->piece + b * VOLUME_BLOCK_BYTES, VOLUME_BLOCK_BYTES, &l->hash) !=
            l->digests[first + b]) {
            kprintf("[TagFS] '%s' changed while it was laid: its block %llu is not the block its digest was taken "
                    "of\n", f->name, (unsigned long long)(l->offset / VOLUME_BLOCK_BYTES + b));
            rc = ERR_CHECKSUM;
        }
    if (rc == OK && blocks)
        rc = VolumeLayWritten(l, f->start + l->offset / VOLUME_BLOCK_BYTES, l->piece, blocks * VOLUME_BLOCK_BYTES,
                              "the system's files");
    if (rc != OK) {
        VolumeLayEnd(l, rc);
        return;
    }
    l->offset += want;
    if (l->offset < f->size || VolumeLayFileDone(l))
        VolumeLayNext(l, VOLUME_LAY_DATA);
    else
        VolumeLayNext(l, VOLUME_LAY_BOOK);
}

/* One copy of the Deed — GEOMETRY · FRAME · BORN · BOOT · PACKED, as every
 * maker stamps one — into `block`. */
static void VolumeLayDeed(const VolumeLaying* l, uint8_t* block, uint32_t role)
{
    const VolumeLaidFile* k   = &l->files[0];
    VolumeGeometry        geo = VolumeGeometryLaid();
    VolumeBorn            born;
    VolumeBoot            boot;
    DeedPen               pen;
    memset(&born, 0, sizeof(born));
    memset(&boot, 0, sizeof(boot));
    born.created_unix  = l->made_at;
    memcpy(born.maker, "boxos kernel", 12);
    boot.kernel_block  = (uint32_t)k->start;
    boot.kernel_blocks = (uint32_t)k->blocks;
    boot.kernel_bytes  = (uint32_t)k->size;
    DeedPenOpen(&pen, block, DEED_PEN_BYTES);
    (void)DeedPenSay(&pen, VOLUME_STAMP_GEOMETRY, &geo, sizeof(geo));
    (void)DeedPenSay(&pen, VOLUME_STAMP_FRAME, &l->frame, sizeof(l->frame));
    (void)DeedPenSay(&pen, VOLUME_STAMP_BORN, &born, sizeof(born));
    (void)DeedPenSay(&pen, VOLUME_STAMP_BOOT, &boot, sizeof(boot));
    (void)DeedPenSay(&pen, VOLUME_STAMP_PACKED, &l->packed, sizeof(l->packed));
    DeedPenSetUuid(&pen, l->uuid);
    DeedPenSetGround(&pen, l->blocks * VOLUME_BLOCK_SECTORS, (l->blocks - 1u) * VOLUME_BLOCK_SECTORS);
    DeedPenSetRole(&pen, role);
    DeedPenSeal(&pen);
}

/* The book — the Tally's nodes in both copies, then slot 0 naming them — on
 * the medium with the files, and only then the Deed at both ends, on the
 * medium in its turn. Laid, the house is asked to take it in. */
static void VolumeLayBookTurn(VolumeLaying* l)
{
    const BindSink    sink   = { VolumeLaySink, l };
    const SpineHead   head   = { LAY_TALLY_SEQ, 0, 0, l->made_at * 1000000000ull };
    const SpineCounts counts = VolumeLayCounts(l);
    uint64_t          failed = 0;
    int               bound  = BindTally(&sink, &l->plan, l->uuid, LAY_TALLY_SEQ, &failed);
    if (bound == BIND_OK)
        bound = BindSpine(&sink, &l->plan, l->frame.spine_block, l->leaves.grain, l->uuid, &head, &counts,
                          LAY_SLOT_SEQ, &failed);
    error_t rc = bound == BIND_OK ? OK : bound == BIND_NO_MEMORY ? ERR_NO_MEMORY : VolumeLayRefusal(l, "the book");
    if (rc == OK)
        rc = VolumeLayFlush(l, "the files and the book before their Deed");
    if (rc == OK) {
        memset(l->piece, 0, DEED_PEN_BYTES);
        VolumeLayDeed(l, l->piece, VOLUME_DEED_ROLE_HEAD);
        rc = VolumeLayWritten(l, 0, l->piece, DEED_PEN_BYTES, "the Deed");
    }
    if (rc == OK) {
        memset(l->piece, 0, DEED_PEN_BYTES);
        VolumeLayDeed(l, l->piece, VOLUME_DEED_ROLE_TAIL);
        rc = VolumeLayWritten(l, l->blocks - 1u, l->piece, DEED_PEN_BYTES, "the Deed's far copy");
    }
    if (rc == OK)
        rc = VolumeLayFlush(l, "the Deed");
    if (rc != OK) {
        VolumeLayEnd(l, rc);
        return;
    }
    char text[33];
    DeedUuidText(l->uuid, text);
    kprintf("[TagFS] seat %u: a volume is laid on entry %u — %s, %llu block(s), %u file(s) the system keeps on it, "
            "the kernel in one run from block %llu named in its Deed; the house is to take it in\n", l->seat,
            l->entry, text, (unsigned long long)l->blocks, l->count, (unsigned long long)l->files[0].start);
    l->roles = l->count;
    VolumeLayFilesLetGo(l, TagFSHome());
    BindPlanFree(&l->plan);
    BindLeavesFree(&l->leaves);
    kfree(l->piece);
    l->piece = NULL;
    l->step  = VOLUME_LAY_TAKEN;
    __atomic_store_n(&s_laying, l, __ATOMIC_RELEASE);
    TagFSHouseLook();
}

static void VolumeLayTurn(HelmWaiter* w)
{
    VolumeLaying* l = container_of(w, VolumeLaying, turn);
    switch (l->step) {
    case VOLUME_LAY_DIGEST: VolumeLayDigestTurn(l); break;
    case VOLUME_LAY_BLANK:  VolumeLayBlankTurn(l); break;
    case VOLUME_LAY_DATA:   VolumeLayDataTurn(l); break;
    case VOLUME_LAY_BOOK:   VolumeLayBookTurn(l); break;
    case VOLUME_LAY_TAKEN:  break;      /* the house's to move on (VolumeLayServe) */
    }
}

// ----------------------------------------------------------------------------
// The beginning, and the end
// ----------------------------------------------------------------------------

/* The laying over: what it holds let go, the answer given and the next one
 * let in. */
static void VolumeLayEnd(VolumeLaying* l, error_t rc)
{
    if (l->rc == OK)
        l->rc = rc;
    VolumeLayFilesLetGo(l, TagFSHome());
    BindPlanFree(&l->plan);
    BindLeavesFree(&l->leaves);
    kfree(l->piece);
    l->piece = NULL;
    if (l->rc == OK)
        kprintf("[TagFS] volume %u is laid: %u file(s) the system keeps, the kernel named in its Deed\n", l->number,
                l->roles);
    else
        kprintf("[TagFS] no volume was laid on seat %u (error %u)\n", l->seat, (unsigned)l->rc);
    __atomic_store_n(&s_laying, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&s_busy, 0u, __ATOMIC_RELEASE);
    l->ended(l);
}

/* The files the system keeps opened and placed, and the book laid out to see
 * it fits: nothing written yet. */
static error_t VolumeLayPlanned(VolumeLaying* l)
{
    TagFSVolume* home  = TagFSHome();
    uint32_t     grain = VolumeGeometryLaid().grain_bytes / VOLUME_BLOCK_BYTES;
    l->frame           = VolumeFrameLaid(grain);
    l->blocks          = l->ground.sectors / VOLUME_BLOCK_SECTORS;
    l->made_at         = rtc_get_unix64();
    l->packed          = (VolumePacked){ .packed_unix = l->made_at, .shipment = VOLUME_SHIPMENT_START,
                                         .verdict = BUILDPLATE_VERDICT_UNSTAMPED };
    RandomGuid(l->uuid);
    BoxHashInit(&l->hash, l->uuid, sizeof(l->uuid));
    if (l->blocks <= l->frame.data_block + 1u) {
        kprintf("[TagFS] seat %u: entry %u is %llu block(s) — too small for a volume\n", l->seat, l->entry,
                (unsigned long long)l->blocks);
        return ERR_TAGFS_NO_SPACE;
    }
    if (!tagfs_enter(home))
        return ERR_DEVICE_NOT_READY;
    error_t rc = VolumeLayFilesTaken(l, home);
    tagfs_leave(home);
    if (rc == OK)
        rc = VolumeLayBookPlanned(l);   /* the digests not taken yet: what the book takes, they do not change */
    if (rc == OK) {
        l->packed.file_count = l->count;
        l->piece             = kmalloc(LAY_PLATE_KEPT + LAY_PIECE);
        rc                   = l->piece ? OK : ERR_NO_MEMORY;
    }
    return rc;
}

error_t VolumeLayBegin(VolumeLaying* l)
{
    if (__atomic_exchange_n(&s_busy, 1u, __ATOMIC_ACQ_REL))
        return ERR_WOULD_BLOCK;
    l->rc        = OK;
    l->number    = 0;
    l->roles     = 0;
    l->files     = NULL;
    l->count     = 0;
    l->tags      = NULL;
    l->tag_count = 0;
    l->tag_room  = 0;
    l->digests   = NULL;
    l->piece     = NULL;
    l->kept      = 0;
    l->plates    = 0;
    l->at        = 0;
    l->offset    = 0;
    l->front     = 0;
    l->turn      = (HelmWaiter){ .run = VolumeLayTurn };
    memset(&l->leaves, 0, sizeof(l->leaves));
    memset(&l->plan, 0, sizeof(l->plan));
    error_t rc = VolumeLayGround(l);
    if (rc == OK)
        rc = VolumeLayPlanned(l);
    if (rc != OK) {
        VolumeLayFilesLetGo(l, TagFSHome());
        BindPlanFree(&l->plan);
        BindLeavesFree(&l->leaves);
        kfree(l->piece);
        l->piece = NULL;
        __atomic_store_n(&s_busy, 0u, __ATOMIC_RELEASE);
        return rc;
    }
    kprintf("[TagFS] seat %u: a volume is to be laid on entry %u — %u file(s) the system keeps, %llu block(s) of "
            "them\n", l->seat, l->entry, l->count, (unsigned long long)(l->end - l->frame.data_block));
    VolumeLayNext(l, VOLUME_LAY_DIGEST);
    return OK;
}

/* The house took the volume in and mounted it: the answer. A volume the
 * house will not mount is said. What is still coming — looked for, taken in,
 * owed a mount, or being mounted now — is read before whether it is mounted,
 * and the house's look before whether it took the volume in: a mount
 * that ends between the two is seen as coming, and the next pass finds it
 * mounted, never as one the house would not mount. */
static void VolumeLayTaken(VolumeLaying* l)
{
    bool         looking = TagFSHouseLooking();
    TagFSVolume* vol     = TagFSVolumeOfUuid(l->uuid);
    bool         coming  = vol ? __atomic_load_n(&vol->mount.arrival_owed, __ATOMIC_ACQUIRE) ||
                                 __atomic_load_n(&vol->mount.mount_owed, __ATOMIC_ACQUIRE) ||
                                 __atomic_load_n(&vol->mount.attending, __ATOMIC_ACQUIRE)
                               : looking;
    if (vol && TagFSVolumeMounted(vol)) {
        l->number = TagFSVolumeNumber(vol);
        VolumeLayEnd(l, OK);
        return;
    }
    if (!coming) {
        kprintf("[TagFS] the house did not %s the volume laid on seat %u\n", vol ? "mount" : "take in", l->seat);
        VolumeLayEnd(l, ERR_TAGFS_NOT_INITIALIZED);
    }
}

void VolumeLayServe(void)
{
    if (!__atomic_load_n(&s_laying, __ATOMIC_ACQUIRE) || __atomic_exchange_n(&s_serving, 1u, __ATOMIC_ACQ_REL))
        return;
    VolumeLaying* l = __atomic_load_n(&s_laying, __ATOMIC_ACQUIRE);     /* ended meanwhile: none */
    if (l)
        VolumeLayTaken(l);
    __atomic_store_n(&s_serving, 0u, __ATOMIC_RELEASE);
}
