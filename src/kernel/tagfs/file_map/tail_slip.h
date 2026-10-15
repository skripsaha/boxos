#ifndef TAGFS_TAIL_SLIP_H
#define TAGFS_TAIL_SLIP_H

#include "file_record.h"

/*
 * TailSlip — the bytes the Slips of a format-v2 volume's ribbon lay over a
 * file's last block (spec_v2 §2.8, H-7).
 *
 * A writer that adds a few bytes to a file whose last block is not full
 * carries them in the stitch, not in a block of fresh ground: `slip_off ·
 * slip_len · slip` in a Born or a Shaped lays them over the file's LAST block
 * — the block ⌈size/4096⌉ − 1 after the record — whose base is what the
 * file's last run names: the medium's bytes of a DATA block, zeros of a hole.
 * Every Slip of the file lies over the ones before it, in the order of their
 * stitches; a record whose splices change the last block lets them all go
 * (§16.3 п.7), and so does the Tally, which carries no Slip.
 *
 * Memory holds the sum of them: the bytes laid and which of the block's 4096
 * they cover — never more than a block a file, however many Slips the ribbon
 * carries. It is immutable once made and shared by every record that keeps
 * it (FileRecordSplice, FileRecordRelabel), freed with the last.
 *
 * Every road a read of a file takes lays it on the last block it gives back
 * (read/read.h, ReadSinkBlock): the stream through a handle and its window,
 * and the port's job. This kernel reads Slips and writes none — a volume that
 * carries one stands under glass (§16.1 п.2): folding a Slip into a block is
 * the writer's, with the writer of Slips.
 */

/* `under` — NULL for none — with `len` bytes more laid at `off` over it: a new
 * TailSlip with one reference, or NULL with no memory. off + len is at most a
 * block, which the caller has held the Slip to. */
TailSlip*       TailSlipLaid(const TailSlip* under, uint32_t off, const uint8_t* bytes, uint32_t len);

/* One reference more (the slip back, for a caller's assignment; NULL stays
 * NULL), and one let go — the last frees it. */
const TailSlip* TailSlipHold(const TailSlip* slip);
void            TailSlipLetGo(const TailSlip* slip);

/* Whether file block `k` of `rec` carries Slips — it is the file's last and
 * the record keeps some — and when it does, `block` (the base the read laid
 * there: the medium's bytes, or zeros for a hole) takes them over it. */
bool            TailSlipOver(const FileRecord* rec, uint64_t k, uint8_t* block);
bool            TailSlipOn(const FileRecord* rec, uint64_t k);

#endif /* TAGFS_TAIL_SLIP_H */
