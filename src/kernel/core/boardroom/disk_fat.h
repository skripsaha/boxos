#ifndef BOARDROOM_DISK_FAT_H
#define BOARDROOM_DISK_FAT_H

#include "ktypes.h"
#include "fat.h"

/*
 * A FAT32 on a run of a seated disk's blocks (src/include/fat.h): the medium
 * a FatVolume reads and writes through. A FAT's sector is its disk's block
 * (UEFI 2.10 §13.3.1: firmware reads an ESP in its medium's block), counted
 * from the run's first; every call asks the seat first, and a disk that left
 * — or another seated since — refuses it.
 */
typedef struct {
    FatMedium medium;
    uint8_t   seat;
    uint32_t  seating;
    uint64_t  first;        /* the run's first sector, in the room's 512-byte sectors */
    uint32_t  per;          /* the room's sectors a block is */
} DiskFatPlace;

/* `p` made the medium of blocks `first` to `first + blocks - 1` of the disk
 * in `seat` — the occupant `seating` — of `block_bytes` a block. */
void DiskFatPlaced(DiskFatPlace* p, uint8_t seat, uint32_t seating, uint64_t first, uint64_t blocks,
                   uint32_t block_bytes);

#endif /* BOARDROOM_DISK_FAT_H */
