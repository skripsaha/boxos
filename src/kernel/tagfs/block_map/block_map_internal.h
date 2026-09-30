#ifndef TAGFS_BLOCK_MAP_INTERNAL_H
#define TAGFS_BLOCK_MAP_INTERNAL_H

#include "block_map.h"
#include "../tagfs.h"
#include "../plot/plot_width.h"

/*
 * The block map's own parts, shared by block_map.c (the windows, the cache,
 * the edits, the mount), block_map_take.c (choosing where a take goes) and
 * allotment.c (what growing files keep in hand). Everything here is read and
 * changed under the volume's state lock (MapLock), except a sector on its way
 * to or from the medium, which only its transfer touches.
 */

#define MAP_WINDOW_BLOCKS  (TAGFS_SECTOR_SIZE * 8u)    /* the blocks one sector of the map covers */
#define MAP_PAGE_SLOTS     64u                        /* a budget: 32 KiB of sectors on any volume */
#define MAP_NO_WINDOW      0xFFFFFFFFu

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
 */
typedef struct {
    uint16_t free;
    uint16_t longest;
    uint16_t at;
    uint16_t head;
    uint16_t tail;
    uint8_t  flags;
    uint8_t  slot;     /* the cache slot holding its sector, + 1; 0 when none does */
} BlockMapWindow;

enum { MAP_PAGE_EMPTY = 0, MAP_PAGE_READY, MAP_PAGE_READING, MAP_PAGE_WRITING };

/* One sector of the map in memory. While it travels (READING, WRITING) its
 * bits belong to the transfer, and an edit meant for it is written down as a
 * delta instead. */
typedef struct {
    uint32_t          window;
    uint32_t          lru;
    volatile uint32_t holder;   /* core index + 1 of the transfer in flight, else 0 */
    volatile uint8_t  state;
    uint8_t           dirty;    /* it holds a change the medium has not been sent */
    uint8_t           bits[TAGFS_SECTOR_SIZE];
} BlockMapPage;

/* An edit made without its sector, inside one window, laid onto the sector in
 * the order it was made when the sector next comes in. */
typedef struct {
    uint32_t start;
    uint32_t count;
    uint32_t take;     /* 1 a take, 0 a give */
} BlockMapDelta;

typedef struct {
    bool            up;
    uint32_t        blocks;       /* the data run */
    uint32_t        windows;
    uint64_t        map_vlba;     /* the map's first sector, in volume sectors */
    BlockMapWindow *win;
    BlockMapPage   *page;
    uint32_t        owed;         /* windows flagged MAP_W_OWED */
    BlockMapDelta  *delta;
    uint32_t        deltas;
    uint32_t        delta_cap;
    uint32_t        gives;        /* of the deltas, the gives */
    uint32_t        work_lo;      /* every window with a debt or a delta lies in */
    uint32_t        work_hi;      /* [work_lo, work_hi]; empty when lo > hi */
    uint32_t        lru_tick;
    uint32_t        first_room;   /* no window below it has a block a take may have */
    bool            no_room_said;
    uint8_t         view[TAGFS_SECTOR_SIZE];   /* scratch, under the state lock or at mount */
} BlockMapState;

extern BlockMapState g_map;

TagFSLock    *MapLock(void);

/* The window's sector when it is in memory and still, else NULL. */
BlockMapPage *MapReady(uint32_t w);

/* The window as a take sees it: its bits, the blocks past the data run, and
 * every Allotment's blocks, all set. */
void          MapViewOf(uint32_t w, const uint8_t *bits, uint8_t *view);

/* The summary measured again from the window's sector, which is READY — or,
 * at mount, from the sector as read. */
void          MapMeasure(uint32_t w);
void          MapMeasureBits(uint32_t w, const uint8_t *bits);

static inline bool MapBit(const uint8_t *bits, uint32_t off)
{
    return (bits[off / 8] & (uint8_t)(1u << (off % 8))) != 0;
}

/* The blocks [start, start + count) stop, or start, being ones a take may
 * have — an Allotment kept or given back — with the summaries told: measured
 * again where the sector is in memory, bounds kept honest where it is not. */
void          MapAvailabilityChanged(uint32_t start, uint32_t count, bool lost);

/* The window has a debt or a delta: the settle's range is widened to it. */
void          MapWorkAt(uint32_t w);

/* MapOwed: window w carries a take the medium has not heard. */
void          MapOwedMark(uint32_t w);

/* Room for `more` deltas, got now: an edit is written down whole or not made. */
bool          MapDeltaRoom(uint32_t more);

/* An edit of [start, start + count), inside one window, written down to be laid
 * onto its sector; room was made first. */
void          MapDeltaAdd(uint32_t start, uint32_t count, bool take);

/* Every delta of window w laid onto its sector `p`, in the order made — as the
 * sector comes in, or as a write of it ends. */
void          MapDeltaApply(uint32_t w, BlockMapPage *p);

/* How many windows of the run have no sector in memory. */
uint32_t      MapUnreadWindows(uint32_t start, uint32_t count);

/* Take a run a take may have (MapTakeKnown), or one an Allotment kept for the
 * taker (MapTakeKept). The deltas the run needs are made room for first
 * (MapDeltaRoom); the Ledger's free count and MapOwed follow. */
void          MapTakeKnown(uint32_t start, uint32_t count);
void          MapTakeKept(uint32_t start, uint32_t count);

/* With no TagFS lock held: the window's sector fetched into the cache. OK;
 * ERR_WOULD_BLOCK when it is on its way on this very core, beneath the
 * caller; or the medium's refusal. */
error_t       MapFetch(uint32_t w, const char *site);

/* ---- choosing where a take goes (block_map_take.c) --------------------- */

#define MAP_LOOKS 4u

typedef struct {
    uint32_t want;
    uint32_t hint;
    bool     best_fit;   /* the smallest run of `want`, else the near rule */
    bool     found;
    uint32_t start;      /* the run the take comes from, as known */
    uint32_t len;
    uint32_t at;         /* where the take starts in it */
    uint32_t got;        /* and how much of `want` it gives */
    uint32_t far;        /* |at - hint| */
    uint32_t look[MAP_LOOKS];   /* windows whose sectors would decide better */
    uint32_t looks;
    uint32_t chain_from;        /* a walk of whole free windows from here ... */
    uint32_t chain_stop;        /* ... ended at this window, whose head it took ... */
    uint32_t chain_len;         /* ... and came to this many blocks; 0: none kept */
} MapPick;

void          MapPlan(MapPick *k, bool may_read);

/* A door stops fetching sectors to decide better after this many rounds. */
#define MAP_LOOK_ROUNDS 16u

/* No run of `want` could be had: said once for each run of refusals, with the
 * two numbers that tell "full" from "in pieces". Under the state lock. */
void          MapSayNoRoom(uint32_t want);

/* ---- what growing files keep in hand (allotment.c) --------------------- */

void          AllotmentOverlay(uint32_t w, uint8_t *view);
bool          AllotmentTakeBackLocked(uint32_t keep_fid);
uint64_t      AllotmentKeptLocked(void);
void          AllotmentDropAll(void);

#endif /* TAGFS_BLOCK_MAP_INTERNAL_H */
