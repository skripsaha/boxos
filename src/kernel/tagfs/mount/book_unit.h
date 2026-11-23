#ifndef TAGFS_MOUNT_BOOK_UNIT_H
#define TAGFS_MOUNT_BOOK_UNIT_H

#include "../tagfs.h"
#include "volume_book.h"

/*
 * BookUnitRead — sectors of a volume's book read off the medium and
 * each judged once, by the alphabet every reader of the book keeps
 * (volume_book.h): whether it holds as a sector of this volume's book at its
 * own address, and whether the run of them is one whole unit — a slot, a
 * node of the Tally, a stitch — of the kind and seq its reader expects.
 *
 * ‼ A SECTOR THE MEDIUM CANNOT READ IS AN ANSWER ABOUT THE BOOK, NOT A MEDIUM
 * THAT DID NOT ANSWER. ERR_BAD_SECTOR (UNC, MEDIUM ERROR) on a run is read
 * again a sector at a time, so the one that will not read is named and the
 * rest are judged — a slot torn there, a copy of a node that does not hold,
 * a stitch closed and damaged: the book's reader decides by where it lies,
 * and no seat:ready will change it, so it is never
 * counted as a refusal. Any other refusal is the medium not answering: it
 * is counted (TagFSNoteReadRefused) and handed back, and the mount it ends
 * is asked again when a seat answers again — the arrangement every reader of
 * a mount keeps (tagfs_read_block, SpineRead).
 *
 * ‼ SUMMED BY THE CRC32 INSTRUCTION, HELD TO THE TABLE FIRST. The kernel sums a
 * sector with KCrc32c, the crc32 instruction where every core has SSE4.2
 * (crypto.c); before the first sector of a book is judged, once a boot, that
 * instruction is held to the book's own table (volume_book.h) over a
 * check text and a sector's worth of bytes. A processor whose instruction
 * answers otherwise is said, and every core sums by the table from then on.
 *
 * Memory is the heap's: a unit of any length is read into buffers asked for
 * before the read, never on a kernel stack. Inside the mount, or with no
 * TagFS lock held; the I/O stands in its call.
 */

/* What one sector read came to. */
#define BOOK_SECTOR_UNREAD  0u   /* the medium cannot read it (ERR_BAD_SECTOR) */
#define BOOK_SECTOR_LOOSE   1u   /* read, and not a sector of this book at this address */
#define BOOK_SECTOR_HOLDS   2u   /* read, and holds as a sector of this book where it lies */

typedef struct {
    uint64_t       vlba;         /* where the run begins, a sector of the volume */
    uint32_t       count;        /* sectors */
    uint8_t*       sectors;      /* count * 512, as read; zeros where one would not read */
    uint8_t*       state;        /* count, BOOK_SECTOR_* each */
    uint8_t*       stream;       /* count * 488 when whole, the unit's stream; else NULL */
    bool           whole;        /* every sector holds, one unit of the kind and seq asked */
    BookSectorHead head;         /* the first sector's head, when whole */
    char           why[112];     /* when not whole: why, in the words the census says it */
} BookUnit;

/*
 * `count` sectors of the volume from `vlba`, read and judged into *out: each
 * sector's state, and whether they are one whole unit of `kind` (0: any) and
 * of `seq` (0: any). OK — read and judged, whole or not; ERR_NO_MEMORY; or
 * the medium's refusal, counted. *out is the caller's to let go of
 * (BookUnitLetGo) on every answer.
 */
error_t  BookUnitRead(TagFSVolume* vol, uint64_t vlba, uint32_t count, uint16_t kind, uint64_t seq, BookUnit* out);
void     BookUnitLetGo(BookUnit* u);

/* The same judgement of `count` sectors read already, `sectors` — zeros where
 * one would not read — by a reader past the volume's door (mount/
 * perambulation.c): nothing of them folded into the seqs seen. OK, or
 * ERR_NO_MEMORY; *out the caller's to let go of on every answer. */
error_t  BookUnitOf(TagFSVolume* vol, uint64_t vlba, uint32_t count, uint16_t kind, uint64_t seq, const uint8_t* sectors,
                   BookUnit* out);

/* A refusal of the medium — tagfs_volume_read's `rc`, not ERR_BAD_SECTOR —
 * counted for the mount's road back (tagfs.c), and answered as what it is:
 * ERR_DEVICE_NOT_READY, or ERR_READ_FAILED. For a reader of the volume's
 * ground that reads it as no unit of the book (ribbon_verify.c). */
error_t  BookUnitRefused(TagFSVolume* vol, int rc);

/* The largest seq a sector read through BookUnitRead held at since the mount
 * forgot it — what the next seq is given past. Read
 * by the mount, with its door shut; raised by every reader of the book on any
 * core, the mount's and the DIGESTS leaves a read of a block opens after it. */
uint64_t BookSeqSeen(TagFSVolume* vol);
void     BookSeqSeenForget(TagFSVolume* vol);

/* A seq the book's writer gave a unit (settle/ribbon.c), folded into the ones
 * seen: no reader or writer of the volume gives it again this mount. */
void     BookSeqGiven(TagFSVolume* vol, uint64_t seq);

/* Whether every byte of the run is zero — a slot never written. */
bool     BookUnitBlank(const BookUnit* u);

/* The first sector of a whole unit whose lead is not the one its records give
 * it, given the stream offsets they begin at (`starts`, rising — §16.3 п.4),
 * with the lead it names and the lead they give it; u->count when every
 * sector's lead is its records'. */
uint32_t BookUnitLeadWrong(const BookUnit* u, const uint32_t* starts, uint32_t n, uint32_t* named,
                           uint32_t* given);

/* What one volume holds of book_unit.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    /* Whether the crc32 instruction has been held to the book's table this boot,
     * and whether a want of memory to hold it was said. */
    volatile uint32_t sums_judged;
    volatile uint32_t sums_short;
    /* The largest seq a sector of the book read held at: the next seq is past
     * everything the mount saw hold, the torn tail and the direct scan among it —
     * every reader of a book reads through here, so nothing read is left out. The
     * mount reads it with its door shut; but the DIGESTS leaves a read of a block
     * opens after the mount (TallyDigestFind) read through here too, on any core
     * at once — so it only ever rises, by an atomic max, and no fold is lost to
     * another. */
    uint64_t seq_seen;
} BookUnitState;

#endif /* TAGFS_MOUNT_BOOK_UNIT_H */
