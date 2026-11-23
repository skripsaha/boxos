#include "disk_boot.h"
#include "disk_table.h"
#include "disk_fat.h"
#include "boardroom.h"
#include "efi_boot.h"
#include "boxos_esp.h"
#include "rtc.h"
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

/* The ESP `part` of a disk of `block_bytes`: the loader under BoxOS's own
 * directory, and as the removable-media default when the ESP is BoxOS's own. */
static error_t EspWritten(const DiskBootAsk* a, const DiskPart* part, uint32_t block_bytes, DiskBootSaid* said)
{
    DiskFatPlace place;
    FatVolume*   v = kmalloc(sizeof(FatVolume));
    if (!v)
        return ERR_NO_MEMORY;
    DiskFatPlaced(&place, a->seat, a->seating, part->first, part->last - part->first + 1u, block_bytes);
    bool own = false;
    int  rc  = FatOpen(v, &place.medium);
    if (rc == FAT_OK) {
        FatStamp(v, rtc_get_unix64());
        rc = EspOwn(v, &own);
    }
    if (rc == FAT_OK)
        rc = FatPut(v, ESP_LOADER, a->loader, a->loader_bytes, NULL);
    if (rc == FAT_OK && own)
        rc = FatPut(v, ESP_FALLBACK, a->loader, a->loader_bytes, NULL);
    kfree(v);
    if (rc != FAT_OK)
        return EspRefusal(a, rc, "BoxOS's loader");
    said->fallback = own;
    kprintf("[DiskBoot] seat %u: BoxOS's loader, %u bytes, put on the ESP in entry %u as %s%s\n", a->seat,
            a->loader_bytes, a->entry, ESP_LOADER,
            own ? " and as the removable-media default: the ESP is BoxOS's own"
                : " — the removable-media default left as it is: the ESP is shared");
    return OK;
}

error_t DiskBootLay(const DiskBootAsk* a, DiskBootSaid* said)
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
    if (rc == OK)
        rc = EspWritten(a, &part, block_bytes, said);
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
