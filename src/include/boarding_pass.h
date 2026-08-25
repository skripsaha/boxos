#ifndef BOARDING_PASS_H
#define BOARDING_PASS_H

#include "ktypes.h"

/*
 * The Boarding Pass — what the loader hands the kernel about its own arrival.
 *
 * boot_info describes the MACHINE: where memory is, where the framebuffer is,
 * where the firmware left its tables. This describes the JOURNEY: which volume
 * this kernel was read out of, which loader read it, and off what medium. Two
 * different kinds of fact, written by different code for different readers, and
 * putting them in one structure is how boot_info's version field came to mean
 * both "which firmware" and "which fields are filled" at the same time.
 *
 * The one it exists for first is the volume. A kernel in long mode cannot ask
 * the firmware which device it was booted from, so the Boardroom has been
 * choosing by a rule — "the removable medium wins" — and printing, honestly,
 * that it was guessing. On a machine carrying BoxOS on an internal disk with
 * any flash drive in a socket, that guess is wrong. The loader is the one part
 * of the system that KNOWS, because it just read the superblock; it simply had
 * nowhere to write the answer down.
 *
 *
 * ‼ WHY STAMPS AND NOT FIELDS
 *
 * A pass carries stamps: each one names what it is, states its own length, and
 * is followed by the next. A reader that meets a stamp it does not recognise
 * steps over it by that length and carries on.
 *
 * That is the whole of the forward compatibility, and it is why there are no
 * reserved fields here and no version ladder to climb. A loader that learns
 * something new adds a stamp; a kernel that has not been taught about it is
 * unaffected. A kernel that wants something the loader is too old to say finds
 * the stamp missing and says so, which is a fact rather than a crash. Neither
 * side has to be upgraded with the other, and neither has to guess what a
 * block of padding was supposed to mean.
 */

/*
 * Where it is. Below one megabyte, in the window the loaders own and the
 * physical allocator never hands out — the same arrangement boot_info has, and
 * for the same reason: the kernel has to be able to read it before it has
 * anything to read it with.
 *
 * stage2's memory map claims 0x0A000 for boot_info, 0x0A200 and 0x0A400 for
 * its two TagFS buffers. This is the next clear page, and the assertions at the
 * bottom of stage2.asm enforce the map rather than describe it.
 */
#define BOARDING_PASS_ADDR    0xA600
#define BOARDING_PASS_BYTES   512

#ifdef BOARDING_PASS_ADDR_FROM_BUILD
_Static_assert(BOARDING_PASS_ADDR == BOARDING_PASS_ADDR_FROM_BUILD,
               "the boarding pass address disagrees with the image build: the "
               "loaders write the block where the Makefile says and the kernel "
               "reads it where this header says, and a boot only finds out "
               "when the pass comes back blank");
#endif
#define BOARDING_PASS_MAGIC   0x53415042u    /* 'B','P','A','S' */
#define BOARDING_PASS_VERSION 1

typedef struct __attribute__((packed)) {
    uint32_t magic;             /* +0  BOARDING_PASS_MAGIC */
    uint16_t version;           /* +4  BOARDING_PASS_VERSION */
    uint16_t header_bytes;      /* +6  where the first stamp starts */
    uint16_t used_bytes;        /* +8  header + every stamp */
    uint16_t capacity;          /* +10 how much room the block has */
    uint16_t count;             /* +12 how many stamps */
    uint16_t reserved;          /* +14 zero */
} BoardingPassHeader;

_Static_assert(sizeof(BoardingPassHeader) == 16, "a boarding pass header is 16 bytes");

/*
 * One stamp. `bytes` counts the payload only; the payload starts immediately
 * after this and the next stamp begins at the next four-byte boundary, so a
 * reader can walk the block without knowing what any of it means.
 */
typedef struct __attribute__((packed)) {
    uint16_t kind;
    uint16_t bytes;
} BoardingStampHeader;

_Static_assert(sizeof(BoardingStampHeader) == 4, "a stamp header is 4 bytes");

/* Kinds. Numbers are permanent: a kind that is retired is never reused, so a
 * new loader and an old kernel can never disagree about what a stamp means. */
#define BOARDING_STAMP_VOLUME  1    /* BoardingVolume */
#define BOARDING_STAMP_MEDIUM  2    /* BoardingMedium */
#define BOARDING_STAMP_LOADER  3    /* BoardingLoader */

/*
 * The volume this kernel was read out of, by the identity the filesystem keeps
 * for itself. Sixteen bytes written when the volume was created, which is the
 * only thing about a medium that survives being moved between machines,
 * sockets and controllers.
 */
typedef struct __attribute__((packed)) {
    uint8_t uuid[16];
} BoardingVolume;

/*
 * How the loader reached it. Not needed to identify the volume — that is what
 * the UUID is for — but it is what turns "the kernel did not find its volume"
 * into an answerable question on a machine whose only diagnostic is a screen.
 */
#define BOARDING_FIRMWARE_BIOS 0
#define BOARDING_FIRMWARE_UEFI 1

typedef struct __attribute__((packed)) {
    uint8_t  firmware;          /* BOARDING_FIRMWARE_* */
    uint8_t  bios_drive;        /* the DL the firmware handed stage1; 0xFF if none */
    uint16_t reserved;
} BoardingMedium;

_Static_assert(sizeof(BoardingMedium) == 4, "a medium stamp is 4 bytes");

/* Which loader wrote this pass. On a board that boots through CSM one week and
 * UEFI the next, "which loader produced this" is a real question and there has
 * been nothing on the screen that answers it. */
typedef struct __attribute__((packed)) {
    char     name[12];          /* NUL-padded, not necessarily NUL-terminated */
    uint16_t major;
    uint16_t minor;
} BoardingLoader;

_Static_assert(sizeof(BoardingLoader) == 16, "a loader stamp is 16 bytes");

#endif /* BOARDING_PASS_H */
