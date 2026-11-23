#include "disk_look.h"
#include "disk_fat.h"
#include "boxos_esp.h"
#include "volume_deed.h"
#include "klib.h"

/* The head of a partition every signature below lies in. */
#define LOOK_BYTES      4096u
#define EXT_SUPER       1024u      /* ext2/3/4's superblock; HFS+'s volume header too */

static bool LookAt(const uint8_t* b, uint32_t at, const char* text, uint32_t bytes)
{
    return memcmp(b + at, text, bytes) == 0;
}

static uint64_t LookLe(const uint8_t* p, uint32_t bytes)
{
    uint64_t v = 0;
    for (uint32_t i = bytes; i-- > 0;)
        v = v << 8 | p[i];
    return v;
}

/* Whether the block holding byte `at` of the partition `run` begins, at that
 * byte, a BitLocker volume's metadata ("-FVE-FS-"): how a BitLocker To Go
 * volume, whose head reads as a FAT's, is told (libblkid bitlocker.c). */
static bool LookFveAt(DiskFatPlace* run, uint32_t block_bytes, uint64_t at)
{
    if (!at || at % 64u)
        return false;
    uint8_t* b = kmalloc(block_bytes);
    if (!b)
        return false;
    bool fve = at % block_bytes + 8u <= block_bytes && run->medium.read(run->medium.ctx, at / block_bytes, 1, b) == 0 &&
               LookAt(b, (uint32_t)(at % block_bytes), "-FVE-FS-", 8);
    kfree(b);
    return fve;
}

/*
 * What the head `b` of a partition holds, by the signatures libblkid knows
 * (util-linux libblkid/src/superblocks): BitLocker's "-FVE-FS-" in the OEM
 * field — or, on a head that reads as a FAT's, at the offset u64 440 names —,
 * NTFS's and exFAT's OEM fields, a FAT's type label with its 0x55AA, ext's
 * magic 0xEF53 in its superblock (ext4 by an incompatible feature only it
 * has), APFS's container "NXSB", HFS+'s "H+"/"HX", swap's last ten bytes of
 * its first page; a BoxOS volume by its Deed.
 */
static DiskHolds LookHolds(const uint8_t* b, DiskFatPlace* run, uint32_t block_bytes)
{
    if (b[0] == VOLUME_DEED_MAGIC_0 && b[1] == VOLUME_DEED_MAGIC_1 && b[2] == VOLUME_DEED_MAGIC_2 &&
        b[3] == VOLUME_DEED_MAGIC_3 && b[4] == VOLUME_DEED_MAGIC_4 && b[5] == VOLUME_DEED_MAGIC_5 &&
        b[6] == VOLUME_DEED_MAGIC_6 && b[7] == VOLUME_DEED_MAGIC_7)
        return DISK_HOLDS_BOXOS;
    bool sealed = b[510] == 0x55 && b[511] == 0xAA;
    if (LookAt(b, 3, "-FVE-FS-", 8))
        return DISK_HOLDS_BITLOCKER;
    if (LookAt(b, 3, "MSWIN4.1", 8) && LookFveAt(run, block_bytes, LookLe(b + 440, 8)))
        return DISK_HOLDS_BITLOCKER;
    if (LookAt(b, 3, "NTFS    ", 8) && sealed)
        return DISK_HOLDS_NTFS;
    if (LookAt(b, 3, "EXFAT   ", 8))
        return DISK_HOLDS_EXFAT;
    if (sealed && (LookAt(b, 0x52, "FAT32   ", 8) || LookAt(b, 0x36, "FAT12   ", 8) || LookAt(b, 0x36, "FAT16   ", 8) ||
                   LookAt(b, 0x36, "FAT     ", 8)))
        return DISK_HOLDS_FAT;
    if (b[EXT_SUPER + 0x38] == 0x53 && b[EXT_SUPER + 0x39] == 0xEF) {
        uint32_t incompat = (uint32_t)LookLe(b + EXT_SUPER + 0x60, 4);
        uint32_t ro       = (uint32_t)LookLe(b + EXT_SUPER + 0x64, 4);
        if (incompat & 0x8u)
            return DISK_HOLDS_UNKNOWN;          /* an external journal, no file system of its own */
        return (incompat & (0x40u | 0x80u | 0x200u)) || (ro & ~0x7u) ? DISK_HOLDS_EXT4 : DISK_HOLDS_EXT;
    }
    if (LookAt(b, 32, "NXSB", 4))
        return DISK_HOLDS_APFS;
    if (b[EXT_SUPER] == 'H' && (b[EXT_SUPER + 1] == '+' || b[EXT_SUPER + 1] == 'X'))
        return DISK_HOLDS_HFS;
    if (LookAt(b, LOOK_BYTES - 10u, "SWAPSPACE2", 10) || LookAt(b, LOOK_BYTES - 10u, "SWAP-SPACE", 10) ||
        LookAt(b, LOOK_BYTES - 10u, "S1SUSPEND", 9) || LookAt(b, LOOK_BYTES - 10u, "S2SUSPEND", 9) ||
        LookAt(b, LOOK_BYTES - 10u, "ULSUSPEND", 9) || LookAt(b, LOOK_BYTES - 10u, "LINHIB0001", 10))
        return DISK_HOLDS_SWAP;
    for (uint32_t i = 0; i < LOOK_BYTES; i++)
        if (b[i])
            return DISK_HOLDS_UNKNOWN;
    return DISK_HOLDS_EMPTY;
}

static bool LookNameIs(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

/* One directory under an ESP's EFI: a system's loaders, listed — but the
 * removable-media default's own directory, which belongs to no system. */
static int LookLoader(void* ctx, const char* name, const FatEntry* e)
{
    DiskLook* look = ctx;
    char      dir[DISK_LOADER_NAME_MOST + 1];
    size_t    n = 0;
    while (name[n] && n < DISK_LOADER_NAME_MOST) {
        dir[n] = name[n];
        n++;
    }
    dir[n] = 0;
    if (!(e->attr & FAT_ATTR_DIRECTORY) || name[0] == '.' || LookNameIs(dir, "BOOT"))
        return 0;
    if (look->loaders_len + n + 1u > sizeof(look->loaders)) {
        look->flags |= DISK_PART_MORE_LOADERS;
        return 0;
    }
    memcpy(look->loaders + look->loaders_len, dir, n + 1u);
    look->loaders_len = (uint16_t)(look->loaders_len + n + 1u);
    return 0;
}

/* The loaders the ESP on `run` carries, and whether its default is laid. */
static void LookLoaders(DiskFatPlace* run, DiskLook* look)
{
    FatVolume* v = kmalloc(sizeof(FatVolume));
    FatEntry   fallback;
    if (!v)
        return;
    if (FatOpen(v, &run->medium) == FAT_OK) {
        (void)FatEach(v, "EFI", LookLoader, look);
        if (FatFind(v, ESP_FALLBACK, &fallback) == FAT_OK)
            look->flags |= DISK_PART_DEFAULT_LOADER;
    }
    kfree(v);
}

void DiskLookAt(const DiskTable* t, uint32_t index, DiskLook* look)
{
    memset(look, 0, sizeof(*look));
    const DiskPart* p      = &t->parts[index];
    uint64_t        blocks = p->last - p->first + 1u;
    uint32_t        want   = t->block_bytes < LOOK_BYTES ? LOOK_BYTES / t->block_bytes : 1u;
    if (!t->block_bytes || blocks < want)
        return;
    DiskFatPlace run;
    DiskFatPlaced(&run, t->seat, t->seating, p->first, blocks, t->block_bytes);
    uint8_t* b = kmalloc((uint64_t)want * t->block_bytes);
    if (!b)
        return;
    if (run.medium.read(run.medium.ctx, 0, want, b) == 0)
        look->holds = (uint8_t)LookHolds(b, &run, t->block_bytes);
    kfree(b);
    if (p->kind == DISK_PART_ESP && look->holds == DISK_HOLDS_FAT)
        LookLoaders(&run, look);
}
