#ifndef TAGFS_SETTLE_H
#define TAGFS_SETTLE_H

#include "../tagfs.h"

/*
 * Settle — the cycle that takes what was inked to the medium.
 *
 * One cycle at a time, held by the Settle's Scribe (g_settle_scribe, whose
 * holder is the committer), and no TagFS lock across the medium:
 *
 *   S0  under the state lock, memory only: the target (every Ink so far), the
 *       tail closed onto a Spare when it holds anything unsettled, the pool's
 *       closed blocks and tail taken, the Ledger's image taken;
 *   S1  the tag registry, the block map, the file table, the integrity map —
 *       the digests of the ground at rest zeroed first;
 *   S2  FLUSH#1, when the cycle names blocks or tags a kept flush does not
 *       yet cover: the data and the tags lie on the medium before any record
 *       names them;
 *   S3  the pool's closed blocks, then its tail;
 *   S4  the Ledger, into its older copy — the only writer it has — sealed
 *       with the cycle's SettleMark (volume_ledger.h): the pool from
 *       chain_first to where this cycle's records end, the registry up to
 *       its last record, each with the CRC32 a mount holds the medium to;
 *   S5  FLUSH#2;
 *   S6  under the state lock: settled_gen is the target, the closed blocks
 *       written leave memory, and Fallow gives back what has rested enough.
 *
 * A refusal anywhere from S1 on declares nothing settled: said once for the
 * episode, answered to every waiter the cycle covered, and every step is one
 * the next cycle makes again the same way — the same bytes to the same
 * places, the same older copy of the Ledger.
 *
 * In step 1.4 a cycle runs when somebody asks for one and waits: an anchor, a
 * demand (the Use Context remembered, the integrity map named), a volume short
 * of room while Fallow rests blocks, and the machine stopping. Cadence — Lull
 * — arrives with step 1.7.
 */

extern Scribe g_settle_scribe;

/* A cycle that began after this call covers every Ink made before it:
 * OK once it settled, or its refusal. With no TagFS lock held; takes the
 * volume's door itself. */
error_t  TagFSSettle(void);

/* The volume is short and Fallow rests blocks: cycles run until some come
 * back — at most 1 + FallowLag() of them. OK when blocks came back,
 * ERR_TAGFS_NO_SPACE when none rest, or a cycle's refusal; ERR_WOULD_BLOCK at
 * once for a caller that may not wait for a cycle — under a TagFS lock or a
 * Scribe, or a baton continuation. Inside the door. */
error_t  SettleForRoom(void);

/*
 * The machine is stopping (RT-12): the last cycles, run by the halt itself
 * before interrupts go off, right after the asynchronous writes drained — and
 * never after the cores are stopped, when a committer could be frozen in the
 * middle of the medium. They give back every Spare no link names, and every
 * block Fallow rests, as its rule allows once they settled — so no clean
 * shutdown leaves a block taken that nothing names. A committer whose core is
 * no longer running is an abandoned cycle: it is taken over and begun again
 * from S0. The answer is kept for tagfs_shutdown.
 */
error_t  TagFSSettleForHalt(void);

/* What the halt still owes the volume, read with no lock: Inks no cycle has
 * settled, and whether a cycle is running on another core that still runs —
 * nothing at all when no volume is mounted. */
void     SettleOwed(uint64_t* unsettled, bool* committer_busy);

/* What the halt's last cycle answered — ERR_TAGFS_METADATA_ERROR when none
 * ran — and how many Inks no cycle has settled now. */
error_t  SettleHaltAnswer(uint64_t* unsettled);

/* The settled gen and the settled cycles — for the stand. */
uint64_t SettledGen(void);
uint64_t SettledCycles(void);

/* A mount's first word: nothing settled yet. With the door shut. */
void     SettleMountReset(void);

/*
 * What the mount stood on, for the seals the cycles of this mount write: the
 * pool block and the bytes of its records the first cycle's own records begin
 * after — the stood seal's pool_to, or the tail as a mount with no seal found
 * it — and whether the medium was ever found holding less than a settled
 * cycle's seal said (SETTLE_MEDIUM_LIED): every seal from now on carries it,
 * and ground Fallow rests waits one cycle more for good. With the door shut.
 */
void     SettleMountStand(uint32_t from_block, uint32_t from_bytes, bool medium_lied);

/* Whether this volume's seals carry SETTLE_MEDIUM_LIED. */
bool     SettleMediumLied(void);

#endif /* TAGFS_SETTLE_H */
