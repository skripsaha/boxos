#ifndef ROADSTEAD_H
#define ROADSTEAD_H

#include "ktypes.h"
#include "error.h"

/*
 * Roadstead — the water outside a port, where a ship that finds every berth
 * taken drops anchor and is called in, in its turn, when one comes free.
 *
 * A medium has a fixed number of places for commands: thirty-two NCQ tags or
 * one non-queued slot on an AHCI port, one turn on a Bulk-Only USB disk. What
 * used to happen to a command that found them all taken was an answer about
 * THAT INSTANT — ERR_BUSY, or a spin on the caller's core — and the caller had
 * no event to wait for, because a place coming free woke nobody. Now the
 * command moors here, under the device's own lock and in the same critical
 * section that found no place, so a place freed a moment later cannot be a
 * wake nobody receives; and whoever frees one calls the berth at the head in.
 *
 * Served in the order moored, and a newcomer never overtakes a berth already
 * waiting. The one exception is a berth put back where it stood (a command a
 * fault interrupted and the port proved innocent), which goes first.
 *
 * door_wanted: the berth at the head wants the whole port — the one command
 * class that cannot share it (AHCI 1.3.1 §1.7, FLUSH CACHE EXT beside queued
 * commands). While it waits nothing behind it is called in, so the slots drain
 * and the flush gets the port instead of starving behind a stream of reads.
 *
 * Two kinds of berth. One whose caller STANDS over it (a synchronous command)
 * leaves `answered` NULL; being called in hands it the slot, and its caller
 * issues its own command. One nobody stands over carries its request — the
 * sectors, the page, and `answered`, run once the medium has answered — and is
 * issued by whoever calls it in. Either kind lives in its owner's memory, never
 * the device's, so mooring one cannot fail for want of room.
 *
 * Every list operation here runs under the device's lock; nothing here takes
 * one of its own.
 */

typedef enum {
    ROADSTEAD_TAKEN = 0,   /* a place was free: it is the caller's now */
    ROADSTEAD_WAIT,        /* moored: the berth is called in its turn */
    ROADSTEAD_GONE         /* the medium is not in the machine */
} RoadsteadAnswer;

typedef enum {
    BERTH_ONE = 0,         /* one place — a command slot, a turn */
    BERTH_WHOLE            /* the whole port, for the command that cannot share it */
} RoadsteadWant;

typedef enum {
    BERTH_MOORED = 0,      /* waiting in the roadstead */
    BERTH_CALLED,          /* called in: `slot` is its own */
    BERTH_GONE             /* the medium left while it waited */
} RoadsteadFate;

/* Shaped like every seat's asynchronous callback, so the same function can be
 * handed to any of them: `index` names the port or unit, `slot` the command
 * slot it ran on (zero where a medium has no such thing). */
typedef void (*RoadsteadAnswered)(uint8_t index, uint8_t slot,
                                  error_t status, void* ctx);

typedef struct RoadsteadBerth {
    struct RoadsteadBerth* next;
    uint8_t            want;       /* RoadsteadWant */
    volatile uint8_t   fate;       /* RoadsteadFate — the stander reads it */
    uint8_t            slot;       /* the place it was called to */
    uint8_t            core;       /* the core standing over it (BERTH_WHOLE) */

    /* The request of a berth nobody stands over. */
    uint64_t           lba;
    void*              dma_phys;
    uint32_t           sectors;
    bool               write;
    RoadsteadAnswered  answered;   /* NULL: a caller stands over this berth */
    void*              ctx;

    /* Of a write refused, the sectors from `lba` the medium took before the
     * sector it failed at — read by `answered`; 0 when the medium names no
     * such sector, which ours do not yet (BoardroomWrite's `landed`). */
    uint32_t           landed;

    /* The room stands between the medium and whoever handed it a berth nobody
     * stands over (boardroom.c, seat_async): `answered` is the room's own,
     * which notes what the seat said and passes it on to `theirs`. The chair
     * and the occupant of it the command went to, so an answer that comes
     * after the chair changed hands is not put down to whoever sits in it
     * now. */
    RoadsteadAnswered  theirs;
    void*              theirs_ctx;
    uint32_t           seating;
    uint8_t            seat;
#ifdef CONFIG_VOLUME_TESTS
    /* BLOCKFAIL let the sectors before the plan's first one go out as their
     * own command: their answer becomes the refusal the caller was to hear. */
    bool               struck;
#endif
} RoadsteadBerth;

typedef struct Roadstead {
    RoadsteadBerth* head;
    RoadsteadBerth* tail;
    bool            door_wanted;   /* the berth at the head wants the whole port */
} Roadstead;

/* Under the device's lock. */
void            RoadsteadMoor(Roadstead* r, RoadsteadBerth* b);
void            RoadsteadMoorFirst(Roadstead* r, RoadsteadBerth* b);
void            RoadsteadCastOff(Roadstead* r);          /* unlinks the head */
RoadsteadBerth* RoadsteadClear(Roadstead* r);            /* the whole list, unlinked */

static inline RoadsteadBerth* RoadsteadHead(const Roadstead* r) { return r->head; }
static inline bool RoadsteadIsEmpty(const Roadstead* r) { return r->head == NULL; }

/* Outside every lock: the berth's fate, told to whoever stands over it. */
void RoadsteadCallIn(RoadsteadBerth* b, uint8_t slot);
void RoadsteadSendAway(RoadsteadBerth* b);

/*
 * Stand over a moored berth until it is called in or sent away, turning
 * `turn(ctx)` on every pass — the device's own way of making progress from
 * where the caller stands (drain what the medium answered, call in whoever may
 * go). No clock: what ends the wait is a place coming free or the medium
 * leaving, and every command holding a place is bounded by its own facts.
 */
RoadsteadFate RoadsteadStandBy(RoadsteadBerth* b, void (*turn)(void* ctx), void* ctx);

#endif
