#ifndef TAGFS_BOOT_H
#define TAGFS_BOOT_H

/*
 * TagFS on-disk structures needed at boot time.
 * Must match kernel's tagfs.h and create_tagfs_v1.h exactly.
 * No kernel headers included — standalone for UEFI environment.
 */

#include "uefi.h"

/* The volume's own formats, shared with the kernel and the tool that writes
 * them. <stdint.h> is not available here; uefi.h defines the fixed-width types
 * these headers take from their includer. */
#include "volume_deed.h"
#include "volume_ledger.h"

/* And the book a format-v2 volume keeps (spec_v2 §9.2, §16.7 п.2): its
 * letters, the Tally's nodes, the Spine's slots and the one rule a slot is
 * chosen by, the ribbon's stitches, and the digest a block is held to — the
 * text the kernel and mkfs read it by, not a copy of it. */
#include "volume_spine.h"
#include "volume_ribbon.h"
#include "volume_digest.h"

/* =========================================================================
 * Constants (must match tagfs.h / create_tagfs_v1.h)
 * ========================================================================= */

#define TAGFS_FILETBL_MAGIC      0x54465442U   /* "TFTB" */
#define TAGFS_MPOOL_MAGIC        0x544D504CU   /* "TMPL" */

/* The volume's block and its sectors are the Deed header's (volume_deed.h),
 * under the names this loader has always used for them. */
#define TAGFS_BLOCK_SIZE         VOLUME_BLOCK_BYTES
#define TAGFS_SECTOR_SIZE        VOLUME_DEED_SECTOR_BYTES
#define TAGFS_SECTORS_PER_BLOCK  VOLUME_BLOCK_SECTORS

/* No superblock sector here any more: this loader reads the medium's partition
 * table to find the volume's ground, and the Deed at the head of that ground
 * to find everything else. See src/include/volume_deed.h — which this header
 * includes rather than restating, because a second copy of a format is a
 * format that will disagree with itself. */

#define TAGFS_FILE_ACTIVE        (1U << 0)

#define TAGFS_MPOOL_DATA_SIZE     4080
#define TAGFS_FTABLE_PER_BLOCK    510

/* Metadata record header layout (must match pack_metadata_record in create_tagfs_v1.c):
 *   uint16_t record_len     +0
 *   uint32_t file_id        +2
 *   uint32_t flags          +6
 *   uint64_t size           +10
 *   uint64_t created_time   +18
 *   uint64_t modified_time  +26
 *   uint16_t tag_count      +34
 *   uint16_t extent_count   +36
 *   uint16_t name_len       +38
 *   uint16_t crc16          +40
 *   uint16_t tag_ids[tag_count]  +42
 *   FileExtent extents[]    after tag_ids
 *   char     filename[]     after extents
 */
#define META_RECORD_LEN_OFF      0
#define META_RECORD_FILE_ID_OFF  2
#define META_RECORD_FLAGS_OFF    6
#define META_RECORD_SIZE_OFF     10
#define META_RECORD_TAGCOUNT_OFF 34
#define META_RECORD_EXTCOUNT_OFF 36
#define META_RECORD_NAMELEN_OFF  38
#define META_RECORD_CRC_OFF      40   /* over the whole record, these two bytes as zeros */
#define META_RECORD_VARDATA_OFF  42   /* start of tag_ids[] */

/* =========================================================================
 * On-disk structures (packed, must be 512 / 4096 bytes exactly)
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    uint32_t meta_block;
    uint32_t meta_offset;
} TagBootFileTableEntry;

typedef struct __attribute__((packed)) {
    uint32_t              magic;
    uint32_t              next_block;
    uint32_t              entry_count;
    uint32_t              _reserved;
    TagBootFileTableEntry entries[TAGFS_FTABLE_PER_BLOCK];
} TagBootFileTableBlock;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t next_block;
    uint16_t used_bytes;
    uint16_t record_count;
    uint8_t  _reserved[4];
    uint8_t  payload[TAGFS_MPOOL_DATA_SIZE];
} TagBootMetaPoolBlock;

typedef struct __attribute__((packed)) {
    uint32_t start_block;
    uint16_t block_count;
} TagBootFileExtent;

/* Compile-time size checks */
_Static_assert(sizeof(TagBootFileTableBlock)  == 4096, "TagBootFileTableBlock must be 4096 bytes");
_Static_assert(sizeof(TagBootMetaPoolBlock)   == 4096, "TagBootMetaPoolBlock must be 4096 bytes");
_Static_assert(sizeof(TagBootFileExtent)      == 6,    "TagBootFileExtent must be 6 bytes");

#endif /* TAGFS_BOOT_H */
