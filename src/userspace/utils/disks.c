/*
 * disks — the machine's disks, what lies on each, and their tables laid by
 * the person's hands.
 *
 *   disks                        every disk: what it is, its table, its
 *                                partitions and the volume standing on each,
 *                                and its free space
 *   disks 1                      disk 1 alone
 *   disks 1 new                  a new table over all of disk 1: everything
 *                                on it is gone
 *   disks 1 add boxos            a BoxOS partition over disk 1's largest
 *                                free run; `add boxos 20G` of that size
 *   disks 1 add efi 512M         an EFI system partition of that size
 *   disks 1 add bios             a BIOS boot partition, a grain long: where
 *                                a BIOS start's second stage lies, so the
 *                                disk starts through a BIOS too
 *   disks 1 remove 2             BoxOS's partition in entry 2 taken away
 *   disks 1 boot 0 2             BoxOS made to start from the ESP in entry 0:
 *                                the loader volume 2 keeps put on it, and the
 *                                machine's firmware told of it — first in its
 *                                order if the person says so; on a disk with a
 *                                BIOS boot partition, a BIOS starts it too
 *   disks 1 grow 2               BoxOS's partition in entry 2 grown into all
 *                                the free space right after it; `grow 2 40G`
 *                                to that size — a volume on it takes the
 *                                ground at its next mount
 *   disks 1 mend                 both copies of disk 1's table put right
 *
 * A disk is named by its number, the seat it sits in, by what its medium
 * calls itself, or by the name of a volume standing on it (`disks Zen`); a
 * volume by its name or number (disk_named, volume_named); a name several
 * share is asked again by number. Nothing is changed
 * before the person at the screen sees what goes and says yes; with nobody
 * there, nothing is (box/form.h form_person_here). What another system keeps
 * is never touched: a partition is laid only in free space, and only BoxOS's
 * own are taken away. A disk a volume of the house stands on is never laid
 * anew. Sizes are in K, M, G or T — thousands, as `volumes` tells them —
 * and a partition begins and ends on the grain the disk is built around.
 *
 * Its exit: 0 when the disks were shown or the change made, or the person
 * said no; 1 when the change was refused or nobody was there to be asked.
 */

#include "box/print.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/disk.h"
#include "box/file.h"
#include "box/string.h"
#include "box/error.h"
#include "box/system.h"
#include "fat.h"
#include "boxos_bios_boot.h"

BOX_FORM("takes optional text \"a disk's name or number, and new, add, remove, grow, boot or mend\"\n");

/* ── telling ─────────────────────────────────────────────────────────────── */

static uint64_t disks_bytes(const DiskListed *d, uint64_t blocks)
{
    return blocks * d->BlockBytes;
}

/* "a" or "an" before a kind's name, as it is said. */
static const char *disks_a(uint8_t kind)
{
    const char *name = DiskKindName((DiskPartKind)kind);
    return strchr("AEIOUaeiou", name[0]) ? "an" : "a";
}

static const char *disks_table_said(const DiskListed *d)
{
    switch (d->Table) {
    case DISK_TABLE_NONE:     return "no table";
    case DISK_TABLE_MBR:      return "the old MBR table";
    case DISK_TABLE_GPT:      return "GPT";
    case DISK_TABLE_GPT_LOST: return "a GPT neither copy of which holds";
    default:                  return "a table it would not give up";
    }
}

/* The volume of the house standing on a partition, as `volumes` shows it. */
static void disks_volume_said(uint32_t number, char *out, size_t room)
{
    VolumeListed *v = NULL;
    if (volume_info(number, &v) != 0) {
        snprintf(out, room, "volume %u", number);
        return;
    }
    char shown[STORAGE_VOLUME_NAME_MOST + 64];
    volume_shown(v, shown, sizeof(shown));
    snprintf(out, room, "volume %u, %s%s", number, shown, v->Home ? " (home)" : "");
    free(v);
}

/* What a partition holds, after its volume when one stands on it: an ESP the
 * systems its loaders start, any other what its first bytes say. Nothing for
 * a volume of the house, which is said already, nor for a head this system
 * cannot read. */
static void disks_holds_said(const DiskPartListed *p, char *out, size_t room)
{
    out[0] = '\0';
    if (p->Volume || p->Holds == DISK_HOLDS_UNKNOWN) return;
    if (p->Kind == DISK_PART_ESP && p->Holds == DISK_HOLDS_FAT) {
        if (!p->LoaderCount) {
            snprintf(out, room, p->DefaultLoader ? " — starts the loader it keeps as the firmware's default" : " — empty");
            return;
        }
        size_t at = (size_t)snprintf(out, room, " — starts ");
        for (size_t k = 0; k < p->LoaderCount && at < room; k++)
            at += (size_t)snprintf(out + at, room - at, "%s%s", k ? ", " : "", disk_loader_shown(p->Loaders[k]));
        if (p->MoreLoaders && at < room) snprintf(out + at, room - at, " and more");
        return;
    }
    snprintf(out, room, " — %s", DiskHoldsName((DiskHolds)p->Holds));
}

static void disks_say(const DiskListed *d)
{
    char size[24];
    size_shown(disks_bytes(d, d->Blocks), size, sizeof(size));
    printf("Disk %u — %s, %s, %s", d->Seat, d->Name[0] ? d->Name : "a medium", d->Blocks ? size : "its size unsaid",
           disks_table_said(d));
    if (d->BlockBytes != 512)
        printf(", in %u-byte blocks", d->BlockBytes);
    printf("%s%s\n", d->Removable ? ", removable" : "", d->CarriesHouse ? " — a volume of the house stands on it" : "");
    if (d->Copies & DISK_COPY_FIRST_DAMAGED)
        printf("   its table's first copy is damaged, the far one is read: `disks %u mend` puts it right\n", d->Seat);
    if (d->Copies & DISK_COPY_FAR_DAMAGED)
        printf("   its table's far copy is damaged: `disks %u mend` puts it right\n", d->Seat);
    if (d->Copies & DISK_COPY_DIFFER)
        printf("   its table's two copies say different things, the first is read: `disks %u mend` puts it right\n",
               d->Seat);
    if (d->Copies & DISK_COPY_FAR_ELSEWHERE)
        printf("   its table was made for a smaller disk: `disks %u mend` gives it the whole of this one\n", d->Seat);
    if (d->RefusesWrites)
        printf("   it refuses writes: it is protected\n");

    for (size_t i = 0; i < d->PartCount; i++) {
        const DiskPartListed *p = &d->Parts[i];
        char volume[STORAGE_VOLUME_NAME_MOST + 96] = "";
        char holds[DISK_LOADERS_MOST * (DISK_LOADER_NAME_MOST + 2u) + 48u] = "";
        size_shown(disks_bytes(d, p->Blocks), size, sizeof(size));
        if (p->Volume) {
            char said[STORAGE_VOLUME_NAME_MOST + 80];
            disks_volume_said(p->Volume - 1u, said, sizeof(said));
            snprintf(volume, sizeof(volume), " — %s", said);
        }
        disks_holds_said(p, holds, sizeof(holds));
        printf("   entry %u  %s, %s%s%s%s%s%s\n", p->Entry, DiskKindName((DiskPartKind)p->Kind), size,
               p->Name[0] ? " \"" : "", p->Name, p->Name[0] ? "\"" : "", volume, holds);
    }
    for (size_t i = 0; i < d->FreeCount; i++) {
        size_shown(disks_bytes(d, d->Free[i].Blocks), size, sizeof(size));
        printf("   free  %s, from block %llu\n", size, (unsigned long long)d->Free[i].First);
    }
    if (!d->CarriesHouse &&
        (d->Table == DISK_TABLE_NONE || d->Table == DISK_TABLE_MBR || d->Table == DISK_TABLE_GPT_LOST))
        printf("   `disks %u new` lays a new table over it, and everything on it is gone\n", d->Seat);
}

/* What a refused change is told as. */
static const char *disks_refusal(int rc)
{
    switch (-rc) {
    case ERR_ACCESS_DENIED:     return "only the person's hands change a disk's table";
    case ERR_OBJECT_NOT_FOUND:  return "the disk is not there any more, or another sits in its place";
    case ERR_WOULD_BLOCK:       return "another disk's table is being changed: ask again";
    case ERR_BUSY:              return "a volume of the house stands there";
    case ERR_INVALID_STATE:     return "the disk has no table to change: `new` lays one";
    case ERR_CORRUPTED:         return "its table's copies are to be put right first: `mend`";
    case ERR_OUT_OF_RANGE:      return "there is no such free run, or no such partition";
    case ERR_QUOTA_EXCEEDED:    return "its table has no entry left";
    case ERR_INVALID_ARGUMENT:  return "only BoxOS's own partitions are taken away";
    case ERR_PERMISSION_DENIED: return "the disk refuses writes: it is protected";
    case ERR_DEVICE_NOT_READY:  return "the disk left on the way";
    default:                    return "the disk refused it";
    }
}

/* ── asking ──────────────────────────────────────────────────────────────── */

/* Yes or no from the person at the screen to `question` — nobody there is
 * said, and is no. */
static bool disks_agreed(const char *question)
{
    if (!form_person_here()) {
        println("disks: nobody is at the screen to say yes, so nothing was changed");
        return false;
    }
    return form_yes(question);
}

/* A size in K, M, G or T (thousands, an optional B after), into bytes; 0 for
 * a word that is not one. */
static uint64_t disks_size(const char *word)
{
    uint64_t n = 0;
    const char *at = word;
    if (*at < '0' || *at > '9') return 0;
    while (*at >= '0' && *at <= '9' && n < 1000000000000000ull) n = n * 10u + (uint64_t)(*at++ - '0');
    uint64_t unit = *at == 'K' || *at == 'k' ? 1000ull : *at == 'M' || *at == 'm' ? 1000000ull
                  : *at == 'G' || *at == 'g' ? 1000000000ull : *at == 'T' || *at == 't' ? 1000000000000ull : 0;
    if (!unit) return 0;
    at++;
    if (*at == 'B' || *at == 'b') at++;
    return *at ? 0 : n * unit;
}

/* ── changing ────────────────────────────────────────────────────────────── */

static int disks_new(const DiskListed *d)
{
    if (d->CarriesHouse) {
        printf("Disk %u is not laid anew: a volume of the house stands on it\n", d->Seat);
        return 1;
    }
    disks_say(d);
    printf("A new table over all of disk %u: %s gone for good\n", d->Seat,
           d->PartCount ? "every partition above, and all it holds, is" : "whatever it holds is");
    if (!disks_agreed("lay it?")) {
        println("Nothing was changed");
        return form_person_here() ? 0 : 1;
    }
    int rc = disk_lay(d);
    if (rc != 0) {
        printf("Disk %u was not laid anew: %s\n", d->Seat, disks_refusal(rc));
        return 1;
    }
    printf("Disk %u has a new table\n", d->Seat);
    return 0;
}

static int disks_add(const DiskListed *d, const char *what)
{
    uint8_t     kind = 0;
    const char *rest = what;
    if (d->Table != DISK_TABLE_GPT || d->Copies) {
        printf("No partition was laid on disk %u: %s\n", d->Seat,
               disks_refusal(d->Table != DISK_TABLE_GPT ? -ERR_INVALID_STATE : -ERR_CORRUPTED));
        return 1;
    }
    if (strncmp(rest, "boxos", 5) == 0 && (rest[5] == '\0' || rest[5] == ' ')) {
        kind = DISK_PART_BOXOS;
        rest += 5;
    } else if (strncmp(rest, "efi", 3) == 0 && (rest[3] == '\0' || rest[3] == ' ')) {
        kind = DISK_PART_ESP;
        rest += 3;
    } else if (strncmp(rest, "bios", 4) == 0 && (rest[4] == '\0' || rest[4] == ' ')) {
        kind = DISK_PART_BIOS_BOOT;
        rest += 4;
    } else {
        println("A partition laid is boxos, efi or bios: disks <disk> add boxos [size], disks <disk> add efi <size>, "
                "disks <disk> add bios");
        return 1;
    }
    if (kind == DISK_PART_BIOS_BOOT && d->BlockBytes != BIOS_SECTOR_BYTES) {
        printf("A BIOS reads %u-byte sectors and disk %u's blocks are %u bytes: it starts through UEFI alone\n",
               BIOS_SECTOR_BYTES, d->Seat, d->BlockBytes);
        return 1;
    }
    while (*rest == ' ') rest++;
    uint64_t want = *rest ? disks_size(rest) : 0;
    if (*rest && !want) {
        printf("\"%s\" is not a size: 512M, 20G, 1T\n", rest);
        return 1;
    }
    if (!want && kind == DISK_PART_ESP) {
        println("An EFI system partition is given its size: disks <number> add efi 512M");
        return 1;
    }

    /* The first free run that holds the size, from its start; with no size,
     * the whole of the largest. */
    uint64_t grain  = DiskGrainBlocks(d->BlockBytes, d->PhysicalBytes);
    if (!want && kind == DISK_PART_BIOS_BOOT)
        want = grain * d->BlockBytes;    /* a grain: far more than the second stage, and nothing wasted on the grain */
    uint64_t blocks = want ? (want + d->BlockBytes - 1u) / d->BlockBytes : 0;
    blocks          = (blocks + grain - 1u) / grain * grain;
    const DiskFreeListed *run = NULL;
    for (size_t i = 0; i < d->FreeCount; i++) {
        bool better = want ? (!run && d->Free[i].Blocks >= blocks) : (!run || d->Free[i].Blocks > run->Blocks);
        if (better)
            run = &d->Free[i];
    }
    if (!run) {
        printf("Disk %u has no free run %s\n", d->Seat, want ? "that large" : "at all");
        return 1;
    }
    if (!want) blocks = run->Blocks;
    /* An ESP is a FAT32 (UEFI 2.10 §13.3.1), and a FAT32 is so many
     * clusters at the least: the kernel's rule (fat.h), asked first. */
    uint64_t least = kind == DISK_PART_ESP ? (FatLeastSectors(d->BlockBytes) + grain - 1u) / grain * grain : 0;
    if (blocks < least) {
        char most[24];
        size_shown(disks_bytes(d, least), most, sizeof(most));
        printf("An EFI system partition is a FAT32, %s at the least on disk %u, whose blocks are %u bytes\n", most,
               d->Seat, d->BlockBytes);
        return 1;
    }

    char size[24];
    size_shown(disks_bytes(d, blocks), size, sizeof(size));
    printf("%s %s partition of %s on disk %u, from block %llu\n", disks_a(kind)[1] ? "An" : "A",
           DiskKindName((DiskPartKind)kind), size, d->Seat, (unsigned long long)run->First);
    if (!disks_agreed("lay it?")) {
        println("Nothing was changed");
        return form_person_here() ? 0 : 1;
    }
    uint16_t entry = 0;
    int      rc    = disk_add(d, kind, run->First, blocks, &entry);
    if (rc != 0) {
        printf("No partition was laid on disk %u: %s\n", d->Seat, disks_refusal(rc));
        return 1;
    }
    printf("Disk %u has %s %s partition in entry %u\n", d->Seat, disks_a(kind), DiskKindName((DiskPartKind)kind),
           entry);
    return 0;
}

static int disks_remove(const DiskListed *d, const char *what)
{
    if (*what < '0' || *what > '9') {
        println("A partition is named by its entry: disks <number> remove <entry>");
        return 1;
    }
    uint32_t entry = 0;
    while (*what >= '0' && *what <= '9' && entry <= 0xFFFFu) entry = entry * 10u + (uint32_t)(*what++ - '0');
    const DiskPartListed *p = NULL;
    for (size_t i = 0; i < d->PartCount; i++)
        if (d->Parts[i].Entry == entry) p = &d->Parts[i];
    if (!p || *what) {
        printf("Disk %u has no partition in entry %u\n", d->Seat, entry);
        return 1;
    }
    int refused = d->Table != DISK_TABLE_GPT ? -ERR_INVALID_STATE : d->Copies ? -ERR_CORRUPTED
                : p->Kind != DISK_PART_BOXOS ? -ERR_INVALID_ARGUMENT : p->Mounted ? -ERR_BUSY : 0;
    if (refused) {
        printf("Entry %u of disk %u stays: %s\n", entry, d->Seat, disks_refusal(refused));
        return 1;
    }
    char size[24];
    size_shown(disks_bytes(d, p->Blocks), size, sizeof(size));
    printf("Entry %u of disk %u, %s %s partition of %s, is taken away: what it holds is gone for good\n", entry,
           d->Seat, disks_a(p->Kind), DiskKindName((DiskPartKind)p->Kind), size);
    if (!disks_agreed("take it away?")) {
        println("Nothing was changed");
        return form_person_here() ? 0 : 1;
    }
    int rc = disk_remove(d, (uint16_t)entry);
    if (rc != 0) {
        printf("Entry %u of disk %u stays: %s\n", entry, d->Seat, disks_refusal(rc));
        return 1;
    }
    printf("Entry %u of disk %u is taken away\n", entry, d->Seat);
    return 0;
}

static int disks_grow(const DiskListed *d, const char *what)
{
    if (*what < '0' || *what > '9') {
        println("A partition is named by its entry: disks <number> grow <entry> [size]");
        return 1;
    }
    uint32_t entry = 0;
    while (*what >= '0' && *what <= '9' && entry <= 0xFFFFu) entry = entry * 10u + (uint32_t)(*what++ - '0');
    while (*what == ' ') what++;
    uint64_t want = *what ? disks_size(what) : 0;
    if (*what && !want) {
        printf("\"%s\" is not a size: 512M, 20G, 1T\n", what);
        return 1;
    }
    const DiskPartListed *p = NULL;
    for (size_t i = 0; i < d->PartCount; i++)
        if (d->Parts[i].Entry == entry) p = &d->Parts[i];
    if (!p) {
        printf("Disk %u has no partition in entry %u\n", d->Seat, entry);
        return 1;
    }
    int refused = d->Table != DISK_TABLE_GPT ? -ERR_INVALID_STATE : d->Copies ? -ERR_CORRUPTED
                : p->Kind != DISK_PART_BOXOS ? -ERR_INVALID_ARGUMENT : 0;
    if (refused) {
        printf("Entry %u of disk %u does not grow: %s\n", entry, d->Seat,
               refused == -ERR_INVALID_ARGUMENT ? "only BoxOS's own partitions grow" : disks_refusal(refused));
        return 1;
    }
    char size[24];
    size_shown(disks_bytes(d, p->Blocks), size, sizeof(size));
    printf("Entry %u of disk %u, %s, grows into the free space right after it%s%s%s\n", entry, d->Seat, size,
           want ? " to " : "", want ? what : "", p->Mounted ? " — the volume on it takes it at its next mount" : "");
    if (!disks_agreed("grow it?")) {
        println("Nothing was changed");
        return form_person_here() ? 0 : 1;
    }
    uint64_t grown = 0;
    int      rc    = disk_grow(d, (uint16_t)entry, want ? (want + d->BlockBytes - 1u) / d->BlockBytes : 0, &grown);
    if (rc != 0) {
        printf("Entry %u of disk %u stays as it was: %s\n", entry, d->Seat,
               rc == -ERR_OUT_OF_RANGE ? "there is no free space right after it to grow into, or it is that size"
                                       : disks_refusal(rc));
        return 1;
    }
    size_shown(disks_bytes(d, grown), size, sizeof(size));
    printf("Entry %u of disk %u is %s now\n", entry, d->Seat, size);
    return 0;
}

/* What a refused boot answers. */
static const char *disks_boot_refusal(int rc)
{
    switch (-rc) {
    case ERR_INVALID_ARGUMENT: return "the volume does not stand on this disk";
    case ERR_FILE_NOT_FOUND:   return "the volume keeps no loader";
    case ERR_CORRUPTED:        return "the ESP does not read as a FAT32";
    case ERR_TAGFS_NO_SPACE:   return "the ESP has no room for the loader";
    case ERR_WRITE_FAILED:     return "the disk or the firmware refused a write";
    default:                   return disks_refusal(rc);
    }
}

static int disks_boot(const DiskListed *d, const char *what)
{
    uint32_t entry = 0, volume = 0;
    bool     read  = *what >= '0' && *what <= '9';
    while (*what >= '0' && *what <= '9' && entry <= 0xFFFFu) entry = entry * 10u + (uint32_t)(*what++ - '0');
    read = read && *what == ' ';
    while (*what == ' ') what++;
    int called = read ? volume_named(&what, &volume) : 0;
    if (called > 1) {
        println("More than one volume is called so — name it by its number (volumes shows them)");
        return 1;
    }
    if (called != 1 || *what) {
        println("BoxOS is made to start from an ESP: disks <disk> boot <the ESP's entry> <a volume on the disk>");
        return 1;
    }
    const DiskPartListed *esp = NULL;
    bool                  on  = false;
    for (size_t i = 0; i < d->PartCount; i++) {
        if (d->Parts[i].Entry == entry) esp = &d->Parts[i];
        on |= d->Parts[i].Volume == volume + 1u;
    }
    if (!esp || esp->Kind != DISK_PART_ESP || d->Table != DISK_TABLE_GPT) {
        printf("Entry %u of disk %u is no EFI system partition of a GPT: `disks %u add efi <size>` lays one\n", entry,
               d->Seat, d->Seat);
        return 1;
    }
    if (!on) {
        printf("Volume %u does not stand on disk %u: the loader on an ESP starts a volume of its own disk\n", volume,
               d->Seat);
        return 1;
    }
    printf("BoxOS is to start from the ESP in entry %u of disk %u: the loader volume %u keeps goes on it, and the "
           "machine's firmware is told of it\n", entry, d->Seat, volume);
    if (!disks_agreed("go on?")) {
        println("Nothing was changed");
        return form_person_here() ? 0 : 1;
    }
    bool       first  = form_yes("Start BoxOS first when the machine starts?");
    DiskBooted booted = { 0 };
    int        rc     = disk_boot(d, (uint16_t)entry, volume, first, &booted);
    if (rc != 0) {
        printf("BoxOS was not made to start from disk %u: %s\n", d->Seat, disks_boot_refusal(rc));
        return 1;
    }
    printf("BoxOS starts from the ESP in entry %u of disk %u%s\n", entry, d->Seat,
           booted.Fallback ? " — its loader is the ESP's default too, the ESP being BoxOS's own" : "");
    if (booted.Bios)
        printf("A BIOS starts it too: the second stage in the disk's BIOS boot partition, the boot sector's code "
               "over its first sector\n");
    if (booted.Given)
        printf("The firmware's boot option %04X starts it, %s\n", booted.Option,
               first ? "first when the machine starts" : "among the others: the firmware's menu offers it");
    else
        println("This start knows no firmware to tell (a BIOS start): the firmware is not given a boot option");
    return 0;
}

static int disks_mend(const DiskListed *d)
{
    if (d->Table != DISK_TABLE_GPT || !d->Copies) {
        printf("Disk %u's table has nothing to put right\n", d->Seat);
        return 0;
    }
    disks_say(d);
    if (!disks_agreed("put both copies of its table right?")) {
        println("Nothing was changed");
        return form_person_here() ? 0 : 1;
    }
    int rc = disk_mend(d);
    if (rc != 0) {
        printf("Disk %u's table stays as it was: %s\n", d->Seat, disks_refusal(rc));
        return 1;
    }
    printf("Disk %u's table is put right: both copies hold and agree\n", d->Seat);
    return 0;
}

/* ── the command ─────────────────────────────────────────────────────────── */

static int disks_usage(void)
{
    println("disks, or disks <disk> [new | add boxos [size] | add efi <size> | remove <entry> | grow <entry> [size] | boot <entry> <volume> | mend]");
    return 1;
}

static int disks_run(const char *text, DiskListed *disks, size_t count)
{
    if (!text[0]) {
        for (size_t i = 0; i < count; i++) disks_say(&disks[i]);
        if (!count) println("No disk has a medium in it");
        return 0;
    }
    const char       *named  = text;
    const DiskListed *d      = NULL;
    size_t            called = disk_named(&text, disks, count, &d);
    if (called > 1) {
        println("More than one disk is called so — name it by its number (disks shows them)");
        return 1;
    }
    if (!called) {
        if (*named < '0' || *named > '9') return disks_usage();
        printf("There is no disk numbered %.*s\n", (int)strcspn(named, " "), named);
        return 1;
    }
    if (!*text) {
        disks_say(d);
        return 0;
    }
    if (strcmp(text, "new") == 0) return disks_new(d);
    if (strcmp(text, "mend") == 0) return disks_mend(d);
    if (strncmp(text, "add ", 4) == 0) return disks_add(d, text + 4);
    if (strncmp(text, "remove ", 7) == 0) return disks_remove(d, text + 7);
    if (strncmp(text, "grow ", 5) == 0) return disks_grow(d, text + 5);
    if (strncmp(text, "boot ", 5) == 0) return disks_boot(d, text + 5);
    return disks_usage();
}

int main(void)
{
    const char *text = form_text(0);
    if (!text) {
        exit(1);
        return 1;
    }
    DiskListed *disks = NULL;
    size_t      count = 0;
    int         rc    = disk_list(&disks, &count);
    if (rc != 0) {
        printf("The disks could not be listed: %s\n", disks_refusal(rc));
        exit(1);
        return 1;
    }
    int status = disks_run(text, disks, count);
    free(disks);
    exit(status);
    return status;
}
