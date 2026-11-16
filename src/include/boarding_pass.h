#ifndef BOARDING_PASS_H
#define BOARDING_PASS_H

/*
 * The Boarding Pass — what the loader hands the kernel about its own arrival.
 *
 * boot_info describes the MACHINE: where memory is, where the framebuffer is,
 * where the firmware left its tables. This describes the JOURNEY: which volume
 * this kernel was read out of, which loader read it, and off what medium. Two
 * different kinds of fact, written by different code for different readers.
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
 * That is the whole of the forward compatibility, and it is why there is no
 * version to climb: a pass is known by its magic. A loader that learns
 * something new adds a stamp; a kernel that has not been taught about it is
 * unaffected. A kernel that wants
 * something the loader is too old to say finds the stamp missing and says so,
 * which is a fact rather than a crash. Neither side has to be upgraded with
 * the other, and neither has to guess what a block of padding was supposed to
 * mean.
 *
 *
 * ‼ THE HEADER
 *
 * From the first release on, the twelve bytes below keep their offsets, widths
 * and meanings for as long as there is a boarding pass, and a loader that needs
 * to say something new says it in a stamp. `header_bytes` is what a reader
 * uses to find the first stamp, so a later loader may make the header LONGER
 * without any reader having to be taught how: a reader that can find the
 * stamps can read the stamps it knows.
 *
 * Every offset of it, and of the stamps, is spelled a second time in
 * stage2.asm, which writes the pass raw; the assertions below hold them.
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

typedef struct __attribute__((packed)) {
    uint32_t magic;             /* +0  BOARDING_PASS_MAGIC */
    uint16_t header_bytes;      /* +4  where the first stamp starts */
    uint16_t used_bytes;        /* +6  header + every stamp */
    uint16_t capacity;          /* +8  how much room the block has */
    uint16_t count;             /* +10 how many stamps */
} BoardingPassHeader;

_Static_assert(sizeof(BoardingPassHeader) == 12,
               "a boarding pass header is 12 bytes — stage2 BOARDING_HDR_BYTES, "
               "and a four-byte boundary the first stamp starts on");
_Static_assert(__builtin_offsetof(BoardingPassHeader, header_bytes) == 4,  "stage2 writes header_bytes at +4");
_Static_assert(__builtin_offsetof(BoardingPassHeader, used_bytes)   == 6,  "stage2 writes used_bytes at +6");
_Static_assert(__builtin_offsetof(BoardingPassHeader, capacity)     == 8,  "stage2 writes capacity at +8");
_Static_assert(__builtin_offsetof(BoardingPassHeader, count)        == 10, "stage2 writes count at +10");

/*
 * One stamp. `bytes` counts the payload only; the payload starts immediately
 * after this and the next stamp begins at the next four-byte boundary, so a
 * reader can walk the block without knowing what any of it means.
 */
typedef struct __attribute__((packed)) {
    uint16_t kind;
    uint16_t bytes;
} BoardingStampHeader;

_Static_assert(sizeof(BoardingStampHeader) == 4, "a stamp header is 4 bytes — stage2 BOARDING_STAMP_HDR");

/* Kinds. Dense until the first release; from it on a kind that is retired is
 * never reused, so a new loader and an old kernel can never disagree about
 * what a stamp means. */
#define BOARDING_STAMP_VOLUME  1    /* BoardingVolume */
#define BOARDING_STAMP_MEDIUM  2    /* BoardingMedium */
#define BOARDING_STAMP_LOADER  3    /* BoardingLoader */
#define BOARDING_STAMP_SEAL    4    /* BoardingSeal — always the last one */
#define BOARDING_STAMP_JOURNAL 5    /* BoardingJournal */
#define BOARDING_STAMP_MOMENTS 6    /* BoardingMoment, as many as were marked */
#define BOARDING_STAMP_STARTUP  7   /* BoardingStartup */
#define BOARDING_STAMP_CORES    8   /* BoardingCores */
#define BOARDING_STAMP_KEEP_LOG 9   /* BoardingKeepLog */

/*
 * The volume this kernel was read out of, by the identity the filesystem keeps
 * for itself. Sixteen bytes written when the volume was created, which is the
 * only thing about a medium that survives being moved between machines,
 * sockets and controllers.
 */
typedef struct __attribute__((packed)) {
    uint8_t uuid[16];
} BoardingVolume;

_Static_assert(sizeof(BoardingVolume) == 16, "a volume stamp is 16 bytes — stage2 BOARDING_VOLUME_BYTES");

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

_Static_assert(sizeof(BoardingMedium) == 4, "a medium stamp is 4 bytes — stage2 BOARDING_MEDIUM_BYTES");
_Static_assert(__builtin_offsetof(BoardingMedium, bios_drive) == 1, "stage2 writes bios_drive after firmware");

/* Which loader wrote this pass. On a board that boots through CSM one week and
 * UEFI the next, "which loader produced this" is a real question and there has
 * been nothing on the screen that answers it. */
typedef struct __attribute__((packed)) {
    char name[12];              /* NUL-padded, not necessarily NUL-terminated */
} BoardingLoader;

_Static_assert(sizeof(BoardingLoader) == 12, "a loader stamp is 12 bytes — stage2 BOARDING_LOADER_BYTES");

/*
 * The seal — what says the pass arrived the way the loader wrote it.
 *
 * Everything else on the block is a fact the loader states. This is the one
 * that says the block is still the block: a CRC-32 (ISO 3309, the sum GPT and
 * the Deed are already specified in) over every byte AHEAD of this stamp's
 * payload — the header, every stamp before it, and this stamp's own four-byte
 * kind/length pair. Nothing sums itself, so there is no hole to arrange.
 *
 * Which makes the seal the LAST stamp, always, and the rule is worth stating
 * rather than inferring: the loader has to know the final header before it can
 * sum it, so `used_bytes` and `count` are written to include the seal and then
 * the sum is taken. A reader finds the seal by walking, and what it walked is
 * exactly what the sum covers.
 *
 * ‼ IT IS NOT A SIGNATURE. It catches a block that rotted, was half-written
 * by a loader that died, or was walked over by firmware between the write and
 * ExitBootServices — which is the case it was added for, and which is real:
 * the loader writes this block into low memory it must ask the firmware to set
 * aside, and a firmware that hands those pages to something else instead
 * leaves a pass that still carries a valid magic. It stops nobody who means
 * harm, and a kernel that says otherwise would be lying.
 *
 * A pass with no seal is not a broken pass. Loaders older than this stamp
 * wrote none, and a kernel that meets one believes it and SAYS it could not
 * check — which is a fact, and the alternative is refusing to boot a machine
 * whose loader is simply older than its kernel.
 */
typedef struct __attribute__((packed)) {
    uint32_t crc32;
} BoardingSeal;

_Static_assert(sizeof(BoardingSeal) == 4, "a seal stamp is 4 bytes — stage2 BOARDING_SEAL_BYTES");

/*
 * The loader's journal — every line it said, kept instead of printed.
 *
 * A loader that boots quietly still has things to say, and the place they are
 * worth reading is the kernel's log, beside everything that follows them. So
 * the loader keeps its lines in its own memory and names it here; the kernel
 * puts them into its log ring before it manages memory at all
 * (BoardingPassTake), which is what makes the loader's own pages a safe place
 * for them: nothing can be handed those pages before they are read. Below
 * 4 GB, in the loader's identity map. The text is as said, '\n' ending a
 * line; `lost` counts what was said past the room the loader had.
 */
typedef struct __attribute__((packed)) {
    uint64_t addr;              /* physical address of the text */
    uint32_t bytes;             /* how much of it there is */
    uint32_t lost;              /* bytes said past its room, not kept */
} BoardingJournal;

_Static_assert(sizeof(BoardingJournal) == 16, "a journal stamp is 16 bytes");

/*
 * One moment of the loader's journey: a step, in the loader's own word, and
 * the processor's time-stamp counter as the step was reached. The stamp holds
 * as many as were marked, in the order they were — its length says how many.
 * The step is a word rather than a number so that a loader which learns to
 * mark a new step needs no kernel taught about it: whoever shows the journey
 * shows the words it was given.
 */
#define BOARDING_MOMENT_WORD 8

typedef struct __attribute__((packed)) {
    char     what[BOARDING_MOMENT_WORD];    /* NUL-padded, not necessarily NUL-terminated */
    uint64_t tsc;
} BoardingMoment;

_Static_assert(sizeof(BoardingMoment) == 16, "a moment is 16 bytes — stage2 BOARDING_MOMENT_BYTES");

/*
 * What the person asked of the kernel, in gangway.settings
 * (boxos_gangway_settings.h) or on Gangway's panel for one start: a stamp for
 * each setting the file or the panel sets, written by Gangway — which of the
 * two, its journal says. Where there is none — a setting left alone, a BIOS
 * boot, whose stage2 reads no settings, or a loader older than these — the
 * kernel goes its own way: the screen quiet, every core started, and what it
 * says kept through a reset only in a kernel built to (PRINTTOFILE).
 *
 * STARTUP: what the screen shows while the machine starts. QUIET, the screen
 * as the firmware left it until the display takes it, the Emergency Screen
 * woken by an error; ALL, the Emergency Screen from the kernel's first line.
 */
#define BOARDING_STARTUP_QUIET 0u
#define BOARDING_STARTUP_ALL   1u

typedef struct __attribute__((packed)) {
    uint8_t show;               /* BOARDING_STARTUP_* */
    uint8_t reserved[3];
} BoardingStartup;

_Static_assert(sizeof(BoardingStartup) == 4, "a startup stamp is 4 bytes");

/* CORES: the most processors the kernel starts, the one it runs on first
 * among them — 0 for every one the machine has. */
typedef struct __attribute__((packed)) {
    uint32_t most;
} BoardingCores;

_Static_assert(sizeof(BoardingCores) == 4, "a cores stamp is 4 bytes");

/* KEEP_LOG: whether what the machine says is kept through a reset — the
 * carry-over window (klib_logring.h) — for lastsaid to read after it. */
typedef struct __attribute__((packed)) {
    uint8_t keep;               /* 1 kept, 0 not */
    uint8_t reserved[3];
} BoardingKeepLog;

_Static_assert(sizeof(BoardingKeepLog) == 4, "a keep-log stamp is 4 bytes");

#endif /* BOARDING_PASS_H */
