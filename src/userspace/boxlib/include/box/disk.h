#ifndef BOX_DISK_H
#define BOX_DISK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"
#include "boxos_disks.h"

/*
 * The disks of the machine and their tables (boxos_disks.h): every program
 * may see them; a box that asks `change-disks` changes them as the person's
 * hands (-ERR_ACCESS_DENIED otherwise).
 *
 *   DiskListed *disks; size_t count;
 *   disk_list(&disks, &count);               every disk with a medium in it
 *   disk_lay(&disks[1]);                     a new GPT over all of disk 1
 *   disk_add(&disks[1], DISK_PART_BOXOS, disks[1].Free[0].First,
 *            disks[1].Free[0].Blocks, NULL); a BoxOS partition in its free run
 *   free(disks);
 *
 * A change names the disk as the list gave it — its seat and which occupant of
 * the seat it was — and a seat that changed hands since is refused
 * (-ERR_OBJECT_NOT_FOUND): ask the list again. The answer comes once the
 * table is on the medium and read back as it was meant; a cut or a refusal on
 * the way leaves the old table or the new, whole. The refusals are
 * boxos_decks.h's, hw.disk.*: -ERR_BUSY for a disk or partition a volume of
 * the house stands on, -ERR_INVALID_STATE for a disk with no GPT to change,
 * -ERR_CORRUPTED for one whose copies are to be mended first,
 * -ERR_OUT_OF_RANGE for a run that is not free or an entry with no partition,
 * -ERR_QUOTA_EXCEEDED for a full table, -ERR_PERMISSION_DENIED for a medium
 * that refuses writes, -ERR_WOULD_BLOCK while another change is under way.
 */

typedef struct {
    uint8_t     Kind;       /* DiskPartKind */
    bool        Mounted;    /* a volume of the house stands on it */
    uint16_t    Entry;      /* its place in the table */
    uint32_t    Volume;     /* that volume's number + 1; 0 for none */
    uint64_t    First;      /* the disk's blocks */
    uint64_t    Blocks;
    uint8_t     Type[16];   /* its GPT type; an MBR's byte in Type[0] */
    uint8_t     Unique[16]; /* its own GUID, what it is known by across seats; zeros on an MBR */
    uint8_t     Holds;      /* DiskHolds: what its first bytes say it holds */
    bool        DefaultLoader;  /* an ESP whose removable-media default is laid */
    bool        MoreLoaders;    /* an ESP carrying more loaders than are listed */
    size_t      LoaderCount;
    const char *Loaders[DISK_LOADERS_MOST];  /* an ESP's: the directories under its EFI, a system's each */
    const char *Name;       /* the name its table gives it, UTF-8 */
} DiskPartListed;

typedef struct {
    uint64_t First;         /* on the grain, at least one long */
    uint64_t Blocks;
} DiskFreeListed;

typedef struct {
    uint8_t         Seat;
    uint8_t         Board;          /* 1 SATA (AHCI), 2 IDE, 3 USB */
    uint8_t         Table;          /* DiskTableState */
    uint8_t         Copies;         /* DISK_COPY_* of a GPT */
    bool            Removable;
    bool            RefusesWrites;
    bool            CarriesHouse;   /* a volume of the house stands on it: never laid anew */
    uint32_t        Seating;
    uint32_t        BlockBytes;
    uint32_t        PhysicalBytes;  /* 0: the medium would not say */
    uint64_t        Blocks;         /* 0: the medium would not say */
    uint8_t         Guid[16];       /* a GPT's own, what the disk is known by across seats; zeros otherwise */
    const char     *Name;           /* what the medium calls itself */
    size_t          PartCount;
    DiskPartListed *Parts;          /* in the order of their first block */
    size_t          FreeCount;
    DiskFreeListed *Free;
} DiskListed;

/* Every disk with a medium in it, in the order of its seat, as one block:
 * free(*out) lets it go. 0, or a negative -error_t. */
int disk_list(DiskListed **out, size_t *count);

/* The disk a person names at the front of `text`, among the `count` of
 * `disks`: by what its medium calls itself or the name of a volume of the
 * house standing on it — the longest the text begins with (name_begins,
 * box/file.h) — or by its number, the seat it sits in. *text moved past it
 * and the spaces after it, *out the disk. How many disks are called so: 1; 0
 * for none, nor one of that number; more when several share the name — *out
 * the first, and the person is to say which by its number. */
size_t disk_named(const char **text, const DiskListed *disks, size_t count, const DiskListed **out);

/* The system an ESP's loader directory starts, as a person is told it —
 * "Microsoft" is Windows, "ubuntu" Ubuntu —, or the directory's own name for
 * one this list does not know. */
const char *disk_loader_shown(const char *dir);

int disk_lay(const DiskListed *disk);
int disk_add(const DiskListed *disk, uint8_t kind, uint64_t first, uint64_t blocks, uint16_t *entry);
int disk_remove(const DiskListed *disk, uint16_t entry);
int disk_mend(const DiskListed *disk);

/* What making BoxOS start from an ESP did (disk_boot). */
typedef struct {
    uint16_t Option;        /* the firmware's Boot#### given, when Given */
    bool     Given;         /* a boot option given: a start through UEFI */
    bool     Fallback;      /* the ESP's removable-media default written: the ESP is BoxOS's own */
    bool     Bios;          /* the BIOS loaders laid: a BIOS starts it too (boxos_bios_boot.h) */
} DiskBooted;

/* BoxOS made to start from the ESP in `entry` of `disk`: the loader the
 * volume numbered `volume` keeps — a volume on that disk — put on the ESP,
 * and the firmware given a boot option naming it, first in its order for
 * `first` — and, on a disk with a BIOS boot partition, the BIOS loaders the
 * volume keeps laid too. 0, and what was done in *booted (NULL for none); or a negative
 * -error_t: -ERR_INVALID_ARGUMENT for an entry that is no ESP or a volume not
 * on the disk, -ERR_FILE_NOT_FOUND for a volume that keeps no loader,
 * -ERR_CORRUPTED for an ESP that is no FAT32, -ERR_TAGFS_NO_SPACE for one
 * with no room. */
int disk_boot(const DiskListed *disk, uint16_t entry, uint32_t volume, bool first, DiskBooted *booted);

/* BoxOS's partition in `entry` grown into the room right after it, to
 * `blocks` long or all of that room for 0; its length now in *grown (NULL for
 * none). A volume standing on it takes the ground at its next mount.
 * -ERR_OUT_OF_RANGE for no room after it, or a length it already reaches. */
int disk_grow(const DiskListed *disk, uint16_t entry, uint64_t blocks, uint64_t *grown);

/* A new volume laid on BoxOS's partition in `entry` of `disk`, called `name`
 * (NULL or "" for none), and given the system — every file the home volume
 * keeps in a role, the kernel its Deed names for a loader — by the person's
 * hands (change-disks in the box). Answered once all of it is settled: 0, its
 * number in the house in *number and the files the system keeps laid on it in
 * *roles (either may be NULL); or a negative -error_t: -ERR_BUSY for a
 * partition a volume of the house stands on, -ERR_INVALID_ARGUMENT for one
 * that is not BoxOS's or a name that is no name, -ERR_TAGFS_NO_SPACE for one
 * too small, -ERR_WOULD_BLOCK while another laying is under way, and the
 * disks' refusals above. */
int volume_lay(const DiskListed *disk, uint16_t entry, const char *name, uint32_t *number, uint32_t *roles);

/* What a partition is called is DiskKindName (boxos_disks.h), and where one
 * may begin DiskGrainBlocks(disk->BlockBytes, disk->PhysicalBytes): the
 * kernel's own rules, one copy of each. */

#ifdef __cplusplus
}
#endif

#endif /* BOX_DISK_H */
