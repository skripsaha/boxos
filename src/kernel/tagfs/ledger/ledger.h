#ifndef TAGFS_LEDGER_H
#define TAGFS_LEDGER_H

#include "../../../lib/kernel/ktypes.h"
#include "../../../include/volume_ledger.h"
#include "../../core/error/error.h"
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
 * One writer at a time — the Ledger's Scribe (scribe.h) — and that writer
 * holds no lock while the sector is on its way to the medium. What it writes
 * is a snapshot taken under the volume's state lock and then the Ledger's
 * own: the counters exactly as they stood at one instant, never a mix of two.
 * Its lock guards memory only — the seq and copy the volume stands on, and
 * the Use Context it remembers — and is a leaf under the state lock.
 *
 * The counters live in the volume's state (TagFSState.ledger) and are changed
 * there, under the state lock, by whoever changes them. A write never puts
 * anything back into them: a copy taken before the write and stored over them
 * after it would erase every change another core made while the sector
 * travelled — a file id handed out twice, a free count that drifts.
 */

/*
 * Write the Ledger: the medium then holds a copy no older than this call.
 *
 * Change a counter or a field under the volume's state lock FIRST, let the
 * lock go, then call this: the pass that answers the call takes its snapshot
 * after the call asked, so it carries the change. Called with no TagFS lock
 * held — the call may wait for a pass another core is making.
 *
 *   OK                        a pass that began after this call wrote a copy
 *                             the medium took;
 *   ERR_TAGFS_METADATA_ERROR  that pass was refused by the medium (said, with
 *                             the copy the volume still stands on), or this
 *                             volume has no room for two copies;
 *   ERR_WOULD_BLOCK           a Ledger write on this very core is suspended
 *                             beneath this call — waiting for it here would
 *                             never end. What was changed goes out with the
 *                             next Ledger write.
 */
error_t  tagfs_write_ledger(void);

/*
 * The Use Context the volume remembers — what its person was doing when they
 * left (volume_ledger.h). Kept in the Ledger and written with it.
 *
 * remember: say `list` (comma-joined tag names, `len` bytes, no NUL; len 0
 * says none) to the volume and write the Ledger. *remembered reports whether
 * the volume now holds exactly this list — false when the medium would not
 * take the write, or when the list is longer than the record holds, in which
 * case the volume forgets rather than keeping half. Takes no door: the
 * caller is inside the volume already (use_context.c takes it).
 *
 * recall: copy what the volume remembers into `buf` when it fits `cap`;
 * returns the byte length either way (0 = none), so a caller can size one.
 *
 * peek: one copy of the Ledger read off the medium by the Ledger's own
 * reader, its remembered context included — a proof, not a way in.
 */
error_t  tagfs_remember_use_context(const char *list, size_t len, bool *remembered);
size_t   tagfs_recall_use_context(char *buf, size_t cap);
bool     tagfs_ledger_peek(uint32_t copy, VolumeLedger *out, char *tail, uint16_t *tail_len);

/* The mount takes up the newer intact copy — the counters into the volume's
 * state, the copy it stands on and its Use Context into the Ledger's own
 * memory. Both unreadable refuses the mount. */
error_t  ledger_load(void);

/* The volume is let go of: what it remembered goes with it, and the next one
 * is read, not assumed. */
void     LedgerLetGo(void);

/* The Ledger's lock. Taken nowhere outside ledger.c: the stand reads its
 * Witnessed count to see whether a Ledger write crossed the medium under it
 * (stand_lock_tests.c). */
extern TagFSLock g_ledger_lock;

#endif /* TAGFS_LEDGER_H */
