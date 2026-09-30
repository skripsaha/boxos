#ifndef TAGFS_LEDGER_H
#define TAGFS_LEDGER_H

#include "../../../lib/kernel/ktypes.h"
#include "../../../include/volume_ledger.h"
#include "../../core/error/error.h"
#include "../../core/use_context/use_context.h"
#include "../lock_rank/lock_rank.h"

/*
 * The Ledger — what is true of the volume today (volume_ledger.h).
 *
 * Two copies, one block apart. A read takes the newer of the two that is
 * intact; a write goes to the older one with a sequence number one higher. So
 * the copy that survives a torn write is never the copy being written, and
 * recovery is a comparison rather than a repair.
 *
 * ‼ WHO WRITES IT, AND UNDER WHAT.
 *
 * One writer: the Settle cycle (settle/), at its S4, and no lock is held while
 * the sector is on its way to the medium. What it writes is a snapshot taken
 * at the cycle's S0 under the volume's state lock and then the Ledger's own:
 * the counters exactly as they stood at one instant — the instant of the pool
 * the cycle writes — never a mix of two. The Ledger's lock guards memory only
 * — the seq and copy the volume stands on, and the Use Context it remembers —
 * and is a leaf under the state lock.
 *
 * The counters live in the volume's state (TagFSState.ledger) and are changed
 * there, under the state lock, by whoever changes them. A write never puts
 * anything back into them: a copy taken before the write and stored over them
 * after it would erase every change another core made while the sector
 * travelled — a file id handed out twice, a free count that drifts. Whoever
 * needs a change on the medium asks for a cycle (TagFSSettle).
 */

/* S0, under the volume's state lock: the image the cycle writes, for the
 * copy it does not stand on, with a seq past every one the medium holds. */
void     LedgerImageTake(void);

/* S4, by the Settle's holder with no TagFS lock held: the image, sealed with
 * `mark` (its `settle` is the image's seq), to the medium. OK, and the volume
 * stands on it; or ERR_TAGFS_METADATA_ERROR — said, with the copy the volume
 * still stands on. */
error_t  LedgerImageWrite(const SettleMark *mark);

/*
 * The Use Context the volume remembers — what its person was doing when they
 * left (volume_ledger.h). Kept in the Ledger and written with it, as a text
 * shared by reference with the context that said it (use_context.h).
 *
 * remember: say `said` (the comma-joined text; NULL says none), the `saying`-th
 * word the machine has said, to the volume, and a Settle cycle carries it to
 * the medium — none when the medium already holds exactly it. A word older
 * than one the volume was already told changes nothing: two sayings that race
 * leave the later one whole. *remembered reports whether the volume now holds
 * exactly this text — false when the cycle was refused, when a later word
 * stands, or when the text is longer than the record holds, in which case the
 * volume forgets rather than keeping half. The caller is inside the volume
 * already (use_context.c takes the door) and holds no TagFS lock.
 *
 * recall: what the volume remembers, in one take — a reference the caller
 * drops (UseTextDrop), or NULL when it remembers none — and in *out_saying
 * (optional) which word it is, 0 for the medium's own.
 *
 * peek: one copy of the Ledger read off the medium by the Ledger's own
 * reader, its remembered context included — a proof, not a way in.
 */
error_t  tagfs_remember_use_context(UseText *said, uint64_t saying, bool *remembered);
UseText *tagfs_recall_use_context(uint64_t *out_saying);
bool     tagfs_ledger_peek(uint32_t copy, VolumeLedger *out, char *tail, uint16_t *tail_len);

/* The mount reads both copies and takes up the newer intact one's counters
 * into the volume's state for now. Both unreadable refuses the mount. */
error_t  ledger_load(void);

/* Of the copies ledger_load read: which is the newest intact one, and a
 * copy's seal — false when the copy is not intact or carries none that is
 * its own (LedgerHas, settle == its seq). */
uint32_t LedgerNewestCopy(void);
bool     LedgerCopySeal(uint32_t copy, SettleMark *out);

/* The copy the mount stands on, once it has held the seals against the
 * medium: its record into the volume's state, and its Use Context into the
 * Ledger's own memory; the copies read are let go. A copy that is not intact
 * is the newest. */
void     LedgerStandOn(uint32_t copy);

/* The volume is let go of: what it remembered goes with it, and the next one
 * is read, not assumed. */
void     LedgerLetGo(void);

/* The next file id out of `led`'s counter, which moves past it — the only
 * place an id is issued. A create calls it on the volume's state
 * (TagFSState.ledger) under the state lock. Every id is issued once and none
 * is zero: a counter with no id left, or one an earlier kernel let wrap to 0,
 * is refused ERR_TAGFS_FID_EXHAUSTED, said with kprintf, and left as it is. */
error_t  LedgerIssueFileId(VolumeLedger *led, uint32_t *out_fid);

/* The Ledger's lock. Taken nowhere outside ledger.c: the stand reads its
 * Witnessed count to see whether a Ledger write crossed the medium under it
 * (stand_lock_tests.c). */
extern TagFSLock g_ledger_lock;

#endif /* TAGFS_LEDGER_H */
