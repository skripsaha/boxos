#include "settle_internal.h"
#include "../metadata_pool/meta_pool.h"
#include "boardroom.h"

/*
 * The waiters of the Settle (settle.h): who is answered by which cycle, and
 * the one rule for a cycle the medium refused.
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
static volatile uint32_t  s_parked;         /* a refusal that may pass waits for seat:ready */
static volatile uint64_t  s_ready_gen;      /* seat:ready naming the volume's seat, counted */
static uint64_t           s_ready_at_begin; /* the count the cycle in flight began after */

bool SettleParked(void)
{
    return __atomic_load_n(&s_parked, __ATOMIC_ACQUIRE) != 0;
}

bool SettleWaitersOwed(void)
{
    return __atomic_load_n(&s_count, __ATOMIC_ACQUIRE) != 0;
}

error_t SettleWaitNow(uint64_t gen)
{
    if (gen <= __atomic_load_n(&s_answered_upto, __ATOMIC_ACQUIRE))
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
    return w->cycle ? SettledCycles() >= w->cycle : w->gen <= upto;
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
    s_ready_at_begin = __atomic_load_n(&s_ready_gen, __ATOMIC_ACQUIRE);
}

void SettleWaitersMountReset(void)
{
    SettleWaitersAnswerAll(ERR_DEVICE_NOT_READY);
    __atomic_store_n(&s_answered_upto, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_parked, 0u, __ATOMIC_RELEASE);
}

/*
 * ‼ ONE RULE FOR A REFUSED CYCLE. A medium that takes no write never will:
 * every waiter hears the code now. So does every waiter of a seat that never
 * says it answers again — its refusal is as final as the medium makes it. A
 * medium that left is its open book's to decide (mount/away.h): the waiters
 * stay, parked until the book is taken up or given up. Any other refusal may
 * pass: the waiters stay, and the cadence waits for seat:ready — unless the
 * seat said it already while this cycle ran, when the next cycle may go at
 * once. No attempt is counted.
 */
static void SettleWaitersRefused(error_t rc)
{
    error_t code = rc;
    if (TagFSMediumLeft()) {
        __atomic_store_n(&s_parked, 1u, __ATOMIC_RELEASE);
        return;
    }
    if (TagFSReadOnly())
        code = ERR_TAGFS_READ_ONLY;
    else if (BoardroomSeatAnnouncesReady(tagfs_get_seat())) {
        if (__atomic_load_n(&s_ready_gen, __ATOMIC_ACQUIRE) == s_ready_at_begin)
            __atomic_store_n(&s_parked, 1u, __ATOMIC_RELEASE);
        return;
    }
    SettleWaitersAnswerAll(code);
}

void SettleWaitersRunEnded(error_t rc, uint64_t target, bool unclosed)
{
    if (rc != OK) {
        SettleWaitersRefused(rc);
        return;
    }
    uint64_t upto = PoolClosedSettledGen();
    if (unclosed && target > upto)
        upto = target;
    uint64_t seen = __atomic_load_n(&s_answered_upto, __ATOMIC_RELAXED);
    while (seen < upto &&
           !__atomic_compare_exchange_n(&s_answered_upto, &seen, upto, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        ;
    __atomic_store_n(&s_parked, 0u, __ATOMIC_RELEASE);
    SettleWaitersSweep();
}

void SettleSeatReady(void)
{
    __atomic_add_fetch(&s_ready_gen, 1u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_parked, 0u, __ATOMIC_RELEASE);
    SettleKnock();
}
