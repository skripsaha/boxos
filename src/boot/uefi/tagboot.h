#ifndef TAGBOOT_H
#define TAGBOOT_H

/*
 * What crosses between the files of TagBoot, and nothing else.
 *
 * The loader is one EFI image linked from more than one file: tagboot.c holds
 * the firmware, the medium, the kernel's load and the jump; tagboot_deed.c
 * takes up what a volume's Deed says; tagboot_attest.c holds the kernel of a
 * format-v2 volume to its book, read by tagboot_lectern.c. Every other
 * function and variable stays static to its own file, so a name here is a
 * name another file of the loader really uses.
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
/* The block the far copy of a format-v2 volume's Deed sits in: its data run,
 * and every block its book may name, end there. */
extern uint64_t     g_volume_end;

/*
 * Take up the Deed the probe already read and checked (tagboot.c,
 * FindGroundOn) out of `deed_block`, and say which volume stands on the
 * ground at `ground_start` and how its book is laid out. Anything this loader
 * refuses is said in a line of its own first; the caller stops.
 */
EFI_STATUS DeedTakeUp(const uint8_t *deed_block, uint64_t ground_start);

/*
 * tagboot_attest.c: the kernel BOOT names on a format-v2 volume — `blocks`
 * blocks of the volume from `first`, loaded at `image` — held to the digests
 * its book keeps of them, read through `read` (tagboot.c's reader of the
 * volume, 512-byte sectors counted from its ground) into pages `bs` gives.
 * What the book says is said in TagBoot's lines; the answer is the sentence to
 * stop with when a block does not match, NULL when the kernel may be jumped
 * into — attested, or with nothing to hold it to, said (spec_v2 §16.7 п.4).
 */
typedef EFI_STATUS (*AttestReader)(uint64_t vlba, uint32_t count_512, void *buffer);
const char *AttestBootKernel(EFI_BOOT_SERVICES *bs, AttestReader read, uint64_t first, uint64_t blocks,
                             const uint8_t *image);

#endif /* TAGBOOT_H */
