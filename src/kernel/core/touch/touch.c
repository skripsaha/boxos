#include "touch.h"
#include "sync_ops.h"
#include "logbook.h"
#include "touch_queue.h"
#include "touch_ring.h"
#include "pit.h"
#include "process.h"
#include "kring.h"
#include "kresult.h"
#include "result.h"
#include "klib.h"
#include "clockboard.h"
#include "vmm.h"
#include "pmm.h"
#include "tagfs.h"
#include "atomics.h"
#include "amp.h"
#include "per_core.h"
#include "kcore.h"
#include "lapic.h"
#include "irqchip.h"
#include "baton.h"
#include "kernel_config.h"
#include "manifest_exec.h"
#include "boxos_crate.h"
#include "op_registry.h"

static TouchBucket *g_bucket_l1[TOUCH_BUCKET_L1_ENTRIES];

static volatile uint32_t g_total_subs;

static volatile uint64_t g_touch_publish_calls;
static volatile uint64_t g_touch_subscribers_visited;
static volatile uint64_t g_touch_delivered;
static volatile uint64_t g_touch_emit_fail;
static volatile uint64_t g_touch_push_fail;

static uint32_t          g_react_depth[CONFIG_MAX_CORES];
static volatile uint64_t g_react_depth_drops;
#define TOUCH_DROP_ANNOUNCE_MS 1000u
static volatile uint64_t g_touch_drop_announced_ms;

void TouchStatsSnapshot(uint64_t out[5])
{
    out[0] = __atomic_load_n(&g_touch_publish_calls,       __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_touch_subscribers_visited, __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_touch_delivered,           __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_touch_emit_fail,           __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&g_touch_push_fail,           __ATOMIC_RELAXED);
}

void TouchInit(void)
{
    memset(g_bucket_l1, 0, sizeof(g_bucket_l1));
    TouchQueueInit();
    debug_printf("[TOUCH] inverted-index subsystem initialized (L1=%u slots)\n",
                 TOUCH_BUCKET_L1_ENTRIES);
}

static inline TouchBucket *touch_bucket_lookup(TouchTag tag_id)
{
    if (tag_id == TOUCH_TAG_INVALID) return NULL;
    TouchBucket *leaf = __atomic_load_n(&g_bucket_l1[tag_id >> 8],
                                        __ATOMIC_ACQUIRE);
    if (!leaf) return NULL;
    return &leaf[tag_id & 0xFF];
}

static TouchBucket *touch_bucket_get_or_create(TouchTag tag_id)
{
    if (tag_id == TOUCH_TAG_INVALID) return NULL;
    uint16_t hi = tag_id >> 8;
    uint16_t lo = tag_id & 0xFF;
    TouchBucket *leaf = __atomic_load_n(&g_bucket_l1[hi], __ATOMIC_ACQUIRE);
    if (!leaf) {
        TouchBucket *fresh = (TouchBucket *)kmalloc(sizeof(TouchBucket) *
                                                    TOUCH_BUCKET_L2_ENTRIES);
        if (!fresh) return NULL;
        memset(fresh, 0, sizeof(TouchBucket) * TOUCH_BUCKET_L2_ENTRIES);
        for (uint32_t i = 0; i < TOUCH_BUCKET_L2_ENTRIES; i++) {
            spinlock_init(&fresh[i].lock);
            fresh[i].tag_id = (uint16_t)((hi << 8) | i);
        }
        TouchBucket *expected = NULL;
        if (!__atomic_compare_exchange_n(&g_bucket_l1[hi], &expected, fresh,
                                         false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            kfree(fresh);
            leaf = expected;
        } else {
            leaf = fresh;
        }
    }
    return &leaf[lo];
}


void TouchTagResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare)
{
    *out_full = TOUCH_TAG_INVALID;
    *out_bare = TOUCH_TAG_INVALID;
    if (!tag || tag[0] == '\0') return;

    TouchLogbookLookup(tag, out_full, out_bare);
    if (*out_bare != TOUCH_TAG_INVALID) return;

    char key[256], value[256];
    tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));

    bool has_value   = (value[0] != '\0');
    bool is_wildcard = has_value && value[0] == '.' && value[1] == '.' &&
                                    value[2] == '.' && value[3] == '\0';

    *out_bare = tagfs_tag_intern(key);
    if (*out_bare == TAGFS_INVALID_TAG_ID) *out_bare = TOUCH_TAG_INVALID;

    if (has_value && !is_wildcard) {
        char full[512];
        tagfs_format_tag(full, sizeof(full), key, value);
        *out_full = tagfs_tag_intern(full);
        if (*out_full == TAGFS_INVALID_TAG_ID) *out_full = TOUCH_TAG_INVALID;
    }
}



error_t TouchPolicySet(TouchTag tag_id, TouchPolicy policy, TouchCapability cap)
{
    TouchBucket *b = touch_bucket_get_or_create(tag_id);
    if (!b) return ERR_NO_MEMORY;
    spin_lock(&b->lock);
    b->policy     = (uint8_t)policy;
    b->capability = (uint8_t)cap;
    b->flags     |= TOUCH_BUCKET_FLAG_REGISTERED;
    spin_unlock(&b->lock);
    return OK;
}

bool TouchPolicyGet(TouchTag tag_id, TouchPolicy *out_policy,
                    TouchCapability *out_cap)
{
    TouchBucket *b = touch_bucket_lookup(tag_id);
    if (!b || !(b->flags & TOUCH_BUCKET_FLAG_REGISTERED)) {
        if (out_policy) *out_policy = TOUCH_POLICY_EDGE;
        if (out_cap)    *out_cap    = TOUCH_CAP_OPEN;
        return false;
    }
    if (out_policy) *out_policy = (TouchPolicy)b->policy;
    if (out_cap)    *out_cap    = (TouchCapability)b->capability;
    return true;
}

uint32_t TouchPolicyLatchedSnapshot(TouchTag tag_id, uint8_t *out_buf)
{
    if (!out_buf) return 0;
    TouchBucket *b = touch_bucket_lookup(tag_id);
    if (!b) return 0;
    uint32_t plen = 0;
    spin_lock(&b->lock);
    if (b->latched_payload && b->latched_plen > 0) {
        plen = b->latched_plen;
        if (plen > BOXOS_TOUCH_PAYLOAD_MAX) plen = BOXOS_TOUCH_PAYLOAD_MAX;
        memcpy(out_buf, b->latched_payload, plen);
    }
    spin_unlock(&b->lock);
    return plen;
}

uint8_t TouchPolicyLevelState(TouchTag tag_id)
{
    TouchBucket *b = touch_bucket_lookup(tag_id);
    if (!b) return 0;
    return b->level_state;
}

void TouchPolicySetLevelState(TouchTag tag_id, uint8_t state)
{
    TouchBucket *b = touch_bucket_get_or_create(tag_id);
    if (!b) return;
    b->level_state = state;
}

bool TouchHasAnyListenersForTag(TouchTag tag_id)
{
    TouchBucket *b = touch_bucket_lookup(tag_id);
    if (!b) return false;
    return __atomic_load_n(&b->sub_count,   __ATOMIC_ACQUIRE) > 0 ||
           __atomic_load_n(&b->watch_count, __ATOMIC_ACQUIRE) > 0;
}

bool TouchHasAnyListeners(void)
{
    return __atomic_load_n(&g_total_subs, __ATOMIC_ACQUIRE) > 0;
}

static uint64_t touch_emit_payload(process_t *target, TouchTag tag_id,
                                   uint16_t flags, uint32_t source_pid,
                                   const void *kpayload, uint32_t plen)
{
    uint64_t total = sizeof(Touch) + plen;
    uint32_t pages = (uint32_t)((total + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    uint64_t bytes = (uint64_t)pages * PMM_PAGE_SIZE;
    uint64_t vaddr = __atomic_fetch_add(&target->cabin->buf_heap_next, bytes,
                                        __ATOMIC_ACQ_REL);

    for (uint32_t i = 0; i < pages; i++) {
        void *page = pmm_alloc(1);
        if (!page) return 0;
        uint64_t va = vaddr + (i * PMM_PAGE_SIZE);
        vmm_map_result_t r = vmm_map_page(target->cabin->vmm, va,
                                          (uint64_t)page, VMM_FLAGS_USER_RW);
        if (!r.success) { pmm_free(page, 1); return 0; }
    }

    Touch hdr = {
        .tag_id        = tag_id,
        .flags         = flags,
        .source_pid    = source_pid,
        .payload_len   = plen,
        .payload_addr  = (plen > 0) ? (vaddr + sizeof(Touch)) : 0,
        .timestamp_tsc = rdtsc(),
        .reserved      = 0,
    };
    if (vmm_user_buf_commit_out(target->cabin->vmm, vaddr,
                                &hdr, sizeof(Touch)) != OK)
        return 0;
    if (plen > 0 && kpayload &&
        vmm_user_buf_commit_out(target->cabin->vmm, vaddr + sizeof(Touch),
                                kpayload, plen) != OK)
        return 0;
    return vaddr;
}

static void touch_wake_remote(process_t *target)
{
    if (g_amp.total_cores <= 1) return;
    uint8_t core = target->home_core;
    if (core < g_amp.total_cores && core != amp_get_core_index()) {
        lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
    }
}


typedef struct TouchOwed {
    struct TouchOwed *next;
    uint16_t tag_id;
    uint16_t flags;
    uint32_t source_pid;
    uint32_t plen;
    uint8_t  payload[BOXOS_TOUCH_PAYLOAD_MAX];
} TouchOwed;

static TouchRing *touch_owed_ring(process_t *proc)
{
    if (!proc || !proc->touch_ring_phys) return NULL;
    return (TouchRing *)vmm_phys_to_virt(proc->touch_ring_phys);
}

static uint32_t touch_owed_bound(process_t *proc)
{
    TouchRing *rr = touch_owed_ring(proc);
    return rr ? rr->hdr.slot_count_max : 0u;
}

static void touch_owed_slip_locked(process_t *proc, uint32_t depth)
{
    TouchRing *rr = touch_owed_ring(proc);
    if (rr) __atomic_store_n(&rr->hdr.owed, (uint64_t)depth, __ATOMIC_RELEASE);
}

static bool touch_owed_append(process_t *proc, TouchTag tag_id, uint16_t flags,
                              uint32_t source_pid, const void *kpayload,
                              uint32_t plen)
{
    uint32_t bound = touch_owed_bound(proc);
    if (bound == 0) return false;
    if (__atomic_load_n(&proc->owed_count, __ATOMIC_RELAXED) >= bound)
        return false;

    if (plen > BOXOS_TOUCH_PAYLOAD_MAX) plen = BOXOS_TOUCH_PAYLOAD_MAX;

    TouchOwed *n = (TouchOwed *)kmalloc(sizeof(TouchOwed));
    if (!n) return false;
    n->next       = NULL;
    n->tag_id     = tag_id;
    n->flags      = flags;
    n->source_pid = source_pid;
    n->plen       = plen;
    if (plen > 0 && kpayload) memcpy(n->payload, kpayload, plen);

    bool taken = false;
    spin_lock(&proc->owed_lock);
    uint32_t depth = __atomic_load_n(&proc->owed_count, __ATOMIC_RELAXED);
    if (depth < bound) {
        if (proc->owed_tail) proc->owed_tail->next = n;
        else                 proc->owed_head       = n;
        proc->owed_tail = n;
        depth++;
        __atomic_store_n(&proc->owed_count, depth, __ATOMIC_RELEASE);
        touch_owed_slip_locked(proc, depth);
        taken = true;
    }
    spin_unlock(&proc->owed_lock);

    if (!taken) { kfree(n); return false; }

    if (process_get_state(proc) == PROC_WAITING)
        process_set_state(proc, PROC_WORKING);
    touch_wake_remote(proc);
    return true;
}

void TouchOwedHandOver(process_t *proc)
{
    if (!proc) return;
    if (__atomic_load_n(&proc->owed_count, __ATOMIC_RELAXED) == 0) return;

    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&proc->owed_draining, &expected, 1u,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;

    for (;;) {
        spin_lock(&proc->owed_lock);
        TouchOwed *n = proc->owed_head;
        if (n) {
            proc->owed_head = n->next;
            if (!proc->owed_head) proc->owed_tail = NULL;
            n->next = NULL;
        }
        spin_unlock(&proc->owed_lock);
        if (!n) break;

        if (!KTouchPush(proc, n->tag_id, n->flags, n->source_pid,
                        n->plen ? n->payload : NULL, n->plen)) {
            spin_lock(&proc->owed_lock);
            n->next = proc->owed_head;
            proc->owed_head = n;
            if (!proc->owed_tail) proc->owed_tail = n;
            spin_unlock(&proc->owed_lock);
            break;
        }

        kfree(n);
        spin_lock(&proc->owed_lock);
        uint32_t left = __atomic_sub_fetch(&proc->owed_count, 1, __ATOMIC_ACQ_REL);
        touch_owed_slip_locked(proc, left);
        spin_unlock(&proc->owed_lock);

        __atomic_add_fetch(&g_touch_delivered, 1, __ATOMIC_RELAXED);
    }

    __atomic_store_n(&proc->owed_draining, 0u, __ATOMIC_RELEASE);
}

void TouchOwedRelease(process_t *proc)
{
    if (!proc) return;

    spin_lock(&proc->owed_lock);
    TouchOwed *n = proc->owed_head;
    proc->owed_head  = NULL;
    proc->owed_tail  = NULL;
    __atomic_store_n(&proc->owed_count, 0u, __ATOMIC_RELEASE);
    touch_owed_slip_locked(proc, 0u);
    spin_unlock(&proc->owed_lock);

    while (n) {
        TouchOwed *next = n->next;
        kfree(n);
        n = next;
    }
}

static void touch_drop_announce(process_t *target, TouchTag tag_id)
{
    uint64_t fails = __atomic_add_fetch(&g_touch_push_fail, 1, __ATOMIC_RELAXED);
    uint64_t now_ms = clockboard_uptime_ms();
    uint64_t last   = __atomic_load_n(&g_touch_drop_announced_ms, __ATOMIC_RELAXED);
    if (now_ms - last >= TOUCH_DROP_ANNOUNCE_MS &&
        __atomic_compare_exchange_n(&g_touch_drop_announced_ms, &last, now_ms,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        kprintf("[TOUCH] ERROR: dropped a publish to pid %u (tag %u) — ring full "
                "AND its owed queue is a full ring behind (%u held); %lu dropped "
                "since boot. That subscriber has consumed nothing for a whole "
                "ring's worth of events; one blocked on this event will not be "
                "woken by it\n",
                target->pid, (unsigned)tag_id,
                (unsigned)__atomic_load_n(&target->owed_count, __ATOMIC_RELAXED),
                (unsigned long)fails);
    }
}

void TouchRestDeliver(process_t *target, TouchTag tag_id,
                      const void *kpayload, uint32_t plen,
                      uint32_t source_pid, uint16_t flags)
{
    if (!target) return;

    if (__atomic_load_n(&target->destroying, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_touch_push_fail, 1, __ATOMIC_RELAXED);
        return;
    }

    if (__atomic_load_n(&target->owed_count, __ATOMIC_RELAXED) != 0) {
        TouchOwedHandOver(target);
        if (__atomic_load_n(&target->owed_count, __ATOMIC_RELAXED) != 0) {
            if (!touch_owed_append(target, tag_id, flags, source_pid,
                                   kpayload, plen))
                touch_drop_announce(target, tag_id);
            return;
        }
    }

    if (KTouchPush(target, tag_id, flags, source_pid, kpayload, plen)) {
        __atomic_add_fetch(&g_touch_delivered, 1, __ATOMIC_RELAXED);
        touch_wake_remote(target);
        return;
    }

    if (touch_owed_append(target, tag_id, flags, source_pid, kpayload, plen))
        return;

    touch_drop_announce(target, tag_id);
    touch_wake_remote(target);
}

static void touch_interrupt_deliver(process_t *proc, TouchSub *sub,
                                    TouchTag tag_id, const void *kpayload,
                                    uint32_t plen, uint32_t source_pid,
                                    uint16_t flags)
{
    if (process_get_state(proc) != PROC_WAITING) return;

    spin_lock(&proc->irq_lock);

    if (proc->irq_active) {
        TouchPending *p = kmalloc(sizeof(TouchPending));
        if (p) {
            p->tag_id     = tag_id;
            p->flags      = flags;
            p->source_pid = source_pid;
            p->plen       = plen > sizeof(p->payload) ? sizeof(p->payload) : plen;
            if (p->plen > 0 && kpayload) memcpy(p->payload, kpayload, p->plen);
            p->next = (TouchPending *)proc->irq_pending_head;
            proc->irq_pending_head = p;
        }
        spin_unlock(&proc->irq_lock);
        return;
    }

    uint64_t touch_vaddr = touch_emit_payload(proc, tag_id, flags, source_pid,
                                              kpayload, plen);
    if (touch_vaddr == 0) {
        spin_unlock(&proc->irq_lock);
        return;
    }

    proc->irq_saved_rip    = proc->context.rip;
    proc->irq_saved_rsp    = proc->context.rsp;
    proc->irq_saved_rflags = proc->context.rflags;

    proc->context.rip = sub->u.irq.handler_addr;
    proc->context.rsp = sub->u.irq.stack_top;
    proc->context.rdi = touch_vaddr;
    proc->irq_active  = 1;

    spin_unlock(&proc->irq_lock);

    process_set_state(proc, PROC_WORKING);
    touch_wake_remote(proc);
}

static bool touch_depth_enter(uint8_t core)
{
    uint32_t depth = __atomic_add_fetch(&g_react_depth[core], 1, __ATOMIC_RELAXED);

    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    uint64_t top   = per_core_current_kstack_top();
    uint64_t floor = per_core_current_kstack_floor();
    bool drop;
    if (top != 0 && floor != 0 && rsp > floor && rsp <= top) {
        drop = (rsp - floor) < CONFIG_TOUCH_REACT_STACK_MARGIN;
    } else {
        drop = (depth > CONFIG_TOUCH_REACT_DEPTH_MAX);
    }
    if (drop) {
        __atomic_sub_fetch(&g_react_depth[core], 1, __ATOMIC_RELAXED);
        uint64_t n = __atomic_add_fetch(&g_react_depth_drops, 1, __ATOMIC_RELAXED);
        if ((n & (n - 1)) == 0)
            debug_printf("[TOUCH] re-entry guard drop on core %u (drops=%lu)\n",
                         (unsigned)core, (unsigned long)n);
        return false;
    }
    return true;
}

static inline void touch_depth_leave(uint8_t core)
{
    __atomic_sub_fetch(&g_react_depth[core], 1, __ATOMIC_RELAXED);
}

static void touch_react_deliver(process_t *proc, TouchSub *sub,
                                TouchTag tag_id, const void *kpayload,
                                uint32_t plen, uint32_t source_pid,
                                uint16_t flags)
{
    if (sub->u.manifest == MANIFEST_HANDLE_INVALID) return;

    uint8_t core = amp_get_core_index();
    if (!touch_depth_enter(core)) return;

    uint64_t vaddr = touch_emit_payload(proc, tag_id, flags, source_pid,
                                        kpayload, plen);
    if (vaddr == 0) goto out_dec;

    uint64_t total = sizeof(Touch) + plen;
    Crate *crates_kbuf = (Crate *)kmalloc(sizeof(Crate));
    if (!crates_kbuf) goto out_dec;
    *crates_kbuf = (Crate){
        .magic    = CRATE_MAGIC,
        .flags    = 0,
        .kind     = CRATE_KIND_INPUT,
        ._pad0    = 0,
        ._pad1    = 0,
        .addr     = vaddr,
        .size     = total,
        .capacity = total,
    };

    bool async_owned = false;
    OpContext ctx = {
        .proc              = proc,
        .target_pid        = 0,
        .flags             = 0,
        .pier_id           = 0,
        .crate_count       = 1,
        .crates_uaddr      = 0,
        .async_owns_crates = &async_owned,
    };
    ManifestExecResult exec_result;
    ManifestExecute(sub->u.manifest, crates_kbuf, 1, &ctx, &exec_result);

    if (!async_owned) {
        kfree(crates_kbuf);
    }

out_dec:
    touch_depth_leave(core);
}


struct TouchWatch {
    struct TouchWatch *next;
    struct TouchWatch *prev;
    TouchBucket       *bucket;
    TouchWatchFn       fn;
    void              *ctx;
    atomic_u32_t       ref;
    TouchTag           tag_id;
};

static void touch_watch_release(TouchWatch *w)
{
    if (__atomic_sub_fetch(&w->ref, 1, __ATOMIC_ACQ_REL) != 0) return;
    kfree(w);
}

TouchWatch *TouchWatchSet(TouchTag tag_id, TouchWatchFn fn, void *ctx)
{
    if (!fn || tag_id == TOUCH_TAG_INVALID) return NULL;

    TouchBucket *b = touch_bucket_get_or_create(tag_id);
    if (!b) return NULL;

    TouchWatch *w = (TouchWatch *)kmalloc(sizeof(TouchWatch));
    if (!w) return NULL;

    w->fn     = fn;
    w->ctx    = ctx;
    w->bucket = b;
    w->tag_id = tag_id;
    w->ref    = 1;
    w->prev   = NULL;

    spin_lock(&b->lock);
    w->next = b->watch_head;
    if (b->watch_head) b->watch_head->prev = w;
    b->watch_head = w;
    __atomic_add_fetch(&b->watch_count, 1, __ATOMIC_RELEASE);
    spin_unlock(&b->lock);

    __atomic_add_fetch(&g_total_subs, 1, __ATOMIC_RELAXED);
    return w;
}

void TouchWatchClear(TouchWatch *w)
{
    if (!w) return;
    TouchBucket *b = w->bucket;

    spin_lock(&b->lock);
    if (w->prev) w->prev->next = w->next;
    else if (b->watch_head == w) b->watch_head = w->next;
    if (w->next) w->next->prev = w->prev;
    w->next = NULL;
    w->prev = NULL;
    __atomic_sub_fetch(&b->watch_count, 1, __ATOMIC_RELEASE);
    spin_unlock(&b->lock);

    __atomic_sub_fetch(&g_total_subs, 1, __ATOMIC_RELAXED);
    touch_watch_release(w);
}

uint32_t TouchWatchCount(TouchTag tag_id)
{
    TouchBucket *b = touch_bucket_lookup(tag_id);
    if (!b) return 0;
    return __atomic_load_n(&b->watch_count, __ATOMIC_ACQUIRE);
}

static void touch_sub_release(TouchSub *sub)
{
    if (__atomic_sub_fetch(&sub->ref, 1, __ATOMIC_ACQ_REL) != 0) return;
    if (sub->mode == TOUCH_REACT && sub->u.manifest != MANIFEST_HANDLE_INVALID)
        ManifestRelease(sub->u.manifest);
    kfree(sub);
}


typedef struct {
    process_t       *proc;
    TouchSub        *sub;
    uint8_t          mode;
} TouchSnap;

enum { TOUCH_SNAP_STACK = 64 };

static bool deliver_one(TouchSnap *e, TouchTag tag_id,
                        const void *kpayload, uint32_t plen,
                        uint32_t source_pid, uint16_t flags,
                        TouchPolicy policy)
{
    if (e->proc->destroying) return false;

    if (policy == TOUCH_POLICY_LATCHED) {
        uint8_t expected = 0;
        if (!__atomic_compare_exchange_n(&e->sub->has_pending, &expected, 1,
                                         false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED)) {
            return true;
        }
        e->sub->pending_plen = plen > sizeof(e->sub->pending_payload)
                               ? (uint32_t)sizeof(e->sub->pending_payload)
                               : plen;
        if (e->sub->pending_plen > 0 && kpayload)
            memcpy(e->sub->pending_payload, kpayload, e->sub->pending_plen);

        TouchBucket *bcb = e->sub->bucket;
        if (bcb) {
            uint32_t copy_plen = (plen > BOXOS_TOUCH_PAYLOAD_MAX)
                                 ? BOXOS_TOUCH_PAYLOAD_MAX
                                 : plen;
            spin_lock(&bcb->lock);
            if (!bcb->latched_payload) {
                bcb->latched_payload = (uint8_t *)kmalloc(BOXOS_TOUCH_PAYLOAD_MAX);
            }
            if (bcb->latched_payload) {
                if (copy_plen > 0 && kpayload) {
                    memcpy(bcb->latched_payload, kpayload, copy_plen);
                }
                bcb->latched_plen = (uint16_t)copy_plen;
            }
            spin_unlock(&bcb->lock);
        }
    }

    switch ((TouchMode)e->mode) {
    case TOUCH_REST:
        TouchRestDeliver(e->proc, tag_id, kpayload, plen, source_pid, flags);
        break;
    case TOUCH_REACT:
        touch_react_deliver(e->proc, e->sub, tag_id, kpayload, plen,
                            source_pid, flags);
        break;
    case TOUCH_INTERRUPT:
        touch_interrupt_deliver(e->proc, e->sub, tag_id, kpayload, plen,
                                source_pid, flags);
        break;
    }
    return true;
}

static void touch_watch_deliver(TouchBucket *b, TouchTag tag_id,
                                const void *kpayload, uint32_t plen,
                                uint32_t source_pid)
{
    enum { WATCH_SNAP_STACK = 8 };
    TouchWatch *stack[WATCH_SNAP_STACK];
    TouchWatch **snap = stack;
    uint32_t snap_cap = WATCH_SNAP_STACK;
    uint32_t snap_n   = 0;

    spin_lock(&b->lock);
    for (TouchWatch *w = b->watch_head; w; w = w->next) {
        if (snap_n >= snap_cap) {
            uint32_t new_cap = snap_cap * 4;
            TouchWatch **heap_buf = (TouchWatch **)kmalloc(sizeof(TouchWatch *) * new_cap);
            if (!heap_buf) break;
            memcpy(heap_buf, snap, sizeof(TouchWatch *) * snap_n);
            if (snap != stack) kfree(snap);
            snap = heap_buf;
            snap_cap = new_cap;
        }
        __atomic_add_fetch(&w->ref, 1, __ATOMIC_RELAXED);
        snap[snap_n++] = w;
    }
    spin_unlock(&b->lock);

    uint8_t core = amp_get_core_index();
    if (touch_depth_enter(core)) {
        for (uint32_t i = 0; i < snap_n; i++)
            snap[i]->fn(tag_id, kpayload, plen, source_pid, snap[i]->ctx);
        touch_depth_leave(core);
    }

    for (uint32_t i = 0; i < snap_n; i++) touch_watch_release(snap[i]);
    if (snap != stack) kfree(snap);
}

uint32_t TouchPublishId(TouchTag tag_id, const void *kpayload, uint32_t plen,
                        uint32_t source_pid, uint16_t flags)
{
    TouchBucket *b = touch_bucket_lookup(tag_id);
    if (!b) return 0;

    TouchPolicy     policy = (TouchPolicy)b->policy;
    TouchCapability cap    = (TouchCapability)b->capability;

    if (cap == TOUCH_CAP_KERNEL_ONLY && source_pid != 0) return 0;

    if (policy == TOUCH_POLICY_LEVEL) {
        uint8_t new_state = (plen > 0 && kpayload)
                            ? ((const uint8_t *)kpayload)[0] : 0;
        b->level_state = new_state;
        if (new_state == 0) return 0;
    }

    uint32_t n_subs   = __atomic_load_n(&b->sub_count,   __ATOMIC_ACQUIRE);
    uint32_t n_watches = __atomic_load_n(&b->watch_count, __ATOMIC_ACQUIRE);
    if (n_subs == 0 && n_watches == 0) return 0;

    __atomic_add_fetch(&g_touch_publish_calls, 1, __ATOMIC_RELAXED);

    if (n_watches != 0) touch_watch_deliver(b, tag_id, kpayload, plen, source_pid);
    if (n_subs == 0) return 0;

    TouchSnap stack[TOUCH_SNAP_STACK];
    TouchSnap *snap = stack;
    uint32_t snap_cap = TOUCH_SNAP_STACK;
    uint32_t snap_n   = 0;

    spin_lock(&b->lock);
    for (TouchSub *s = b->head; s; s = s->bucket_next) {
        if (s->proc->destroying) continue;
        process_state_t st = process_get_state(s->proc);
        if (st == PROC_CRASHED || st == PROC_DONE) continue;

        if (cap == TOUCH_CAP_OWNERS && source_pid != 0) {
            if (!process_has_tag_id(s->proc, tag_id)) {
            }
        }

        if (snap_n >= snap_cap) {
            uint32_t new_cap = snap_cap * 4;
            TouchSnap *heap_buf = (TouchSnap *)kmalloc(sizeof(TouchSnap) * new_cap);
            if (!heap_buf) break;
            memcpy(heap_buf, snap, sizeof(TouchSnap) * snap_n);
            if (snap != stack) kfree(snap);
            snap = heap_buf;
            snap_cap = new_cap;
        }

        process_ref_inc(s->proc);
        __atomic_add_fetch(&s->ref, 1, __ATOMIC_RELAXED);
        snap[snap_n].proc = s->proc;
        snap[snap_n].sub  = s;
        snap[snap_n].mode = s->mode;
        snap_n++;
    }
    spin_unlock(&b->lock);

    bool owners_ok = true;
    if (cap == TOUCH_CAP_OWNERS && source_pid != 0) {
        process_t *sender = process_find_ref(source_pid);
        owners_ok = sender && process_has_tag_id(sender, tag_id);
        if (sender) process_ref_dec(sender);
    }

    uint32_t handed = 0;
    if (owners_ok) {
        for (uint32_t i = 0; i < snap_n; i++) {
            __atomic_add_fetch(&g_touch_subscribers_visited, 1, __ATOMIC_RELAXED);
            if (deliver_one(&snap[i], tag_id, kpayload, plen,
                            source_pid, flags, policy))
                handed++;
        }
    }

    for (uint32_t i = 0; i < snap_n; i++) {
        touch_sub_release(snap[i].sub);
        process_ref_dec(snap[i].proc);
    }
    if (snap != stack) kfree(snap);
    return handed;
}

uint32_t TouchPublishPair(TouchTag full_id, TouchTag bare_id,
                          const void *kpayload, uint32_t plen,
                          uint32_t source_pid, uint16_t flags)
{
    uint32_t handed = 0;
    if (full_id != TOUCH_TAG_INVALID)
        handed += TouchPublishId(full_id, kpayload, plen, source_pid, flags);
    if (bare_id != TOUCH_TAG_INVALID && bare_id != full_id)
        handed += TouchPublishId(bare_id, kpayload, plen, source_pid, flags);
    return handed;
}

void TouchPublish(const char *tag, const void *kpayload, uint32_t plen)
{
    TouchTag full, bare;
    TouchLogbookResolve(tag, &full, &bare);
    TouchPublishPair(full, bare, kpayload, plen, 0, TOUCH_FLAG_KERNEL);
}


#define TOUCH_IRQ_PAYLOAD_MAX  64u

_Static_assert((CONFIG_TOUCH_IRQ_RING_SIZE &
                (CONFIG_TOUCH_IRQ_RING_SIZE - 1)) == 0,
               "CONFIG_TOUCH_IRQ_RING_SIZE must be power-of-2");
_Static_assert(CONFIG_TOUCH_IRQ_RING_SIZE >= 16U,
               "CONFIG_TOUCH_IRQ_RING_SIZE too small for typical IRQ burst");

typedef struct {
    uint32_t gen;
    uint32_t done;
    TouchTag full_id;
    TouchTag bare_id;
    uint16_t flags;
    uint16_t plen;
    uint16_t cut_from;
    uint32_t source_pid;
    uint8_t  payload[TOUCH_IRQ_PAYLOAD_MAX];
} TouchIrqSlot;

static TouchIrqSlot      g_touch_irq_ring[CONFIG_TOUCH_IRQ_RING_SIZE];
static volatile uint32_t g_touch_irq_claimed;
static uint32_t          g_touch_irq_read;
static volatile uint64_t g_touch_irq_delivered;
static volatile uint64_t g_touch_irq_lost;
static volatile uint64_t g_touch_irq_said_ms;

typedef struct TouchIrqTally {
    TouchTag              id;
    uint64_t              count;
    struct TouchIrqTally *next;
} TouchIrqTally;
static TouchIrqTally    *g_touch_irq_tally;
static volatile uint64_t g_touch_irq_untallied;

static void touch_irq_tally(TouchTag full_id, TouchTag bare_id)
{
    TouchTag id = (full_id != TOUCH_TAG_INVALID) ? full_id : bare_id;
    for (TouchIrqTally *t = g_touch_irq_tally; t; t = t->next) {
        if (t->id == id) { t->count++; return; }
    }
    TouchIrqTally *n = (TouchIrqTally *)kmalloc(sizeof(*n));
    if (!n) {
        __atomic_fetch_add(&g_touch_irq_untallied, 1, __ATOMIC_RELAXED);
        return;
    }
    n->id    = id;
    n->count = 1;
    n->next  = g_touch_irq_tally;
    __atomic_store_n(&g_touch_irq_tally, n, __ATOMIC_RELEASE);
}

static void touch_irq_ring_serve(void *ctx);
static Knock g_touch_irq_knock = {
    .baton  = { .next = NULL, .run = touch_irq_ring_serve, .ctx = NULL },
    .raised = 0,
};

static bool touch_irq_may_speak(void)
{
    uint64_t now_ms = clockboard_uptime_ms();
    uint64_t last   = __atomic_load_n(&g_touch_irq_said_ms, __ATOMIC_RELAXED);
    return now_ms - last >= TOUCH_DROP_ANNOUNCE_MS &&
           __atomic_compare_exchange_n(&g_touch_irq_said_ms, &last, now_ms,
                                       false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

static void touch_irq_say_lost(uint32_t lost_now, uint32_t claimed)
{
    uint64_t lost = __atomic_add_fetch(&g_touch_irq_lost, lost_now, __ATOMIC_RELAXED);
    if (!touch_irq_may_speak()) return;
    kprintf("[TOUCH] ERROR: %u interrupt event(s) were overwritten before the K-Core "
            "read them — the interrupts outran the K-Core by a whole ring of %u; "
            "since boot: published %u, delivered %lu, lost %lu, still in the ring %u\n",
            lost_now, (unsigned)CONFIG_TOUCH_IRQ_RING_SIZE, claimed,
            (unsigned long)__atomic_load_n(&g_touch_irq_delivered, __ATOMIC_RELAXED),
            (unsigned long)lost, claimed - g_touch_irq_read);
}

static void touch_irq_ring_serve(void *ctx)
{
    (void)ctx;
    KnockOpen(&g_touch_irq_knock);

    for (;;) {
        uint32_t claimed = __atomic_load_n(&g_touch_irq_claimed, __ATOMIC_ACQUIRE);
        if (g_touch_irq_read == claimed) return;

        if (claimed - g_touch_irq_read > CONFIG_TOUCH_IRQ_RING_SIZE) {
            uint32_t gone = claimed - g_touch_irq_read - CONFIG_TOUCH_IRQ_RING_SIZE;
            g_touch_irq_read += gone;
            touch_irq_say_lost(gone, claimed);
        }

        uint32_t      gen = g_touch_irq_read + 1u;
        TouchIrqSlot *s   = &g_touch_irq_ring[g_touch_irq_read &
                                              (CONFIG_TOUCH_IRQ_RING_SIZE - 1)];

        uint32_t seen = __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE);
        if (seen != gen) {
            if ((int32_t)(seen - gen) > 0) {
                g_touch_irq_read++;
                touch_irq_say_lost(1, claimed);
                continue;
            }
            return;
        }
        if (__atomic_load_n(&s->done, __ATOMIC_ACQUIRE) != gen) return;

        TouchIrqSlot copy;
        memcpy(&copy, s, sizeof(copy));
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&s->gen, __ATOMIC_ACQUIRE) != gen) {
            g_touch_irq_read++;
            touch_irq_say_lost(1, claimed);
            continue;
        }
        g_touch_irq_read++;

        if (copy.cut_from != 0 && touch_irq_may_speak()) {
            kprintf("[TOUCH] ERROR: an interrupt's event (tag %u) carried %u bytes and the "
                    "ring keeps %u — it was cut, and what its subscribers get is not what "
                    "was published\n",
                    (unsigned)(copy.full_id != TOUCH_TAG_INVALID ? copy.full_id : copy.bare_id),
                    (unsigned)copy.cut_from, (unsigned)TOUCH_IRQ_PAYLOAD_MAX);
        }
        TouchPublishPair(copy.full_id, copy.bare_id,
                         copy.plen ? copy.payload : NULL,
                         copy.plen, copy.source_pid, copy.flags);
        __atomic_fetch_add(&g_touch_irq_delivered, 1, __ATOMIC_RELAXED);
        touch_irq_tally(copy.full_id, copy.bare_id);
    }
}

void TouchIrqRingAccount(void)
{
    uint32_t claimed = __atomic_load_n(&g_touch_irq_claimed, __ATOMIC_ACQUIRE);
    uint32_t read    = __atomic_load_n(&g_touch_irq_read,    __ATOMIC_ACQUIRE);
    kprintf("[TOUCH] interrupt ring since boot: published %u, delivered %lu, "
            "lost %lu, still in the ring %u\n",
            claimed,
            (unsigned long)__atomic_load_n(&g_touch_irq_delivered, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_touch_irq_lost,      __ATOMIC_RELAXED),
            claimed - read);
    for (TouchIrqTally *t = __atomic_load_n(&g_touch_irq_tally, __ATOMIC_ACQUIRE);
         t; t = t->next) {
        const char *value = NULL;
        const char *key   = TouchLogbookName(t->id, &value);
        if (key && value) kprintf("[TOUCH]   delivered by name: %s:%s %lu\n", key, value,
                                  (unsigned long)t->count);
        else if (key)     kprintf("[TOUCH]   delivered by name: %s %lu\n", key,
                                  (unsigned long)t->count);
        else              kprintf("[TOUCH]   delivered by name: tag %u %lu\n",
                                  (unsigned)t->id, (unsigned long)t->count);
    }
    uint64_t untallied = __atomic_load_n(&g_touch_irq_untallied, __ATOMIC_RELAXED);
    if (untallied) {
        kprintf("[TOUCH]   %lu delivered with no memory to count them by name\n",
                (unsigned long)untallied);
    }
}

void TouchPublishIrqPair(TouchTag full_id, TouchTag bare_id,
                         const void *payload, uint16_t plen,
                         uint32_t source_pid, uint16_t flags)
{
    if (full_id == TOUCH_TAG_INVALID && bare_id == TOUCH_TAG_INVALID) return;

    uint16_t cut_from = 0;
    if (plen > TOUCH_IRQ_PAYLOAD_MAX) {
        cut_from = plen;
        plen     = TOUCH_IRQ_PAYLOAD_MAX;
    }

    uint32_t      raw = atomic_fetch_add_u32(&g_touch_irq_claimed, 1);
    uint32_t      gen = raw + 1u;
    TouchIrqSlot *s   = &g_touch_irq_ring[raw & (CONFIG_TOUCH_IRQ_RING_SIZE - 1)];

    __atomic_store_n(&s->gen, gen, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    s->full_id    = full_id;
    s->bare_id    = bare_id;
    s->flags      = flags;
    s->plen       = plen;
    s->cut_from   = cut_from;
    s->source_pid = source_pid;
    if (plen > 0 && payload) memcpy(s->payload, payload, plen);
    __atomic_store_n(&s->done, gen, __ATOMIC_RELEASE);

    KnockOn(&g_touch_irq_knock);
}


static TouchSub *find_proc_sub(process_t *proc, TouchTag tag_id)
{
    for (TouchSub *s = (TouchSub *)proc->cabin->subs_head; s; s = s->proc_next) {
        if (s->tag_id == tag_id) return s;
    }
    return NULL;
}

error_t TouchClaimSet(process_t *proc, TouchTag tag_id, TouchMode mode,
                      ManifestHandle manifest, uint64_t handler_addr,
                      uint64_t stack_top)
{
    if (!proc) return ERR_NULL_POINTER;
    if (tag_id == TOUCH_TAG_INVALID) return ERR_INVALID_ARGUMENT;

    TouchBucket *b = touch_bucket_get_or_create(tag_id);
    if (!b) return ERR_NO_MEMORY;

    spin_lock(&proc->cabin->subs_lock);
    TouchSub *existing = find_proc_sub(proc, tag_id);
    if (existing) {
        if (existing->mode == TOUCH_REACT &&
            existing->u.manifest != MANIFEST_HANDLE_INVALID)
            ManifestRelease(existing->u.manifest);

        existing->mode = (uint8_t)mode;
        if (mode == TOUCH_REACT) {
            ManifestRetain(manifest);
            existing->u.manifest = manifest;
        } else if (mode == TOUCH_INTERRUPT) {
            existing->u.irq.handler_addr = handler_addr;
            existing->u.irq.stack_top    = stack_top;
        } else {
            existing->u._raw = 0;
        }
        spin_unlock(&proc->cabin->subs_lock);
        return OK;
    }
    spin_unlock(&proc->cabin->subs_lock);

    TouchSub *sub = (TouchSub *)kmalloc(sizeof(TouchSub));
    if (!sub) return ERR_NO_MEMORY;
    memset(sub, 0, sizeof(*sub));
    sub->proc   = proc;
    sub->bucket = b;
    sub->tag_id = tag_id;
    sub->mode   = (uint8_t)mode;
    if (mode == TOUCH_REACT) {
        ManifestRetain(manifest);
        sub->u.manifest = manifest;
    } else if (mode == TOUCH_INTERRUPT) {
        sub->u.irq.handler_addr = handler_addr;
        sub->u.irq.stack_top    = stack_top;
    }
    __atomic_store_n(&sub->ref, 1, __ATOMIC_RELAXED);

    spin_lock(&b->lock);
    sub->bucket_next = b->head;
    sub->bucket_prev = NULL;
    if (b->head) b->head->bucket_prev = sub;
    b->head = sub;
    __atomic_add_fetch(&b->sub_count, 1, __ATOMIC_RELEASE);
    spin_unlock(&b->lock);

    spin_lock(&proc->cabin->subs_lock);
    if (__atomic_load_n(&proc->touch_cleaned, __ATOMIC_ACQUIRE) ||
        find_proc_sub(proc, tag_id)) {
        spin_unlock(&proc->cabin->subs_lock);

        spin_lock(&b->lock);
        if (sub->bucket_prev) sub->bucket_prev->bucket_next = sub->bucket_next;
        else                   b->head = sub->bucket_next;
        if (sub->bucket_next) sub->bucket_next->bucket_prev = sub->bucket_prev;
        __atomic_sub_fetch(&b->sub_count, 1, __ATOMIC_RELEASE);
        spin_unlock(&b->lock);

        if (mode == TOUCH_REACT && sub->u.manifest != MANIFEST_HANDLE_INVALID) {
            ManifestRelease(sub->u.manifest);
            sub->u.manifest = MANIFEST_HANDLE_INVALID;
        }
        touch_sub_release(sub);
        return OK;
    }
    sub->proc_next = (TouchSub *)proc->cabin->subs_head;
    sub->proc_prev = NULL;
    if (proc->cabin->subs_head)
        ((TouchSub *)proc->cabin->subs_head)->proc_prev = sub;
    proc->cabin->subs_head = sub;
    spin_unlock(&proc->cabin->subs_lock);

    __atomic_add_fetch(&g_total_subs, 1, __ATOMIC_RELEASE);
    return OK;
}

static void touch_sub_unlink_bucket(TouchSub *sub)
{
    TouchBucket *b = sub->bucket;
    spin_lock(&b->lock);
    if (sub->bucket_prev) sub->bucket_prev->bucket_next = sub->bucket_next;
    else if (b->head == sub) b->head = sub->bucket_next;
    if (sub->bucket_next) sub->bucket_next->bucket_prev = sub->bucket_prev;
    sub->bucket_next = sub->bucket_prev = NULL;
    uint32_t cur = __atomic_load_n(&b->sub_count, __ATOMIC_RELAXED);
    if (cur > 0)
        __atomic_sub_fetch(&b->sub_count, 1, __ATOMIC_RELEASE);
    spin_unlock(&b->lock);
}

error_t TouchClaimClear(process_t *proc, TouchTag tag_id)
{
    if (!proc) return ERR_NULL_POINTER;

    spin_lock(&proc->cabin->subs_lock);
    TouchSub *sub = find_proc_sub(proc, tag_id);
    if (!sub) {
        spin_unlock(&proc->cabin->subs_lock);
        return ERR_TAG_NOT_FOUND;
    }
    if (sub->proc_prev) sub->proc_prev->proc_next = sub->proc_next;
    else                proc->cabin->subs_head           = sub->proc_next;
    if (sub->proc_next) sub->proc_next->proc_prev = sub->proc_prev;
    sub->proc_next = sub->proc_prev = NULL;
    spin_unlock(&proc->cabin->subs_lock);

    touch_sub_unlink_bucket(sub);

    if (sub->mode == TOUCH_REACT && sub->u.manifest != MANIFEST_HANDLE_INVALID) {
        ManifestRelease(sub->u.manifest);
        sub->u.manifest = MANIFEST_HANDLE_INVALID;
    }
    touch_sub_release(sub);
    __atomic_sub_fetch(&g_total_subs, 1, __ATOMIC_RELEASE);
    return OK;
}

error_t TouchClaimAck(process_t *proc, TouchTag tag_id)
{
    if (!proc) return ERR_NULL_POINTER;
    spin_lock(&proc->cabin->subs_lock);
    TouchSub *sub = find_proc_sub(proc, tag_id);
    if (!sub) { spin_unlock(&proc->cabin->subs_lock); return ERR_TAG_NOT_FOUND; }
    __atomic_store_n(&sub->has_pending, 0, __ATOMIC_RELEASE);
    sub->pending_plen = 0;
    TouchBucket *bcb = sub->bucket;
    spin_unlock(&proc->cabin->subs_lock);

    if (bcb) {
        spin_lock(&bcb->lock);
        if (bcb->latched_payload) {
            kfree(bcb->latched_payload);
            bcb->latched_payload = NULL;
        }
        bcb->latched_plen = 0;
        spin_unlock(&bcb->lock);
    }
    return OK;
}

void TouchCleanupProcess(process_t *proc, int32_t exit_code)
{
    if (!proc) return;
    if (__atomic_exchange_n(&proc->touch_cleaned, 1, __ATOMIC_ACQ_REL)) return;

    TouchSub *mine = NULL;
    spin_lock(&proc->cabin->subs_lock);
    TouchSub *cur = (TouchSub *)proc->cabin->subs_head;
    while (cur) {
        TouchSub *pnext = cur->proc_next;
        if (cur->proc == proc) {
            if (cur->proc_prev) cur->proc_prev->proc_next = cur->proc_next;
            else                proc->cabin->subs_head     = cur->proc_next;
            if (cur->proc_next) cur->proc_next->proc_prev = cur->proc_prev;
            cur->proc_prev = NULL;
            cur->proc_next = mine;
            mine = cur;
        }
        cur = pnext;
    }
    spin_unlock(&proc->cabin->subs_lock);

    uint32_t unlinked = 0;
    TouchSub *s = mine;
    while (s) {
        TouchSub *nx = s->proc_next;
        if (s->bucket) { touch_sub_unlink_bucket(s); unlinked++; }
        if (s->mode == TOUCH_REACT &&
            s->u.manifest != MANIFEST_HANDLE_INVALID) {
            ManifestRelease(s->u.manifest);
            s->u.manifest = MANIFEST_HANDLE_INVALID;
        }
        touch_sub_release(s);
        s = nx;
    }
    if (unlinked > 0)
        __atomic_sub_fetch(&g_total_subs, unlinked, __ATOMIC_RELEASE);

    TouchOwedRelease(proc);

    spin_lock(&proc->irq_lock);
    TouchPending *p = (TouchPending *)proc->irq_pending_head;
    proc->irq_pending_head = NULL;
    spin_unlock(&proc->irq_lock);
    while (p) {
        TouchPending *next = p->next;
        kfree(p);
        p = next;
    }

    struct __attribute__((packed)) {
        uint32_t pid;
        int32_t  exit;
        uint32_t gen;
    } died = { proc->pid, exit_code, proc->generation };
    _Static_assert(sizeof(died) == 12,
                   "process:died wire payload must stay 12 bytes "
                   "(box/touch.h TouchProcessDied: pid@0, exit_code@4, generation@8)");
    TouchPublish("process:died", &died, sizeof(died));

    ProcessGoneDeliver(proc, exit_code);
}

void TouchIrqReturn(process_t *proc)
{
    if (!proc) return;

    proc->context.rip    = proc->irq_saved_rip;
    proc->context.rsp    = proc->irq_saved_rsp;
    proc->context.rflags = proc->irq_saved_rflags;

    spin_lock(&proc->irq_lock);
    proc->irq_active = 0;
    TouchPending *pending = (TouchPending *)proc->irq_pending_head;
    if (pending) {
        proc->irq_pending_head = pending->next;
    }
    spin_unlock(&proc->irq_lock);

    if (pending) {
        spin_lock(&proc->cabin->subs_lock);
        TouchSub *sub = find_proc_sub(proc, pending->tag_id);
        bool reuse = sub && sub->mode == TOUCH_INTERRUPT;
        spin_unlock(&proc->cabin->subs_lock);
        if (reuse) {
            touch_interrupt_deliver(proc, sub, pending->tag_id,
                                    pending->plen > 0 ? pending->payload : NULL,
                                    pending->plen,
                                    pending->source_pid,
                                    pending->flags);
        }
        kfree(pending);
    }
}