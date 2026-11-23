/*
 * The hardware deck's disks (boxos_decks.h hw.disks.list, hw.disk.*): what
 * the kernel's DiskTable reads of each disk, laid as boxos_disks.h's records,
 * and the changes of a table asked by the person's hands.
 *
 * What stands where is put together here and nowhere else: the room knows
 * media, TagFS knows volumes, and a change of a table is told by TagFS which
 * of a disk's partitions a volume of the house stands on (TagFSVolumesOn), so
 * DiskTable never touches one.
 */

#include "klib.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "boxos_disks.h"
#include "hardware_deck.h"
#include "crate_io.h"
#include "process.h"
#include "manifest_auth.h"
#include "disk_boot.h"
#include "boxos_esp.h"
#include "boxos_bios_boot.h"
#include "tagfs.h"
#include "boardroom.h"
#include "disk_table.h"
#include "disk_look.h"

/* The loader BoxOS puts on an ESP is a few hundred KiB; one past this is no
 * loader of this system's. */
#define DISKS_LOADER_MOST  (16u << 20)

/* The most volumes of the house one disk is asked about: a table names at most
 * this many BoxOS partitions anybody stands on before the rest are counted. */
#define DISKS_VOLUMES_MOST 32u

/* What stands on the disk in `seat`: the volumes' numbers and the first block
 * of each one's partition, in `t`'s blocks. */
typedef struct {
    uint32_t count;
    uint32_t numbers[DISKS_VOLUMES_MOST];
    uint64_t firsts[DISKS_VOLUMES_MOST];
} DisksStanding;

static void DisksStandingOn(uint8_t seat, uint32_t block_bytes, DisksStanding* s)
{
    uint64_t grounds[DISKS_VOLUMES_MOST];
    uint32_t found = TagFSVolumesOn(seat, s->numbers, grounds, DISKS_VOLUMES_MOST);
    uint64_t ratio = block_bytes / BOARDROOM_SECTOR_BYTES;
    s->count       = found < DISKS_VOLUMES_MOST ? found : DISKS_VOLUMES_MOST;
    for (uint32_t i = 0; i < s->count; i++)
        s->firsts[i] = ratio ? grounds[i] / ratio : 0;
}

/* The number + 1 of the volume of the house standing on a partition from
 * `first`; 0 for none. */
static uint32_t DisksVolumeAt(const DisksStanding* s, uint64_t first)
{
    for (uint32_t i = 0; i < s->count; i++)
        if (s->firsts[i] == first)
            return s->numbers[i] + 1u;
    return 0;
}

/* One disk's record and those of its partitions — each with what it holds,
 * `looks` — and free runs, laid at kp + pos, or only measured with kp NULL:
 * the position past them. */
static uint64_t DisksRecord(uint8_t* kp, uint64_t pos, const DiskTable* t, const DisksStanding* s,
                            const DiskLook* looks)
{
    const char* name     = BoardroomSeatName(t->seat);
    uint16_t    name_len = (uint16_t)(name ? strnlen(name, DISK_NAME_MOST) : 0);
    uint64_t    at       = pos + DISK_LISTED_HEAD + name_len;
    if (kp) {
        uint8_t  flags = (BoardroomSeatIsRemovable(t->seat) ? DISK_REMOVABLE : 0u) |
                         (BoardroomSeatRefusesWrites(t->seat) ? DISK_REFUSES_WRITES : 0u) |
                         (s->count ? DISK_CARRIES_HOUSE : 0u);
        uint16_t parts = (uint16_t)t->part_count, free = (uint16_t)t->free_count;
        uint8_t* r     = kp + pos;
        memset(r, 0, DISK_LISTED_HEAD);
        r[0] = t->seat;
        r[1] = (uint8_t)BoardroomSeatKind(t->seat);
        r[2] = flags;
        r[3] = (uint8_t)t->state;
        memcpy(r + 4, &t->seating, 4);
        memcpy(r + 8, &t->block_bytes, 4);
        memcpy(r + 12, &t->physical_bytes, 4);
        memcpy(r + 16, &t->blocks, 8);
        r[24] = t->copies;
        memcpy(r + 26, &parts, 2);
        memcpy(r + 28, &free, 2);
        memcpy(r + 30, &name_len, 2);
        if (t->state == DISK_TABLE_GPT)
            memcpy(r + 32, t->disk_guid, 16);
        memcpy(r + DISK_LISTED_HEAD, name, name_len);
    }
    for (uint32_t i = 0; i < t->part_count; i++) {
        const DiskPart* p      = &t->parts[i];
        const DiskLook* look   = &looks[i];
        uint16_t        len    = (uint16_t)strnlen(p->name, DISK_NAME_MOST);
        uint32_t        volume = p->kind == DISK_PART_BOXOS ? DisksVolumeAt(s, p->first) : 0;
        if (kp) {
            uint8_t* r      = kp + at;
            uint64_t blocks = p->last - p->first + 1u;
            r[0] = (uint8_t)p->kind;
            r[1] = (uint8_t)((volume ? DISK_PART_MOUNTED : 0u) | look->flags);
            memcpy(r + 2, &p->entry, 2);
            memcpy(r + 4, &volume, 4);
            memcpy(r + 8, &p->first, 8);
            memcpy(r + 16, &blocks, 8);
            memcpy(r + 24, p->type, 16);
            memcpy(r + 40, &len, 2);
            memcpy(r + 42, p->unique, 16);
            r[58] = look->holds;
            r[59] = 0;
            memcpy(r + 60, &look->loaders_len, 2);
            memcpy(r + DISK_PART_LISTED_HEAD, p->name, len);
            memcpy(r + DISK_PART_LISTED_HEAD + len, look->loaders, look->loaders_len);
        }
        at += DISK_PART_LISTED_HEAD + len + look->loaders_len;
    }
    for (uint32_t i = 0; i < t->free_count; i++) {
        if (kp) {
            memcpy(kp + at, &t->free[i].first, 8);
            memcpy(kp + at + 8, &t->free[i].blocks, 8);
        }
        at += DISK_FREE_LISTED;
    }
    return at;
}

/* HW_DISKS_LIST  out_crate: [u32 delivered][u32 total] and the records of every
 * disk with a medium in it, in the order of its seat, while they fit. Each
 * disk's table is read once for the measure and the laying both. */
int HwDisksList(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE)
        return ERR_INVALID_ARGUMENT;
    Crate* out = &crates[op->out_crate];
    if (out->capacity < 8)
        return ERR_BUFFER_TOO_SMALL;

    uint8_t        seats = BoardroomSeatCount();
    DiskTable*     tables = seats ? (DiskTable*)kmalloc(seats * sizeof(DiskTable)) : NULL;
    DisksStanding* standing = seats ? (DisksStanding*)kmalloc(seats * sizeof(DisksStanding)) : NULL;
    DiskLook**     looks = seats ? (DiskLook**)kmalloc(seats * sizeof(DiskLook*)) : NULL;
    if (seats && (!tables || !standing || !looks)) {
        kfree(tables);
        kfree(standing);
        kfree(looks);
        return ERR_NO_MEMORY;
    }

    int      rc    = OK;
    uint32_t total = 0, delivered = 0, read = 0;
    uint64_t need  = 8;
    for (uint8_t seat = 0; seat < seats && rc == OK; seat++) {
        if (DiskTableRead(seat, &tables[read]) != OK)
            continue;
        DisksStandingOn(seat, tables[read].block_bytes, &standing[read]);
        uint32_t parts = tables[read].part_count;
        looks[read]    = (DiskLook*)kmalloc((parts ? parts : 1u) * sizeof(DiskLook));
        if (!looks[read]) {
            DiskTableLetGo(&tables[read]);
            rc = ERR_NO_MEMORY;
            break;
        }
        for (uint32_t i = 0; i < parts; i++)
            DiskLookAt(&tables[read], i, &looks[read][i]);
        uint64_t past = DisksRecord(NULL, need, &tables[read], &standing[read], looks[read]);
        if (past <= out->capacity && delivered == total) {
            need = past;
            delivered++;
        }
        total++;
        read++;
    }

    uint8_t* kp = rc == OK ? crate_out_alloc(out, need) : NULL;
    if (rc == OK && !kp) {
        rc = ERR_INVALID_ADDRESS;
    } else if (rc == OK) {
        uint64_t pos = 8;
        for (uint32_t i = 0; i < delivered; i++)
            pos = DisksRecord(kp, pos, &tables[i], &standing[i], looks[i]);
        memcpy(kp, &delivered, 4);
        memcpy(kp + 4, &total, 4);
        rc = crate_out_commit(out, ctx, kp, pos);
        crate_buf_free(kp);
        if (rc == OK)
            out->size = pos;
    }
    for (uint32_t i = 0; i < read; i++) {
        DiskTableLetGo(&tables[i]);
        kfree(looks[i]);
    }
    kfree(tables);
    kfree(standing);
    kfree(looks);
    return rc;
}

/* The disk a change names — [u8 seat][u32 seating] at the head of params —
 * and what stands on it. */
static int DisksAsked(const ManifestOp* op, uint32_t more, const OpContext* ctx, uint8_t* seat, uint32_t* seating,
                      DisksStanding* standing, DiskInUse* used)
{
    if (!ManifestCabinWears(ctx, AUTH_TAG_CHANGE_DISKS))   /* the person's hands on the disks */
        return ERR_ACCESS_DENIED;
    if (op->param_size < 5u + more)
        return ERR_INVALID_ARGUMENT;
    *seat = op->params[0];
    memcpy(seating, op->params + 1, 4);
    uint32_t block = BoardroomSeatLogicalBytes(*seat);
    standing->count = 0;
    if (block)
        DisksStandingOn(*seat, block, standing);
    used->count  = standing->count;
    used->firsts = standing->firsts;
    return OK;
}

/* HW_DISK_LAY  params: [u8 seat][u32 seating] */
int HwDiskLay(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx)
{
    (void)crates; (void)crate_count;
    uint8_t       seat;
    uint32_t      seating;
    DisksStanding standing;
    DiskInUse     used;
    int           rc = DisksAsked(op, 0, ctx, &seat, &seating, &standing, &used);
    if (rc != OK)
        return rc;
    return (int)DiskTableLay(seat, seating, &used);
}

/* HW_DISK_ADD  params: [u8 seat][u32 seating][u8 kind][u64 first][u64 blocks];
 * out_crate (optional): [u16 entry] */
int HwDiskAdd(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx)
{
    (void)crate_count;
    uint8_t       seat;
    uint32_t      seating;
    DisksStanding standing;
    DiskInUse     used;
    int           rc = DisksAsked(op, 17, ctx, &seat, &seating, &standing, &used);
    if (rc != OK)
        return rc;
    uint64_t first, blocks;
    uint16_t entry = 0;
    memcpy(&first, op->params + 6, 8);
    memcpy(&blocks, op->params + 14, 8);
    rc = (int)DiskTableAdd(seat, seating, (DiskPartKind)op->params[5], first, blocks, &entry);
    if (rc == OK && op->out_crate != CRATE_INDEX_NONE && crate_write(&crates[op->out_crate], ctx, &entry, 2) != OK)
        return ERR_INVALID_ADDRESS;
    return rc;
}

/* The BIOS loader the home volume keeps under `name`, read whole into *out
 * (the caller's to kfree); NULL when it keeps none. */
static uint8_t* DisksKept(const char* name, uint32_t most, uint32_t* bytes)
{
    uint8_t* bytes_read = NULL;
    *bytes = 0;
    return TagFSKeptRead(TagFSHome(), name, most, &bytes_read, bytes) == OK ? bytes_read : NULL;
}

/*
 * Whether the partition in `entry` of the disk, of another kind than BoxOS's,
 * is BoxOS's own to take away: an ESP holding nothing of any system — no
 * loader's directory, no default (DiskLookAt) — or a BIOS boot partition
 * whose head is BoxOS's second stage, as the home volume keeps it. Another
 * system's ESP or GRUB's second stage is never BoxOS's.
 */
static bool DisksOwned(uint8_t seat, uint32_t seating, uint16_t entry)
{
    DiskTable t;
    if (DiskTableRead(seat, &t) != OK)
        return false;
    bool owned = false;
    for (uint32_t i = 0; t.seating == seating && i < t.part_count; i++) {
        if (t.parts[i].entry != entry)
            continue;
        if (t.parts[i].kind == DISK_PART_ESP) {
            DiskLook look;
            DiskLookAt(&t, i, &look);
            owned = (look.holds == DISK_HOLDS_FAT || look.holds == DISK_HOLDS_EMPTY) && !look.loaders_len &&
                    !(look.flags & (DISK_PART_DEFAULT_LOADER | DISK_PART_MORE_LOADERS));
        } else if (t.parts[i].kind == DISK_PART_BIOS_BOOT && t.block_bytes == BIOS_SECTOR_BYTES) {
            uint32_t bytes  = 0;
            uint8_t* stage2 = DisksKept(BIOS_STAGE2_KEPT, DISKS_LOADER_MOST, &bytes);
            uint8_t  head[BIOS_SECTOR_BYTES];
            owned = stage2 && bytes >= BIOS_SECTOR_BYTES &&
                    BoardroomRead(seat, t.parts[i].first, 1, head) == 0 && memcmp(head, stage2, sizeof(head)) == 0;
            kfree(stage2);
        }
    }
    DiskTableLetGo(&t);
    return owned;
}

/* HW_DISK_REMOVE  params: [u8 seat][u32 seating][u16 entry] */
int HwDiskRemove(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx)
{
    (void)crates; (void)crate_count;
    uint8_t       seat;
    uint32_t      seating;
    DisksStanding standing;
    DiskInUse     used;
    int           rc = DisksAsked(op, 2, ctx, &seat, &seating, &standing, &used);
    if (rc != OK)
        return rc;
    uint16_t entry;
    memcpy(&entry, op->params + 5, 2);
    return (int)DiskTableRemove(seat, seating, entry, &used, DisksOwned(seat, seating, entry));
}

/* HW_DISK_GROW  params: [u8 seat][u32 seating][u16 entry][u64 blocks];
 * out_crate (optional): [u64 blocks] */
int HwDiskGrow(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx)
{
    (void)crate_count;
    uint8_t       seat;
    uint32_t      seating;
    DisksStanding standing;
    DiskInUse     used;
    int           rc = DisksAsked(op, 10, ctx, &seat, &seating, &standing, &used);
    if (rc != OK)
        return rc;
    uint16_t entry;
    uint64_t blocks, grown = 0;
    memcpy(&entry, op->params + 5, 2);
    memcpy(&blocks, op->params + 7, 8);
    rc = (int)DiskTableGrow(seat, seating, entry, blocks, &grown);
    if (rc == OK && op->out_crate != CRATE_INDEX_NONE && crate_write(&crates[op->out_crate], ctx, &grown, 8) != OK)
        return ERR_INVALID_ADDRESS;
    return rc;
}


/* HW_DISK_BOOT  params: [u8 seat][u32 seating][u16 entry][u32 volume][u8 first];
 * out_crate (optional): [u16 option][u8 option given][u8 default written] */
int HwDiskBoot(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx)
{
    (void)crate_count;
    uint8_t       seat;
    uint32_t      seating, number;
    DisksStanding standing;
    DiskInUse     used;
    int           rc = DisksAsked(op, 7, ctx, &seat, &seating, &standing, &used);
    if (rc != OK)
        return rc;
    DiskBootAsk a = { .seat = seat, .seating = seating, .first = op->params[11] != 0 };
    memcpy(&a.entry, op->params + 5, 2);
    memcpy(&number, op->params + 7, 4);
    bool on = false;
    for (uint32_t i = 0; i < standing.count; i++)
        on |= standing.numbers[i] == number;
    TagFSVolume* vol = on ? TagFSVolumeNumbered(number) : NULL;
    if (!vol) {
        kprintf("[DiskBoot] seat %u: volume %u does not stand on this disk — a loader on its ESP starts a volume of "
                "its own disk\n", seat, number);
        return ERR_INVALID_ARGUMENT;
    }
    uint8_t* loader = NULL;
    rc = (int)TagFSKeptRead(vol, ESP_LOADER_KEPT, DISKS_LOADER_MOST, &loader, &a.loader_bytes);
    if (rc == ERR_FILE_NOT_FOUND)
        kprintf("[DiskBoot] volume %u keeps no %s — there is no loader of its to put on an ESP\n", number,
                ESP_LOADER_KEPT);
    if (rc != OK)
        return rc;
    a.loader = loader;
    /* The BIOS loaders, when the volume keeps them: a volume laid before it
     * kept them starts through UEFI alone. */
    uint8_t* stage1 = NULL;
    uint8_t* stage2 = NULL;
    error_t  kept   = TagFSKeptRead(vol, BIOS_STAGE1_KEPT, BIOS_SECTOR_BYTES, &stage1, &a.stage1_bytes);
    if (kept == OK)
        kept = TagFSKeptRead(vol, BIOS_STAGE2_KEPT, DISKS_LOADER_MOST, &stage2, &a.stage2_bytes);
    if (kept == OK) {
        a.stage1 = stage1;
        a.stage2 = stage2;
    } else if (kept != ERR_FILE_NOT_FOUND) {
        kfree(loader);
        kfree(stage1);
        return kept;
    }
    TagFSVolumeUuid(vol, a.uuid);
    DiskBootSaid said;
    rc = (int)DiskBootLay(&a, &said);
    kfree(loader);
    kfree(stage1);
    kfree(stage2);
    if (rc == OK && op->out_crate != CRATE_INDEX_NONE) {
        uint8_t answer[5] = { (uint8_t)said.number, (uint8_t)(said.number >> 8), said.option, said.fallback, said.bios };
        if (crate_write(&crates[op->out_crate], ctx, answer, sizeof(answer)) != OK)
            return ERR_INVALID_ADDRESS;
    }
    return rc;
}

/* HW_DISK_UNBOOT  params: [u8 seat][u32 seating][u16 entry]; out_crate
 * (optional): [u8 options taken away][u8 directory][u8 default][u8 boot code] */
int HwDiskUnboot(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx)
{
    (void)crate_count;
    uint8_t       seat;
    uint32_t      seating;
    DisksStanding standing;
    DiskInUse     used;
    int           rc = DisksAsked(op, 2, ctx, &seat, &seating, &standing, &used);
    if (rc != OK)
        return rc;
    DiskUnbootAsk a = { .seat = seat, .seating = seating };
    memcpy(&a.entry, op->params + 5, 2);
    uint8_t* stage1 = DisksKept(BIOS_STAGE1_KEPT, BIOS_SECTOR_BYTES, &a.stage1_bytes);
    a.stage1        = stage1;
    DiskUnbootSaid said;
    rc = (int)DiskBootTakeAway(&a, &said);
    kfree(stage1);
    if (rc == OK && op->out_crate != CRATE_INDEX_NONE) {
        uint8_t answer[4] = { (uint8_t)(said.options > 255u ? 255u : said.options), said.loader, said.fallback,
                              said.bios };
        if (crate_write(&crates[op->out_crate], ctx, answer, sizeof(answer)) != OK)
            return ERR_INVALID_ADDRESS;
    }
    return rc;
}

/* HW_DISK_MEND  params: [u8 seat][u32 seating] */
int HwDiskMend(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx)
{
    (void)crates; (void)crate_count;
    uint8_t       seat;
    uint32_t      seating;
    DisksStanding standing;
    DiskInUse     used;
    int           rc = DisksAsked(op, 0, ctx, &seat, &seating, &standing, &used);
    if (rc != OK)
        return rc;
    return (int)DiskTableMend(seat, seating);
}
