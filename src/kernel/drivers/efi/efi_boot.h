#ifndef EFI_BOOT_H
#define EFI_BOOT_H

#include "ktypes.h"
#include "error.h"

/*
 * A boot option the firmware's boot manager starts (UEFI 2.10 §3.1.3): a file
 * on a partition of a GPT disk, named by the short-form device path that
 * begins with the Hard Drive media node — §3.1.2 requires a boot manager to
 * boot one, finding the partition by its GUID wherever the disk is attached —
 * then the File Path node and the End node (§10.3.5.1, §10.3.5.4, §10.3.1).
 * The option carries data the image it starts is handed as its LoadOptions.
 */

/* The partition a boot option's file lies on: its place in its GPT from 1,
 * its first block and length in its disk's logical blocks, and its own GUID
 * as its entry holds it. */
typedef struct {
    uint32_t number;
    uint64_t first;
    uint64_t blocks;
    uint8_t  unique[16];
} EfiPartition;

/*
 * Boot#### written — the number of an option of this firmware that names the
 * same file on the same partition already, else the first number no option
 * has — active, described by `description` (ASCII), naming `path` ('/'
 * parted, from the partition's root) on `part`, carrying `data` (`data_bytes`,
 * NULL for none); and the number put in BootOrder first, or last when it is
 * not there. *number the option's. OK; ERR_UNSUPPORTED with no runtime
 * services to ask (a BIOS start); ERR_QUOTA_EXCEEDED for no number free;
 * ERR_NO_MEMORY; ERR_WRITE_FAILED when the firmware refused a write — what it
 * took before kept.
 */
error_t EfiBootOptionPut(const char* description, const EfiPartition* part, const char* path, const void* data,
                         uint32_t data_bytes, bool first, uint16_t* number);

/* Every Boot#### of this firmware that names `path` on `part` deleted, and
 * its number taken out of BootOrder; *removed (NULL for none) how many. OK —
 * none named it is OK too; ERR_UNSUPPORTED with no runtime services to ask (a
 * BIOS start); ERR_WRITE_FAILED when the firmware refused a write — what it
 * took before kept. */
error_t EfiBootOptionsTakeAway(const EfiPartition* part, const char* path, uint32_t* removed);

#endif /* EFI_BOOT_H */
