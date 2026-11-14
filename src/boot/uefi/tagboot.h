#ifndef TAGBOOT_H
#define TAGBOOT_H

/*
 * What crosses between the files of TagBoot, and nothing else.
 *
 * The loader is one EFI image linked from more than one file: tagboot.c holds
 * the firmware, the medium, a format-v1 kernel's load and the jump;
 * tagboot_deed.c takes up what a volume's Deed says; tagboot_attest.c is the
 * desk the Lectern (tagboot_lectern*.c, tagboot_succession.c) chooses the
 * kernel of a format-v2 volume at. Every other function and variable stays
 * static to its own file, so a name here is a name another file of the
 * loader really uses — the Lectern's own are tagboot_lectern_book.h's.
 */

#include "uefi.h"
#include "tagfs_boot.h"
#include "tagboot_lectern.h"

/* Where the kernel is placed and jumped into. */
#define KERNEL_LOAD_ADDR 0x100000ULL

/* tagboot.c: the console, and bytes moved with no library under them. */
void Print(const char *msg);
void PrintDec(uint64_t v);
void PrintHex64(uint64_t v);
void MemCopy(void *dst, const void *src, size_t n);

/*
 * tagboot_deed.c: what the Deed said, filled by DeedTakeUp — LAYOUT for a
 * format-v1 book, FRAME for a format-v2 one, never both; BOOT where the Deed
 * carries one naming a kernel — on a format-v2 volume the kernel tried first,
 * before those its book names by role.
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
/* A format-v2 volume's atom (its GEOMETRY, BookAtomOf) in blocks: a Quire of
 * its ribbon begins on one. */
extern uint64_t     g_volume_atom_blocks;

/*
 * Take up the Deed the probe already read and checked (tagboot.c,
 * FindGroundOn) out of `deed_block`, and say which volume stands on the
 * ground at `ground_start` and how its book is laid out. Anything this loader
 * refuses is said in a line of its own first; the caller stops.
 */
EFI_STATUS DeedTakeUp(const uint8_t *deed_block, uint64_t ground_start);

/*
 * tagboot_attest.c: the kernel of a format-v2 volume, chosen by the Succession
 * — BOOT's first, then the kernels its book names by role — read through
 * `read` (tagboot.c's reader of the volume, 512-byte sectors counted from its
 * ground) into pages `bs` gives, and placed at KERNEL_LOAD_ADDR: NULL with its
 * size in *bytes and the end of its pages in *alloc_end — attested, or with
 * nothing to hold it to, said (spec_v2 §16.7 п.4) — or the sentence to stop
 * with when no kernel on the volume may be jumped into, every one tried said
 * with why.
 */
typedef EFI_STATUS (*AttestReader)(uint64_t vlba, uint32_t count_512, void *buffer);
const char *SuccessionTake(EFI_BOOT_SERVICES *bs, AttestReader read, uint64_t *bytes, uint64_t *alloc_end);

#endif /* TAGBOOT_H */
