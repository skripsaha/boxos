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
/* The tag index's set of the files still arriving (TAGFS_FILE_ARRIVING), set
 * aside by every question, the one for the deleted files alone too — an id
 * past every one a volume gives and no question names, as the set above. */
#define TAGFS_ARRIVING_MEMBERS      0xFFFFFFFEu

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
/* Made by a program of the volume: the record's made_by is the volume's id of
 * that program's file (FileRecord.made_by, nothing in the record names it
 * without this bit). Bit 6 — bit 5 is left to the golden node of the book's
 * tests, which wears it as a bit no kernel names — put on by the create that a
 * program's cabin asks (storage_ops.c) and by nothing else, never taken off
 * and never a tag: who made a file is the volume's word, set once and carried
 * by every change the file goes through. The bit is what puts made_by after
 * the header's keep_ns in the Tally and after the slip of the ribbon's Born
 * (volume_tally.h, volume_ribbon.h). */
#define TAGFS_FILE_MADE_BY          (1 << 6)
/* The file of a program that shows its own tags to other programs: the
 * record's shown_to names them (FileRecord.shown_to, 0 for every program; no
 * record names any without this bit). Bit 7 — put on and taken off with the
 * list by the one Ink of it, InkShown, which the program itself asks through
 * the deck (storage_tags.c), and never a tag: whom a program shows its tags
 * to is the volume's word on the program's own file, and the file let go
 * takes it with it. The bit is what puts the list after made_by in the
 * Tally's header and in the ribbon's Born, and what the ribbon's Labeled
 * SHOWN leaves (volume_tally.h, volume_ribbon.h). */
#define TAGFS_FILE_SHOWN            (1 << 7)
/* The file holds a byte, and its record keeps its first ones (FileRecord.head,
 * TAGFS_HEAD_MOST at the most): what a file is is told by them, by whoever
 * knows what they mean — the volume keeps them exact and knows no format. Bit
 * 8 — put on, taken off and its bytes changed by the Ink of every change of
 * what the file holds, never by a flag a caller names, and never a tag. The
 * bit is what puts the bytes after the list in the Tally's header and in the
 * ribbon's Born, and what the ribbon's Labeled HEAD leaves. */
#define TAGFS_FILE_HEAD             (1 << 8)
#define TAGFS_HEAD_MOST             16u
/* The file holds a signed program, and its record keeps the program's
 * passport (FileRecord.passport): who the program is across every copy and
 * version of it, its author and its name (box_trust.h), bit 63 set so it is
 * never a file's id. Bit 9 — laid by the volume's maker and by the Ink a
 * program's start makes when its passport is another (InkPassport); a change
 * of what the file holds leaves it, so a program written over with its next
 * version stays itself. Never a flag a caller names, and never a tag. The bit
 * is what puts the passport after the head in the Tally's header and in the
 * ribbon's Born, and what the ribbon's Labeled PASSPORT leaves. */
#define TAGFS_FILE_PASSPORT         (1 << 9)
/* The file is still being carried in from another volume (volume/carry.c):
 * born so, with the name, tags and times of the file it is a copy of, it is
 * met by no question, no name, no first bytes and no open of a program, and
 * its writes keep the times it was born with; the carry's last Ink takes the
 * bit off, and from then on it is the file it was. One the bit is still on
 * when no carry is under way — the machine stopped, or the medium left, in
 * the middle of one — is let go by the volume (keep/keep.c), so a copy cut
 * short is never taken for the file. Bit 10 — put on at the birth only and
 * taken off by the carry alone, never a flag a caller names, never a tag. */
#define TAGFS_FILE_ARRIVING         (1 << 10)
/* The sets of the tag index beside the tags that a record's `flags` put its
 * file in — the files kept deleted, the files still arriving — into `sets`:
 * how many. The one rule for every place a file comes into the index or
 * leaves it: a mount, a replay, an Ink. */
static inline uint32_t TagFSAsideSets(uint32_t flags, uint32_t sets[2])
{
    uint32_t n = 0;
    if (flags & TAGFS_FILE_DELETED)
        sets[n++] = TAGFS_DELETED_MEMBERS;
    if (flags & TAGFS_FILE_ARRIVING)
        sets[n++] = TAGFS_ARRIVING_MEMBERS;
    return n;
}

/* Every passport carries bit 63, so no passport is a file's id (which a
 * volume gives from 1 up) — the book's TALLY_PASSPORT_BIT. */
#define TAGFS_PASSPORT_BIT          (1ull << 63)
/* A file's id as the house names it (volume/house.c): its volume's number in
 * bits 48..62 and the volume's own id of it below them, so the home volume's
 * files — number 0 — keep the ids their volume gives. A volume gives no id at
 * or past TAGFS_VOLUME_FILES_MOST, and the house numbers no volume past
 * TAGFS_VOLUMES_MOST. */
#define TAGFS_VOLUME_FILE_BITS      48u
#define TAGFS_VOLUME_FILES_MOST     ((1ull << TAGFS_VOLUME_FILE_BITS) - 1u)
#define TAGFS_VOLUMES_MOST          0x7FFFu

// Handle flags
#define TAGFS_HANDLE_READ           (1 << 0)
#define TAGFS_HANDLE_WRITE          (1 << 1)
#define TAGFS_HANDLE_APPEND         (1 << 2)   /* every write lands at the file's size */
#define TAGFS_HANDLE_KEPT           (1 << 3)   /* the volume's own, reaching a file kept in the trash (PlotLoadKept) */
#define TAGFS_HANDLE_ARRIVAL        (1 << 4)   /* the carry's own, reaching a file still arriving (volume/carry.c) */

// Registry constants
#define TAGFS_REG_BUCKETS           512
#define TAGFS_KEY_BUCKETS           128

#endif // TAGFS_CONSTANTS_H
