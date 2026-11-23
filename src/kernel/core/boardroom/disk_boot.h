#ifndef BOARDROOM_DISK_BOOT_H
#define BOARDROOM_DISK_BOOT_H

#include "ktypes.h"
#include "error.h"

/*
 * BoxOS made to start from a disk's ESP (disk_boot.c): the loader put on the
 * ESP under BoxOS's own directory (boxos_esp.h) — and as the removable-media
 * default on an ESP of BoxOS's own alone, never on one another system shares
 * — and the firmware given a boot option for it, naming the volume it boots.
 * And through a BIOS too, when the disk has a BIOS boot partition: stage2 at
 * its head and stage1's code over the protective MBR (boxos_bios_boot.h).
 */

typedef struct {
    uint8_t        seat;
    uint32_t       seating;
    uint16_t       entry;           /* the ESP's place in the disk's GPT */
    const uint8_t* loader;          /* the loader's bytes, the volume's own gangway.efi */
    uint32_t       loader_bytes;
    uint8_t        uuid[16];        /* the volume it boots */
    bool           first;           /* the boot option first in BootOrder */
    const uint8_t* stage1;          /* the BIOS loaders the volume keeps — NULL for none */
    uint32_t       stage1_bytes;
    const uint8_t* stage2;
    uint32_t       stage2_bytes;
} DiskBootAsk;

typedef struct {
    bool     fallback;              /* the removable-media default written: the ESP is BoxOS's own */
    bool     option;                /* the firmware given a boot option */
    uint16_t number;                /* its Boot#### */
    bool     bios;                  /* the BIOS loaders laid: a BIOS starts it too */
} DiskBootSaid;

/*
 * OK, and what was done in *said; or ERR_OBJECT_NOT_FOUND for a seat that is
 * empty or holds another disk since; ERR_OUT_OF_RANGE for no partition in
 * that entry; ERR_INVALID_ARGUMENT for one that is no ESP; ERR_INVALID_STATE
 * for a disk that is not a GPT; ERR_CORRUPTED for an ESP whose FAT32 does not
 * read; ERR_TAGFS_NO_SPACE for one with no room; ERR_PERMISSION_DENIED,
 * ERR_WRITE_FAILED, ERR_DEVICE_NOT_READY for a medium that refused or left.
 * A firmware with no runtime services (a BIOS start) is given no option, and
 * the answer is OK with said->option false. A disk with no BIOS boot
 * partition, or a volume that keeps no BIOS loaders, is given none of them,
 * said->bios false; ERR_TAGFS_NO_SPACE for a BIOS boot partition too small
 * for stage2, ERR_INVALID_STATE for a sector 0 that is no protective MBR.
 */
error_t DiskBootLay(const DiskBootAsk* a, DiskBootSaid* said);

/*
 * BoxOS made to start from that disk no more — what removing BoxOS from a disk
 * begins with: the boot sector's code, when it is BoxOS's — `stage1`, the
 * volume's — taken off sector 0, the table kept; every boot option of the
 * firmware naming BoxOS's loader on the ESP in `entry`; the ESP's
 * removable-media default when it is BoxOS's own loader, byte for byte; and
 * last every file under BoxOS's own directory there and the directory with
 * them — a cut leaves that directory standing, so asked again it takes up
 * what is left. Another system's loader, default or code is never touched.
 */
typedef struct {
    uint8_t        seat;
    uint32_t       seating;
    uint16_t       entry;           /* the ESP */
    const uint8_t* stage1;          /* BoxOS's boot sector, to know its code by; NULL for none */
    uint32_t       stage1_bytes;
} DiskUnbootAsk;
typedef struct {
    uint32_t options;               /* boot options taken away */
    bool     loader;                /* BoxOS's directory taken away */
    bool     fallback;              /* the default taken away: it was BoxOS's */
    bool     bios;                  /* BoxOS's code taken off sector 0 */
} DiskUnbootSaid;
/* OK, and what was done in *said; the refusals DiskBootLay's. */
error_t DiskBootTakeAway(const DiskUnbootAsk* a, DiskUnbootSaid* said);

#endif /* BOARDROOM_DISK_BOOT_H */
