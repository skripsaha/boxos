/*
 * touch_queue.c — kernel-side scheduled-touch queue (touch_queue.h).
 *
 * An unsorted singly-linked list of nodes their armers own. TouchQueueTick
 * scans it each PIT tick — the depth is the number of timed waits and delayed
 * publishes in flight — and takes every node whose tick came out in that one
 * pass, under the lock, which keeps interrupts off: a tick's cost is the
 * queue's depth, never the depth times the nodes that fire. A WAKE or CALL
 * node may be armed again the moment it leaves the queue, so what its fire
 * needs is copied into the node's own fire fields as it leaves, and the tick's
 * list runs through a link no arm touches.
 */

#include "touch_queue.h"
#include "chit.h"     /* ChitDue — a deadline that fires is an answer determined */
#include "touch.h"
#include "klib.h"
#include "atomics.h"
#include "process.h"
#include "scheduler.h"   /* sched_ring_home — one transition, one bell */
#include "baton.h"       /* the deadline rides the process's own baton */
#include "sync_ops.h"    /* SyncTimeoutDeliver */

static TouchQueueNode *g_head;
static spinlock_t      g_lock;

void TouchQueueInit(void)
{
    spinlock_init(&g_lock);
    g_head = NULL;
}

/* Under g_lock. */
static void touch_queue_link(TouchQueueNode *n, uint64_t fire_tick)
{
    n->fire_tick = fire_tick;
    if (n->armed)
        return;             /* already waiting: it moves to the new tick */
    n->armed = 1;
    n->next  = g_head;
    g_head   = n;
}

/* Under g_lock: false when `n` is not in the list. */
static bool touch_queue_unlink(TouchQueueNode *n)
{
    if (!n->armed)
        return false;
    for (TouchQueueNode **cur = &g_head; *cur; cur = &(*cur)->next) {
        if (*cur == n) {
            *cur     = n->next;
            n->next  = NULL;
            n->armed = 0;
            return true;
        }
    }
    return false;
}

TouchQueueNode *TouchQueuePublishNode(uint16_t tag_id, const void *payload, uint32_t plen,
                                      uint32_t source_pid, uint16_t flags)
{
    uint32_t        carried = (payload && plen) ? plen : 0;
    TouchQueueNode *n       = kmalloc(sizeof(TouchQueueNode) + carried);
    if (!n)
        return NULL;
    memset(n, 0, sizeof(*n));
    n->kind                 = TOUCH_QUEUE_PUBLISH;
    n->u.publish.tag_id     = tag_id;
    n->u.publish.flags      = flags;
    n->u.publish.source_pid = source_pid;
    n->u.publish.plen       = carried;
    n->u.publish.payload    = carried ? (uint8_t *)(n + 1) : NULL;
    if (carried)
        memcpy(n->u.publish.payload, payload, carried);
    return n;
}

void TouchQueueArm(TouchQueueNode *n, uint64_t fire_tick)
{
    spin_lock(&g_lock);
    touch_queue_link(n, fire_tick);
    spin_unlock(&g_lock);
}

void TouchQueueArmWake(process_t *target, uint64_t fire_tick, uint32_t owes_result,
                       uint32_t park_seq)
{
    TouchQueueNode *n = &target->park_wake;

    /* The reference the armed node holds, taken before the node can be seen:
     * the tick that fires it drops it, and so does a disarm. */
    process_ref_inc(target);
    spin_lock(&g_lock);
    bool was_armed        = n->armed;
    n->kind               = TOUCH_QUEUE_WAKE;
    n->u.wake.target      = target;
    n->u.wake.owes_result = owes_result;
    n->u.wake.park_seq    = park_seq;
    touch_queue_link(n, fire_tick);
    spin_unlock(&g_lock);
    if (was_armed)
        process_ref_dec(target);    /* the node already held one */
}

void TouchQueueArmCall(TouchQueueNode *n, void (*call)(void), uint64_t fire_tick)
{
    spin_lock(&g_lock);
    n->kind   = TOUCH_QUEUE_CALL;
    n->u.call = call;
    touch_queue_link(n, fire_tick);
    spin_unlock(&g_lock);
}

void TouchQueueDisarm(TouchQueueNode *n)
{
    spin_lock(&g_lock);
    bool       was_armed = touch_queue_unlink(n);
    process_t *held      = (was_armed && n->kind == TOUCH_QUEUE_WAKE) ? n->u.wake.target : NULL;
    spin_unlock(&g_lock);
    if (held)
        process_ref_dec(held);
}

/* A scheduled WAKE has expired (PIT-tick / interrupt context). Two parts:
 *
 *   1. The bare reschedule (PROC_WAITING -> PROC_WORKING + IPI) — done HERE, in
 *      IRQ context. It is O(1), allocation-free, touches NO VMM, and is the
 *      original, long-shipping timeout behavior. It makes the waiter runnable
 *      immediately, which is what un-stalls a single-core box whose only
 *      userspace strand is parked. It also fully serves touch_await, which has
 *      no addr_wait entry to deliver a Result to.
 *
 *   2. The pass of the process's own deadline baton (process_t.deadline_baton,
 *      a never-drop embedded node, baton.h). SyncTimeoutDeliver then runs on a
 *      K-Core and, for a park that is linked, timed and past its deadline,
 *      KResultPushes ERR_TIMEOUT (cabin VMM — MUST be off the IRQ path: VMM
 *      work in the timer IRQ starved cores of TLB-shootdown ACKs under 16-core
 *      load → shootdown-timeout panic).
 *
 * Both parts ask the same question first — is this wake still for the sleep
 * it was armed for? park_seq is that question for both: a wait that ends early
 * leaves its wake armed, and a reschedule that consulted nothing pulled the
 * strand out of whatever park came NEXT.
 *
 * Part 1 additionally needs the strand to be WAITING; part 2 does not: a
 * deadline that fires between the park's link and its sleep (an SMI can hold
 * a K-Core that long) is still owed, and the deliverer judges it by the
 * park's state. One baton per process, so a fire that finds it already
 * queued (the previous deadline's pass not yet run) folds into that pass.
 * The armed node's reference keeps the process whole through all of it. */
static void touch_queue_fire_wake(process_t *target, uint32_t owes_result, uint32_t park_seq)
{
    bool this_sleep = !target->destroying &&
                      __atomic_load_n(&target->park_seq, __ATOMIC_ACQUIRE) == park_seq;

    if (this_sleep && process_get_state(target) == PROC_WAITING) {
        /* The deadline is the event: from this tick the answer (ERR_TIMEOUT) is
         * determined and the kernel owes it. Mark it due here, where the strand
         * is provably still in this park, so a delivery that never comes is a
         * chit DUE for ever, not a silence. touch_await owes no Result and
         * carries no token. */
        if (owes_result)
            ChitDue(target, target->addr_wait_entry.submit_cookie);
        if (!process_set_state(target, PROC_WORKING))
            sched_ring_home(target);
    }

    if (this_sleep && owes_result) {
        uint8_t idle = 0;
        if (__atomic_compare_exchange_n(&target->deadline_passed, &idle, 1u,
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            process_ref_inc(target);              /* released by SyncTimeoutDeliver */
            target->deadline_baton.run = SyncTimeoutDeliver;
            target->deadline_baton.ctx = target;
            BatonPass(&target->deadline_baton);
        }
    }
}

static volatile uint8_t g_ticking;       /* a tick under way */

#ifdef CONFIG_VOLUME_TESTS
static volatile uint32_t g_tick_most;

uint32_t TouchQueueTickMost(void)
{
    return __atomic_exchange_n(&g_tick_most, 0u, __ATOMIC_ACQ_REL);
}
#endif

/* Every node whose tick came, out of the queue in one pass: the tick's own
 * list of them, oldest armed last. */
static TouchQueueNode *touch_queue_take_due(uint64_t now)
{
    TouchQueueNode  *due    = NULL;
    TouchQueueNode **tail   = &due;
    uint32_t         looked = 0;

    spin_lock(&g_lock);
    for (TouchQueueNode **cur = &g_head; *cur; looked++) {
        TouchQueueNode *n = *cur;
        if (now < n->fire_tick) {
            cur = &n->next;
            continue;
        }
        *cur          = n->next;
        n->next       = NULL;
        n->armed      = 0;
        n->fired_kind = n->kind;
        n->fired      = n->u;
        n->fired_next = NULL;
        *tail         = n;
        tail          = &n->fired_next;
    }
    spin_unlock(&g_lock);
#ifdef CONFIG_VOLUME_TESTS
    if (looked > __atomic_load_n(&g_tick_most, __ATOMIC_RELAXED))
        __atomic_store_n(&g_tick_most, looked, __ATOMIC_RELAXED);
#endif
    return due;
}

void TouchQueueTick(uint64_t now)
{
    if (__atomic_exchange_n(&g_ticking, 1u, __ATOMIC_ACQUIRE))
        return;

    /* Each node's link is read before its fire: a WAKE's fire may drop the
     * last reference to the process the node lives in, and a PUBLISH's frees
     * the node. */
    for (TouchQueueNode *n = touch_queue_take_due(now), *next; n; n = next) {
        next = n->fired_next;
        switch (n->fired_kind) {
        case TOUCH_QUEUE_WAKE:
            touch_queue_fire_wake(n->fired.wake.target, n->fired.wake.owes_result,
                                  n->fired.wake.park_seq);
            process_ref_dec(n->fired.wake.target);
            break;
        case TOUCH_QUEUE_CALL:
            n->fired.call();
            break;
        default:
            TouchPublishId(n->fired.publish.tag_id, n->fired.publish.payload,
                           n->fired.publish.plen, n->fired.publish.source_pid,
                           n->fired.publish.flags);
            kfree(n);
            break;
        }
    }
    __atomic_store_n(&g_ticking, 0u, __ATOMIC_RELEASE);
}
