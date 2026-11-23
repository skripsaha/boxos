#ifndef TAGFS_SETTLE_INTERNAL_H
#define TAGFS_SETTLE_INTERNAL_H

#include "settle.h"
#include "baton.h"
#include "amp.h"
#include "touch_queue.h"

/*
 * What the cycle (settle.c), its cadence (cadence.c) and its waiters
 * (settle_wait.c) say to each other, and to nobody else.
 */

/* Why a cycle runs: its reason. */
typedef enum {
    SETTLE_FOR_WAITER = 1,   /* a kernel path asks and waits for what it settles */
    SETTLE_FOR_ROOM,         /* the volume is short while Fallow rests blocks */
    SETTLE_FOR_HALT,         /* the machine is stopping: the last cycle */
    SETTLE_FOR_CADENCE,      /* Lull permitted, or the window ran out, or a demand */
} SettleWhy;

/* The core the cadence runs cycles on: the only core of a one-core machine;
 * else a K-Core that is not the drain core when there is one — the drain core
 * runs every continuation of the machine — and the drain core when it is the
 * only K-Core. */
uint8_t  SettleCommitterCore(void);

/* settle.c: whether a cycle is in flight — whether it is this core's and
 * stands between two of its steps settle.h says; one step of it; a cycle begun
 * by the cadence when the Scribe is free — false when another asker holds it. */
bool     SettleRunInFlight(TagFSVolume* vol);
void     SettleRunStepHere(TagFSVolume* vol);
bool     SettleRunBeginForCadence(TagFSVolume* vol);

/* cadence.c: a cycle took its snapshot — the demands it answers, taken; and
 * the cycle ended: its answer, its own time on the medium, when its S0 was, and
 * the Ink it settled up to. How many times room was asked for (SettleDemand's
 * ROOM), counted: the event a cycle refused for want of ground waits for. */
uint32_t CadenceRunBegins(TagFSVolume* vol);
void     CadenceRunEnded(TagFSVolume* vol, error_t rc, uint64_t busy_us, uint64_t s0_us, uint64_t target);
void     CadenceMountReset(TagFSVolume* vol);
uint64_t CadenceRoomAsked(TagFSVolume* vol);

/* settle_wait.c: a cycle took its snapshot — the seat:ready it began after is
 * noted, so a refusal can tell whether the medium already said it answers
 * again; the cycle's end, by its holder before the Give: the park its refusal
 * leaves, or none once it settled (the rule over SettleWaitersPark); and a
 * mount's first word for the waiters. */
void     SettleWaitersRunBegins(TagFSVolume* vol);
void     SettleWaitersPark(TagFSVolume* vol, error_t rc);
void     SettleWaitersMountReset(TagFSVolume* vol);

/* settle_wait.c: when the park standing now was decided — 0 when none stands;
 * and its one line, for Nightwatch (run from a baton). */
uint64_t SettleParkedSinceUs(TagFSVolume* vol);
void     SettleParkSay(TagFSVolume* vol);

/* The time the cadence counts in: microseconds of the TSC. */
uint64_t SettleNowUs(void);

#ifdef CONFIG_VOLUME_TESTS
/* settle.c: `rc`, or — when it is OK — the refusal the stand chose for the
 * next cycle, once (TagFSStandSettleRefuses). */
error_t  SettleStandRefusalHere(error_t rc);

/* settle.c: the volume's seat rings seat:ready here, once, when the stand
 * chose this turn of the cycle (TagFSStandSettleRings): just before the cycle
 * takes the count its refusal is judged by, or right after its TAKE. */
void     SettleStandRingsHere(TagFSVolume* vol, bool before_its_count);
#endif

/* settle.c: a cycle settled — every Ink up to `target`, as the `cycle`-th of
 * this mount, the volume sealed clean or not — under the state lock, at the
 * cycle's DONE. */
void     SettleSettledLocked(TagFSVolume* vol, uint64_t target, uint64_t cycle, bool clean);

/* settle.c: the blocks a settled cycle's DONE gave back from Fallow, under the
 * state lock — what an asker for room judges a cycle by (SettleForRoom). */
void     SettleGivenLocked(TagFSVolume* vol, uint64_t given);

/* settle.c: a cycle asked for and waited through — this call's own, or one
 * another core was making — for `why`; its answer (settle_halt.c). And a
 * mount's first word for the halt. */
error_t  SettleAskedFor(TagFSVolume* vol, SettleWhy why);
void     SettleHaltMountReset(TagFSVolume* vol);

/* The measure from above of each tree of a whole Tally, in blocks of one
 * copy. */
typedef struct {
    uint64_t files;
    uint64_t tags;
    uint64_t use;
    uint64_t digests;
} TallyCostParts;

/* What one volume holds of endpaper.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    TallyCostParts kept_above;  /* the last measure from above, in blocks */
    volatile uint64_t kept_from;  /* the data handed when the measure before it began */
    volatile uint64_t kept_began;  /* ... and when this one began */
    volatile uint32_t kept_epoch;  /* the mounting they are of; 0 for none */
    volatile uint32_t kept_turn;  /* odd while a measure is being kept */
} EndpaperState;

/* The cycle in flight: the Scribe holder's alone, but `active` and `core`,
 * which the cadence of every core reads. */
typedef struct {
    volatile uint8_t active;
    volatile uint8_t core;
    bool             in_step;
    bool             door;       /* the cycle holds the volume's door itself */
    SettleWhy        why;
    error_t          answer;     /* how the last cycle ended */
    uint64_t         cover;      /* the Scribe tickets it answers */
    uint64_t         s0_us;
    uint64_t         busy_us;    /* its own time on the medium, step by step */
} SettleRun;

/* What one volume holds of settle.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    /*
     * The Settle cycle (settle.h): a book's stitch, sewn step by step (sewing.c).
     * Its holder is the one committer, and is never switched away from while it
     * holds the Scribe (scribe.h). Between two steps the holder's core is back in
     * its loop, and allocates there as any other work does: the heap's reserve is
     * the steps' alone.
     */
    Scribe scribe;
    /* Under the state lock. */
    uint64_t settled_gen;
    uint64_t cycles;  /* cycles settled this mount; the next is settle.cycles + 1 */
    /* Said once for each run of refused cycles, until one settles. */
    bool failing;
    /* Every block the cycles ever gave back, counted under the state lock: what
     * an asker for room judges a cycle by (SettleForRoom). */
    uint64_t given_count;
    /* Whether the newest settled cycle sealed the volume clean: the halt's last
     * word on the medium, asked by its drain (SettleOwed). */
    volatile bool sealed_clean;
    /* Steps any cycle took this boot — how the halt's drain sees a cycle on
     * another core going on (SettleStepsTaken). */
    volatile uint64_t steps;
    SettleRun run;
    /* The refusal of a cycle on a volume that takes no write — under glass, or a
     * book no Quire took a write of — said once a mount. */
    volatile uint8_t refusal_said;
} SettleState;

/* What one volume holds of settle_halt.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    /* The halt's last cycle, for tagfs_shutdown. */
    bool halt_ran;
    error_t halt_answer;
    /* The machine is stopping: from the halt's shut on, every cycle begun is one
     * of its last (SettleRunBegin), and no change begins (TagFSShutForHalt).
     * Raised once, never lowered. */
    volatile bool halting;
} SettleHaltState;

/* What one volume holds of cadence.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    volatile uint64_t d_us;  /* the last settled cycle's own time on the medium */
    volatile uint64_t last_end_us;  /* when the last cycle ended */
    volatile uint64_t owed_since_us;  /* when the oldest change no cycle settled was inked */
    volatile uint64_t eager_gen;  /* the newest Ink that starts a cycle by Lull */
    volatile uint32_t demand;  /* SettleDemandKind */
    volatile uint64_t lull_fire;  /* the tick the Lull's node was last armed for */
    volatile int32_t held;  /* the test runner's holds */
    volatile uint64_t room_asked;  /* SETTLE_DEMAND_ROOM, counted */
    bool binds_said;  /* the window binds the Lull: said once an episode */
    TouchQueueNode lull_node;
    /*
     * ‼ A PROGRAM'S DEBT IS ITS OWN INK, OF THIS MOUNT. What a program leaves
     * unsettled is the newest Ink one of its own calls made — never the volume's
     * newest, which a reader that leaves would otherwise owe for a writer still
     * running, and never an Ink of a mount before this one, whose gens begin again
     * at 1. So an Ink has a place in one order across every mount of the boot —
     * this mount's base and its gen — and a program keeps the newest place of its
     * own (process_t, volume_ink_place): a place below this mount's base is a
     * mount's before it, and owes nothing here. One number, so it is kept by one
     * compare-and-swap from any core that serves the program.
     *
     * The core that made an Ink remembers its place: the guide serves a program's
     * call without giving its core away, so an Ink the core made while serving it
     * is the call's own (TagFSInkedHere, read before and after the handler).
     */
    volatile uint64_t ink_base;  /* the place of this mount's Ink 0 */
    volatile uint64_t ink_top;  /* the newest gen of this mount given a place */
    volatile uint64_t inked_here[MAX_CORES];  /* the place of each core's newest Ink */
    volatile uint64_t inked_call[MAX_CORES];  /* and the number of the call that core was serving (TagFSCallBegins) */
    /*
     * ‼ A TALLY GOES A STEP A TURN, BETWEEN THE CYCLES (binder.h, §4.3). Its step
     * is the committer's work too: taken when no cycle is due, and — so a stream
     * of cycles never starves it — the next turn after each cycle ends while one
     * is under way. A waiter's cycle waits for one step at most: a node laid or
     * measured, a DIGESTS leaf read, a flush, the slot — or the slice, which is
     * memory alone, as long as a walk of every live file's runs takes
     * (binder_slice.c). But while a write short of room waits on the Tally under
     * way (binder.h, BinderRoomAwaited) every turn is the Tally's until it ends:
     * the write asks a cycle each round it is told to wait, and a cycle between
     * every two steps was a stitch each on a ribbon a full volume keeps one Quire
     * for — a Tally of two hundred steps took two hundred stitches, the ribbon ran
     * out under it and the delete that would have given the ground back was
     * refused (fullvolume, roomback). Its end asks the cycle that wakes the write
     * (BinderRoomEnded).
     */
    volatile uint8_t binder_turn;
    uint64_t watch_since;  /* the debt the last look saw unclaimed */
    uint64_t watch_settled;
    bool watch_seen;
    bool watch_said;
    Baton watch_baton;
    volatile uint32_t watch_passed;
    Baton park_baton;
    volatile uint32_t park_passed;
    uint64_t park_said;  /* when the park last said was decided */
} CadenceState;

/* What one volume holds of settle_wait.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    TagFSLock wait_lock;
    SettleWaiter* first;
    SettleWaiter* last;
    volatile uint32_t count;
    volatile uint64_t answered_upto;  /* a waiter for an Ink up to this is answered OK */
    volatile uint64_t ready_gen;  /* the volume's own rings: its book taken up */
    uint64_t ready_at_begin;  /* the rings the cycle in flight began after */
    volatile uint64_t park;  /* count | seat | SettleParkKind */
    volatile uint64_t park_us;  /* when the park was decided */
} SettleWaitState;

void SettleVolumeBegin(TagFSVolume* vol);

void CadenceVolumeBegin(TagFSVolume* vol);

void SettleWaitVolumeBegin(TagFSVolume* vol);

#endif /* TAGFS_SETTLE_INTERNAL_H */
