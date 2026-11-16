#ifndef BOXOS_CROSSINGS_H
#define BOXOS_CROSSINGS_H

/*
 * boxos_crossings.h — the crossings: every start Gangway hands a kernel over,
 * kept on the medium it started from, and whether it landed.
 *
 * They live in the front of the medium, in the first mebibyte BoxOS's layout
 * leaves empty past stage2 (the Makefile, "Bootloader layout"): a head 64 KiB
 * in, then CROSSINGS_SLOTS slots of 4 KiB — each 4 KiB-aligned, so a medium of
 * 4096-byte sectors writes a slot as one of its own and nothing beside it.
 * Not the volume's: the book is the person's, and how the machine started is
 * the machine's. The tool that makes the medium lays the head
 * (tools/crossings.py lay); a medium whose front carries none is not written
 * — not a stranger's disk, not zeros, not a front laid out by anyone else.
 *
 * A crossing is written whole into its slot, its own CRC (ISO 3309, Gangway's
 * Crc32 and the kernel's KCrc32) over it: one that does not check out — a
 * write the power cut — is no crossing, and the slot is the next one written.
 * Gangway writes this start's into the slot after the newest, its number one
 * past it, once the panel, if any, is left (gangway_crossings.c); the kernel,
 * told the slot and the number by a Boarding Pass stamp (BOARDING_STAMP_
 * CROSSING), marks it landed when the display takes the screen
 * (src/kernel/core/boarding/crossing.c). CROSSINGS_MISSES_PANEL starts in a
 * row that did not land open the panel at the next by themselves.
 *
 * Includer must provide uint8_t, uint16_t, uint32_t and uint64_t BEFORE
 * including this header.
 */

#define CROSSINGS_HEAD_SECTOR   128u    /* 64 KiB into the medium, 512-byte sectors */
#define CROSSINGS_SLOT_SECTORS  8u
#define CROSSINGS_SLOT_BYTES    4096u
#define CROSSINGS_SLOTS         32u
#define CROSSINGS_FIRST_SLOT    (CROSSINGS_HEAD_SECTOR + CROSSINGS_SLOT_SECTORS)
#define CROSSINGS_END_SECTOR    (CROSSINGS_FIRST_SLOT + CROSSINGS_SLOTS * CROSSINGS_SLOT_SECTORS)
/* The first sector a partition of the medium may begin at: none may lie over
 * the crossings, and BoxOS's own begin at the first mebibyte. */
#define CROSSINGS_FRONT_END     2048u

#define CROSSINGS_MISSES_PANEL  2u

#define CROSSINGS_HEAD_MAGIC    "BOXCROSS"
#define CROSSING_MAGIC          "CROSSING"

_Static_assert(CROSSINGS_SLOT_SECTORS * 512u == CROSSINGS_SLOT_BYTES, "a slot is 4 KiB");
_Static_assert(CROSSINGS_HEAD_SECTOR % CROSSINGS_SLOT_SECTORS == 0, "slots lie on 4 KiB");
_Static_assert(CROSSINGS_END_SECTOR <= CROSSINGS_FRONT_END, "the crossings stay in the front");

typedef struct __attribute__((packed)) {
    char     magic[8];       /* CROSSINGS_HEAD_MAGIC */
    uint32_t size;           /* sizeof(CrossingsHead) */
    uint32_t crc;            /* over the head, this field zero */
    uint32_t slots;          /* CROSSINGS_SLOTS */
    uint32_t slot_sectors;   /* CROSSINGS_SLOT_SECTORS */
    uint64_t first_slot;     /* CROSSINGS_FIRST_SLOT */
} CrossingsHead;

_Static_assert(sizeof(CrossingsHead) == 32, "the head is 32 bytes");

#define CROSSING_KERNEL_HELD       1u   /* the kernel was attested by the book */
#define CROSSING_KERNEL_UNCHECKED  2u   /* nothing to hold it to, nothing showing it broken */

#define CROSSING_PANEL_NONE        0u
#define CROSSING_PANEL_KEY         1u   /* the person held Space */
#define CROSSING_PANEL_MISSES      2u   /* it opened by itself: starts before did not land */

/* Which of what gangway.settings sets the start asked, by the file or the
 * panel — the bits Gangway keeps them by. */
#define CROSSING_ASKED_SCREEN      1u
#define CROSSING_ASKED_STARTUP     2u
#define CROSSING_ASKED_CORES       4u
#define CROSSING_ASKED_KEEP_LOG    8u
#define CROSSING_ASKED_SCENE      16u

#define CROSSING_LANDED            1u

typedef struct __attribute__((packed)) {
    char     magic[8];       /* CROSSING_MAGIC */
    uint32_t size;           /* sizeof(Crossing) */
    uint32_t crc;            /* over the crossing, this field zero */
    uint64_t number;         /* the start's number on this medium, from 1 */
    uint8_t  volume[16];     /* the Deed's uuid of the volume it started from */
    uint16_t year;           /* the firmware's clock as the start began — */
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  clock;          /* — 1 when it told the time, the fields zero when not */
    uint8_t  kernel;         /* CROSSING_KERNEL_* */
    uint8_t  panel;          /* CROSSING_PANEL_* */
    uint8_t  startup;        /* BOARDING_STARTUP_*, as handed on */
    uint8_t  keep_log;       /* 1 asked on */
    uint32_t asked;          /* CROSSING_ASKED_* */
    uint32_t cores;          /* the most started, 0 every one */
    uint32_t screen_width;   /* the mode the kernel was handed, 0 by 0 for none */
    uint32_t screen_height;
    uint32_t misses;         /* starts in a row before this one that did not land */
    uint32_t landed;         /* CROSSING_LANDED, by the kernel */
    uint32_t landed_ms;      /* its first instruction to the display taking the screen */
    uint8_t  scene;          /* 1 asked on */
    uint8_t  reserved[15];
} Crossing;

_Static_assert(sizeof(Crossing) == 96, "a crossing is 96 bytes");

#endif /* BOXOS_CROSSINGS_H */
