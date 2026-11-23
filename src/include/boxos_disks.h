#ifndef BOXOS_DISKS_H
#define BOXOS_DISKS_H

/*
 * The disks of the machine as the kernel (boardroom/disk_table.h), Gangway and
 * the programs (boxlib disk.h) all speak of them: the kinds of partition a
 * table names, the types that mark them, and the record hw.disks.list answers
 * with. The includer brings the integer types, as every header here.
 *
 * A type GUID is written in the order a GPT stores it — its first three fields
 * little-endian — which is the order it lies in on the medium, and so the
 * order to compare in.
 */

/* What a partition is, as a person is told it. */
typedef enum {
    DISK_PART_OTHER = 0,          /* a type nobody here knows */
    DISK_PART_BOXOS,              /* the ground a BoxOS volume stands on */
    DISK_PART_ESP,                /* the EFI system partition: the firmware's loaders */
    DISK_PART_WINDOWS,            /* Microsoft basic data: Windows', or data every system shares */
    DISK_PART_WINDOWS_RESERVED,   /* Microsoft reserved, which holds nothing a person keeps */
    DISK_PART_WINDOWS_RECOVERY,   /* Windows' recovery */
    DISK_PART_LINUX,              /* a Linux file system: data, root, home */
    DISK_PART_LINUX_SWAP,
    DISK_PART_LINUX_LVM,
    DISK_PART_BIOS_BOOT,          /* where a BIOS loader keeps its second stage */
    DISK_PART_APPLE,              /* APFS, HFS+ */
    DISK_PART_KINDS
} DiskPartKind;

/* cf8ae49a-d26a-4959-9a6f-71c932e0c9eb — minted for BoxOS, never reissued. */
#define DISK_TYPE_BOXOS  { 0x9a, 0xe4, 0x8a, 0xcf, 0x6a, 0xd2, 0x59, 0x49, \
                           0x9a, 0x6f, 0x71, 0xc9, 0x32, 0xe0, 0xc9, 0xeb }
/* c12a7328-f81f-11d2-ba4b-00a0c93ec93b — UEFI 2.10 §5.3.3. */
#define DISK_TYPE_ESP    { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, \
                           0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b }
/* The one byte an MBR marks BoxOS ground with, since the image was first laid. */
#define DISK_MBR_BOXOS      0x7Fu
/* The MBR entry that says "this disk is a GPT" (UEFI 2.10 §5.2.3). */
#define DISK_MBR_PROTECTIVE 0xEEu

/* The kind a GPT type names. A static table in a function, so a file that
 * never asks carries none of it. */
static inline DiskPartKind DiskKindOfType(const uint8_t type[16])
{
    static const struct {
        uint8_t kind;
        uint8_t guid[16];
    } known[] = {
        { DISK_PART_BOXOS, DISK_TYPE_BOXOS },
        { DISK_PART_ESP, DISK_TYPE_ESP },
        /* ebd0a0a2-b9e5-4433-87c0-68b6b72699c7 */
        { DISK_PART_WINDOWS, { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                               0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } },
        /* e3c9e316-0b5c-4db8-817d-f92df00215ae */
        { DISK_PART_WINDOWS_RESERVED, { 0x16, 0xe3, 0xc9, 0xe3, 0x5c, 0x0b, 0xb8, 0x4d,
                                        0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae } },
        /* de94bba4-06d1-4d40-a16a-bfd50179d6ac */
        { DISK_PART_WINDOWS_RECOVERY, { 0xa4, 0xbb, 0x94, 0xde, 0xd1, 0x06, 0x40, 0x4d,
                                        0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac } },
        /* 0fc63daf-8483-4772-8e79-3d69d8477de4 */
        { DISK_PART_LINUX, { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                             0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 } },
        /* 4f68bce3-e8cd-4db1-96e7-fbcaf984b709 — root, x86-64 */
        { DISK_PART_LINUX, { 0xe3, 0xbc, 0x68, 0x4f, 0xcd, 0xe8, 0xb1, 0x4d,
                             0x96, 0xe7, 0xfb, 0xca, 0xf9, 0x84, 0xb7, 0x09 } },
        /* 933ac7e1-2eb4-4f13-b844-0e14e2aef915 — home */
        { DISK_PART_LINUX, { 0xe1, 0xc7, 0x3a, 0x93, 0xb4, 0x2e, 0x13, 0x4f,
                             0xb8, 0x44, 0x0e, 0x14, 0xe2, 0xae, 0xf9, 0x15 } },
        /* 0657fd6d-a4ab-43c4-84e5-0933c84b4f4f */
        { DISK_PART_LINUX_SWAP, { 0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43,
                                  0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f } },
        /* e6d6d379-f507-44c2-a23c-238f2a3df928 */
        { DISK_PART_LINUX_LVM, { 0x79, 0xd3, 0xd6, 0xe6, 0x07, 0xf5, 0xc2, 0x44,
                                 0xa2, 0x3c, 0x23, 0x8f, 0x2a, 0x3d, 0xf9, 0x28 } },
        /* 21686148-6449-6e6f-744e-656564454649 */
        { DISK_PART_BIOS_BOOT, { 0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e,
                                 0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 } },
        /* 7c3457ef-0000-11aa-aa11-00306543ecac — APFS */
        { DISK_PART_APPLE, { 0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11,
                             0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } },
        /* 48465300-0000-11aa-aa11-00306543ecac — HFS+ */
        { DISK_PART_APPLE, { 0x00, 0x53, 0x46, 0x48, 0x00, 0x00, 0xaa, 0x11,
                             0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } },
    };
    for (unsigned k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
        unsigned b = 0;
        while (b < 16 && known[k].guid[b] == type[b])
            b++;
        if (b == 16)
            return (DiskPartKind)known[k].kind;
    }
    return DISK_PART_OTHER;
}

/* The kind an MBR's type byte names. */
static inline DiskPartKind DiskKindOfMbr(uint8_t type)
{
    switch (type) {
    case DISK_MBR_BOXOS:            return DISK_PART_BOXOS;
    case 0xEF:                      return DISK_PART_ESP;
    case 0x07: case 0x0B: case 0x0C:
    case 0x0E: case 0x06:           return DISK_PART_WINDOWS;
    case 0x27:                      return DISK_PART_WINDOWS_RECOVERY;
    case 0x83:                      return DISK_PART_LINUX;
    case 0x82:                      return DISK_PART_LINUX_SWAP;
    case 0x8E:                      return DISK_PART_LINUX_LVM;
    case 0xAF:                      return DISK_PART_APPLE;
    default:                        return DISK_PART_OTHER;
    }
}

/* What a partition of `kind` is, in a word or two, as a person is told it. */
static inline const char* DiskKindName(DiskPartKind kind)
{
    switch (kind) {
    case DISK_PART_BOXOS:            return "BoxOS";
    case DISK_PART_ESP:              return "EFI system";
    case DISK_PART_WINDOWS:          return "Windows data";
    case DISK_PART_WINDOWS_RESERVED: return "Windows reserved";
    case DISK_PART_WINDOWS_RECOVERY: return "Windows recovery";
    case DISK_PART_LINUX:            return "Linux";
    case DISK_PART_LINUX_SWAP:       return "Linux swap";
    case DISK_PART_LINUX_LVM:        return "Linux LVM";
    case DISK_PART_BIOS_BOOT:        return "BIOS boot";
    case DISK_PART_APPLE:            return "Apple";
    default:                         return "other";
    }
}

/* What the table of a disk is in. */
typedef enum {
    DISK_TABLE_NONE = 0,     /* no table: a blank disk, or one only a whole new table can use */
    DISK_TABLE_MBR,          /* the old table: read and shown, not written */
    DISK_TABLE_GPT,          /* a GPT whose two copies both hold */
    DISK_TABLE_GPT_LOST,     /* the disk says it is a GPT and no copy of the table holds */
    DISK_TABLE_REFUSED,      /* the medium would not give its table up: nothing is known */
} DiskTableState;

/* What else a GPT's two copies say (DISK_TABLE_GPT). Each is mended from the
 * other by hw.disk.mend, by a person's word; the kernel never mends one by
 * itself. */
#define DISK_COPY_FIRST_DAMAGED  0x01u   /* the first copy does not hold: the far one is read */
#define DISK_COPY_FAR_DAMAGED    0x02u   /* the far copy does not hold */
#define DISK_COPY_DIFFER         0x04u   /* both hold, and say different things: the first is read */
#define DISK_COPY_FAR_ELSEWHERE  0x08u   /* the far copy is not at the end of the disk: the table was made for a smaller one */

/* A disk's flags in a listed record. */
#define DISK_REMOVABLE           0x01u   /* its medium can be pulled out */
#define DISK_REFUSES_WRITES      0x02u   /* its medium refused a write as protected */
#define DISK_CARRIES_HOUSE       0x04u   /* a volume of the house stands on it: never laid anew */

/* A partition's flags in a listed record. */
#define DISK_PART_MOUNTED        0x01u   /* a volume of the house stands on it, `volume` its number + 1 */

/*
 * hw.disks.list — out_crate = [u32 delivered][u32 total], then `delivered`
 * disks while they fit, in the order of their seats:
 *
 *   disk:      [u8 seat][u8 board (BoardKind)][u8 flags (DISK_*)][u8 table (DiskTableState)]
 *              [u32 seating][u32 block_bytes][u32 physical_bytes][u64 blocks]
 *              [u8 copies (DISK_COPY_*)][u8 reserved][u16 parts][u16 free][u16 name_len]
 *              [name]                            — DISK_LISTED_HEAD bytes before the name
 *   partition: [u8 kind (DiskPartKind)][u8 flags (DISK_PART_*)][u16 entry][u32 volume]
 *              [u64 first_block][u64 blocks][u8 type[16]][u16 name_len][name]
 *                                                — DISK_PART_LISTED_HEAD before the name
 *   free run:  [u64 first_block][u64 blocks]     — DISK_FREE_LISTED bytes
 *
 * `parts` partitions, then `free` free runs follow each disk. Blocks are the
 * medium's own (block_bytes each). `seating` is which occupant of the seat
 * the disk is: every change names it back, and a seat that changed hands
 * since is refused (ERR_OBJECT_NOT_FOUND). A free run begins on the grain
 * (DISK_GRAIN_BYTES) and is at least one grain long. `entry` is the
 * partition's place in its table; `type` its type, zeros on an MBR but its
 * byte in type[0]; `name` the one the table gives it, UTF-8.
 */
#define DISK_LISTED_HEAD         32u
#define DISK_PART_LISTED_HEAD    42u
#define DISK_FREE_LISTED         16u
#define DISK_NAME_MOST           108u    /* 36 UTF-16 units of a GPT name, as UTF-8 */

/* Where a partition may begin: on a mebibyte — every flash translation layer
 * and 4Kn medium is built around it, and every partitioning tool of fifteen
 * years uses it. */
#define DISK_GRAIN_BYTES         (1024u * 1024u)

/* The grain in a disk's own blocks: a mebibyte, or one physical block when
 * that is larger (`physical_bytes` 0 when the medium would not say). */
static inline uint64_t DiskGrainBlocks(uint32_t block_bytes, uint32_t physical_bytes)
{
    uint64_t grain = DISK_GRAIN_BYTES / block_bytes;
    uint64_t phys  = physical_bytes > block_bytes ? physical_bytes / block_bytes : 1u;
    return grain > phys ? grain : phys;
}

#endif /* BOXOS_DISKS_H */
