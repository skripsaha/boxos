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
#include "touch.h"
#include "klib.h"
#include "atomics.h"
#include "process.h"
#include "amp.h"
#include "lapic.h"
#include "irqchip.h"

#define TQ_KIND_PUBLISH 0   /* deliver tag_id with payload via TouchPublishId */
#define TQ_KIND_WAKE    1   /* wake target_pid: state -> PROC_WORKING + IPI   */

typedef struct TouchQueueNode {
    struct TouchQueueNode *next;
    uint64_t fire_tick;
    uint32_t source_pid;
    uint32_t target_pid;
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

void TouchQueueWakeAfter(uint32_t target_pid, uint64_t after_ticks)
{
    TouchQueueNode *n = touch_queue_alloc_node(NULL, 0);
    if (!n) return;
    n->kind       = TQ_KIND_WAKE;
    n->target_pid = target_pid;
    n->fire_tick  = after_ticks;
    touch_queue_link(n);
}

static void touch_queue_fire_wake(uint32_t target_pid)
{
    process_t *target = process_find_ref(target_pid);
    if (!target) return;
    if (target->destroying) { process_ref_dec(target); return; }
    if (process_get_state(target) == PROC_WAITING) {
        process_set_state(target, PROC_WORKING);
        if (g_amp.total_cores > 1) {
            uint8_t core = target->home_core;
            if (core < g_amp.total_cores && core != amp_get_core_index()) {
                lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
            }
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
            touch_queue_fire_wake(n->target_pid);
        } else {
            TouchPublishId(n->tag_id, n->payload, n->plen,
                           n->source_pid, n->flags);
        }
        touch_queue_free_node(n);
    }
}
