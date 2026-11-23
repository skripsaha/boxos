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

/* The blocks of the grain on `t`'s medium: a mebibyte, or one physical block
 * when that is larger. */
uint64_t DiskTableGrain(const DiskTable* t);

const char* DiskPartKindName(DiskPartKind kind);

#endif /* DISK_TABLE_H */
