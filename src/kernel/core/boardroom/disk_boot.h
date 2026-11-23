#ifndef BOARDROOM_DISK_BOOT_H
#define BOARDROOM_DISK_BOOT_H

#include "ktypes.h"
#include "error.h"

/*
 * BoxOS made to start from a disk's ESP (disk_boot.c): the loader put on the
 * ESP under BoxOS's own directory (boxos_esp.h) — and as the removable-media
 * default on an ESP of BoxOS's own alone, never on one another system shares
 * — and the firmware given a boot option for it, naming the volume it boots.
 */

typedef struct {
    uint8_t        seat;
    uint32_t       seating;
    uint16_t       entry;           /* the ESP's place in the disk's GPT */
    const uint8_t* loader;          /* the loader's bytes, the volume's own gangway.efi */
    uint32_t       loader_bytes;
    uint8_t        uuid[16];        /* the volume it boots */
    bool           first;           /* the boot option first in BootOrder */
} DiskBootAsk;

typedef struct {
    bool     fallback;              /* the removable-media default written: the ESP is BoxOS's own */
    bool     option;                /* the firmware given a boot option */
    uint16_t number;                /* its Boot#### */
} DiskBootSaid;

/*
 * OK, and what was done in *said; or ERR_OBJECT_NOT_FOUND for a seat that is
 * empty or holds another disk since; ERR_OUT_OF_RANGE for no partition in
 * that entry; ERR_INVALID_ARGUMENT for one that is no ESP; ERR_INVALID_STATE
 * for a disk that is not a GPT; ERR_CORRUPTED for an ESP whose FAT32 does not
 * read; ERR_TAGFS_NO_SPACE for one with no room; ERR_PERMISSION_DENIED,
 * ERR_WRITE_FAILED, ERR_DEVICE_NOT_READY for a medium that refused or left.
 * A firmware with no runtime services (a BIOS start) is given no option, and
 * the answer is OK with said->option false.
 */
error_t DiskBootLay(const DiskBootAsk* a, DiskBootSaid* said);

#endif /* BOARDROOM_DISK_BOOT_H */
