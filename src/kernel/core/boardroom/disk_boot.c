#include "disk_boot.h"
#include "disk_table.h"
#include "disk_fat.h"
#include "boardroom.h"
#include "efi_boot.h"
#include "boxos_esp.h"
#include "boxos_crossings.h"
#include "boxos_bios_boot.h"
#include "rtc.h"
#include "crypto.h"
#include "klib.h"

/* Part `index` of a '/'-parted path into `out` (`room` bytes, ended). */
static void PathPartOf(const char* path, uint32_t index, char* out, uint32_t room)
{
    uint32_t n = 0;
    for (; index && *path; path++)
        index -= *path == '/';
    while (*path && *path != '/' && n + 1u < room)
        out[n++] = *path++;
    out[n] = 0;
}

static bool NameIs(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

/* What an ESP's EFI directory holds, as far as whose it is goes. */
typedef struct {
    char ours[16], fallback_dir[16], fallback[16];
    bool other;                 /* a directory of another system's */
    bool ours_there;            /* BoxOS's own directory */
} EspLook;

static int EspSeen(void* ctx, const char* name, const FatEntry* e)
{
    EspLook* l = ctx;
    if (!(e->attr & FAT_ATTR_DIRECTORY))
        return 0;
    if (NameIs(name, l->ours))
        l->ours_there = true;
    else if (!NameIs(name, l->fallback_dir))
        l->other = true;
    return 0;
}

/* Whether the ESP is BoxOS's own: under EFI, no directory of another
 * system's, and no removable-media default but one BoxOS put there. */
static int EspOwn(FatVolume* v, bool* own)
{
    EspLook  look;
    FatEntry fallback;
    memset(&look, 0, sizeof(look));
    PathPartOf(ESP_DIR, 1, look.ours, sizeof(look.ours));
    PathPartOf(ESP_FALLBACK, 1, look.fallback_dir, sizeof(look.fallback_dir));
    int rc = FatEach(v, "EFI", EspSeen, &look);
    if (rc != FAT_OK && rc != FAT_NOT_FOUND)
        return rc;
    rc = FatFind(v, ESP_FALLBACK, &fallback);
    if (rc != FAT_OK && rc != FAT_NOT_FOUND)
        return rc;
    *own = !look.other && (rc == FAT_NOT_FOUND || look.ours_there);
    return FAT_OK;
}

/* What a refused FAT call answers. */
static error_t EspRefusal(const DiskBootAsk* a, int rc, const char* what)
{
    if (rc == FAT_NO_ROOM) {
        kprintf("[DiskBoot] seat %u: the ESP in entry %u has no room for %s\n", a->seat, a->entry, what);
        return ERR_TAGFS_NO_SPACE;
    }
    if (rc != FAT_IO) {
        kprintf("[DiskBoot] seat %u: the ESP in entry %u does not read as a FAT32 (%d) — %s is not put there\n",
                a->seat, a->entry, rc, what);
        return ERR_CORRUPTED;
    }
    if (!BoardroomSeatOccupied(a->seat) || BoardroomSeatSeating(a->seat) != a->seating) {
        kprintf("[DiskBoot] seat %u: the disk left while %s was written\n", a->seat, what);
        return ERR_DEVICE_NOT_READY;
    }
    if (BoardroomSeatRefusesWrites(a->seat)) {
        kprintf("[DiskBoot] seat %u: the medium refuses writes — it is protected\n", a->seat);
        return ERR_PERMISSION_DENIED;
    }
    kprintf("[DiskBoot] seat %u: the medium refused the write of %s\n", a->seat, what);
    return ERR_WRITE_FAILED;
}

/* The crossings file on `v` (boxos_crossings.h) — laid with its head when
 * there is none, or one Gangway could not keep them in: the wrong length, or
 * not in one run. One that holds is left as it is, its crossings with it. */
static int CrossingsLaid(FatVolume* v, bool* laid)
{
    FatEntry e;
    uint64_t run, count;
    int      rc = FatFind(v, ESP_CROSSINGS, &e);
    *laid       = false;
    if (rc == FAT_OK && e.size == CROSSINGS_FILE_BYTES && FatRun(v, &e, &run, &count) == FAT_OK)
        return FAT_OK;
    if (rc != FAT_OK && rc != FAT_NOT_FOUND)
        return rc;
    uint8_t* file = kmalloc(CROSSINGS_FILE_BYTES);
    if (!file)
        return FAT_IO;
    memset(file, 0, CROSSINGS_FILE_BYTES);
    CrossingsHead* h = (CrossingsHead*)file;
    memcpy(h->magic, CROSSINGS_HEAD_MAGIC, sizeof(h->magic));
    h->size         = sizeof(*h);
    h->slots        = CROSSINGS_SLOTS;
    h->slot_sectors = CROSSINGS_SLOT_SECTORS;
    h->first_slot   = CROSSINGS_FIRST_SLOT;
    h->crc          = KCrc32(file, sizeof(*h));
    rc              = FatPut(v, ESP_CROSSINGS, file, CROSSINGS_FILE_BYTES, NULL);
    kfree(file);
    *laid = rc == FAT_OK;
    return rc;
}

/* The ESP `part` of a disk of `block_bytes`: the loader under BoxOS's own
 * directory, and as the removable-media default when the ESP is BoxOS's own. */
static error_t EspWritten(const DiskBootAsk* a, const DiskPart* part, uint32_t block_bytes, DiskBootSaid* said)
{
    DiskFatPlace place;
    FatVolume*   v = kmalloc(sizeof(FatVolume));
    if (!v)
        return ERR_NO_MEMORY;
    DiskFatPlaced(&place, a->seat, a->seating, part->first, part->last - part->first + 1u, block_bytes);
    bool own = false, laid = false;
    int  rc  = FatOpen(v, &place.medium);
    if (rc == FAT_OK) {
        FatStamp(v, rtc_get_unix64());
        rc = EspOwn(v, &own);
    }
    if (rc == FAT_OK)
        rc = FatPut(v, ESP_LOADER, a->loader, a->loader_bytes, NULL);
    if (rc == FAT_OK && own)
        rc = FatPut(v, ESP_FALLBACK, a->loader, a->loader_bytes, NULL);
    if (rc == FAT_OK)
        rc = CrossingsLaid(v, &laid);
    kfree(v);
    if (rc != FAT_OK)
        return EspRefusal(a, rc, "BoxOS's loader");
    said->fallback = own;
    kprintf("[DiskBoot] seat %u: BoxOS's loader, %u bytes, put on the ESP in entry %u as %s%s; its crossings %s\n",
            a->seat, a->loader_bytes, a->entry, ESP_LOADER,
            own ? " and as the removable-media default: the ESP is BoxOS's own"
                : " — the removable-media default left as it is: the ESP is shared",
            laid ? "laid as " ESP_CROSSINGS : "kept where they were");
    return OK;
}

/* What a refused write of the BIOS loaders answers. */
static error_t BiosRefusal(const DiskBootAsk* a, const char* what)
{
    if (!BoardroomSeatOccupied(a->seat) || BoardroomSeatSeating(a->seat) != a->seating) {
        kprintf("[DiskBoot] seat %u: the disk left while %s was written\n", a->seat, what);
        return ERR_DEVICE_NOT_READY;
    }
    if (BoardroomSeatRefusesWrites(a->seat)) {
        kprintf("[DiskBoot] seat %u: the medium refuses writes — it is protected\n", a->seat);
        return ERR_PERMISSION_DENIED;
    }
    kprintf("[DiskBoot] seat %u: the medium refused %s\n", a->seat, what);
    return ERR_WRITE_FAILED;
}

/*
 * The BIOS loaders laid on a disk of 512-byte blocks (boxos_bios_boot.h):
 * stage2 at the head of the BIOS boot partition `bios`, and then stage1's code
 * over the protective MBR — its table, its disk signature and its 0x55AA kept
 * — told the partition's first block. In that order, each flushed: a cut
 * between leaves sector 0 as it was, and a BIOS that runs it finds what it
 * found before.
 */
static error_t BiosLaid(const DiskBootAsk* a, const DiskPart* bios)
{
    uint64_t blocks  = bios->last - bios->first + 1u;
    uint32_t sectors = (a->stage2_bytes + BIOS_SECTOR_BYTES - 1u) / BIOS_SECTOR_BYTES;
    if (a->stage1_bytes != BIOS_SECTOR_BYTES || !a->stage2_bytes) {
        kprintf("[DiskBoot] the volume's %s is %u bytes and its %s %u — a boot sector is one sector, and a second "
                "stage is something: no BIOS loader is laid\n", BIOS_STAGE1_KEPT, a->stage1_bytes, BIOS_STAGE2_KEPT,
                a->stage2_bytes);
        return ERR_CORRUPTED;
    }
    if (sectors > blocks) {
        kprintf("[DiskBoot] seat %u: the BIOS boot partition in entry %u is %llu sector(s), and %s takes %u\n",
                a->seat, bios->entry, (unsigned long long)blocks, BIOS_STAGE2_KEPT, sectors);
        return ERR_TAGFS_NO_SPACE;
    }

    DiskFatPlace run, head;
    DiskFatPlaced(&run, a->seat, a->seating, bios->first, blocks, BIOS_SECTOR_BYTES);
    DiskFatPlaced(&head, a->seat, a->seating, 0, 1, BIOS_SECTOR_BYTES);
    uint8_t* stage2 = kmalloc((uint64_t)sectors * BIOS_SECTOR_BYTES);
    if (!stage2)
        return ERR_NO_MEMORY;
    memset(stage2, 0, (uint64_t)sectors * BIOS_SECTOR_BYTES);
    memcpy(stage2, a->stage2, a->stage2_bytes);
    bool landed = run.medium.write(run.medium.ctx, 0, sectors, stage2) == 0 && run.medium.flush(run.medium.ctx) == 0;
    kfree(stage2);
    if (!landed)
        return BiosRefusal(a, BIOS_STAGE2_KEPT);

    uint8_t sector[BIOS_SECTOR_BYTES];
    if (head.medium.read(head.medium.ctx, 0, 1, sector) != 0)
        return BiosRefusal(a, "the read of sector 0");
    bool protective = sector[510] == 0x55 && sector[511] == 0xAA;
    bool claimed    = false;
    for (uint32_t i = 0; i < 4u; i++)
        claimed |= sector[446u + 16u * i + 4u] == DISK_MBR_PROTECTIVE;
    if (!protective || !claimed) {
        kprintf("[DiskBoot] seat %u: sector 0 is no GPT's protective MBR — the boot sector's code is not laid over "
                "it\n", a->seat);
        return ERR_INVALID_STATE;
    }
    memcpy(sector, a->stage1, BIOS_STAGE2_LBA_AT);
    for (uint32_t b = 0; b < 8u; b++)
        sector[BIOS_STAGE2_LBA_AT + b] = (uint8_t)(bios->first >> (8u * b));
    if (head.medium.write(head.medium.ctx, 0, 1, sector) != 0 || head.medium.flush(head.medium.ctx) != 0)
        return BiosRefusal(a, "the boot sector's code");
    kprintf("[DiskBoot] seat %u: a BIOS starts BoxOS too — %s, %u sector(s), at the head of the BIOS boot partition "
            "in entry %u, and the boot sector's code over the protective MBR, told block %llu\n", a->seat,
            BIOS_STAGE2_KEPT, sectors, bios->entry, (unsigned long long)bios->first);
    return OK;
}

error_t DiskBootLay(const DiskBootAsk* a, DiskBootSaid* said)
{
    memset(said, 0, sizeof(*said));
    DiskTable t;
    error_t   rc = DiskTableRead(a->seat, &t);
    if (rc != OK)
        return rc;
    DiskPart part, bios;
    bool     found = false, biosed = false;
    for (uint32_t i = 0; i < t.part_count; i++) {
        if (t.parts[i].entry == a->entry) {
            part  = t.parts[i];
            found = true;
        }
        if (t.parts[i].kind == DISK_PART_BIOS_BOOT && !biosed) {
            bios   = t.parts[i];
            biosed = true;
        }
    }
    uint32_t block_bytes = t.block_bytes;
    if (t.seating != a->seating || t.state == DISK_TABLE_REFUSED)
        rc = t.state == DISK_TABLE_REFUSED ? ERR_DEVICE_NOT_READY : ERR_OBJECT_NOT_FOUND;
    else if (t.state != DISK_TABLE_GPT)
        rc = ERR_INVALID_STATE;
    else if (!found)
        rc = ERR_OUT_OF_RANGE;
    else if (part.kind != DISK_PART_ESP)
        rc = ERR_INVALID_ARGUMENT;
    DiskTableLetGo(&t);
    if (rc == OK)
        rc = EspWritten(a, &part, block_bytes, said);
    if (rc == OK && biosed && !a->stage1)
        kprintf("[DiskBoot] seat %u has a BIOS boot partition, and the volume keeps no BIOS loaders — a BIOS does not "
                "start it\n", a->seat);
    if (rc == OK && biosed && a->stage1 && block_bytes != BIOS_SECTOR_BYTES)
        kprintf("[DiskBoot] seat %u is addressed in %u-byte blocks, and a BIOS reads %u-byte sectors — its BIOS boot "
                "partition is given no loader\n", a->seat, block_bytes, BIOS_SECTOR_BYTES);
    else if (rc == OK && biosed && a->stage1) {
        rc         = BiosLaid(a, &bios);
        said->bios = rc == OK;
    }
    if (rc != OK)
        return rc;

    EfiPartition   where = { .number = (uint32_t)a->entry + 1u, .first = part.first,
                             .blocks = part.last - part.first + 1u };
    EspVolumeAsked asked;
    memcpy(where.unique, part.unique, 16);
    memcpy(asked.magic, ESP_VOLUME_MAGIC, sizeof(asked.magic));
    memcpy(asked.uuid, a->uuid, 16);
    rc = EfiBootOptionPut("BoxOS", &where, ESP_LOADER, &asked, sizeof(asked), a->first, &said->number);
    if (rc == ERR_UNSUPPORTED) {
        kprintf("[DiskBoot] this start has no firmware runtime to tell (a BIOS start): no boot option is "
                "written — %s\n", said->fallback ? "the ESP's removable-media default starts BoxOS"
                                                 : "the firmware's menu does not offer BoxOS until one is");
        return OK;
    }
    said->option = rc == OK;
    return rc;
}

/* The names under one directory, for taking them away one by one. */
typedef struct {
    char     names[16][FAT_NAME_MOST + 1];
    uint32_t count;
    bool     more;
} DirNames;

static int DirNamed(void* ctx, const char* name, const FatEntry* e)
{
    (void)e;
    DirNames* d = ctx;
    if (name[0] == '.')
        return 0;
    if (d->count == sizeof(d->names) / sizeof(d->names[0])) {
        d->more = true;
        return 0;
    }
    size_t n = strnlen(name, FAT_NAME_MOST);
    memcpy(d->names[d->count], name, n);
    d->names[d->count++][n] = 0;
    return 0;
}

/* Whether the files `a` and `b` of `v` hold the same bytes. */
static bool FatSameFiles(FatVolume* v, const FatEntry* a, const FatEntry* b)
{
    if (a->size != b->size || a->size > 16u << 20)
        return false;
    uint8_t* x = kmalloc(a->size ? a->size : 1u);
    uint8_t* y = kmalloc(b->size ? b->size : 1u);
    bool     same = x && y && FatRead(v, a, 0, x, a->size) == (int64_t)a->size &&
                FatRead(v, b, 0, y, b->size) == (int64_t)b->size && memcmp(x, y, a->size) == 0;
    kfree(x);
    kfree(y);
    return same;
}

/* BoxOS's files taken off the ESP `part`: its default first, when it is
 * BoxOS's loader, then its own directory whole. */
static int EspCleared(const DiskUnbootAsk* a, const DiskPart* part, uint32_t block_bytes, DiskUnbootSaid* said)
{
    DiskFatPlace place;
    FatVolume*   v = kmalloc(sizeof(FatVolume));
    DirNames*    d = kmalloc(sizeof(DirNames));
    if (!v || !d) {
        kfree(v);
        kfree(d);
        return FAT_IO;
    }
    memset(d, 0, sizeof(*d));
    DiskFatPlaced(&place, a->seat, a->seating, part->first, part->last - part->first + 1u, block_bytes);
    FatEntry loader, fallback;
    int      rc = FatOpen(v, &place.medium);
    if (rc == FAT_OK) {
        FatStamp(v, rtc_get_unix64());
        if (FatFind(v, ESP_LOADER, &loader) == FAT_OK && FatFind(v, ESP_FALLBACK, &fallback) == FAT_OK &&
            FatSameFiles(v, &loader, &fallback)) {
            rc             = FatRemove(v, ESP_FALLBACK);
            said->fallback = rc == FAT_OK;
            char fallback_dir[32] = "EFI/";
            PathPartOf(ESP_FALLBACK, 1, fallback_dir + 4, sizeof(fallback_dir) - 4);
            int  dir_gone = rc == FAT_OK ? FatRemove(v, fallback_dir) : rc;
            if (dir_gone != FAT_OK && dir_gone != FAT_NOT_DIRECTORY)
                rc = dir_gone;      /* another file keeping the directory keeps it */
        }
    }
    /* The directory is taken away last: while it stands, the disk still
     * shows BoxOS's loader, and removing again takes up what is left. */
    while (rc == FAT_OK) {
        memset(d, 0, sizeof(*d));
        int listed = FatEach(v, ESP_DIR, DirNamed, d);
        if (listed == FAT_NOT_FOUND)
            break;
        rc = listed;
        for (uint32_t i = 0; rc == FAT_OK && i < d->count; i++) {
            char path[sizeof(ESP_DIR) + FAT_NAME_MOST + 2];
            ksnprintf(path, sizeof(path), "%s/%s", ESP_DIR, d->names[i]);
            rc = FatRemove(v, path);
        }
        if (rc == FAT_OK && !d->more) {
            rc           = FatRemove(v, ESP_DIR);
            said->loader = rc == FAT_OK;
            break;
        }
    }
    kfree(v);
    kfree(d);
    return rc;
}

error_t DiskBootTakeAway(const DiskUnbootAsk* a, DiskUnbootSaid* said)
{
    memset(said, 0, sizeof(*said));
    DiskTable t;
    error_t   rc = DiskTableRead(a->seat, &t);
    if (rc != OK)
        return rc;
    DiskPart part;
    bool     found = false;
    for (uint32_t i = 0; i < t.part_count; i++)
        if (t.parts[i].entry == a->entry) {
            part  = t.parts[i];
            found = true;
        }
    uint32_t block_bytes = t.block_bytes;
    if (t.seating != a->seating || t.state == DISK_TABLE_REFUSED)
        rc = t.state == DISK_TABLE_REFUSED ? ERR_DEVICE_NOT_READY : ERR_OBJECT_NOT_FOUND;
    else if (t.state != DISK_TABLE_GPT)
        rc = ERR_INVALID_STATE;
    else if (!found)
        rc = ERR_OUT_OF_RANGE;
    else if (part.kind != DISK_PART_ESP)
        rc = ERR_INVALID_ARGUMENT;
    DiskTableLetGo(&t);
    if (rc != OK)
        return rc;

    DiskBootAsk asked = { .seat = a->seat, .seating = a->seating, .entry = a->entry };

    /* The boot sector's code first, the firmware's entries next, BoxOS's
     * directory on the ESP last: a cut anywhere leaves that directory, so
     * removing again comes back here and takes up what is left. */
    if (a->stage1 && a->stage1_bytes == BIOS_SECTOR_BYTES && block_bytes == BIOS_SECTOR_BYTES) {
        DiskFatPlace head;
        uint8_t      sector[BIOS_SECTOR_BYTES];
        DiskFatPlaced(&head, a->seat, a->seating, 0, 1, BIOS_SECTOR_BYTES);
        if (head.medium.read(head.medium.ctx, 0, 1, sector) != 0)
            return BiosRefusal(&asked, "the read of sector 0");
        if (memcmp(sector, a->stage1, BIOS_STAGE2_LBA_AT) == 0) {
            memset(sector, 0, BIOS_STAGE2_LBA_AT + 8u);
            if (head.medium.write(head.medium.ctx, 0, 1, sector) != 0 || head.medium.flush(head.medium.ctx) != 0)
                return BiosRefusal(&asked, "sector 0 without BoxOS's code");
            said->bios = true;
        }
    }

    EfiPartition where = { .number = (uint32_t)a->entry + 1u, .first = part.first,
                           .blocks = part.last - part.first + 1u };
    memcpy(where.unique, part.unique, 16);
    rc = EfiBootOptionsTakeAway(&where, ESP_LOADER, &said->options);
    if (rc == ERR_UNSUPPORTED)
        rc = OK;            /* a BIOS start: no firmware to tell */
    if (rc != OK)
        return rc;

    int fat = EspCleared(a, &part, block_bytes, said);
    if (fat != FAT_OK)
        return EspRefusal(&asked, fat, "the taking away of BoxOS's loader");
    kprintf("[DiskBoot] seat %u: BoxOS starts from it no more — %s%s%u boot option(s) of the firmware taken "
            "away%s\n", a->seat, said->loader ? "its directory taken off the ESP, " : "",
            said->fallback ? "the ESP's default with it, being BoxOS's, " : "", said->options,
            said->bios ? ", and its code off the boot sector" : "");
    return OK;
}
