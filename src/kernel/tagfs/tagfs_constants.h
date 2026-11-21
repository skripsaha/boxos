#ifndef TAGFS_CONSTANTS_H
#define TAGFS_CONSTANTS_H

// ============================================================================
// TagFS Production Constants
// All TagFS modules include this file - no circular dependencies. Each fact
// here has this one home in the kernel; tagfs.h includes it and repeats none.
// ============================================================================

#include "../../lib/kernel/ktypes.h"     /* volume_deed.h takes its types from the includer */
#include "../../include/volume_deed.h"
#include "../../include/tagfs_reserved.h"   /* TAGFS_MAX_TAG_ID, the tag widths: the builder's too */

/* The volume's block and its sectors are the format's, owned by the Deed's
 * header; these are the names the kernel has always used for them. */
#define TAGFS_BLOCK_SIZE            VOLUME_BLOCK_BYTES
#define TAGFS_SECTOR_SIZE           VOLUME_DEED_SECTOR_BYTES
#define TAGFS_BLOCK_SECTORS         VOLUME_BLOCK_SECTORS

/* No disk layout here any more. Where each part of a volume sits is stated by
 * that volume's own Deed, in blocks counted from its own start, and read at
 * mount — see tagfs.h. These were absolute sectors of the whole medium, which
 * is why a volume could only ever exist in one place. */

// Tag constants
#define TAGFS_INVALID_TAG_ID        0xFFFFFFFFu
/* The one id past a volume's (TAGFS_MAX_TAG_ID, tagfs_reserved.h): no tag's, never in the registry nor in a file's tags,
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
//   RETIRED   — the tag is retired (bit 2): its id is known and given no more,
//               no lookup finds it and no file may wear it (tag_registry.c,
//               TagRegistryRetire).
#define TAGFS_TAG_FLAG_RETIRED      0x04

// File flags — dense until the first release; from it on, a bit once given is
// never given again.
#define TAGFS_FILE_ACTIVE           (1 << 0)
/* Rolled back by a mount's verdict (Undone): its last write did
 * not land, and it stands as it stood before. The system's own word in the
 * file's record, put there by the Ink the verdict's first cycle makes
 * (mount/errata.h) — never a tag: a file's tags are its user's. */
#define TAGFS_FILE_UNDONE           (1 << 1)
/* The file's ROLE — what the system keeps it as — is bits 2 and 3 of its
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
 * then the system's — is what Gangway looks the kernel for (gangway_lectern.c).
 * A program started from a system file is started with the system's
 * authority (system_ops.c). The values stand in place: a role is
 * `flags & TAGFS_FILE_ROLE_MASK`, and the larger value is the first looked for. */
#define TAGFS_FILE_ROLE_MASK        12
#define TAGFS_FILE_ROLE_ORDINARY    0
#define TAGFS_FILE_ROLE_SYSTEM      (1 << 2)
#define TAGFS_FILE_ROLE_BOOT        (1 << 3)
#define TAGFS_FILE_ROLE_KERNEL      12
/* Deleted, and kept until the time its record names (FileRecord.keep_until):
 * the volume's queries do not see it, and it can be brought back until then;
 * when the time passes the volume lets it go by itself, its blocks to Fallow
 * (keep/keep.h). Bit 4, after the role's two: put on by InkKeep and taken off
 * by InkRestore, never by a flag a caller names, and never a tag — what a
 * person calls the trash is the volume's word on the file, not a label he
 * wears. The bit is what the ribbon's Labeled KEEP leaves and what puts the
 * header's keep_ns in the Tally (volume_ribbon.h, volume_tally.h). */
#define TAGFS_FILE_DELETED          (1 << 4)

// Handle flags
#define TAGFS_HANDLE_READ           (1 << 0)
#define TAGFS_HANDLE_WRITE          (1 << 1)
#define TAGFS_HANDLE_APPEND         (1 << 2)   /* every write lands at the file's size */

// Registry constants
#define TAGFS_REG_BUCKETS           512
#define TAGFS_KEY_BUCKETS           128

#endif // TAGFS_CONSTANTS_H
