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
 * The bookkeeping lives in the volume's book, which the FRAME stamp says where
 * to find. The Deed is written when the volume is made and read for the rest
 * of its life.
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
 * The volume's own block: the unit every block number in a Deed is counted in
 * — BOOT's and FRAME's alike — and the unit the volume is read and written
 * in.
 *
 * ‼ ONE OWNER. It used to be spelled 4096 in five places: twice in the
 * kernel's headers, once in the UEFI loader's, once in the host tool and once
 * in stage2, held together by comments. The kernel's TAGFS_BLOCK_SIZE, the
 * loader's and the tool's are now these names under their old ones, and
 * stage2 — which cannot include a C header — is held to them by the static
 * assertions beside the prologue's (src/kernel/tagfs/deed/deed.h).
 *
 * GEOMETRY still states the block the volume was laid out in, because a Deed
 * is read by kernels not yet written: one that meets a volume of another block
 * says so and does not mount it.
 */
#define VOLUME_BLOCK_BYTES    4096u
#define VOLUME_BLOCK_SECTORS  8u        /* VOLUME_BLOCK_BYTES / VOLUME_DEED_SECTOR_BYTES */

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

/* Kinds are dense until the first release. From it on a kind that is retired
 * is never reused, so a new mkfs and an old kernel can never disagree about
 * what a stamp means. stage2 reads BOOT by the number written here: the build
 * writes these lines out for it (Makefile, volume_stamps.inc), and an
 * assembler that spelled the kind for itself would be a second owner of it. */
#define VOLUME_STAMP_GEOMETRY  1    /* VolumeGeometry */
#define VOLUME_STAMP_BORN      2    /* VolumeBorn     */
#define VOLUME_STAMP_BOOT      3    /* VolumeBoot     */
#define VOLUME_STAMP_PACKED    4    /* VolumePacked   */
#define VOLUME_STAMP_FRAME     5    /* VolumeFrame — where the volume's book is */

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
 * Where the kernel is, for the loader that has to find it before there is
 * anything to find it with.
 *
 * The kernel is a file inside the volume, so a loader in sixteen-bit real mode
 * would otherwise have to understand the book to reach it. This says
 * it outright, in the volume's own blocks: read from here, this many, this
 * many bytes.
 *
 * It belongs in the Deed rather than in the book because it
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

/*
 * WHAT WAS PUT ON THIS VOLUME, AND OUT OF WHAT.
 *
 * The kernel carries a plate saying which tree it was built from (see
 * buildplate_format.h) — but the kernel is one file of seventy on the medium,
 * and it cannot vouch for the other sixty-nine. A machine boots a SHIPMENT: a
 * kernel, a shell, the utilities, the test binaries. This stamp is the
 * shipment's own half of the pedigree, and it is written by whoever packs the
 * volume rather than by whoever compiles.
 *
 * `shipment` is one number over every file packed, in pack order, contents and
 * all — so a volume carrying yesterday's display.elf beside today's kernel is
 * a volume that says so. `tree` and `verdict` are COPIED OUT OF THE KERNEL'S
 * OWN PLATE as it is packed, which is what ties the two halves together
 * without inventing a second opinion: if the kernel says UNDER TEST, the
 * volume says UNDER TEST, and the machine can print it at mount even when the
 * kernel that would have said so never got far enough to speak.
 *
 * Values for `verdict` are BUILDPLATE_VERDICT_* from buildplate_format.h. The
 * numbers are repeated there rather than included here, because this header is
 * shared with an assembler and a host tool and chooses nothing for its
 * includer.
 */
typedef struct __attribute__((packed)) {
    uint64_t packed_unix;       /* when the volume was packed */
    uint64_t shipment;          /* over every file packed, in pack order */
    uint64_t tree;              /* the kernel plate's tree fingerprint */
    uint32_t file_count;        /* how many files `shipment` spans */
    uint32_t verdict;           /* BUILDPLATE_VERDICT_* */
} VolumePacked;

/* The shipment's number: FNV-1a 64 over every file packed, in pack order —
 * its name and a NUL, its bytes, a NUL — so two files whose contents were
 * swapped are another shipment. One rule for every packer of a volume: the
 * host's (tools/create_tagfs_cargo.c) and the kernel's
 * (tagfs/volume/volume_lay.c). */
#define VOLUME_SHIPMENT_START  1469598103934665603ULL

static inline uint64_t VolumeShipmentFold(uint64_t h, const void *p, uint64_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (uint64_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/*
 * WHERE THE BOOK BEGINS — the stamp that says this volume carries a book, and
 * the two places a reader starts from. A Deed without it is no TagFS volume.
 *
 * `spine_block` is slot 0 of the Spine ring: the slots are single blocks at
 * spine_block + i * spine_stride, i < spine_slots, each the book's own root as
 * of one Tally. A ring rather than one place: the slot being written is the
 * one a power cut tears, and the slot before it can be lost with it — rotten,
 * or torn under a flush the medium lied about. One being written, one under
 * the blow, one that survives both: three, which a maker lays a grain apart,
 * the step the whole volume is aligned to. A reader takes two as the least
 * that is still a ring, and names fewer than three as a volume made by
 * another maker.
 *
 * `data_block` is the first block the allocator may hand out. Every block
 * number the book or a file names is a block OF THE VOLUME — not of a data
 * run — so turning one into a sector is block * 8, once.
 *
 * ‼ WHY THERE IS NO LENGTH HERE. Where the volume ends is already in the
 * prologue: the far copy of this Deed sits in its last block, so the data run
 * ends at tail_sector / VOLUME_BLOCK_SECTORS. A volume that takes the ground
 * behind it moves `sectors` and `tail_sector` (DeedPenSetGround) and nothing
 * here: the map of the blocks is derived from the book and kept nowhere.
 *
 * Everything else a book needs — where its ribbon runs, how big its quires
 * are, the atom it writes in — is a property of the medium and the allocator,
 * read at mount, and not of the volume.
 */
typedef struct __attribute__((packed)) {
    uint64_t data_block;        /* first block of the data run, in volume blocks */
    uint64_t spine_block;       /* Spine slot 0, in volume blocks                */
    uint32_t spine_slots;       /* how many slots the ring has                   */
    uint32_t spine_stride;      /* blocks from one slot to the next              */
} VolumeFrame;
_Static_assert(sizeof(VolumeFrame) == 24, "VolumeFrame is 24 bytes on the medium");

/* The GEOMETRY every maker of this format lays a volume out for — a decision
 * of the format's, not a fact about any medium: its numbers counted in
 * 512-byte sectors, everything of it on a 4096-byte grid (the kernel compares
 * this with what the medium says of itself, and says so when the volume
 * assumed a finer grid than the medium has), the conventional mebibyte grain,
 * and the volume's own block. */
static inline VolumeGeometry VolumeGeometryLaid(void)
{
    VolumeGeometry g;
    g.logical_bytes  = VOLUME_DEED_SECTOR_BYTES;
    g.physical_bytes = VOLUME_BLOCK_BYTES;
    g.grain_bytes    = 1024u * 1024u;
    g.block_bytes    = VOLUME_BLOCK_BYTES;
    return g;
}

/* The ring as a maker of this format lays it down (above). */
#define VOLUME_FRAME_SPINE_SLOTS  3u

/* The FRAME every maker of this format lays — the host's volume maker and the
 * kernel's alike — on ground aligned to `grain` blocks: the Deed's grain, a
 * grain to each Spine slot, and the data run from the grain after the last of
 * them. */
static inline VolumeFrame VolumeFrameLaid(uint32_t grain)
{
    VolumeFrame f;
    f.spine_block  = grain;
    f.spine_stride = grain;
    f.spine_slots  = VOLUME_FRAME_SPINE_SLOTS;
    f.data_block   = (uint64_t)grain * (1u + VOLUME_FRAME_SPINE_SLOTS);
    return f;
}

/*
 * Does a FRAME hold — the one rule every reader of one keeps: the kernel at
 * its format check (tagfs/deed/deed.c, DeedFormatRefused), the UEFI loader
 * before it takes the kernel BOOT names (gangway_deed.c), and volume-read on
 * the host in the same words (tools/volumeread/ondisk.py, VolumeFrameCheck).
 * Here, so the three cannot hold a FRAME to three rules. `frame` and `bytes`
 * are the stamp as the reader's stamp walk found it; `tail_sector` is the
 * prologue's, the far copy's place, whose block is the volume's last. NULL
 * when it holds, or the sentence that says why it does not.
 *
 * A FRAME that holds can still be worth a line, and `note` gets it: one
 * longer than this struct was written by a kernel that knows more than the
 * reader, and a ring of other than VOLUME_FRAME_SPINE_SLOTS slots is a volume
 * made by another maker. NULL when there is nothing to say.
 *
 * The ring's slots are single blocks at spine_block + i * spine_stride. With
 * a stride of at least one they are distinct by construction — unless the
 * ring runs past the last block number and comes round, which is the one way
 * left for two of them to land together or on the Deed, and the one sum here
 * that can overflow. The product cannot: two u32 make less than 2^64.
 *
 * The stamp is read field by field through the packed struct, which every
 * compiler of the three builds reads at any alignment; no library is called.
 */
static inline const char *VolumeFrameCheck(const void *frame, uint16_t bytes,
                                           uint64_t tail_sector, const char **note)
{
    const VolumeFrame *f = (const VolumeFrame *)frame;

    if (note) *note = 0;
    if (!frame || bytes < sizeof(VolumeFrame))
        return "it is shorter than a FRAME";

    uint64_t spine  = f->spine_block, data = f->data_block;
    uint32_t slots  = f->spine_slots, stride = f->spine_stride;

    if (slots < 2u)
        return "its Spine ring has fewer than two slots, which is no ring";
    if (stride == 0u)
        return "its Spine slots are all one block — a ring with no stride";
    if (spine == 0u)
        return "its Spine begins on block 0, which is the Deed's own";

    uint64_t reach = (uint64_t)(slots - 1u) * stride;
    if (spine > ~(uint64_t)0 - reach)
        return "its Spine ring runs past the last block number there is and "
               "comes round onto the blocks before it";
    if (spine + reach >= data)
        return "its Spine ring runs into the data run";
    if (data >= tail_sector / VOLUME_BLOCK_SECTORS)
        return "its data run starts at or past the volume's last block, where "
               "the far copy of the Deed sits";

    if (note) {
        int longer = bytes > sizeof(VolumeFrame);
        int other  = slots != VOLUME_FRAME_SPINE_SLOTS;
        if (longer && other)
            *note = "it is longer than the FRAME this reader knows and its Spine "
                    "ring is not the one this format lays down — written by a "
                    "kernel that knows more, or by another maker";
        else if (longer)
            *note = "it is longer than the FRAME this reader knows — written by "
                    "a kernel that knows more";
        else if (other)
            *note = "its Spine ring is not the one this format lays down — a "
                    "volume made by another maker";
    }
    return 0;
}

#endif /* VOLUME_DEED_H */
