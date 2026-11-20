/*
 * touch_queue.c — what the kernel does at a moment named in advance
 * (touch_queue.h).
 *
 * A pairing heap of nodes their armers own, the nearest moment at its root:
 * a node armed is melded in at once, and the nearest or any other is taken
 * out in a time that grows with the log of how many wait, not with their
 * number — a deadline armed by one strand among hundreds parked costs what it
 * costs alone, under a lock that keeps interrupts off. Its links live in the
 * nodes, so arming still cannot fail. A pass takes the nodes whose moment
 * came out from the root one after another, so it looks at the nodes that
 * fire and one more, and the keeper's one-shot is set from the root. A pass
 * takes every node whose moment came out in one go, under the lock. A WAKE or CALL node may be
 * armed again the moment it leaves the queue, so what its fire needs is
 * copied into the node's own fire fields as it leaves, and the pass's list
 * runs through a link no arm touches.
 *
 * Lock ordering: g_lock is a leaf. Under it the keeper reads the clock and
 * writes its own LAPIC (lapic_oneshot_after) — both lock-free — and nothing
 * else; the IPI that tells the keeper is sent with it let go.
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
#include "alarm_ops.h"   /* AlarmFire */
#include "amp.h"         /* the keeper: the core that ran TouchQueueInit */
#include "irqchip.h"     /* DEADLINE_VECTOR */
#include "lapic.h"       /* the keeper's one-shot, the IPI that tells it */
#include "pit.h"         /* pit_get_uptime_us — the clock every moment is named in */

static TouchQueueNode *g_root;           /* the nearest moment */
static uint64_t        g_order;          /* arms counted — under g_lock */
static spinlock_t      g_lock;
static uint64_t        g_set_us;         /* what the keeper's one-shot is set for; 0 nothing — under g_lock */
static bool            g_keeping;        /* the keeper has a one-shot (written once, at init) */
static uint8_t         g_keeper;         /* its core */

/* The one-shot's longest wait. Past it the keeper is interrupted early, finds
 * nothing due and sets it again, so a TSC whose rate is a hair off the clock's
 * never moves a far moment by more than its share of a second. */
#define TOUCH_QUEUE_LONGEST_US 1000000ull

/* Under g_lock, on the keeper: its one-shot set for the nearest moment, or
 * taken back when there is none. */
static void touch_queue_set_locked(void)
{
    uint64_t at = g_root ? g_root->fire_us : 0;
    g_set_us = at;
    if (!at) {
        lapic_oneshot_after(0);
        return;
    }
    uint64_t now  = pit_get_uptime_us();
    uint64_t wait = at > now ? at - now : 1;
    lapic_oneshot_after(wait < TOUCH_QUEUE_LONGEST_US ? wait : TOUCH_QUEUE_LONGEST_US);
}

void TouchQueueInit(void)
{
    spinlock_init(&g_lock);
    g_root    = NULL;
    g_order   = 0;
    g_set_us  = 0;
    g_keeper  = amp_get_core_index();
    g_keeping = lapic_oneshot_init(DEADLINE_VECTOR);
    if (!g_keeping)
        kprintf("[TOUCH] no one-shot timer on core %u: a moment named in advance fires on the "
                "tick, %u a second\n", (unsigned)g_keeper, (unsigned)SCHEDULER_DEFAULT_TICK_HZ);
}

/* Whether `a` fires before `b`: the nearer moment, or of one moment the
 * first armed. */
static bool touch_queue_before(const TouchQueueNode *a, const TouchQueueNode *b)
{
    return a->fire_us < b->fire_us || (a->fire_us == b->fire_us && a->order < b->order);
}

/* Two heaps, either empty, as one: the root that fires later becomes the
 * first child of the other. */
static TouchQueueNode *touch_queue_meld(TouchQueueNode *a, TouchQueueNode *b)
{
    if (!a) return b;
    if (!b) return a;
    if (touch_queue_before(b, a)) {
        TouchQueueNode *held = a;
        a = b;
        b = held;
    }
    b->prev    = a;
    b->sibling = a->child;
    if (a->child) a->child->prev = b;
    a->child   = b;
    a->sibling = NULL;
    a->prev    = NULL;
    return a;
}

/* The heaps of a list of siblings, `first` the first, as one: melded in
 * pairs from the first on, and the pairs melded from the last back — the
 * pairing that keeps each take amortised to the log of the heap. */
static TouchQueueNode *touch_queue_pairs(TouchQueueNode *first)
{
    TouchQueueNode *paired = NULL;
    while (first) {
        TouchQueueNode *a = first, *b = a->sibling;
        first      = b ? b->sibling : NULL;
        a->sibling = NULL;
        a->prev    = NULL;
        if (b) {
            b->sibling = NULL;
            b->prev    = NULL;
        }
        TouchQueueNode *made = touch_queue_meld(a, b);
        made->sibling        = paired;
        paired               = made;
    }
    TouchQueueNode *root = NULL;
    while (paired) {
        TouchQueueNode *next = paired->sibling;
        paired->sibling      = NULL;
        root                 = touch_queue_meld(root, paired);
        paired               = next;
    }
    return root;
}

/* Under g_lock: `n` out of the heap — the root, or any node cut from its
 * parent with what lies under it, melded back in; false when it is not
 * armed. */
static bool touch_queue_unlink(TouchQueueNode *n)
{
    if (!n->armed)
        return false;
    TouchQueueNode *under = touch_queue_pairs(n->child);
    n->child              = NULL;
    if (n == g_root) {
        g_root = under;
    } else {
        if (n->prev->child == n) n->prev->child = n->sibling;
        else                     n->prev->sibling = n->sibling;
        if (n->sibling) n->sibling->prev = n->prev;
        g_root = touch_queue_meld(g_root, under);
    }
    n->sibling = NULL;
    n->prev    = NULL;
    n->armed   = 0;
    return true;
}

/* Under g_lock: `n` at its moment, after every node of the same moment or
 * nearer — a node already waiting moves. When it is now the nearest and nearer
 * than the keeper's one-shot is set for, the one-shot is set again: here, on
 * the keeper; true when another core must tell the keeper, its lock let go. */
static bool touch_queue_link(TouchQueueNode *n, uint64_t fire_us)
{
    (void)touch_queue_unlink(n);
    n->fire_us = fire_us;
    n->order   = ++g_order;
    n->child   = NULL;
    n->sibling = NULL;
    n->prev    = NULL;
    n->armed   = 1;
    g_root     = touch_queue_meld(g_root, n);
    if (!g_keeping || g_root != n || (g_set_us != 0 && fire_us >= g_set_us))
        return false;
    if (amp_get_core_index() == g_keeper) {
        touch_queue_set_locked();
        return false;
    }
    return true;
}

/* A nearer moment named on another core: the keeper told to look again. */
static void touch_queue_tell_keeper(bool tell)
{
    if (tell)
        lapic_send_ipi(g_amp.cores[g_keeper].lapic_id, DEADLINE_VECTOR);
}

TouchQueueNode *TouchQueuePublishNode(uint32_t tag_id, const void *payload, uint32_t plen,
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

void TouchQueueArm(TouchQueueNode *n, uint64_t fire_us)
{
    spin_lock(&g_lock);
    bool tell = touch_queue_link(n, fire_us);
    spin_unlock(&g_lock);
    touch_queue_tell_keeper(tell);
}

void TouchQueueArmWake(process_t *target, uint64_t fire_us, uint32_t owes_result,
                       uint32_t park_seq)
{
    TouchQueueNode *n = &target->park_wake;

    /* The reference the armed node holds, taken before the node can be seen:
     * the pass that fires it drops it, and so does a disarm. */
    process_ref_inc(target);
    spin_lock(&g_lock);
    bool was_armed        = n->armed;
    n->kind               = TOUCH_QUEUE_WAKE;
    n->u.wake.target      = target;
    n->u.wake.owes_result = owes_result;
    n->u.wake.park_seq    = park_seq;
    bool tell             = touch_queue_link(n, fire_us);
    spin_unlock(&g_lock);
    touch_queue_tell_keeper(tell);
    if (was_armed)
        process_ref_dec(target);    /* the node already held one */
}

void TouchQueueArmAlarm(process_t *target, uint64_t fire_us)
{
    TouchQueueNode *n = &target->alarm_node;

    /* The reference the armed node holds, taken before the node can be seen,
     * as the wake's is. */
    process_ref_inc(target);
    spin_lock(&g_lock);
    bool was_armed       = n->armed;
    n->kind              = TOUCH_QUEUE_ALARM;
    n->u.alarm.target    = target;
    bool tell            = touch_queue_link(n, fire_us);
    spin_unlock(&g_lock);
    touch_queue_tell_keeper(tell);
    if (was_armed)
        process_ref_dec(target);    /* the node already held one */
}

void TouchQueueArmCall(TouchQueueNode *n, void (*call)(void), uint64_t fire_us)
{
    spin_lock(&g_lock);
    n->kind   = TOUCH_QUEUE_CALL;
    n->u.call = call;
    bool tell = touch_queue_link(n, fire_us);
    spin_unlock(&g_lock);
    touch_queue_tell_keeper(tell);
}

void TouchQueueDisarm(TouchQueueNode *n)
{
    spin_lock(&g_lock);
    bool       was_armed = touch_queue_unlink(n);
    process_t *held      = NULL;
    if (was_armed && n->kind == TOUCH_QUEUE_WAKE)
        held = n->u.wake.target;
    else if (was_armed && n->kind == TOUCH_QUEUE_ALARM)
        held = n->u.alarm.target;
    spin_unlock(&g_lock);
    if (held)
        process_ref_dec(held);
}

/* A scheduled WAKE has expired (interrupt context: the keeper's one-shot, or
 * the tick). Two parts:
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
        /* The deadline is the event: from this moment the answer (ERR_TIMEOUT) is
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

static volatile uint8_t g_firing;        /* a pass under way */

#ifdef CONFIG_VOLUME_TESTS
static volatile uint32_t g_fire_most;

uint32_t TouchQueueFireMost(void)
{
    return __atomic_exchange_n(&g_fire_most, 0u, __ATOMIC_ACQ_REL);
}
#endif

/* Every node whose moment came, out of the queue in one pass: the pass's own
 * list of them, nearest first. The root is the nearest, so the pass takes the
 * root while its moment came and stops at the first still to come. */
static TouchQueueNode *touch_queue_take_due(uint64_t now_us)
{
    TouchQueueNode  *due    = NULL;
    TouchQueueNode **tail   = &due;
    uint32_t         looked = 0;

    spin_lock(&g_lock);
    while (g_root) {
        TouchQueueNode *n = g_root;
        looked++;
        if (now_us < n->fire_us)
            break;
        (void)touch_queue_unlink(n);
        n->fired_kind = n->kind;
        n->fired      = n->u;
        n->fired_next = NULL;
        *tail         = n;
        tail          = &n->fired_next;
    }
    spin_unlock(&g_lock);
#ifdef CONFIG_VOLUME_TESTS
    if (looked > __atomic_load_n(&g_fire_most, __ATOMIC_RELAXED))
        __atomic_store_n(&g_fire_most, looked, __ATOMIC_RELAXED);
#endif
    return due;
}

/* The pass: false when another was under way and this one did nothing. */
static bool touch_queue_fire_due(uint64_t now_us)
{
    if (__atomic_exchange_n(&g_firing, 1u, __ATOMIC_ACQUIRE))
        return false;

    /* Each node's link is read before its fire: a WAKE's fire may drop the
     * last reference to the process the node lives in, and a PUBLISH's frees
     * the node. */
    for (TouchQueueNode *n = touch_queue_take_due(now_us), *next; n; n = next) {
        next = n->fired_next;
        switch (n->fired_kind) {
        case TOUCH_QUEUE_WAKE:
            touch_queue_fire_wake(n->fired.wake.target, n->fired.wake.owes_result,
                                  n->fired.wake.park_seq);
            process_ref_dec(n->fired.wake.target);
            break;
        case TOUCH_QUEUE_ALARM:
            AlarmFire(n->fired.alarm.target);
            process_ref_dec(n->fired.alarm.target);
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
    __atomic_store_n(&g_firing, 0u, __ATOMIC_RELEASE);
    return true;
}

void TouchQueueFireDue(uint64_t now_us)
{
    (void)touch_queue_fire_due(now_us);
}

/* A pass the keeper's interrupt found under way — the startup tests run one
 * with interrupts on — leaves the next look this far out, rather than an
 * interrupt a microsecond for as long as that pass runs. */
#define TOUCH_QUEUE_BUSY_US 100u

void TouchQueueDeadline(void)
{
    bool fired = touch_queue_fire_due(pit_get_uptime_us());
    spin_lock(&g_lock);
    if (fired) {
        touch_queue_set_locked();
    } else {
        g_set_us = pit_get_uptime_us() + TOUCH_QUEUE_BUSY_US;
        lapic_oneshot_after(TOUCH_QUEUE_BUSY_US);
    }
    spin_unlock(&g_lock);
}
