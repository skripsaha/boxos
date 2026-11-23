#ifndef TAGFS_TAIL_SLIP_H
#define TAGFS_TAIL_SLIP_H

#include "file_record.h"

/*
 * TailSlip — the bytes the Slips of a volume's ribbon lay over a file's last
 * block.
 *
 * A writer that adds a few bytes to a file whose last block is not full
 * carries them in the stitch, not in a block of fresh ground: `slip_off ·
 * slip_len · slip` in a Born or a Shaped lays them over the file's LAST block
 * — the block ⌈size/4096⌉ − 1 after the record — whose base is what the
 * file's last run names: the medium's bytes of a DATA block, zeros of a hole.
 * Every Slip of the file lies over the ones before it, in the order of their
 * stitches; a record whose splices change the last block lets them all go
 * (§16.3 п.7), the file's size cuts them (§16.5 п.13), and the Tally carries
 * none — each tail is laid in a block before it (Engross, §16.5 п.14).
 *
 * Memory holds the sum of them: the bytes laid and which of the block's 4096
 * they cover — never more than a block a file, however many Slips the ribbon
 * carries. It is immutable once a record holds it and shared by every record
 * that keeps it (FileRecordSplice, FileRecordRelabel), freed with the last.
 * The one that is not yet a record's — a write's own, which a Plot fills and
 * its Ink folds (TailSlipBlank, TailSlipFold) — is its Plot's alone.
 *
 * Every road a read of a file takes lays it on the last block it gives back
 * (read/read.h, ReadSinkBlock): the stream through a handle and its window,
 * and the port's job; and every road a write takes over that block lays it
 * under the write (plot_write.c, plot_window.c).
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

/*
 * The one rule a record meets the Slips of the record it was built from by —
 * the Ink's and the replay's alike (FileRecordSplice): `rec`, made of `old` by
 * splices (`spliced`) and given its own size, keeps old's Slips unless a
 * splice changed the last block — its place in the file or the block it lies
 * on — and keeps them cut to its size: a growth inside the block after a cut
 * reads the base, never the bytes the cut took (§16.5 п.13). rec->slip is set
 * to a reference of its own — a new TailSlip only where the cut takes a byte.
 * OK, or ERR_NO_MEMORY with rec->slip NULL.
 */
error_t         TailSlipKept(TagFSVolume* vol, const FileRecord* old, FileRecord* rec, bool spliced);

/* The span the Slips cover, from the first byte they cover to the last:
 * [*lo, *lo + the answer), 0 for none. */
uint32_t        TailSlipHull(const TailSlip* slip, uint32_t* lo);

/* Whether `slip` (NULL for none), [off, off + len) laid over it, covers one
 * span with no byte bare inside it — the one shape a writer lays a Slip in, so
 * the change a stitch carries is always one span of bytes it holds (§16.5
 * п.13): a write that would break it lays the block instead (plot_slip.c). */
bool            TailSlipJoins(const TailSlip* slip, uint32_t off, uint32_t len);

/* A write's own Slip, before any record holds it: a TailSlip covering
 * nothing, one reference — its Plot's — or NULL with no memory. The write's
 * bytes go to TailSlipBytes(s) + off, and TailSlipCovers marks them laid. */
TailSlip*       TailSlipBlank(void);
uint8_t*        TailSlipBytes(TailSlip* slip);
void            TailSlipCovers(TailSlip* slip, uint32_t off, uint32_t len);

/* The write's Slip `slip`, whose own bytes are [off, off + len), made the
 * file's whole tail: every other byte as `under` — the Slips the record it is
 * inked onto keeps (TailSlipKept), NULL for none — has it. Asked again of
 * another `under` when the Ink goes round, while the Plot alone holds it. */
void            TailSlipFold(TailSlip* slip, uint32_t off, uint32_t len, const TailSlip* under);

/*
 * TailSlipDelta — the Slip a stitch lays for a file that went from `was`
 * (NULL for one born since the last stitch) to `now`, by splices or none
 * (`spliced`): the span of now's last block from the first byte a reader of
 * `was` holds otherwise — once the record's splices and size stand, by
 * TailSlipKept's rule — to the last: [*off, *off + the answer), 0 for none.
 * The change of the cycle, never the tail the Slips have grown (§16.5 п.13);
 * one rule with tools/volumeread/stitch.py (SlipDelta). With `out` (a block's
 * room), the span's bytes: now's, a byte its Slips do not cover the zero of
 * the hole beneath — over a DATA block the writer leaves no such byte inside
 * the span (TailSlipJoins), and one found is a DEFECT, said.
 */
uint32_t        TailSlipDelta(const FileRecord* was, const FileRecord* now, bool spliced, uint32_t* off,
                              uint8_t* out);

/*
 * TailSlipClaim — the bytes of a stitch's Slip a change from `cur` to `rec`
 * may account for, the span its Ink claims: the hull of rec's Slips when the
 * change rode a Slip (`rides`), joined to what its size brings into a last
 * block that is a hole. A cut earlier in the cycle may have taken Slips a
 * reader of what settled holds there, and the stitch lays the hole's zeros
 * over them (TailSlipDelta) — bytes a span, which rides no Slip, claims too.
 * So the claims of a cycle's Inks hold the span its stitch lays (P21).
 */
uint32_t        TailSlipClaim(const FileRecord* cur, const FileRecord* rec, bool rides);

#endif /* TAGFS_TAIL_SLIP_H */
