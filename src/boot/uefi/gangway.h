#ifndef GANGWAY_H
#define GANGWAY_H

/*
 * What crosses between the files of Gangway, and nothing else.
 *
 * The loader is one EFI image linked from more than one file: gangway.c holds
 * the firmware, the medium and the jump; gangway_deed.c takes up what a
 * volume's Deed says; gangway_attest.c is the desk the Lectern
 * (gangway_lectern*.c, gangway_succession.c) chooses the volume's kernel at.
 * Every other function and variable stays static to its own file, so a name
 * here is a name another file of the loader really uses — the Lectern's own
 * are gangway_lectern_book.h's.
 */

#include "uefi.h"
/* The volume's own format, shared with the kernel and the tool that writes it
 * — the text they read it by, not a copy of it. <stdint.h> is not available
 * here; uefi.h defines the fixed-width types these headers take from their
 * includer. The book's (its Spine, ribbon and digests) come in with the
 * Lectern's header. */
#include "volume_deed.h"
#include "gangway_lectern.h"

/* Where the kernel is placed and jumped into. */
#define KERNEL_LOAD_ADDR 0x100000ULL

/* gangway.c: the console, and bytes moved with no library under them. */
void Print(const char *msg);
void PrintDec(uint64_t v);
void PrintHex64(uint64_t v);
void MemCopy(void *dst, const void *src, size_t n);

/*
 * gangway_deed.c: what the Deed said, filled by DeedTakeUp — the FRAME its
 * book is laid out by; BOOT where the Deed carries one naming a kernel, the
 * kernel tried first, before those its book names by role.
 */
extern VolumeFrame  g_frame;
extern VolumeBoot   g_boot;
extern int          g_have_boot;
extern uint8_t      g_volume_uuid[16];
/* The block the far copy of the volume's Deed sits in: its data run, and
 * every block its book may name, end there. */
extern uint64_t     g_volume_end;
/* The volume's atom (its GEOMETRY, BookAtomOf) in blocks: a Quire of its
 * ribbon begins on one. */
extern uint64_t     g_volume_atom_blocks;

/*
 * Take up the Deed the probe read and checked (gangway.c, FindGroundOn) out
 * of `deed_block`: EFI_SUCCESS when the volume on the ground at `ground_start`
 * is a book this loader boots, said in a line of its own; else why it is not
 * — a Deed with no FRAME, or one whose FRAME, block or BOOT does not hold — is
 * said with the line left open, and the caller ends it naming the medium it
 * passes over (gangway.c, BlockIoProbeVolume).
 */
EFI_STATUS DeedTakeUp(const uint8_t *deed_block, uint64_t ground_start);

/*
 * gangway_attest.c: the volume's kernel, chosen by the Succession
 * — BOOT's first, then the kernels its book names by role — read through
 * `read` (gangway.c's reader of the volume, 512-byte sectors counted from its
 * ground) into pages `bs` gives, and placed at KERNEL_LOAD_ADDR: NULL with its
 * size in *bytes and the end of its pages in *alloc_end — attested, or with
 * nothing to hold it to, said — or the sentence to stop
 * with when no kernel on the volume may be jumped into, every one tried said
 * with why.
 */
typedef EFI_STATUS (*AttestReader)(uint64_t vlba, uint32_t count_512, void *buffer);
const char *SuccessionTake(EFI_BOOT_SERVICES *bs, AttestReader read, uint64_t *bytes, uint64_t *alloc_end);

#endif /* GANGWAY_H */
