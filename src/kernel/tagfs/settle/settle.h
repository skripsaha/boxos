#ifndef TAGFS_SETTLE_H
#define TAGFS_SETTLE_H

#include "../tagfs.h"

/*
 * Settle — the cycle that takes what was inked to the medium.
 *
 * One cycle at a time, held by the Settle's Scribe (g_settle_scribe, whose
 * holder is the committer), and no TagFS lock across the medium:
 *
 *   Glean's pass (glean.h), but in a machine's last cycles: blocks at the pool
 *       chain's front taken off, their live records laid again in the tail;
 *   S0  under the state lock, memory only: the target (every Ink so far), the
 *       tail closed onto a Spare when a waiter is owed a closed block, the
 *       pool's closed blocks and tail taken, the Ledger's image taken, naming
 *       the Use Context's chain — a new one, its blocks staked just before S0
 *       with no lock held, when the person's word changed (use_memory.h);
 *   S1  the tag registry, the block map, the file table, the pool's head
 *       named again once the chain begins past it, the integrity map — the
 *       digests of the ground at rest zeroed first — and the new chain's
 *       links;
 *   S2  FLUSH#1, when the cycle names blocks or tags a kept flush does not
 *       yet cover: the data, the tags and the chain lie on the medium before
 *       any record names them;
 *   S3  the pool's closed blocks, then its tail;
 *   S4  the Ledger, into its older copy — the only writer it has — sealed
 *       with the cycle's SettleMark (volume_ledger.h): the pool from
 *       chain_first to where this cycle's records end, the registry up to
 *       its last record, each with the CRC32 a mount holds the medium to;
 *       then the pool's reserve of Spares made whole again, as it was just
 *       before S0 — their stamps are writes S5 keeps (meta_pool.h);
 *   S5  FLUSH#2;
 *   S6  under the state lock: settled_gen is the target, the closed blocks
 *       written leave memory, the demand for the Use Context is answered, and
 *       Fallow gives back what has rested enough; then every waiter the cycle
 *       covered is answered.
 *
 * ‼ THE CYCLE IS AN AUTOMATON OF STEPS. The committer runs one step — S1's
 * parts one at a time, S3 one block at a time — and goes back to its loop
 * between two: the next pocket, one (kcore.c), the batons and the halt's
 * pass, the services and the volume's Helm work. It holds the Scribe from S0
 * to S6, so every other asker still waits for, or is answered by, the cycle in
 * flight; one on the committer's own core, between two of its steps, runs the
 * rest of the cycle itself rather than wait for itself.
 *
 * A refusal anywhere from S1 on declares nothing settled: said once for the
 * episode, and every step is one the next cycle makes again the same way —
 * the same bytes to the same places, the same older copy of the Ledger.
 */

extern Scribe g_settle_scribe;

/* A cycle that began after this call covers every Ink made before it, and lays
 * every record up to it in a closed block: OK once it settled, or its refusal.
 * A kernel path's own demand — the integrity map's new place, the stand — with
 * no TagFS lock held; takes the volume's door itself. A program never waits
 * here: an anchor waits through its Chit (SettleWaitLink). */
error_t  TagFSSettle(void);

/* The volume is short and Fallow rests blocks, or Glean may take the pool's
 * slack off its chain: cycles run until some come back — at most
 * 2 + FallowLag() of them — for a caller in the kernel standing in its call; a
 * program's call never runs one (helm.h, settle_ops.h). OK when the cycles gave
 * ground back, or when nothing rests and the chain has no slack — the take
 * asked again then says the last word itself — or after one cycle of a sweep
 * of Glean's a delete waits on (glean.h); ERR_TAGFS_NO_SPACE when none came
 * back; or a cycle's refusal. ERR_WOULD_BLOCK at once for a caller that may not
 * wait for a cycle — under a TagFS lock, a Scribe or a baton continuation —
 * but the Settle's own holder between two steps of its cycle, which runs that
 * cycle to its end first. Inside the door. */
error_t  SettleForRoom(void);

/*
 * The machine is stopping (RT-12): the last cycles, run by the halt itself
 * before interrupts go off, right after the asynchronous writes drained — and
 * never after the cores are stopped, when a committer could be frozen in the
 * middle of the medium. They give back every Spare no link names, and every
 * block Fallow rests, as its rule allows once they settled — or, held only by
 * an open handle, write it free in the map — so no clean shutdown leaves a
 * block taken that nothing names. From the halt's shut on, a cycle anybody
 * begins is one of the halt's. A committer whose core is no longer running is
 * an abandoned cycle: it is taken over and begun again from S0. A volume whose
 * medium is away runs no cycle: the stopping machine is the event its open
 * book waited for, and the book is given up — every waiter answered
 * (mount/away.h). The answer is kept for tagfs_shutdown. They seal the volume
 * clean only when no change is in flight: none holds or waits for a file's
 * Helm — a write's bytes in place may land after the seal, and its digest with
 * them or not, and a clean seal is the next mount's word that every digest on
 * the medium stands (RT-22, mount.c MountAfterDoor).
 */
error_t  TagFSSettleForHalt(void);

/*
 * The machine is stopping (RT-12), said by the halt before its drain begins:
 * from here every cycle begun is one of the halt's, and the volume takes no
 * change that has not begun — a write, a span, a defrag, a create, a rename, a
 * tag, a delete is refused ERR_DEVICE_NOT_READY before anything of it is done
 * (SettleHalting, asked by the first half of every change). One begun before
 * runs to its end, a window at most, and the halt's cycles settle it. A change
 * begun after the halt's last cycle took its snapshot would be on nobody's
 * books: a write in place lands with the digest of the bytes before it under a
 * clean seal, and reads as rot on the next mount; and changes nobody stops keep
 * the drain going for ever. Raised once, never lowered.
 */
void     TagFSShutForHalt(void);
bool     SettleHalting(void);

/* What the halt still owes the volume, read with no lock: Inks no cycle has
 * settled, whether a cycle is running on another core that still runs, and
 * whether the last cycle that settled sealed the volume clean — nothing at all
 * when no volume is mounted. And the steps every cycle took this boot, on any
 * core, the halt's own too: a cycle that still takes them is going on,
 * however long it is. */
void     SettleOwed(uint64_t* unsettled, bool* committer_busy, bool* sealed_clean);
uint64_t SettleStepsTaken(void);

/* What the halt's last cycle answered — ERR_TAGFS_METADATA_ERROR when none
 * ran — and how many Inks no cycle has settled now. */
error_t  SettleHaltAnswer(uint64_t* unsettled);

/* The settled gen and the settled cycles — for the stand. */
uint64_t SettledGen(void);
uint64_t SettledCycles(void);

#ifdef CONFIG_VOLUME_TESTS
/* A cycle begun as the cadence begins one — it closes the tail only as far as
 * a waiter is owed — and run to its end on this core: OK once it settled,
 * ERR_BUSY when another asker holds the Settle, or the cycle's refusal. For
 * the stand, whose runner holds the cadence: cycles that close for nobody let
 * Glean bring the pool's chain down to what its live records fill. */
error_t  TagFSSettleAsCadence(void);

/* A cycle begun as the cadence begins one, on this core, and left standing
 * before its first step — this core then holds the Settle between two of its
 * steps, as a committer serving a pocket does: true, or false when another
 * asker holds it. The next cycle asked for on this core runs it to its end
 * first (TagFSSettle). For the stand. */
bool     TagFSStandSettleBegin(void);

/* The next cycle's seat rings seat:ready once, as a driver's recovery would
 * while the cycle runs — noted, and said later by the room's pass: just before
 * the cycle takes the count a refusal of it is judged by (`before_its_count`),
 * or right after its S0. For the stand. */
void     TagFSStandSettleRings(bool before_its_count);

/* The next cycle's block map answers `code` once its own writes are done —
 * on a book, the making of its stitch answers it before anything is made — a
 * refusal that is not the medium's (ERR_NO_MEMORY, ERR_WOULD_BLOCK), as a
 * step meets one. For the stand. */
void     TagFSStandSettleRefuses(error_t code);

/* The demands standing for the next cycle (SettleDemandKind), read and left
 * as they are. For the stand. */
uint32_t TagFSStandSettleDemands(void);

/* The cycles TagFSSettleForHalt runs, run here without stopping anything —
 * every later cycle is the stand's own again: OK once they settled, or the
 * refusal. For the stand, inside the door. */
error_t  TagFSStandSettleForHalt(void);

/* The volume shut for a halt as TagFSShutForHalt shuts it — said nowhere —
 * and, for the stand alone, opened again (`shut` false). */
void     TagFSStandShutForHalt(bool shut);
#endif

/* A mount's first word: nothing settled yet, and no waiter of the volume that
 * went before it left unanswered. With the door shut. */
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

// ============================================================================
// Cadence — what starts a cycle (cadence.c)
//
// ‼ LULL PERMITS; AN EMPTY QUEUE, THE WINDOW RUN OUT, OR A DEMAND STARTS. Every
// Ink wakes the committer's core, right where it is made (cadence.c). That core
// runs the cycle at its service point, which its loop reaches after every
// pocket it serves (RT-11) — and a cycle Lull permits begins only once no
// pocket waits on its queue, so it never stands before work already handed
// in. Lull is k·D, D the last cycle's own time on the medium, measured, and
// k = 100/SETTLE_SHARE − 1: settling takes no more than SETTLE_SHARE percent
// of the medium's time. It is a budget, never a timeout — nothing is refused
// when it runs out, a cycle is only allowed. Clamped to one PIT tick below and
// the window above.
//
// The window is the promise — no change of the volume waits longer than
// SETTLE_WINDOW_US for its cycle to begin, ext4's own five seconds: once the
// oldest unsettled change is that old, or once a demand comes (a waiter in a
// Chit, a tail that filled, a volume short while Fallow rests blocks, a
// program leaving with changes unsettled), the cycle begins between two
// pockets, not only after the last one.
//
// A frequent small writer — small appends to one file, each smaller than what
// settling it costs the medium, again before a Lull has passed (on a book,
// before the window has: its cycles come at most one a window) — or one whose
// file wears settle:lazy, does not start cycles by Lull: its changes wait for
// the window or a demand.
// ============================================================================

#define SETTLE_SHARE      5u                  /* percent of the medium's time */
#define SETTLE_WINDOW_US  5000000ull          /* the promise: ext4's commit interval */

typedef enum {
    SETTLE_DEMAND_ROOM  = 1u << 0,  /* the allocator is short while Fallow rests blocks,
                                       or while Glean may take the pool's slack off */
    SETTLE_DEMAND_LEAVE = 1u << 1,  /* a program left with changes no cycle settled */
    SETTLE_DEMAND_TALLY = 1u << 2,  /* a book's ribbon wants a Tally to take it in: no next_quire
                                       could be given — a Tally's work, not a cycle's (binder.h) */
    SETTLE_DEMAND_ERRATA = 1u << 3, /* a book's mount holds a verdict: the session's first cycle lays it
                                       down at once, never after a Lull (§3.5, mount/errata.h) */
} SettleDemandKind;

/* An Ink was made (after its lock was dropped): the window's clock starts for
 * it, and the committer is woken. `lazy`: a frequent small writer's, or its
 * file wears settle:lazy — it does not start a cycle by Lull. */
void     SettleInked(uint64_t gen, bool lazy);

/* A demand: the next cycle begins at the committer's next service point, with
 * pockets waiting or not. */
void     SettleDemand(uint32_t kinds);

/* The committer woken, to look again: a waiter linked, seat:ready, a medium
 * that turned out to take no write. From any context. */
void     SettleKnock(void);

/* The committer's service point: one step of the cycle this core runs, or the
 * start of one that is due, or the Lull's node armed for when one will be.
 * While pockets wait on this core's queue, only the window run out or a demand
 * starts a cycle. On a core that is not the committer's, and runs no cycle, it
 * does nothing. */
void     SettleServiceIfPending(void);

/* Whether this core owes the Settle a turn of its loop before it sleeps: a step
 * of the cycle it runs, a cycle due, or one owed later that no Lull is armed
 * yet to wake it for. */
bool     SettleStepOwedHere(void);

/* On one core: the idle context is owed the next quantum — a cycle in flight,
 * or the window run out, or a demand (RT-10). */
bool     SettleIdleOwed(void);

/* The volume's open book was taken up: a cycle refused by the medium that left
 * may run. Counted, never a flag (settle_wait.c). seat:ready of the volume's
 * seat is counted by the room where its bell is noted, and only knocks here. */
void     SettleSeatReady(void);

/* Nightwatch's look from the PIT tick (RT-11): `now_us`. A debt older than the
 * window that no committer took up, twice at looks a look apart, is said once
 * for the episode. */
void     SettleWatchLook(uint64_t now_us);

/* The Lull now, and the last cycle's time on the medium — for the stand. */
uint64_t SettleLullUs(void);
uint64_t SettleLastDUs(void);

/* A hold on the cadence: while it stands no cycle begins on its own. The test
 * runner's — a test meets only the cycles it drives, and a fault it arms on
 * the volume is met by its own cycle; the tests of the cadence itself let it
 * go for their length — and a stopping machine's, for good, from the start of
 * its drain: the halt runs the volume's last cycles itself. Counted; a cycle
 * in flight runs to its end. */
void     SettleCadenceHold(bool hold);

/*
 * Format v2's writer (settle/sewing.c). StitchBookRefused: no Quire took a
 * stitch's write — the volume may only be read until it is mounted again, part
 * of what TagFSReadOnly answers (M-6). StitchWearsVerdict: this core is the
 * Settle's holder putting settle:undone on the files of the mount's verdict,
 * before its cycle's snapshot — Inks of the cycle itself, which a stopping
 * machine's halt does not refuse (ink.c, InkVolumeBegins).
 */
bool     StitchBookRefused(void);
bool     StitchWearsVerdict(void);

/* A part of format v1's cycle — the pool's Spares, Glean, the Ledger's write —
 * reached on a book: a defect of the road there, which the cycle's dispatch by
 * format shuts (settle.c). Said once a mount and stopped at in a STRICT
 * kernel; true then, and the caller does nothing. False on format v1. With no
 * TagFS lock held. */
bool     SettleV1OnBook(const char* what);

// ============================================================================
// Waiting for a cycle through a Chit (settle_wait.c)
//
// ‼ ONE RULE FOR EVERY WAITER. A program's anchor, its OBJ_WRITE_SETTLED, its
// Use Context remembered: the handler parks the program behind its Chit and
// links a waiter; a cycle that settles the waiter's Ink and lays it in a closed
// block answers it at S6. A medium that takes no write answers every waiter
// with ERR_TAGFS_READ_ONLY; one that left holds them with its open book
// (mount/away.h) — the next cycle answers them once the same volume comes back
// as it was left, and the book given up answers them ERR_DEVICE_NOT_READY; any
// other refusal leaves them in their Chit, and the cadence begins no cycle
// until the event the refusal waits for: seat:ready for the medium's, the
// machine's memory coming back, room asked for (SettleWaitersPark). No attempt
// is counted, and nothing here is ever a timeout; a park that waiters wait on
// longer than the window is said by Nightwatch, once.
// ============================================================================

typedef struct SettleWaiter SettleWaiter;
struct SettleWaiter {
    SettleWaiter* next;
    uint64_t      gen;      /* the Ink it waits for — or 0 for one waiting for ... */
    uint64_t      cycle;    /* ... the cycle this settled cycle count reaches */
    /* The answer, run from a service point or the committer with no TagFS
     * lock held; the waiter is the callee's from here. */
    void        (*answer)(SettleWaiter* w, error_t rc);
};

/* Whether a wait for Ink `gen` is needed: OK or a code when it is answered
 * already, ERR_WOULD_BLOCK when it is not. Inside the door. */
error_t  SettleWaitNow(uint64_t gen);

/* The waiter linked — after the program was parked and its Chit given — and
 * the committer knocked. Inside the door. */
void     SettleWaitLink(SettleWaiter* w, uint64_t gen,
                        void (*answer)(SettleWaiter* w, error_t rc));

/* A waiter for the next cycle whose S0 comes after this call — what a word
 * said to the Ledger's memory waits for (the Use Context): answered when that
 * cycle settles, by the same rule. Inside the door. */
void     SettleWaitLinkCycle(SettleWaiter* w, void (*answer)(SettleWaiter* w, error_t rc));

/* A waiter taken back unanswered — the stand's own, on its way out. True when
 * it was still linked. */
bool     SettleWaitUnlink(SettleWaiter* w);

/* For the cycle and the cadence (settle.c, cadence.c). SettleParked: a
 * refused cycle's park stands — the event it waits for has not come — and the
 * cadence begins no cycle. */
bool     SettleWaitersOwed(void);
uint64_t SettleWaitersCloseUpto(void);
void     SettleWaitersRunEnded(error_t rc, uint64_t target, bool unclosed);
void     SettleWaitersSweep(void);
void     SettleWaitersAnswerAll(error_t code);
bool     SettleParked(void);

#endif /* TAGFS_SETTLE_H */
