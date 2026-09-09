/*
 * touch_queue.c — kernel-side scheduled-touch queue.
 *
 * Unbounded singly-linked list of TouchQueueNode (each kmalloc'd, payload
 * allocated separately when > 0). No fixed capacity: out-of-memory is the
 * only ceiling. List is unsorted; TouchQueueTick scans linearly each PIT
 * tick — typical depth is small (<100), so O(N) is fine and simpler than
 * a heap. Removal is O(1) via prev pointer.
 */

#include "touch_queue.h"
#include "chit.h"     /* ChitDue — a deadline that fires is an answer determined */
#include "touch.h"
#include "klib.h"
#include "atomics.h"
#include "process.h"
#include "amp.h"
#include "lapic.h"
#include "irqchip.h"
#include "baton.h"       /* the deadline rides the process's own baton to a K-Core */
#include "sync_ops.h"    /* SyncTimeoutDeliver */

#define TQ_KIND_PUBLISH 0   /* deliver tag_id with payload via TouchPublishId */
#define TQ_KIND_WAKE    1   /* wake target_pid: state -> PROC_WORKING + IPI   */

typedef struct TouchQueueNode {
    struct TouchQueueNode *next;
    uint64_t fire_tick;
    uint32_t source_pid;
    uint32_t target_pid;
    uint32_t owes_result; /* WAKE only: 1 = an addr_park, owed ERR_TIMEOUT as a Result */
    uint32_t park_seq;    /* WAKE only: target's park_seq at arm time — which sleep this is for */
    uint16_t tag_id;
    uint16_t flags;
    uint32_t plen;
    uint8_t  kind;
    uint8_t *payload;   /* kmalloc'd separately when plen > 0; NULL otherwise */
} TouchQueueNode;

static TouchQueueNode *g_head;
static spinlock_t      g_lock;

void TouchQueueInit(void)
{
    spinlock_init(&g_lock);
    g_head = NULL;
    debug_printf("[TOUCH] queue: unbounded linked list initialized\n");
}

static TouchQueueNode *touch_queue_alloc_node(const void *payload, uint32_t plen)
{
    TouchQueueNode *n = (TouchQueueNode *)kmalloc(sizeof(TouchQueueNode));
    if (!n) return NULL;
    n->next       = NULL;
    n->fire_tick  = 0;
    n->source_pid = 0;
    n->target_pid = 0;
    n->owes_result = 0;
    n->park_seq   = 0;
    n->tag_id     = 0;
    n->flags      = 0;
    n->plen       = 0;
    n->kind       = TQ_KIND_PUBLISH;
    n->payload    = NULL;
    if (plen > 0 && payload) {
        n->payload = (uint8_t *)kmalloc(plen);
        if (!n->payload) { kfree(n); return NULL; }
        memcpy(n->payload, payload, plen);
        n->plen = plen;
    }
    return n;
}

static void touch_queue_free_node(TouchQueueNode *n)
{
    if (!n) return;
    if (n->payload) kfree(n->payload);
    kfree(n);
}

static void touch_queue_link(TouchQueueNode *n)
{
    spin_lock(&g_lock);
    n->next = g_head;
    g_head = n;
    spin_unlock(&g_lock);
}

void TouchQueueEnqueue(uint16_t tag_id, const void *payload, uint32_t plen,
                       uint64_t after_ticks, uint32_t source_pid, uint16_t flags)
{
    TouchQueueNode *n = touch_queue_alloc_node(payload, plen);
    if (!n) return;
    n->kind       = TQ_KIND_PUBLISH;
    n->tag_id     = tag_id;
    n->flags      = flags;
    n->source_pid = source_pid;
    n->fire_tick  = after_ticks;
    touch_queue_link(n);
}

void TouchQueueWakeAfter(uint32_t target_pid, uint64_t after_ticks,
                         uint32_t owes_result, uint32_t park_seq)
{
    TouchQueueNode *n = touch_queue_alloc_node(NULL, 0);
    if (!n) return;
    n->kind        = TQ_KIND_WAKE;
    n->target_pid  = target_pid;
    n->fire_tick   = after_ticks;
    n->owes_result = owes_result;
    n->park_seq    = park_seq;
    touch_queue_link(n);
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
 *      load → shootdown-timeout panic). What stood here was an irq_defer post
 *      that could be dropped, with boxlib holding a +100 ms clock over it.
 *
 * Both parts ask the same question first — is this wake still for the sleep
 * it was armed for? — and for a long time only part 2 did. A wait that ends
 * early leaves its wake armed; part 2 refused such a wake because the park had
 * moved on, while part 1 rescheduled unconditionally and so pulled the strand
 * out of whatever park came NEXT. Nothing was then owed it, so it sat in its
 * userspace wait until its own deadline arrived: a 300 ms sleep measured
 * 250 ms of processor time, on every core count, whenever any earlier wait
 * had left a wake behind. park_seq is that question for both.
 *
 * Part 1 additionally needs the strand to be WAITING; part 2 does not: a
 * deadline that fires between the park's link and its sleep (an SMI can hold
 * a K-Core that long) is still owed, and the deliverer judges it by the
 * park's state. One baton per process, so a fire that finds it already
 * queued (the previous deadline's pass not yet run) folds into that pass. */
static void touch_queue_fire_wake(uint32_t target_pid, uint32_t owes_result,
                                  uint32_t park_seq)
{
    process_t *target = process_find_ref(target_pid);
    if (!target) return;

    bool this_sleep = !target->destroying &&
                      __atomic_load_n(&target->park_seq, __ATOMIC_ACQUIRE) == park_seq;

    if (this_sleep && process_get_state(target) == PROC_WAITING) {
        /* The deadline is the event: from this tick the answer (ERR_TIMEOUT) is
         * determined and the kernel owes it. Mark it due here, where the strand
         * is provably still in this park (WAITING, park_seq unchanged, so it
         * has not re-armed the entry; the ref is held), so a delivery that
         * never comes is a chit DUE for ever, not a silence. touch_await owes
         * no Result and carries no token. */
        if (owes_result)
            ChitDue(target, target->addr_wait_entry.submit_cookie);
        process_set_state(target, PROC_WORKING);
        if (g_amp.total_cores > 1) {
            uint8_t core = target->home_core;
            if (core < g_amp.total_cores && core != amp_get_core_index()) {
                lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
            }
        }
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

    process_ref_dec(target);
}

void TouchQueueTick(uint64_t now)
{
    /* Detach all expired nodes under lock, then fire them outside the lock
     * so TouchPublishId / process state changes don't recurse on g_lock. */
    TouchQueueNode *fire_list = NULL;

    spin_lock(&g_lock);
    TouchQueueNode **cur = &g_head;
    while (*cur) {
        if (now >= (*cur)->fire_tick) {
            TouchQueueNode *n = *cur;
            *cur = n->next;
            n->next = fire_list;
            fire_list = n;
        } else {
            cur = &(*cur)->next;
        }
    }
    spin_unlock(&g_lock);

    while (fire_list) {
        TouchQueueNode *n = fire_list;
        fire_list = n->next;

        if (n->kind == TQ_KIND_WAKE) {
            touch_queue_fire_wake(n->target_pid, n->owes_result, n->park_seq);
        } else {
            TouchPublishId(n->tag_id, n->payload, n->plen,
                           n->source_pid, n->flags);
        }
        touch_queue_free_node(n);
    }
}
