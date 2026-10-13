#ifndef TAGBOOT_H
#define TAGBOOT_H

/*
 * What crosses between the files of TagBoot, and nothing else.
 *
 * The loader is one EFI image linked from more than one file: tagboot.c holds
 * the firmware, the medium, the kernel's load and the jump; tagboot_deed.c
 * takes up what a volume's Deed says. Every other function and variable stays
 * static to its own file, so a name here is a name another file of the loader
 * really uses.
 */

#include "uefi.h"
#include "tagfs_boot.h"

/* tagboot.c: the console, and bytes moved with no library under them. */
void Print(const char *msg);
void PrintDec(uint64_t v);
void MemCopy(void *dst, const void *src, size_t n);

/*
 * tagboot_deed.c: what the Deed said, filled by DeedTakeUp — LAYOUT for a
 * format-v1 book, FRAME for a format-v2 one, never both; BOOT where the Deed
 * carries one naming a kernel. A format-v2 volume has no registry or pool where
 * LAYOUT puts them, so on one BOOT is the only answer to "where is the kernel"
 * until this loader reads its book.
 */
extern VolumeLayout g_layout;
extern VolumeFrame  g_frame;
extern int          g_have_frame;
extern VolumeBoot   g_boot;
extern int          g_have_boot;
extern uint8_t      g_volume_uuid[16];

/*
 * Take up the Deed the probe already read and checked (tagboot.c,
 * FindGroundOn) out of `deed_block`, and say which volume stands on the
 * ground at `ground_start` and how its book is laid out. Anything this loader
 * refuses is said in a line of its own first; the caller stops.
 */
EFI_STATUS DeedTakeUp(const uint8_t *deed_block, uint64_t ground_start);

#endif /* TAGBOOT_H */
