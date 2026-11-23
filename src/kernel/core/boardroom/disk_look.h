#ifndef BOARDROOM_DISK_LOOK_H
#define BOARDROOM_DISK_LOOK_H

#include "ktypes.h"
#include "disk_table.h"

/*
 * What a partition of a disk holds (boxos_disks.h DiskHolds), read off its
 * first 4 KiB as util-linux's libblkid reads it — and, of an ESP, the loaders
 * it carries: the directories under its EFI, and whether its removable-media
 * default is laid (UEFI 2.10 §3.5.1, §13.3.1.3). Nothing is written; a read
 * the medium refuses leaves the partition DISK_HOLDS_UNKNOWN.
 */
typedef struct {
    uint8_t  holds;                 /* DiskHolds */
    uint8_t  flags;                 /* DISK_PART_DEFAULT_LOADER, DISK_PART_MORE_LOADERS */
    uint16_t loaders_len;           /* bytes of `loaders`: the names, each NUL-ended */
    char     loaders[DISK_LOADERS_MOST * (DISK_LOADER_NAME_MOST + 1u)];
} DiskLook;

/* Partition `index` of the table `t` read, as it is now, into *look. */
void DiskLookAt(const DiskTable* t, uint32_t index, DiskLook* look);

#endif /* BOARDROOM_DISK_LOOK_H */
