#ifndef TAGFS_LEDGER_H
#define TAGFS_LEDGER_H

#include "../../../lib/kernel/ktypes.h"
#include "../../../include/volume_ledger.h"
#include "../../core/error/error.h"
#include "../use_memory/use_memory.h"
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
 * the cycle writes — never a mix of two, and the name of the Use Context's
 * chain the cycle leaves (use_memory.h). The Ledger's lock guards memory only
 * — the seq and copy the volume stands on — and is a leaf under the state
 * lock. No text is ever laid in the Ledger's sector.
 *
 * The counters live in the volume's state (TagFSState.ledger) and are changed
 * there, under the state lock, by whoever changes them. A write never puts
 * anything back into them: a copy taken before the write and stored over them
 * after it would erase every change another core made while the sector
 * travelled — a file id handed out twice, a free count that drifts. Whoever
 * needs a change on the medium asks for a cycle (TagFSSettle).
 */

/* S0, under the volume's state lock: the image the cycle writes, for the
 * copy it does not stand on, with a seq past every one the medium holds —
 * naming no Use Context's chain until LedgerImageUseChain names one. The
 * image's seq and the copy it goes to, once taken. */
void     LedgerImageTake(void);
uint64_t LedgerImageSeq(void);
uint32_t LedgerImageSlot(void);
void     LedgerImageUseChain(const UseChainRef *ref);

/* S4, by the Settle's holder with no TagFS lock held: the image, sealed with
 * `mark` (its `settle` is the image's seq), to the medium. OK, and the volume
 * stands on it; or ERR_TAGFS_METADATA_ERROR — said, with the copy the volume
 * still stands on. */
error_t  LedgerImageWrite(const SettleMark *mark);

/* One copy of the Ledger read off the medium by the Ledger's own reader, and
 * in *memory (optional) what it remembers of the Use Context read off the
 * medium too — its chain followed and proven (UseChainRead), or the text a
 * kernel before the chain kept in it; NULL when it remembers none or its
 * chain does not read whole. A proof, not a way in. */
bool     tagfs_ledger_peek(uint32_t copy, VolumeLedger *out, UseText **memory);

/* The mount reads both copies and takes up the newer intact one's counters
 * into the volume's state for now, and what the copies remember of the Use
 * Context right after (UseMemoryLoad). Both unreadable refuses the mount. */
error_t  ledger_load(void);

/* Of the copies ledger_load read: which is the newest intact one, and a
 * copy's seal — false when the copy is not intact or carries none that is
 * its own (LedgerHas, settle == its seq). */
uint32_t LedgerNewestCopy(void);
bool     LedgerCopySeal(uint32_t copy, SettleMark *out);

/* Of the copies ledger_load read, for the Use Context's memory (use_memory.h):
 * a copy's record — NULL when it is not intact — and the text a kernel before
 * the chain kept in it, held (UseTextDrop), NULL when it keeps none. */
const VolumeLedger *LedgerCopyRead(uint32_t copy);
UseText            *LedgerCopyInherited(uint32_t copy);

/* The copy the mount stands on, once it has held the seals against the
 * medium: its record into the volume's state; the copies read are let go. A
 * copy that is not intact is the newest. */
void     LedgerStandOn(uint32_t copy);

/* The volume is let go of: the copies read and what it remembered go with it,
 * and the next one is read, not assumed. */
void     LedgerLetGo(void);

/*
 * A medium that left and came back, asked whether its Ledger is as this kernel
 * left it (mount/away.h).
 *
 * LedgerNewestOn: the newest intact copy on `seat`, the volume's ground
 * beginning at sector `base` — read by the Ledger's own reader and nothing
 * else of the volume's; false when neither copy reads intact.
 *
 * LedgerLeftAs: that copy against what this kernel left — the record the
 * volume stands on once its cycle settled (or the mount stood on), or the
 * image the last S4 aimed at the other copy, landed or not (before any S4, a
 * newer copy the mount would not stand on) — each by its seq and its seal's
 * {settle, closed_crc32, tail_crc32}. AS_LEFT: one of the two,
 * so no BoxOS kernel settled a cycle on the medium meanwhile (a host that
 * changed bytes and left the Ledger alone is not seen). OLDER: a seq below
 * the one a settled cycle wrote — the medium lost a cycle it said it kept, or
 * an older image of it was written back. OTHER: anything else. In
 * *out_settled_seq, the seq of the record that settled.
 *
 * LedgerSettled: at a cycle's S6, the image its S4 wrote is what the volume
 * stands on.
 */
typedef enum { LEDGER_AS_LEFT, LEDGER_OLDER, LEDGER_OTHER } LedgerLeft;
bool       LedgerNewestOn(uint8_t seat, uint64_t base, VolumeLedger *out);
LedgerLeft LedgerLeftAs(const VolumeLedger *newest, uint64_t *out_settled_seq);
void       LedgerSettled(void);

/* The next file id out of `led`'s counter, which moves past it — the only
 * place an id is issued. A create calls it on the volume's state
 * (TagFSState.ledger) under the state lock. Every id is issued once and none
 * is zero: a counter with no id left, or one an earlier kernel let wrap to 0,
 * is refused ERR_TAGFS_FID_EXHAUSTED, said with kprintf, and left as it is. */
error_t  LedgerIssueFileId(VolumeLedger *led, uint64_t *out_fid);

/* The Ledger's lock. Taken nowhere outside ledger.c: the stand reads its
 * Witnessed count to see whether a Ledger write crossed the medium under it
 * (stand_lock_tests.c). */
extern TagFSLock g_ledger_lock;

#endif /* TAGFS_LEDGER_H */
