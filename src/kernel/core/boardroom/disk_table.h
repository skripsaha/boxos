#ifndef DISK_TABLE_H
#define DISK_TABLE_H

#include "ktypes.h"
#include "error.h"
#include "boxos_disks.h"

/*
 * DiskTable — the partition table of a disk, read whole and written in one
 * place.
 *
 * Read: every partition the table names, whatever system it belongs to, with
 * the kind a person is told (boxos_disks.h), the name the table gives it and
 * where it runs; the free runs between them, each beginning on the grain; and
 * what the table's copies say of each other. Ground (ground.c) is the BoxOS
 * partitions of this answer, which the volumes stand on — there is one reader
 * of a GPT in the kernel, and it is this.
 *
 * Written (disk_table_write.c): a new GPT over a whole disk, a partition laid
 * in a free run, one of BoxOS's taken away, a copy mended from the other.
 * Nothing else in the kernel writes a partition table, and every write drops
 * what was remembered of the disk (DiskTableForget).
 *
 * ‼ EVERY NUMBER IS IN THE MEDIUM'S OWN BLOCKS. A table counts in the logical
 * block of its medium — 512 bytes, or 4096 on a 4Kn disk (UEFI 2.10 §5.3) —
 * and so does everything here; the room's 512-byte sectors are met only at
 * its doors (BoardroomRead, BoardroomWrite).
 *
 * ‼ A DISK IS NAMED BY ITS SEAT AND ITS SEATING. A seat is a socket and can be
 * taken again; a table read of one occupant is never the answer for the next,
 * and a change asked of the occupant a person looked at is refused once the
 * seat has changed hands.
 */

typedef struct {
    uint16_t     entry;                      /* its place in the table */
    DiskPartKind kind;
    uint8_t      type[16];                   /* GPT type; an MBR's byte in type[0] */
    uint8_t      unique[16];                 /* GPT: the partition's own GUID; MBR: zeros */
    uint64_t     attributes;                 /* GPT */
    uint64_t     first;                      /* the medium's blocks, both inclusive */
    uint64_t     last;
    char         name[DISK_NAME_MOST + 1];   /* UTF-8, NUL-ended */
} DiskPart;

typedef struct {
    uint64_t first;
    uint64_t blocks;
} DiskFree;

typedef struct {
    uint8_t        seat;
    uint32_t       seating;
    uint32_t       block_bytes;
    uint32_t       physical_bytes;      /* 0: the medium would not say */
    uint64_t       blocks;              /* how far the medium runs; 0: it would not say */
    DiskTableState state;
    uint8_t        copies;              /* DISK_COPY_*, of a GPT */

    /* A GPT's, as the copy read states them. */
    uint8_t        disk_guid[16];
    uint64_t       first_usable;
    uint64_t       last_usable;
    uint32_t       entry_count;
    uint32_t       entry_bytes;
    uint8_t*       entries;             /* entry_count × entry_bytes, as on the medium */
    uint64_t       first_entries;       /* where each copy lies, as its header says: */
    uint64_t       far_header;          /* ... 0 of a copy that does not hold */
    uint64_t       far_entries;

    uint32_t       part_count;
    DiskPart*      parts;               /* in the order of their first block */
    uint32_t       free_count;
    DiskFree*      free;
} DiskTable;

/* The table of the disk in `seat`, into `out`, which DiskTableLetGo lets go:
 * OK with its state — a disk with no table, an MBR one, one whose copies are
 * both lost and one that refused the reads are answers too;
 * ERR_OBJECT_NOT_FOUND for an empty seat; ERR_NO_MEMORY. A table is read off
 * the medium once for each occupant of the seat and remembered; a refusal is
 * never remembered. */
error_t DiskTableRead(uint8_t seat, DiskTable* out);
void    DiskTableLetGo(DiskTable* t);

/* What was remembered of the seat's disk let go: its next read is the medium's. */
void    DiskTableForget(uint8_t seat);

/* What stands on a disk a change is asked of, as the one asking knows it — the
 * room knowing media and not volumes: the first block of every partition a
 * volume of the house stands on. A disk with any is never laid anew, and none
 * of them is taken away. */
typedef struct {
    uint32_t        count;
    const uint64_t* firsts;
} DiskInUse;

/*
 * The changes (disk_table_write.c), each of the disk a person looked at — its
 * seat and its seating — and each answered once the table is on the medium
 * and read back as it was meant. Every one may answer:
 *   ERR_OBJECT_NOT_FOUND  the seat is empty, or holds another disk since;
 *   ERR_WOULD_BLOCK       a change of some disk's table is under way;
 *   ERR_DEVICE_NOT_READY  the medium refused the reads, or left on the way;
 *   ERR_PERMISSION_DENIED the medium refuses writes: it is protected;
 *   ERR_WRITE_FAILED      a write was refused, or did not read back;
 *   ERR_NO_MEMORY.
 * A cut or a refusal on the way leaves the old table or the new, whole.
 */

/* A new GPT over the whole disk, no partition in it: ERR_BUSY for a disk a
 * volume of the house stands on; ERR_INVALID_STATE for a medium that will
 * not say how far it runs; ERR_OUT_OF_RANGE for one too small for a table
 * and a partition. */
error_t DiskTableLay(uint8_t seat, uint32_t seating, const DiskInUse* used);

/* A partition of `kind` — DISK_PART_ESP or DISK_PART_BOXOS — over `blocks`
 * from `first`, which begins on the grain inside one free run; `entry` (NULL
 * for none) the place it took. ERR_INVALID_ARGUMENT for another kind;
 * ERR_INVALID_STATE for a disk with no GPT; ERR_CORRUPTED for one whose
 * copies are to be mended first; ERR_OUT_OF_RANGE for a run that is not
 * free; ERR_QUOTA_EXCEEDED for a table with no entry left. */
error_t DiskTableAdd(uint8_t seat, uint32_t seating, DiskPartKind kind, uint64_t first, uint64_t blocks,
                     uint16_t* entry);

/* BoxOS's partition in `entry` taken away, its run free. ERR_OUT_OF_RANGE
 * for no partition there; ERR_INVALID_ARGUMENT for one of another kind;
 * ERR_BUSY for one a volume of the house stands on; ERR_INVALID_STATE and
 * ERR_CORRUPTED as DiskTableAdd's. */
error_t DiskTableRemove(uint8_t seat, uint32_t seating, uint16_t entry, const DiskInUse* used);

/* BoxOS's partition in `entry` grown into the room right after it — up to the
 * next partition, or the last block the table may use — to `blocks` long, or
 * all of that room for 0; its length kept a whole number of the volume's own
 * blocks. A volume of the house standing on it takes the ground at its next
 * mount, as a volume takes the ground behind it. ERR_OUT_OF_RANGE for no
 * partition there, no room after it, or a length it already reaches;
 * ERR_INVALID_ARGUMENT for one of another kind; ERR_INVALID_STATE and
 * ERR_CORRUPTED as DiskTableAdd's. *grown (NULL for none) the length it has
 * now. */
error_t DiskTableGrow(uint8_t seat, uint32_t seating, uint16_t entry, uint64_t blocks, uint64_t* grown);

/* Both copies of a GPT written from the one read — the far one at the end of
 * the disk, a table made for a smaller one given the blocks it never knew of:
 * OK, nothing written, when they hold and agree. ERR_INVALID_STATE for a disk
 * with no GPT that holds, or a medium that will not say how far it runs. */
error_t DiskTableMend(uint8_t seat, uint32_t seating);

/* The blocks of the grain on `t`'s medium: a mebibyte, or one physical block
 * when that is larger. */
uint64_t DiskTableGrain(const DiskTable* t);

#endif /* DISK_TABLE_H */
