#include "settle_internal.h"
#include "../metadata_pool/meta_pool.h"
#include "../settle_cut/settle_cut.h"
#include "boardroom.h"
#include "pmm.h"

/*
 * The waiters of the Settle (settle.h): who is answered by which cycle, and
 * the one rule for a cycle that was refused.
 *
 * A waiter for Ink `gen` is answered OK once every record up to `gen` lies in
 * a closed block a cycle settled (PoolClosedSettledGen) — or, when a cycle
 * owed that close found no Spare to close onto, once a cycle settled it in the
 * open tail. The list is FIFO under a leaf lock; the answers are given after
 * the lock is let go.
 */

static TagFSLock          s_wait_lock = TAGFS_LOCK_INIT("settlewait", LOCK_RANK_LEAF);
static SettleWaiter*      s_first;
static SettleWaiter*      s_last;
static volatile uint32_t  s_count;
static volatile uint64_t  s_answered_upto;  /* a waiter for an Ink up to this is answered OK */
static volatile uint64_t  s_ready_gen;      /* the volume's own rings: its book taken up */
static uint64_t           s_ready_at_begin; /* the rings the cycle in flight began after */

/*
 * ‼ A PARK IS A COUNT, NEVER A FLAG. What a refused cycle waits for is kept as
 * the kind of its event and where that event's count stood when the cycle
 * began or was refused — with the seat, in one word, so a look reads them at
 * once — and the park stands only while the count has not moved. An event
 * that comes between the refusal and the park is then never lost: no moment
 * exists at which the park is decided after its event was looked for. A flag
 * set after reading the count could land on a seat:ready that came in
 * between, and erase it (X1-4).
 */
typedef enum {
    SETTLE_PARK_NONE = 0,
    SETTLE_PARK_SEAT,       /* the medium refused, or left: seat:ready, or its book taken up */
    SETTLE_PARK_HEAP,       /* the machine had no memory for a step: its pages coming back */
    SETTLE_PARK_ROOM,       /* no ground for a step: room asked for after the refusal */
} SettleParkKind;

#define SETTLE_PARK_KIND_BITS  2u
#define SETTLE_PARK_SEAT_BITS  8u
#define SETTLE_PARK_KIND_MASK  ((1ull << SETTLE_PARK_KIND_BITS) - 1u)
#define SETTLE_PARK_SEAT_SHIFT SETTLE_PARK_KIND_BITS
#define SETTLE_PARK_COUNT_SHIFT (SETTLE_PARK_KIND_BITS + SETTLE_PARK_SEAT_BITS)
#define SETTLE_PARK_COUNT(c)   ((uint64_t)(c) & (~0ull >> SETTLE_PARK_COUNT_SHIFT))

static volatile uint64_t  s_park;           /* count | seat | SettleParkKind */
static volatile uint64_t  s_park_us;        /* when the park was decided */

static void SettlePark(SettleParkKind kind, uint64_t count)
{
    uint64_t seat = tagfs_get_seat();
    __atomic_store_n(&s_park_us, SettleNowUs(), __ATOMIC_RELAXED);
    __atomic_store_n(&s_park,
                     SETTLE_PARK_COUNT(count) << SETTLE_PARK_COUNT_SHIFT |
                         seat << SETTLE_PARK_SEAT_SHIFT | kind,
                     __ATOMIC_RELEASE);
}

/* seat:ready of the volume's seat as the room counts it — where its bell is
 * noted, ahead of the announcement (BoardroomSeatReadyGen), so a bell rung
 * before a cycle took its count is in it however late it is said — and the
 * volume's own: its book taken up (SettleSeatReady). Each only moves forward,
 * so their sum moves exactly when either does. The announcement itself only
 * knocks: counted again, a bell said after a cycle's S0 that was rung before
 * it would lift the park of a refusal it never answered. */
static uint64_t SettleRingsNow(void)
{
    return (uint64_t)BoardroomSeatReadyGen(tagfs_get_seat()) +
           __atomic_load_n(&s_ready_gen, __ATOMIC_ACQUIRE);
}

/* The machine's pages, as the heap's reserve grows from them (kheap.h): the
 * memory a step was refused comes back when there are more of them free than
 * when it was refused. */
static uint64_t SettleHeapNow(void)
{
    return PmmFreePagesNow();
}

/* A park on the medium stands only in the seat it was decided in: a book
 * taken up in another seat is the event itself. */
bool SettleParked(void)
{
    uint64_t park  = __atomic_load_n(&s_park, __ATOMIC_ACQUIRE);
    uint64_t count = park >> SETTLE_PARK_COUNT_SHIFT;
    uint64_t seat  = (park >> SETTLE_PARK_SEAT_SHIFT) & ((1ull << SETTLE_PARK_SEAT_BITS) - 1u);
    switch ((SettleParkKind)(park & SETTLE_PARK_KIND_MASK)) {
    case SETTLE_PARK_SEAT:
        return seat == tagfs_get_seat() && SETTLE_PARK_COUNT(SettleRingsNow()) == count;
    case SETTLE_PARK_HEAP:
        return SETTLE_PARK_COUNT(SettleHeapNow()) <= count;
    case SETTLE_PARK_ROOM:
        return SETTLE_PARK_COUNT(CadenceRoomAsked()) == count;
    default:
        return false;
    }
}

uint64_t SettleParkedSinceUs(void)
{
    return SettleParked() ? __atomic_load_n(&s_park_us, __ATOMIC_RELAXED) : 0;
}

void TagFSHeapCameBack(void)
{
    uint64_t park = __atomic_load_n(&s_park, __ATOMIC_ACQUIRE);
    if ((park & SETTLE_PARK_KIND_MASK) == SETTLE_PARK_HEAP)
        SettleKnock();
}

void SettleParkSay(void)
{
    uint64_t    park  = __atomic_load_n(&s_park, __ATOMIC_ACQUIRE);
    uint64_t    since = __atomic_load_n(&s_park_us, __ATOMIC_RELAXED);
    uint64_t    now   = SettleNowUs();
    const char* event = "room to be asked for";
    if ((park & SETTLE_PARK_KIND_MASK) == SETTLE_PARK_SEAT)
        event = "the volume's seat to say seat:ready, or its book to be taken up";
    else if ((park & SETTLE_PARK_KIND_MASK) == SETTLE_PARK_HEAP)
        event = "the machine's memory to come back";
    kprintf("[Nightwatch] SETTLE PARKED: %u waiter(s) of the volume wait for a cycle, and the "
            "cycle refused %llu ms ago waits for %s — no other event starts it (settled up to "
            "Ink %llu)\n",
            (unsigned)__atomic_load_n(&s_count, __ATOMIC_ACQUIRE),
            (unsigned long long)(now > since ? (now - since) / 1000u : 0), event,
            (unsigned long long)SettledGen());
}

bool SettleWaitersOwed(void)
{
    return __atomic_load_n(&s_count, __ATOMIC_ACQUIRE) != 0;
}

/*
 * ‼ A BOOK'S WAITER IS ANSWERED BY A FLUSH OF ITS OWN MOUNT (X2-3, spec_v2
 * §3.5). What a mount read may have come from the medium's cache, after a warm
 * restart that took no power away: a book's waiter right after its mount —
 * `anchor` with nothing inked yet — is answered only once a stitch of this
 * mount settled under its kept flush, and the cadence runs that one, empty if
 * need be, at once (a waiter is a demand). A volume of format v1 answers by
 * the tail its mount found sealed, as it always has.
 */
static bool SettleMountKept(void)
{
    return tagfs_get_state()->format != DEED_FORMAT_V2 || SettledCycles() > 0;
}

error_t SettleWaitNow(uint64_t gen)
{
    if (gen <= __atomic_load_n(&s_answered_upto, __ATOMIC_ACQUIRE) && SettleMountKept())
        return OK;
    if (TagFSMediumLeft())
        return ERR_DEVICE_NOT_READY;
    if (TagFSReadOnly())
        return ERR_TAGFS_READ_ONLY;
    return ERR_WOULD_BLOCK;
}

static void SettleWaitAppend(SettleWaiter* w)
{
    TagFSLockTake(&s_wait_lock);
    if (s_last)
        s_last->next = w;
    else
        s_first = w;
    s_last = w;
    __atomic_add_fetch(&s_count, 1u, __ATOMIC_ACQ_REL);
    TagFSLockDrop(&s_wait_lock);
    SettleKnock();
}

void SettleWaitLink(SettleWaiter* w, uint64_t gen, void (*answer)(SettleWaiter* w, error_t rc))
{
    w->next   = NULL;
    w->gen    = gen;
    w->cycle  = 0;
    w->answer = answer;
    SETTLE_CUT_ANCHORED(gen);       /* nothing at all unless SETTLECUT=<point> */
    SettleWaitAppend(w);
}

/* A cycle in flight may have taken its S0 before the word was said, so it
 * does not count; the one after it does. In flight is looked at first: a cycle
 * that ends between the two looks is counted, and costs one more cycle, never
 * one too few. */
void SettleWaitLinkCycle(SettleWaiter* w, void (*answer)(SettleWaiter* w, error_t rc))
{
    bool in_flight = SettleRunInFlight();
    w->next   = NULL;
    w->gen    = 0;
    w->cycle  = SettledCycles() + (in_flight ? 2u : 1u);
    w->answer = answer;
    SettleWaitAppend(w);
}

bool SettleWaitUnlink(SettleWaiter* w)
{
    bool found = false;
    TagFSLockTake(&s_wait_lock);
    SettleWaiter* before = NULL;
    for (SettleWaiter* at = s_first; at; before = at, at = at->next) {
        if (at != w)
            continue;
        if (before)
            before->next = at->next;
        else
            s_first = at->next;
        if (s_last == at)
            s_last = before;
        at->next = NULL;
        __atomic_sub_fetch(&s_count, 1u, __ATOMIC_ACQ_REL);
        found = true;
        break;
    }
    TagFSLockDrop(&s_wait_lock);
    return found;
}

/* Whether a waiter is answered OK already. */
static bool SettleWaiterAnswered(const SettleWaiter* w, uint64_t upto)
{
    return w->cycle ? SettledCycles() >= w->cycle : w->gen <= upto && SettleMountKept();
}

uint64_t SettleWaitersCloseUpto(void)
{
    uint64_t upto = 0;
    if (!SettleWaitersOwed())
        return 0;
    TagFSLockTake(&s_wait_lock);
    for (SettleWaiter* w = s_first; w; w = w->next)
        if (w->gen > upto)
            upto = w->gen;
    TagFSLockDrop(&s_wait_lock);
    return upto;
}

/* The waiters `all` or those answered OK already, out of the list: their
 * answers are given by the caller, with no lock held. */
static SettleWaiter* SettleWaitersTake(bool all)
{
    uint64_t      upto  = __atomic_load_n(&s_answered_upto, __ATOMIC_ACQUIRE);
    SettleWaiter* taken = NULL;
    SettleWaiter* end   = NULL;

    TagFSLockTake(&s_wait_lock);
    SettleWaiter** at = &s_first;
    s_last = NULL;
    while (*at) {
        SettleWaiter* w = *at;
        if (all || SettleWaiterAnswered(w, upto)) {
            *at     = w->next;
            w->next = NULL;
            if (end)
                end->next = w;
            else
                taken = w;
            end = w;
            __atomic_sub_fetch(&s_count, 1u, __ATOMIC_ACQ_REL);
            continue;
        }
        s_last = w;
        at     = &w->next;
    }
    TagFSLockDrop(&s_wait_lock);
    return taken;
}

/* Each waiter answered: OK when its Ink is answered already, else `code`. */
static void SettleWaitersAnswer(SettleWaiter* list, error_t code)
{
    uint64_t upto = __atomic_load_n(&s_answered_upto, __ATOMIC_ACQUIRE);
    while (list) {
        SettleWaiter* w = list;
        list = w->next;
        w->next = NULL;
        w->answer(w, SettleWaiterAnswered(w, upto) ? OK : code);
    }
}

void SettleWaitersSweep(void)
{
    if (!SettleWaitersOwed())
        return;
    SettleWaitersAnswer(SettleWaitersTake(false), OK);
}

void SettleWaitersAnswerAll(error_t code)
{
    if (!SettleWaitersOwed())
        return;
    SettleWaitersAnswer(SettleWaitersTake(true), code);
}

void SettleWaitersRunBegins(void)
{
    s_ready_at_begin = SettleRingsNow();
}

void SettleWaitersMountReset(void)
{
    SettleWaitersAnswerAll(ERR_DEVICE_NOT_READY);
    __atomic_store_n(&s_answered_upto, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_park, (uint64_t)SETTLE_PARK_NONE, __ATOMIC_RELEASE);
}

/*
 * ‼ ONE RULE FOR A REFUSED CYCLE (§3): nothing is declared settled, the
 * waiters stay in their Chit, and the cadence begins no cycle until the event
 * the refusal waits for — never on a clock, and never at once:
 *
 *   - a medium that takes no write never will: the volume is read-only, and
 *     every waiter hears so (SettleWaitersRunEnded);
 *   - a medium that left is its open book's to decide (mount/away.h): taken
 *     up, it rings like seat:ready; given up, it answers every waiter;
 *   - a step the machine had no memory for waits for its pages to come back;
 *   - a step with no ground waits for room to be asked for — by a write the
 *     allocator sent away, which is when Fallow has ground for the cycle to
 *     give back; but a book whose stitch has no room gets it back only once a
 *     Tally takes the ribbon's end in and the slots let its Quires go — work
 *     no waiter's park can name an event of: its waiters are told so
 *     (SettleWaitersRunEnded), nothing parks, and the next waiter's cycle is
 *     refused the same, in so many words, until the room came back;
 *   - any other refusal is the medium's: the seat that refused rings
 *     seat:ready on the first command it answers after, whatever kind of
 *     medium sits in it (boardroom.h) — and a seat:ready that came while this
 *     very cycle ran has been counted since its S0, so the next cycle may go
 *     at once.
 *
 * Decided by the cycle's holder before its Give, so no later cycle's end can
 * come before it; a settled cycle lifts whatever park stood. No attempt is
 * counted.
 */
void SettleWaitersPark(error_t rc)
{
    if (rc == OK) {
        __atomic_store_n(&s_park, (uint64_t)SETTLE_PARK_NONE, __ATOMIC_RELEASE);
        return;
    }
    if (TagFSMediumLeft()) {
        SettlePark(SETTLE_PARK_SEAT, s_ready_at_begin);
        return;
    }
    if (TagFSReadOnly()) {
        __atomic_store_n(&s_park, (uint64_t)SETTLE_PARK_NONE, __ATOMIC_RELEASE);
        return;
    }
    switch (rc) {
    case ERR_NO_MEMORY:
        SettlePark(SETTLE_PARK_HEAP, SettleHeapNow());
        break;
    case ERR_TAGFS_NO_SPACE:
        if (tagfs_get_state()->format == DEED_FORMAT_V2)
            __atomic_store_n(&s_park, (uint64_t)SETTLE_PARK_NONE, __ATOMIC_RELEASE);
        else
            SettlePark(SETTLE_PARK_ROOM, CadenceRoomAsked());
        break;
    case ERR_WOULD_BLOCK:
        SettlePark(SETTLE_PARK_ROOM, CadenceRoomAsked());
        break;
    default:
        SettlePark(SETTLE_PARK_SEAT, s_ready_at_begin);
        break;
    }
}

void SettleWaitersRunEnded(error_t rc, uint64_t target, bool unclosed)
{
    if (rc != OK) {
        if (TagFSReadOnly() && !TagFSMediumLeft())
            SettleWaitersAnswerAll(ERR_TAGFS_READ_ONLY);
        /* A book whose ribbon has no room for the cycle's stitch gets it
         * back only through a Tally and the lag of the slots it lets go of
         * (settle/binder.h): its waiters hear so rather than wait on an event
         * no park can name. */
        else if (rc == ERR_TAGFS_NO_SPACE && tagfs_get_state()->format == DEED_FORMAT_V2)
            SettleWaitersAnswerAll(ERR_TAGFS_NO_SPACE);
        return;
    }
    /* A book's stitch is never written again once it settled: everything up
     * to its target is answered. Format v1's record is answered once it lies
     * in a closed block, or in the tail when no Spare was there to close it. */
    uint64_t upto = tagfs_get_state()->format == DEED_FORMAT_V2 ? target : PoolClosedSettledGen();
    if (unclosed && target > upto)
        upto = target;
    uint64_t seen = __atomic_load_n(&s_answered_upto, __ATOMIC_RELAXED);
    while (seen < upto &&
           !__atomic_compare_exchange_n(&s_answered_upto, &seen, upto, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        ;
    SettleWaitersSweep();
}

void SettleSeatReady(void)
{
    __atomic_add_fetch(&s_ready_gen, 1u, __ATOMIC_ACQ_REL);
    SettleKnock();
}
