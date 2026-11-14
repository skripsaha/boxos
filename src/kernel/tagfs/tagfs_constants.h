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
/* The one id past them: no tag's, never in the registry nor in a file's tags,
 * but the tag index's own set of the files that are deleted and kept
 * (TAGFS_FILE_DELETED) — held beside the tags' members so that every query,
 * which already sets members aside, sets these aside by default and asks for
 * them alone when it is asked to (query/query.c). */
#define TAGFS_DELETED_MEMBERS       0x7FFFFFFFu

// TagRegistryEntry.flags bits.
//   HAS_VALUE — entry carries a value (bit 0; persisted, value-presence).
//   SYSTEM    — entry is a reserved-vocabulary key, stamped at every mount
//               (bit 1; re-derived, never the reason for a disk write).
#define TAGFS_TAG_FLAG_HAS_VALUE    0x01
#define TAGFS_TAG_FLAG_SYSTEM       0x02

// File flags
#define TAGFS_FILE_ACTIVE           (1 << 0)
/* Rolled back by a mount's verdict (spec_v2 §2.8, Undone): its last write did
 * not land, and it stands as it stood before. The system's own word in the
 * file's record, put there by the Ink the verdict's first cycle makes
 * (mount/errata.h) — never a tag: a file's tags are its user's. */
#define TAGFS_FILE_UNDONE           (1 << 4)
/* Deleted, and kept until the time its record names (FileRecord.keep_until):
 * the volume's queries do not see it, and it can be brought back until then;
 * when the time passes the volume lets it go by itself, its blocks to Fallow
 * (keep/keep.h). Bit 7, after the role's two: put on by InkKeep and taken off
 * by InkRestore, never by a flag a caller names, and never a tag — what a
 * person calls the trash is the volume's word on the file, not a label he
 * wears. Bit 7 is the Labeled's KEEP verb in the ribbon and the header's
 * keep_ns in the Tally (volume_ribbon.h, volume_tally.h). */
#define TAGFS_FILE_DELETED          (1 << 7)
/* The file's ROLE — what the system keeps it as — is bits 5 and 6 of its
 * record's flags, a number of two bits: 0 an ordinary file, 1 a system file,
 * 2 a boot file, 3 the kernel. Set by the image builder (tools/
 * create_tagfs_cargo.c) as it lays the volume — the kernel is the file it is
 * told is the kernel (--kernel), a system or boot program is one whose own
 * box says so (the part `role`, box_reader.h BoxRoleRead), which the builder
 * trusts because it lays the boxes it has just packed — and by no call a
 * program or a person makes: a file's tags are its user's, and no tag says
 * what the system does with a file. A file in any role but the ordinary is changed, truncated,
 * given up and moved by no call without the system's warrant (ink.c), is laid
 * by neither Dedup nor Conjugate on another's blocks nor another's on its own
 * (dedup_owners.c, conjugate.c), and — the kernel's role first, then the boot's,
 * then the system's — is what TagBoot looks the kernel for (tagboot_lectern.c).
 * A program started from a system file is started with the system's
 * authority (system_ops.c). The values stand in place: a role is
 * `flags & TAGFS_FILE_ROLE_MASK`, and the larger value is the first looked for. */
#define TAGFS_FILE_ROLE_MASK        96
#define TAGFS_FILE_ROLE_ORDINARY    0
#define TAGFS_FILE_ROLE_SYSTEM      (1 << 5)
#define TAGFS_FILE_ROLE_BOOT        (1 << 6)
#define TAGFS_FILE_ROLE_KERNEL      96
/* Bits 1 and 2 named trashed and hidden once, which were tags a file wore and
 * never bits of its record: no kernel set them, and none reads them (the trash
 * is bit 7 now, and no tag of a file says anything to the system). Bit 3 is
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
