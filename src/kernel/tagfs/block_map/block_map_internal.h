#ifndef TAGFS_BLOCK_MAP_INTERNAL_H
#define TAGFS_BLOCK_MAP_INTERNAL_H

#include "block_map.h"
#include "../tagfs.h"

/*
 * The block map's own parts, shared by block_map.c (the windows, the cache,
 * the edits, the mount), block_map_take.c (choosing where a take goes) and
 * allotment.c (what growing files keep in hand). Everything here is read and
 * changed under the volume's state lock (MapLock), except a sector on its way
 * to or from the medium, which only its transfer touches.
 */

#define MAP_WINDOW_BLOCKS  BLOCK_MAP_WINDOW_BLOCKS     /* the blocks one sector of the map covers */
#define MAP_PAGE_SLOTS     64u                        /* a budget: 32 KiB of sectors on any volume */
#define MAP_NO_WINDOW      0xFFFFFFFFu

/* The lists of windows by the longest run they hold: list b holds the windows
 * whose longest is at least 2^(b-1) and less than 2^b, list 13 the whole ones. */
#define MAP_FIT_LISTS      13u

/* BlockMapWindow.flags */
#define MAP_W_EXACT   0x01u   /* longest, at, head and tail are the sector's own numbers, not bounds */
#define MAP_W_DELTAS  0x02u   /* edits made without the sector wait to be laid onto it */
#define MAP_W_OWED    0x04u   /* MapOwed: it carries a take the medium has not heard */

/*
 * What is known of one window without its sector. The blocks counted are the
 * ones a take may have: clear on the map, inside the data run, and kept by no
 * Allotment. `free` is always exact. The others are exact when MAP_W_EXACT
 * says so and otherwise honest lower bounds: a run of at least `longest` such
 * blocks starts at `at`, the first `head` blocks of the window are such
 * blocks, and so are the last `tail`. A decision made on them is never wrong,
 * only sometimes less good than one made on the sector.
 *
 * Every window with a run sits in the list of its longest (MapFitMoved), so a
 * best fit is looked for where it can be, not in every window of the volume;
 * and a window's own deltas are a chain of their own, newest first, so an
 * edit costs what its window holds, not what the volume does.
 */
typedef struct {
    uint16_t free;
    uint16_t longest;
    uint16_t at;
    uint16_t head;
    uint16_t tail;
    uint8_t  flags;
    uint8_t  slot;        /* the cache slot holding its sector, + 1; 0 when none does */
    uint32_t delta_last;  /* its newest delta, + 1; 0 when it has none */
    uint32_t fit_prev;    /* the windows before and after it in its list, + 1; 0 at an end */
    uint32_t fit_next;
} BlockMapWindow;

enum { MAP_PAGE_EMPTY = 0, MAP_PAGE_READY, MAP_PAGE_READING, MAP_PAGE_WRITING };

/* One sector of the map in memory. While it travels (READING, WRITING) its
 * bits belong to the transfer, and an edit meant for it is written down as a
 * delta instead. Writes of the map's sectors are numbered as they begin
 * (BlockMapState.sends), so a settle that waited for one knows whether it
 * began after the settle did, and what it met. */
typedef struct {
    uint32_t          window;
    uint32_t          lru;
    volatile uint32_t holder;   /* core index + 1 of the transfer in flight, else 0 */
    volatile uint8_t  state;
    uint8_t           dirty;    /* it holds a change the medium has not been sent */
    uint8_t           left_out; /* the medium's copy leaves out a block memory holds taken */
    uint8_t           paying;   /* the write on its way carries its window's debt (MapOwed) */
    uint64_t          sent;     /* the number of the write of it begun last */
    uint64_t          ended;    /* of the write of it that ended last since it came in; 0 none */
    error_t           ended_with;   /* and what that write met */
    uint8_t           bits[TAGFS_SECTOR_SIZE];
} BlockMapPage;

/* An edit made without its sector, inside one window, laid onto the sector in
 * the order it was made when the sector next comes in. The deltas live in one
 * pool; a window's are chained from its delta_last, each to the one made
 * before it, and a slot let go is chained into the pool's free ones. */
typedef struct {
    uint64_t start;
    uint32_t count;
    uint32_t take;     /* 1 a take, 0 a give */
    uint32_t older;    /* the window's delta made before it, + 1 — or, let go, the next free slot */
} BlockMapDelta;

/* A run a write took (BlockMapTakeFor, AllotmentTake) that no settled record
 * names yet: left free in every image of the map the medium is sent, until
 * the S1 of the cycle whose snapshot holds the Ink that named it. */
typedef struct {
    uint64_t start;
    uint32_t count;
    uint64_t fid;
    uint64_t gen;      /* the Ink that named it; 0 while none has */
} MapUnsettled;

typedef struct {
    bool            up;
    uint64_t        blocks;       /* the data run */
    uint32_t        windows;
    uint64_t        map_vlba;     /* the map's first sector, in volume sectors */
    BlockMapWindow *win;
    BlockMapPage   *page;
    uint32_t        owed;         /* windows that owe: flagged MAP_W_OWED, or paying by a write on its way */
    uint64_t        sends;        /* writes of the map's sectors begun */
    BlockMapDelta  *delta;
    uint32_t        deltas;       /* in use */
    uint32_t        delta_cap;
    uint32_t        delta_high;   /* slots ever handed out: [0, delta_high) */
    uint32_t        delta_free;   /* a slot let go, + 1; 0 when none is */
    uint32_t        delta_held;   /* of the room past `deltas`, what is held for gives */
    uint32_t        gives;        /* of the deltas, the gives */
    MapUnsettled   *unsettled;
    uint32_t        unsettled_count;
    uint32_t        unsettled_cap;
    uint64_t       *work;         /* a bit for every window with a debt or a delta ... */
    uint64_t       *work_words;   /* ... and one for every word of them with a bit set */
    uint32_t        fit[MAP_FIT_LISTS];   /* the first window of each list, + 1; 0 when empty */
    uint32_t        lru_tick;
    uint32_t        first_room;   /* no window below it has a block a take may have */
    bool            no_room_said;
#ifdef CONFIG_VOLUME_TESTS
    uint32_t        hold_most;    /* the most steps one hold of a plan took, since asked */
    uint32_t        edit_most;    /* the most deltas one edit looked at, since asked */
#endif
    uint8_t         view[TAGFS_SECTOR_SIZE];   /* scratch, under the state lock or at mount */
} BlockMapState;

extern BlockMapState g_map;

/* The window's sector when it is in memory and still, else NULL. */
BlockMapPage *MapReady(uint32_t w);

/* The window as a take sees it: its bits, the blocks past the data run, and
 * every Allotment's blocks, all set. */
void          MapViewOf(uint32_t w, const uint8_t *bits, uint8_t *view);

/* The summary measured again from the window's sector, which is READY — or,
 * at mount, from the sector as read. */
void          MapMeasure(uint32_t w);
void          MapMeasureBits(uint32_t w, const uint8_t *bits);

/* Window w's longest run was `was` and is what its summary says now: it moves
 * to the list that holds its new one. */
void          MapFitMoved(uint32_t w, uint32_t was);

static inline bool MapBit(const uint8_t *bits, uint32_t off)
{
    return (bits[off / 8] & (uint8_t)(1u << (off % 8))) != 0;
}

/* The blocks [start, start + count) stop, or start, being ones a take may
 * have — an Allotment kept or given back — with the summaries told: measured
 * again where the sector is in memory, bounds kept honest where it is not. */
void          MapAvailabilityChanged(uint64_t start, uint32_t count, bool lost);

/* The window has a debt or a delta: its bit is set for the settle to find.
 * MapWorkNext answers the first window at or past w whose bit is set
 * (g_map.windows when none is), and a settle clears the bit of a window it
 * found owing nothing (MapWorkDone). */
void          MapWorkAt(uint32_t w);
uint32_t      MapWorkNext(uint32_t w);
void          MapWorkDone(uint32_t w);

/* MapOwed: window w carries a take the medium has not heard. */
void          MapOwedMark(uint32_t w);

/* Room for `more` deltas, got now: an edit is written down whole or not made.
 * It leaves the room held for Fallow's gives (BlockMapGiveRoomHold) where it
 * is, the heap asked for more when `heap` says so; a give the heap does not
 * serve, or may not ask, draws on that room, a take never. */
bool          MapDeltaRoom(uint32_t more, bool give, bool heap);

/* An edit of [start, start + count), inside one window, written down to be laid
 * onto its sector; room was made first. */
void          MapDeltaAdd(uint64_t start, uint32_t count, bool take);

/* Every delta of window w laid onto its sector `p`, in the order made — as the
 * sector comes in, or as a write of it ends. */
void          MapDeltaApply(uint32_t w, BlockMapPage *p);

/* How many windows of the run have no sector in memory. */
uint32_t      MapUnreadWindows(uint64_t start, uint32_t count);

/* Take a run a take may have (MapTakeKnown), or one an Allotment kept for the
 * taker (MapTakeKept). The deltas the run needs are made room for first
 * (MapDeltaRoom); the Ledger's free count and MapOwed follow. */
void          MapTakeKnown(uint64_t start, uint32_t count);
void          MapTakeKept(uint64_t start, uint32_t count);

/* ---- runs no settled record names yet (block_map.c) -------------------- */

/* Room for one run more, and for the split a give out of a run's middle
 * makes: got now, under the state lock. */
bool          MapUnsettledRoom(void);

/* The run file `fid`'s write just took, under the lock that took it; room was
 * made first. */
void          MapUnsettledAdd(uint64_t fid, uint64_t start, uint32_t count);

/* Every block of [first, first + count) such a run holds, cleared in `bits`
 * (one bit per block from `first`): how many. */
uint32_t      MapUnsettledLeftOut(uint64_t first, uint32_t count, uint8_t *bits);

/* S1 of the cycle whose snapshot holds every Ink up to `target`: the runs
 * they named are the map's own from here on, their sectors owed. */
void          MapUnsettledSettled(uint64_t target);

/* With no TagFS lock held: the window's sector fetched into the cache. OK;
 * ERR_WOULD_BLOCK when it is on its way on this very core, beneath the
 * caller; or the medium's refusal. */
error_t       MapFetch(uint32_t w, const char *site);

/* ---- choosing where a take goes (block_map_take.c) --------------------- */

#define MAP_LOOKS 4u

/* Windows a plan weighs in one hold of the state lock before it lets the lock
 * go, when its caller holds none: a step is one window — its summary, its
 * sector, or one window of a walk of whole free windows. */
#define MAP_PLAN_STEPS 256u

/* Windows of one list a best fit weighs. */
#define MAP_FIT_LOOKS  4u

enum { MAP_PLAN_FRESH = 0, MAP_PLAN_ROOM, MAP_PLAN_HINT, MAP_PLAN_FIT, MAP_PLAN_WALK,
       MAP_PLAN_DONE };

typedef struct {
    uint32_t want;
    uint64_t hint;
    bool     best_fit;   /* the smallest run of `want`, else the near rule */
    bool     found;
    uint64_t start;      /* the run the take comes from, as known */
    uint32_t len;
    uint64_t at;         /* where the take starts in it */
    uint32_t got;        /* and how much of `want` it gives */
    uint64_t far;        /* |at - hint| */
    uint32_t look[MAP_LOOKS];   /* windows whose sectors would decide better */
    uint32_t looks;
    /* How far the plan has come, across the holds it spans. */
    uint8_t  stage;      /* MAP_PLAN_*: FRESH begins it again */
    bool     paused;     /* this hold's steps are spent: MapPlan goes on in the next */
    bool     spanned;    /* it went on in a later hold: its run is asked again before the take */
    uint32_t steps;      /* steps taken in this hold */
    uint32_t lo;         /* the first window with room, as the plan began */
    uint32_t list;       /* the list a best fit weighs next */
    uint64_t reach;      /* how far a run is measured: past it, the plan cannot tell one from another */
    uint64_t ahead;      /* the next window ahead of the hint, and one past the next behind */
    uint64_t behind;
    /* A walk of whole free windows, kept for the hold: [chain_from, chain_upto)
     * are whole, and when chain_ended the window chain_upto is not, its head
     * chain_head ending the run. */
    bool     chain_kept;
    bool     chain_ended;
    uint16_t chain_head;
    uint32_t chain_from;
    uint32_t chain_upto;
} MapPick;

/* The plan, begun — or gone on with, from where its last hold stopped — under
 * the state lock. `may_pause`: the caller lets the lock go between holds, so
 * a hold that has taken MAP_PLAN_STEPS steps stops with k->paused; otherwise
 * the plan runs to its end in this hold. */
void          MapPlan(MapPick *k, bool may_read, bool may_pause);

/* Whether every block of the run is still one a take may have, as memory
 * knows it now: asked of a plan that spanned holds, right before its take. */
bool          MapRunStillFree(uint64_t start, uint32_t count);

/* A door stops fetching sectors to decide better after this many rounds. */
#define MAP_LOOK_ROUNDS 16u

/* No run of `want` could be had: said once for each run of refusals, with the
 * two numbers that tell "full" from "in pieces". Under the state lock. */
void          MapSayNoRoom(uint32_t want);

/* The answer to a take no run could serve, under the state lock: while Fallow
 * rests blocks a Settle cycle would give back it is ERR_WOULD_BLOCK — the room
 * is coming, not gone — and ERR_TAGFS_NO_SPACE, said, when none rest. */
error_t       MapShort(uint32_t want);

/* Under the state lock: how many blocks a take that is not the pool's own
 * Spare may have — the free ones but those the pool's reserve still lacks
 * (meta_pool.h, PoolSparesOwed). */
uint64_t      MapMayHaveLocked(void);

/* ---- what growing files keep in hand (allotment.c) --------------------- */

void          AllotmentOverlay(uint32_t w, uint8_t *view);
uint64_t      AllotmentKeptLocked(void);

#endif /* TAGFS_BLOCK_MAP_INTERNAL_H */
