#ifndef VOLUME_DEED_H
#define VOLUME_DEED_H

/*
 * The Deed — a volume's title to the ground it stands on.
 *
 * It states four things and nothing else: WHICH volume this is, HOW FAR it
 * runs, WHAT GEOMETRY it was laid out for, and WHERE its parts are. All four
 * are settled when the volume is made and never change again, which is what
 * lets a Deed be written once, copied to the far end, and trusted.
 *
 *
 * ‼ WHAT THIS REPLACES, AND WHY
 *
 * The superblock it supersedes lived at sector 1034 — an absolute address,
 * counted from the first sector of the whole medium, spelled out in four
 * separate files (the kernel twice, the UEFI loader, and the host mkfs) with
 * nothing but comments holding them in agreement. A volume could therefore
 * exist in exactly one place on any device, and moving it was not a thing that
 * could be done.
 *
 * It was also 512 fixed bytes with a `reserved[404]` at the end, which four
 * subsystems had already carved up between them by comment: boot hints at
 * [0..15], a CoW manifest at [16..23], a CRC sentinel at [399], the CRC at
 * [400..403]. And stage2 read the volume's identity out of it by a byte offset
 * spelled in assembly, because an assembler cannot ask a C structure where a
 * field is — so the layout was frozen by an assembler.
 *
 * A "reserved" array is a format change waiting to be discovered. This carries
 * STAMPS instead, the same way the Boarding Pass does one layer up: each names
 * what it is, states its own length, and is followed by the next. A reader that
 * meets a stamp it does not know steps over it and carries on. That is the
 * whole of the forward compatibility, and it is why there is no version ladder
 * here and no room set aside for later — later is a new stamp.
 *
 *
 * ‼ NOTHING HERE IS AN ABSOLUTE ADDRESS
 *
 * Every number in a Deed is a length, or an offset from the start of the
 * volume's own ground. Where that ground begins is not the volume's business:
 * it is discovered — from a partition entry, or from the Boarding Pass, or
 * because the medium is ours entirely — and handed to the volume.
 *
 * That is the difference between a volume that lives somewhere and a volume
 * that lives at 1034. Move the partition and it still mounts; write the image
 * to a different medium and it still mounts; give it a bigger partition and it
 * says so rather than reading past its own end.
 *
 *
 * ‼ WHAT IS NOT IN A DEED
 *
 * Free-block counts, the next file id, the next tag id, how many files there
 * are, when it was last written. All of that changes every time somebody
 * writes a byte, and the old superblock kept it beside the volume's identity —
 * so the identity was rewritten, and its checksum recomputed, on every write.
 * A title deed that is rewritten continuously is not a title deed, and copying
 * it to the far end protects nothing when both copies are dirtied together.
 *
 * The bookkeeping lives in its own block inside the volume, which the layout
 * stamp points at. The Deed is written when the volume is made and read for
 * the rest of its life.
 *
 *
 * ‼ TWO COPIES, AT OPPOSITE ENDS
 *
 * The head sits at offset zero of the volume's ground. The tail sits at the
 * far end, at the offset the head states.
 *
 * The superblock this replaces kept its backup one sector after the primary —
 * 1034 and 1035 — with the DiskBook's own superblock at 1036. A 4096-byte
 * physical block spans sectors 1032 to 1039, so all three lived inside ONE
 * physical block of the medium. A single failed erase block took the volume,
 * its backup, and the journal's head together. A backup exists to survive
 * localised damage, and that one survived everything except the case it was
 * for.
 *
 * A tail copy also makes the volume able to check its own extent: knowing
 * where it starts and how far it claims to run, finding its Deed at the far
 * end proves the whole of it is present. An image copied short mounts happily
 * today and reads into ground that is not there.
 */

/*
 * Integer types come from the includer, as everywhere in src/include:
 *   Kernel:    #include "ktypes.h"
 *   Host tool: #include <stdint.h>
 *
 * This header pulls in neither. The kernel defines its own fixed-width types
 * and a <stdint.h> underneath them collides with every one; the host tool has
 * no ktypes.h to find. A shared format header that chooses for its includer
 * can only be right in one of the two places it is shared between.
 */

/* 'B','O','X','D','E','E','D', and a NUL so a hex dump reads as words. */
#define VOLUME_DEED_MAGIC_0  'B'
#define VOLUME_DEED_MAGIC_1  'O'
#define VOLUME_DEED_MAGIC_2  'X'
#define VOLUME_DEED_MAGIC_3  'D'
#define VOLUME_DEED_MAGIC_4  'E'
#define VOLUME_DEED_MAGIC_5  'E'
#define VOLUME_DEED_MAGIC_6  'D'
#define VOLUME_DEED_MAGIC_7  '\0'

/* Which of the two copies this is. A tail read as though it were a head is a
 * misdirected read, not a valid Deed, and saying so costs one byte. */
#define VOLUME_DEED_ROLE_HEAD  1u
#define VOLUME_DEED_ROLE_TAIL  2u

/* The sector everything in a Deed is counted in. Not the medium's block size —
 * a medium is addressed in whatever it likes and the Boardroom translates, so
 * a Deed written on a 512-byte device is the same Deed on a 4Kn one. */
#define VOLUME_DEED_SECTOR_BYTES  512u

/*
 * The prologue: the only part of a Deed at a known offset, and the only part
 * an assembler has to be told about. Kept small, and kept forever.
 *
 * `stamp_bytes` counts the stamps that follow this struct. `crc32` covers the
 * prologue with its own field zeroed, followed by exactly `stamp_bytes` of
 * stamps — so a Deed is checked in one pass without knowing what any stamp
 * means.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic[8];
    uint16_t prologue_bytes;    /* where the first stamp starts */
    uint16_t stamp_bytes;       /* how many bytes of stamps follow */
    uint32_t crc32;             /* prologue (this field zeroed) + stamps */
    uint8_t  uuid[16];          /* which volume this is */
    uint64_t sectors;           /* how far the volume's ground runs */
    uint64_t tail_sector;       /* offset from the start to the tail copy */
    uint32_t role;              /* VOLUME_DEED_ROLE_* */
} VolumeDeed;

/*
 * One stamp. `bytes` counts the payload only; the payload starts immediately
 * after this header and the next stamp begins at the next four-byte boundary,
 * so a reader walks the whole Deed without understanding any of it.
 *
 * Deliberately the same shape as BoardingStampHeader. Two stamp walkers of
 * different shapes in one system is one walker too many.
 */
typedef struct __attribute__((packed)) {
    uint16_t kind;
    uint16_t bytes;
} VolumeStamp;

/* Kinds are permanent. A kind that is retired is never reused, so a new mkfs
 * and an old kernel can never disagree about what a stamp means. */
#define VOLUME_STAMP_GEOMETRY  1    /* VolumeGeometry */
#define VOLUME_STAMP_LAYOUT    2    /* VolumeLayout   */
#define VOLUME_STAMP_BORN      3    /* VolumeBorn     */
#define VOLUME_STAMP_BOOT      4    /* VolumeBoot     */

/*
 * What the volume was laid out for — as opposed to what it is sitting on.
 *
 * `logical_bytes` is the sector size its numbers are counted in.
 * `physical_bytes` is the block size it ASSUMED the medium was built from, and
 * aligned itself to. Almost every flash device is addressed in 512-byte blocks
 * and built from 4096-byte ones; a volume laid out on the 512 grid puts every
 * one of its 4 KiB blocks across two physical ones, and each metadata write
 * costs the device a read, a patch and a write instead of a write.
 *
 * Stating it is what lets the kernel compare it against what the medium says
 * of itself (SBC-4 READ CAPACITY(16) tells us, and now we ask). Three answers,
 * all of them useful: they agree; the volume assumed a coarser grid than the
 * medium has, which is harmless; or the volume assumed a finer one, which
 * works and costs double, and the machine says so instead of wearing out
 * quietly.
 *
 * `grain_bytes` is what the whole volume was aligned to when it was made — the
 * conventional 1 MiB covers 4 KiB and 8 KiB blocks and the erase blocks of
 * every NAND part in use.
 */
typedef struct __attribute__((packed)) {
    uint32_t logical_bytes;
    uint32_t physical_bytes;
    uint32_t grain_bytes;
    uint32_t block_bytes;       /* the volume's own block, 4096 today */
} VolumeGeometry;

/*
 * Where the volume's parts are, in ITS blocks, counted from ITS start.
 *
 * `state_block` is the one that changes: free counts, next ids, how many files,
 * when it was last written. It is named here and kept out of the Deed for the
 * reason at the top of this file — and there are `state_blocks` of it, because
 * a record that is rewritten continuously is the one record that will be found
 * half-written. Two copies, written alternately, each stating which is newer:
 * the older one is intact by construction while the newer one is being made.
 *
 * `data_block` is where file contents start and `data_blocks` is how many
 * there are. Everything the filesystem allocates is counted inside that run,
 * so a block number in a file's metadata means the same thing forever, no
 * matter what is added in front of the data or left spare behind it.
 */
typedef struct __attribute__((packed)) {
    uint64_t total_blocks;      /* the whole volume, Deed and all */
    uint32_t state_block;
    uint32_t state_blocks;
    uint32_t tag_registry_block;
    uint32_t tag_registry_blocks;
    uint32_t file_table_block;
    uint32_t file_table_blocks;
    uint32_t metadata_pool_block;
    uint32_t metadata_pool_blocks;
    uint32_t block_bitmap_block;
    uint32_t block_bitmap_blocks;
    uint32_t disk_book_block;
    uint32_t disk_book_blocks;
    uint32_t data_block;        /* first block that holds file contents */
    uint32_t data_blocks;       /* how many, and the bitmap covers exactly these */
} VolumeLayout;

/*
 * Where the kernel is, for the loader that has to find it before there is
 * anything to find it with.
 *
 * The kernel is a file inside the volume, so a loader in sixteen-bit real mode
 * would otherwise have to understand the metadata pool to reach it. This says
 * it outright, in the volume's own blocks: read from here, this many, this
 * many bytes.
 *
 * It belongs in the Deed rather than in the bookkeeping block because it
 * changes only when the kernel is replaced, which is an act of installation
 * and not a write — and because the loader has the Deed in front of it already
 * and nothing else yet.
 */
typedef struct __attribute__((packed)) {
    uint32_t kernel_block;
    uint32_t kernel_blocks;
    uint32_t kernel_bytes;
    uint32_t reserved;          /* padding to eight; growth is a new stamp */
} VolumeBoot;

/* When it was made, and by what. Not needed to mount; needed to answer "which
 * of these two sticks is the one I wrote on Tuesday". */
typedef struct __attribute__((packed)) {
    uint64_t created_unix;
    char     maker[16];         /* NUL-padded, not necessarily terminated */
} VolumeBorn;

#endif /* VOLUME_DEED_H */
