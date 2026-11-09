#ifndef TAGFS_CONSTANTS_H
#define TAGFS_CONSTANTS_H

// ============================================================================
// TagFS Production Constants
// All TagFS modules include this file - no circular dependencies. Each fact
// here has this one home in the kernel; tagfs.h includes it and repeats none.
// ============================================================================

#include "../../lib/kernel/ktypes.h"     /* volume_deed.h takes its types from the includer */
#include "../../include/volume_deed.h"

/* The volume's block and its sectors are the format's, owned by the Deed's
 * header; these are the names the kernel has always used for them. There is
 * no version number: what a volume's book is written in is said by which
 * stamp its Deed carries (LAYOUT for format v1, FRAME for v2). */
#define TAGFS_BLOCK_SIZE            VOLUME_BLOCK_BYTES
#define TAGFS_SECTOR_SIZE           VOLUME_DEED_SECTOR_BYTES
#define TAGFS_BLOCK_SECTORS         VOLUME_BLOCK_SECTORS

/* No disk layout here any more. Where each part of a volume sits is stated by
 * that volume's own Deed, in blocks counted from its own start, and read at
 * mount — see tagfs.h. These were absolute sectors of the whole medium, which
 * is why a volume could only ever exist in one place. */

// Tag constants
#define TAGFS_INVALID_TAG_ID        0xFFFFFFFFu
/* Volume tag ids stop one short of bit 31. Bit 31 belongs to the kernel's
 * Logbook (core/touch/logbook.h) — the vocabulary of occurrences, which must
 * exist on a machine that never mounted a volume. Capping here is what makes
 * the split an invariant instead of a convention: the volume cannot issue an
 * id that collides with an occurrence even in principle. What a volume's own
 * book can carry may be less (TAGFS_V1_TAG_ID_MOST), and its registry issues
 * no id past that. */
#define TAGFS_MAX_TAG_ID            0x7FFFFFFEu

// TagRegistryEntry.flags bits.
//   HAS_VALUE — entry carries a value (bit 0; persisted, value-presence).
//   SYSTEM    — entry is a reserved-vocabulary key, stamped at every mount
//               (bit 1; re-derived, never the reason for a disk write).
#define TAGFS_TAG_FLAG_HAS_VALUE    0x01
#define TAGFS_TAG_FLAG_SYSTEM       0x02

// File flags
#define TAGFS_FILE_ACTIVE           (1 << 0)
/* Bits 1 and 2 named trashed and hidden once, which are tags a file wears and
 * never bits of its record: no kernel set them, and none reads them. Bit 3 is
 * spent on v1: kernels before Quill step 1.1 took it for copy-on-write, so it
 * is never set and never trusted. */

// Handle flags
#define TAGFS_HANDLE_READ           (1 << 0)
#define TAGFS_HANDLE_WRITE          (1 << 1)
#define TAGFS_HANDLE_APPEND         (1 << 2)   /* every write lands at the file's size */

// Registry constants
#define TAGFS_REG_BUCKETS           512
#define TAGFS_KEY_BUCKETS           128
#define TAGFS_REGISTRY_DATA_SIZE    4080
/* A registry record's head: u16 tag_id, u8 flags, u8 key_len, u16 value_len.
 * A record never crosses a block, so a tag is at most what one block holds. */
#define TAGFS_REGISTRY_RECORD_HEAD  6
#define TAGFS_FTABLE_PER_BLOCK      510

#endif // TAGFS_CONSTANTS_H
