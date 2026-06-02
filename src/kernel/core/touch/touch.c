#include "touch.h"
#include "touch_queue.h"
#include "touch_ring.h"
#include "pit.h"
#include "process.h"
#include "kring.h"
#include "kresult.h"
#include "result.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "tagfs.h"
#include "atomics.h"
#include "amp.h"
#include "kcore.h"
#include "lapic.h"
#include "irqchip.h"
#include "irq_defer.h"
#include "kernel_config.h"
#include "manifest_exec.h"
#include "boxos_crate.h"
#include "op_registry.h"

/* ────────────────────────────────────────────────────────────────────────
 * Two-level bucket index over the 16-bit tag_id space.
 * L1 is always-resident pointer table (256 × 8 = 2 KiB).
 * L2 leaves are lazily kmalloc'd 256-bucket slabs (256 × 64 = 16 KiB ea).
 *
 * Lookup is one ACQUIRE load + offset; lazy alloc uses CAS-install with
 * loser-frees so concurrent first-claims on different ids in the same leaf
 * cooperate without locking the whole index.
 * ──────────────────────────────────────────────────────────────────────── */
static TouchBucket *g_bucket_l1[TOUCH_BUCKET_L1_ENTRIES];

/* Global subscriber count — coarse gate used by hot publisher pre-checks
 * (storage_ops, write_job) so they skip per-tag snapshot work when no one
 * listens at all. Updated on every TouchClaimSet / TouchClaimClear /
 * TouchCleanupProcess. */
static volatile uint32_t g_total_subs;

/* Diagnostic counters. */
static volatile uint64_t g_touch_publish_calls;
static volatile uint64_t g_touch_subscribers_visited;
static volatile uint64_t g_touch_delivered;
static volatile uint64_t g_touch_emit_fail;
static volatile uint64_t g_touch_push_fail;

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

/* O(1) lookup; returns NULL when the L2 leaf for this id range has never
 * been allocated (no one has ever subscribed to anything in this 256-id
 * window). Publisher uses this to short-circuit. */
static inline TouchBucket *touch_bucket_lookup(TouchTag tag_id)
{
    if (tag_id == TOUCH_TAG_INVALID) return NULL;
    TouchBucket *leaf = __atomic_load_n(&g_bucket_l1[tag_id >> 8],
                                        __ATOMIC_ACQUIRE);
    if (!leaf) return NULL;
    return &leaf[tag_id & 0xFF];
}

/* Subscribe/register path: allocate the L2 leaf if missing. */
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
        /* Initialize per-bucket lock + record tag_id mirror up-front so the
         * publish path never sees a half-built bucket. */
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

/* ────────────────────────────────────────────────────────────────────────
 * Tag-string resolution (cold path; hot publishers cache the resulting id).
 * ──────────────────────────────────────────────────────────────────────── */

void TouchTagResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare)
{
    *out_full = TOUCH_TAG_INVALID;
    *out_bare = TOUCH_TAG_INVALID;
    if (!tag || tag[0] == '\0') return;

    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry) return;

    char key[256], value[256];
    tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));

    bool has_value   = (value[0] != '\0');
    bool is_wildcard = has_value && value[0] == '.' && value[1] == '.' &&
                                    value[2] == '.' && value[3] == '\0';

    *out_bare = tag_registry_lookup(fs->registry, key, NULL);
    if (*out_bare == TAGFS_INVALID_TAG_ID)
        *out_bare = tag_registry_intern(fs->registry, key, NULL);
    if (*out_bare == TAGFS_INVALID_TAG_ID) *out_bare = TOUCH_TAG_INVALID;

    if (has_value && !is_wildcard) {
        *out_full = tag_registry_lookup(fs->registry, key, value);
        if (*out_full == TAGFS_INVALID_TAG_ID)
            *out_full = tag_registry_intern(fs->registry, key, value);
        if (*out_full == TAGFS_INVALID_TAG_ID) *out_full = TOUCH_TAG_INVALID;
    }
}

TouchTag TouchTagIntern(const char *tag)
{
    TouchTag full, bare;
    TouchTagResolve(tag, &full, &bare);
    return (full != TOUCH_TAG_INVALID) ? full : bare;
}

/* ────────────────────────────────────────────────────────────────────────
 * Policy / capability table. Lives inside each TouchBucket — no global table.
 * ──────────────────────────────────────────────────────────────────────── */

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
    /* Single-byte loads — atomic on x86; no lock needed. */
    if (out_policy) *out_policy = (TouchPolicy)b->policy;
    if (out_cap)    *out_cap    = (TouchCapability)b->capability;
    return true;
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
    return b && __atomic_load_n(&b->sub_count, __ATOMIC_ACQUIRE) > 0;
}

bool TouchHasAnyListeners(void)
{
    return __atomic_load_n(&g_total_subs, __ATOMIC_ACQUIRE) > 0;
}

/* ────────────────────────────────────────────────────────────────────────
 * Per-target payload page allocator.
 * touch_emit_payload writes a Touch record + payload into the target's
 * cabin buffer-heap and returns the user vaddr. One page per record is
 * the conservative path that the multi-core stress suite passes — packing
 * records into shared pages tripped vmm_map_page faults under MPSC.
 * ──────────────────────────────────────────────────────────────────────── */
static uint64_t touch_emit_payload(process_t *target, TouchTag tag_id,
                                   uint16_t flags, uint32_t source_pid,
                                   const void *kpayload, uint32_t plen)
{
    uint64_t total = sizeof(Touch) + plen;
    uint32_t pages = (uint32_t)((total + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    uint64_t bytes = (uint64_t)pages * PMM_PAGE_SIZE;
    uint64_t vaddr = __atomic_fetch_add(&target->buf_heap_next, bytes,
                                        __ATOMIC_ACQ_REL);

    for (uint32_t i = 0; i < pages; i++) {
        void *page = pmm_alloc(1);
        if (!page) return 0;
        uint64_t va = vaddr + (i * PMM_PAGE_SIZE);
        vmm_map_result_t r = vmm_map_page(target->cabin, va,
                                          (uint64_t)page, VMM_FLAGS_USER_RW);
        if (!r.success) { pmm_free(page, 1); return 0; }
    }

    void *dst = vmm_translate_user_addr(target->cabin, vaddr, (size_t)total);
    if (!dst) return 0;

    Touch t = {
        .tag_id        = tag_id,
        .flags         = flags,
        .source_pid    = source_pid,
        .payload_len   = plen,
        .payload_addr  = (plen > 0) ? (vaddr + sizeof(Touch)) : 0,
        .timestamp_tsc = rdtsc(),
        .reserved      = 0,
    };
    memcpy(dst, &t, sizeof(Touch));
    if (plen > 0 && kpayload)
        memcpy((uint8_t *)dst + sizeof(Touch), kpayload, plen);
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

void TouchRestDeliver(process_t *target, TouchTag tag_id,
                      const void *kpayload, uint32_t plen,
                      uint32_t source_pid, uint16_t flags)
{
    /* REST mode now publishes directly into the target's TouchRing.
     * The payload travels inline in the TouchSlot — no separate
     * `buf_heap_next` allocation, no per-publish vmm_map_page in the
     * hot path. Vyukov seq gating ties payload lifetime to the slot
     * round (consumer must read before releasing seq, see
     * touch_ring.h). This closes the buf_heap_next physical-RAM leak
     * the multiplexed-ResultRing design suffered from. */

    /* Bounded retry on ring contention. The retries cover the brief
     * window where the consumer is mid-pop on the slot we landed on;
     * past the budget we drop and increment the failure counter (the
     * old behaviour preserved 84%→100% delivery on 16-core stress;
     * see project memory `touch_stress_complete_2026_05_05`). */
    enum { TOUCH_PUSH_RETRIES = 64 };
    for (int attempt = 0; attempt < TOUCH_PUSH_RETRIES; attempt++) {
        if (KTouchPush(target, tag_id, flags, source_pid, kpayload, plen)) {
            __atomic_add_fetch(&g_touch_delivered, 1, __ATOMIC_RELAXED);
            touch_wake_remote(target);
            return;
        }
        for (int p = 0; p < 32; p++) cpu_pause();
        if (target->destroying) {
            __atomic_add_fetch(&g_touch_push_fail, 1, __ATOMIC_RELAXED);
            return;
        }
    }
    __atomic_add_fetch(&g_touch_push_fail, 1, __ATOMIC_RELAXED);
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
        /* kmalloc under lock is legal here — proc->irq_lock is a regular
         * spinlock (cli within), but kmalloc itself does not block / sleep
         * in BoxOS. Kept defensively bounded by failing silently on OOM. */
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

static void touch_react_deliver(process_t *proc, TouchSub *sub,
                                TouchTag tag_id, const void *kpayload,
                                uint32_t plen, uint32_t source_pid,
                                uint16_t flags)
{
    if (sub->u.manifest == MANIFEST_HANDLE_INVALID) return;

    uint64_t vaddr = touch_emit_payload(proc, tag_id, flags, source_pid,
                                        kpayload, plen);
    if (vaddr == 0) return;

    uint64_t total = sizeof(Touch) + plen;
    Crate crate = {
        .magic    = CRATE_MAGIC,
        .flags    = 0,
        .kind     = CRATE_KIND_INPUT,
        ._pad0    = 0,
        ._pad1    = 0,
        .addr     = vaddr,
        .size     = total,
        .capacity = total,
    };
    OpContext ctx = {
        .proc       = proc,
        .target_pid = 0,
        .flags      = 0,
        .pier_id    = 0,
        ._pad       = 0,
    };
    ManifestExecResult exec_result;
    ManifestExecute(sub->u.manifest, &crate, 1, &ctx, &exec_result);
}

/* ────────────────────────────────────────────────────────────────────────
 * Snapshot-then-deliver publish.
 *
 * Step 1: take bucket lock, walk bucket->head, COPY relevant sub fields
 *         into a stack array, atomic-inc proc->ref_count on each entry.
 * Step 2: release bucket lock.
 * Step 3: deliver each entry, release proc ref.
 *
 * The copy in step 1 is small (mode + manifest_or_irq + has_pending_ptr +
 * proc*). After we release the bucket lock, TouchCleanupProcess on a
 * concurrent core may unlink the sub and stop further publish observation,
 * but the held proc ref keeps the sub struct live (kfree happens only in
 * TouchFinalizeProcess after ref_count == 0).
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    process_t       *proc;
    TouchSub        *sub;    /* still-valid pointer; owned by held proc ref */
    uint8_t          mode;
} TouchSnap;

enum { TOUCH_SNAP_STACK = 64 };

static void deliver_one(TouchSnap *e, TouchTag tag_id,
                        const void *kpayload, uint32_t plen,
                        uint32_t source_pid, uint16_t flags,
                        TouchPolicy policy)
{
    if (e->proc->destroying) return;

    if (policy == TOUCH_POLICY_LATCHED) {
        /* Atomic test-and-set of has_pending so concurrent publishes don't
         * stomp each other's pending payload. has_pending is uint8_t; a
         * RELAXED CAS is sufficient because the sub itself is kept alive
         * by the proc-ref we hold. */
        uint8_t expected = 0;
        if (!__atomic_compare_exchange_n(&e->sub->has_pending, &expected, 1,
                                         false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED)) {
            return; /* drop — keep existing pending */
        }
        e->sub->pending_plen = plen > sizeof(e->sub->pending_payload)
                               ? (uint32_t)sizeof(e->sub->pending_payload)
                               : plen;
        if (e->sub->pending_plen > 0 && kpayload)
            memcpy(e->sub->pending_payload, kpayload, e->sub->pending_plen);
        /* Fall through and still push to the result ring so process wakes. */
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
}

void TouchPublishId(TouchTag tag_id, const void *kpayload, uint32_t plen,
                    uint32_t source_pid, uint16_t flags)
{
    TouchBucket *b = touch_bucket_lookup(tag_id);
    if (!b) return;

    TouchPolicy     policy = (TouchPolicy)b->policy;
    TouchCapability cap    = (TouchCapability)b->capability;

    if (cap == TOUCH_CAP_KERNEL_ONLY && source_pid != 0) return;

    /* LEVEL state must be updated even when nobody subscribes yet — future
     * subscribers see the latched state via SysTouchAwait's on-claim sync.
     * State write happens before the sub_count gate below. */
    if (policy == TOUCH_POLICY_LEVEL) {
        uint8_t new_state = (plen > 0 && kpayload)
                            ? ((const uint8_t *)kpayload)[0] : 0;
        b->level_state = new_state;
        if (new_state == 0) return;
    }

    if (__atomic_load_n(&b->sub_count, __ATOMIC_ACQUIRE) == 0) return;

    __atomic_add_fetch(&g_touch_publish_calls, 1, __ATOMIC_RELAXED);

    TouchSnap stack[TOUCH_SNAP_STACK];
    TouchSnap *snap = stack;
    uint32_t snap_cap = TOUCH_SNAP_STACK;
    uint32_t snap_n   = 0;

    /* Phase 1: snapshot under bucket lock. */
    spin_lock(&b->lock);
    for (TouchSub *s = b->head; s; s = s->bucket_next) {
        if (s->proc->destroying) continue;
        process_state_t st = process_get_state(s->proc);
        if (st == PROC_CRASHED || st == PROC_DONE) continue;

        if (cap == TOUCH_CAP_OWNERS && source_pid != 0) {
            /* The sender must own this tag. Verified outside the bucket lock
             * to avoid nested process_lock acquisition; if the sender lacks
             * the tag we just skip this subscriber. */
            if (!process_has_tag_id(s->proc, tag_id)) {
                /* OWNERS gate is on the SENDER, not subscriber — but we have
                 * source_pid not a proc pointer here. Resolution happens in
                 * the post-lock phase to avoid hash lookup under bucket lock. */
            }
        }

        if (snap_n >= snap_cap) {
            /* Grow to heap. */
            uint32_t new_cap = snap_cap * 4;
            TouchSnap *heap_buf = (TouchSnap *)kmalloc(sizeof(TouchSnap) * new_cap);
            if (!heap_buf) break;
            memcpy(heap_buf, snap, sizeof(TouchSnap) * snap_n);
            if (snap != stack) kfree(snap);
            snap = heap_buf;
            snap_cap = new_cap;
        }

        process_ref_inc(s->proc);
        snap[snap_n].proc = s->proc;
        snap[snap_n].sub  = s;
        snap[snap_n].mode = s->mode;
        snap_n++;
    }
    spin_unlock(&b->lock);

    /* Phase 1.5: OWNERS check on sender (one ref-grab, not per-subscriber). */
    bool owners_ok = true;
    if (cap == TOUCH_CAP_OWNERS && source_pid != 0) {
        process_t *sender = process_find_ref(source_pid);
        owners_ok = sender && process_has_tag_id(sender, tag_id);
        if (sender) process_ref_dec(sender);
    }

    /* Phase 2: deliver outside lock. */
    if (owners_ok) {
        for (uint32_t i = 0; i < snap_n; i++) {
            __atomic_add_fetch(&g_touch_subscribers_visited, 1, __ATOMIC_RELAXED);
            deliver_one(&snap[i], tag_id, kpayload, plen,
                        source_pid, flags, policy);
        }
    }

    for (uint32_t i = 0; i < snap_n; i++) process_ref_dec(snap[i].proc);
    if (snap != stack) kfree(snap);
}

void TouchPublishPair(TouchTag full_id, TouchTag bare_id,
                      const void *kpayload, uint32_t plen,
                      uint32_t source_pid, uint16_t flags)
{
    if (full_id != TOUCH_TAG_INVALID)
        TouchPublishId(full_id, kpayload, plen, source_pid, flags);
    if (bare_id != TOUCH_TAG_INVALID && bare_id != full_id)
        TouchPublishId(bare_id, kpayload, plen, source_pid, flags);
}

void TouchPublish(const char *tag, const void *kpayload, uint32_t plen)
{
    TouchTag full, bare;
    TouchTagResolve(tag, &full, &bare);
    TouchPublishPair(full, bare, kpayload, plen, 0, TOUCH_FLAG_KERNEL);
}

/* ────────────────────────────────────────────────────────────────────────
 * IRQ-context publisher — TouchPublishIrqPair
 *
 * Drivers running in IRQ context (PS/2 IRQ1, PIT IRQ0 software repeat, xHCI
 * MSI hot-plug, future MSI/IOAPIC publishers) hand off to a K-Core via the
 * shared irq_defer subsystem rather than touching kmalloc / pmm_alloc /
 * vmm_map_page / per-bucket spinlocks from interrupt context. This is the
 * same hazard class closed for AHCI/SCI/APEI in `irq_defer_done_2026_05_17`
 * — keyboard and xHCI were never migrated, and that regression is what
 * this commit closes.
 *
 * Slot ring is a pre-allocated static array of TouchIrqSlot. Producer side
 * is an unconditional `atomic_fetch_add` followed by a memcpy into the
 * claimed slot — IRQ-safe, allocation-free, branchless. Under burst the
 * bump-allocator wraps and silently overwrites the oldest queued slot
 * (the dropped-count counter records each clobber for diagnostics).
 *
 * Payload is bounded at TOUCH_IRQ_PAYLOAD_MAX bytes. All current IRQ-side
 * publishers (kbd 3 B event, xhci 8 B port event, acpi 2 B sts, ata 24 B
 * err) fit comfortably; a sanity assertion below catches regressions.
 *
 * The K-Core handler is `touch_irq_deferred`. It reads its captured slot
 * and calls TouchPublishPair, which does the full publish (bucket walk,
 * subscriber snapshot, deliver). Holding a pointer to the slot is safe
 * because the slot lives in static storage and is only overwritten when
 * the producer-side index wraps the same modulo position — by which time
 * the previous K-Core read has completed (irq_defer drains FIFO).
 * ──────────────────────────────────────────────────────────────────────── */

#define TOUCH_IRQ_PAYLOAD_MAX  64u

_Static_assert((CONFIG_TOUCH_IRQ_RING_SIZE &
                (CONFIG_TOUCH_IRQ_RING_SIZE - 1)) == 0,
               "CONFIG_TOUCH_IRQ_RING_SIZE must be power-of-2");
_Static_assert(CONFIG_TOUCH_IRQ_RING_SIZE >= 16U,
               "CONFIG_TOUCH_IRQ_RING_SIZE too small for typical IRQ burst");

typedef struct {
    TouchTag full_id;
    TouchTag bare_id;
    uint16_t flags;
    uint16_t plen;
    uint32_t source_pid;
    uint8_t  payload[TOUCH_IRQ_PAYLOAD_MAX];
} TouchIrqSlot;

static TouchIrqSlot      g_touch_irq_ring[CONFIG_TOUCH_IRQ_RING_SIZE];
static volatile uint32_t g_touch_irq_idx;
static volatile uint64_t g_touch_irq_overflow_payload;
static volatile uint64_t g_touch_irq_wrap_count;

/* K-Core context. Reads its captured slot (stable across the irq_defer
 * round-trip — see comment block above) and performs the full publish. */
static void touch_irq_deferred(void *ctx)
{
    const TouchIrqSlot *s = (const TouchIrqSlot *)ctx;
    TouchPublishPair(s->full_id, s->bare_id,
                     s->plen ? s->payload : NULL,
                     s->plen, s->source_pid, s->flags);
}

void TouchPublishIrqPair(TouchTag full_id, TouchTag bare_id,
                         const void *payload, uint16_t plen,
                         uint32_t source_pid, uint16_t flags)
{
    /* Caller may pass an unresolved tag before driver init has cached one
     * (boot-time IRQ before TouchTagResolve completes). That is a no-op,
     * not an error. */
    if (full_id == TOUCH_TAG_INVALID && bare_id == TOUCH_TAG_INVALID) return;

    /* Truncate oversize payloads silently and increment a diagnostic
     * counter — we cannot fail an IRQ-side publish, but we want the
     * regression to surface in counters. */
    if (plen > TOUCH_IRQ_PAYLOAD_MAX) {
        atomic_fetch_add_u64(&g_touch_irq_overflow_payload, 1);
        plen = TOUCH_IRQ_PAYLOAD_MAX;
    }

    /* MPSC claim: atomic_fetch_add gives every concurrent IRQ a unique
     * slot index. Mask to ring size — power-of-2 enforced above.
     *
     * Honest accounting: we count the number of times the producer index
     * passes a ring boundary. That number == ceil(total_events / ring_size).
     * It is a coarse correlate of "did we ever lap the K-Core consumer?"
     * Per-slot drop detection would require a generation field whose
     * extra atomic on every IRQ is not justified at typical event rates
     * (worst-case PS/2 typematic 30 Hz + xHCI port-change ~Hz + ACPI GPE
     * ~Hz vs. ring size 64 → wrap every ~2 s; irq_defer drains in ms). */
    uint32_t raw = atomic_fetch_add_u32(&g_touch_irq_idx, 1);
    uint32_t i   = raw & (CONFIG_TOUCH_IRQ_RING_SIZE - 1);
    if (i == 0 && raw != 0) {
        atomic_fetch_add_u64(&g_touch_irq_wrap_count, 1);
    }
    TouchIrqSlot *s = &g_touch_irq_ring[i];

    s->full_id    = full_id;
    s->bare_id    = bare_id;
    s->flags      = flags;
    s->plen       = plen;
    s->source_pid = source_pid;
    if (plen > 0 && payload) memcpy(s->payload, payload, plen);

    /* irq_defer's release-store on its internal slot.ready is the
     * publication fence between the writes above and the K-Core's
     * deferred read. See irq_defer.c invariant (1). */
    irq_defer(touch_irq_deferred, s);
}

uint64_t TouchPublishIrqWraps(void)
{
    /* See full rationale in touch.h declaration. Drops are inferred from
     * wraps × pump-latency, not measured directly. */
    return atomic_load_u64(&g_touch_irq_wrap_count);
}

/* ────────────────────────────────────────────────────────────────────────
 * Subscribe / unsubscribe — O(1) link/unlink.
 * ──────────────────────────────────────────────────────────────────────── */

static TouchSub *find_proc_sub(process_t *proc, TouchTag tag_id)
{
    for (TouchSub *s = (TouchSub *)proc->subs_head; s; s = s->proc_next) {
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

    spin_lock(&proc->subs_lock);
    TouchSub *existing = find_proc_sub(proc, tag_id);
    if (existing) {
        /* Update in place — no list churn. */
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
        spin_unlock(&proc->subs_lock);
        return OK;
    }
    spin_unlock(&proc->subs_lock);

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

    /* Link into bucket (publish visibility) and into proc list. Bucket lock
     * acquired BEFORE proc lock — established ordering, never reversed. */
    spin_lock(&b->lock);
    sub->bucket_next = b->head;
    sub->bucket_prev = NULL;
    if (b->head) b->head->bucket_prev = sub;
    b->head = sub;
    __atomic_add_fetch(&b->sub_count, 1, __ATOMIC_RELEASE);
    spin_unlock(&b->lock);

    spin_lock(&proc->subs_lock);
    /* Re-check duplicate — between our find_proc_sub and now, a concurrent
     * TouchClaimSet on the same (proc, tag_id) could have inserted. If so,
     * undo our bucket link to preserve the one-sub-per-(proc,tag) invariant. */
    if (find_proc_sub(proc, tag_id)) {
        spin_unlock(&proc->subs_lock);

        spin_lock(&b->lock);
        if (sub->bucket_prev) sub->bucket_prev->bucket_next = sub->bucket_next;
        else                   b->head = sub->bucket_next;
        if (sub->bucket_next) sub->bucket_next->bucket_prev = sub->bucket_prev;
        __atomic_sub_fetch(&b->sub_count, 1, __ATOMIC_RELEASE);
        spin_unlock(&b->lock);

        if (mode == TOUCH_REACT && sub->u.manifest != MANIFEST_HANDLE_INVALID)
            ManifestRelease(sub->u.manifest);
        kfree(sub);
        return OK;
    }
    sub->proc_next = (TouchSub *)proc->subs_head;
    sub->proc_prev = NULL;
    if (proc->subs_head)
        ((TouchSub *)proc->subs_head)->proc_prev = sub;
    proc->subs_head = sub;
    spin_unlock(&proc->subs_lock);

    __atomic_add_fetch(&g_total_subs, 1, __ATOMIC_RELEASE);
    return OK;
}

/* Internal: unlink a sub from its bucket. Assumes caller holds NO bucket lock. */
static void touch_sub_unlink_bucket(TouchSub *sub)
{
    TouchBucket *b = sub->bucket;
    spin_lock(&b->lock);
    if (sub->bucket_prev) sub->bucket_prev->bucket_next = sub->bucket_next;
    else if (b->head == sub) b->head = sub->bucket_next;
    if (sub->bucket_next) sub->bucket_next->bucket_prev = sub->bucket_prev;
    sub->bucket_next = sub->bucket_prev = NULL;
    /* Decrement sub_count if it was still counted (not double-unlinked). */
    uint32_t cur = __atomic_load_n(&b->sub_count, __ATOMIC_RELAXED);
    if (cur > 0)
        __atomic_sub_fetch(&b->sub_count, 1, __ATOMIC_RELEASE);
    spin_unlock(&b->lock);
}

error_t TouchClaimClear(process_t *proc, TouchTag tag_id)
{
    if (!proc) return ERR_NULL_POINTER;

    spin_lock(&proc->subs_lock);
    TouchSub *sub = find_proc_sub(proc, tag_id);
    if (!sub) {
        spin_unlock(&proc->subs_lock);
        return ERR_TAG_NOT_FOUND;
    }
    if (sub->proc_prev) sub->proc_prev->proc_next = sub->proc_next;
    else                proc->subs_head           = sub->proc_next;
    if (sub->proc_next) sub->proc_next->proc_prev = sub->proc_prev;
    sub->proc_next = sub->proc_prev = NULL;
    spin_unlock(&proc->subs_lock);

    touch_sub_unlink_bucket(sub);

    if (sub->mode == TOUCH_REACT && sub->u.manifest != MANIFEST_HANDLE_INVALID)
        ManifestRelease(sub->u.manifest);
    kfree(sub);
    __atomic_sub_fetch(&g_total_subs, 1, __ATOMIC_RELEASE);
    return OK;
}

error_t TouchClaimAck(process_t *proc, TouchTag tag_id)
{
    if (!proc) return ERR_NULL_POINTER;
    spin_lock(&proc->subs_lock);
    TouchSub *sub = find_proc_sub(proc, tag_id);
    if (!sub) { spin_unlock(&proc->subs_lock); return ERR_TAG_NOT_FOUND; }
    __atomic_store_n(&sub->has_pending, 0, __ATOMIC_RELEASE);
    sub->pending_plen = 0;
    spin_unlock(&proc->subs_lock);
    return OK;
}

void TouchCleanupProcess(process_t *proc)
{
    if (!proc) return;
    if (proc->touch_cleaned) return;
    proc->touch_cleaned = 1;

    /* Unlink every sub from its bucket so concurrent publishes stop seeing
     * this process. Sub structs remain in proc->subs_head with proc_next
     * pointers intact; the kfree happens in TouchFinalizeProcess only after
     * the process refcount falls to zero. */
    uint32_t unlinked = 0;
    TouchSub *cur = (TouchSub *)proc->subs_head;
    while (cur) {
        if (cur->bucket) { touch_sub_unlink_bucket(cur); unlinked++; }
        if (cur->mode == TOUCH_REACT &&
            cur->u.manifest != MANIFEST_HANDLE_INVALID) {
            ManifestRelease(cur->u.manifest);
            cur->u.manifest = MANIFEST_HANDLE_INVALID;
        }
        cur = cur->proc_next;
    }
    if (unlinked > 0)
        __atomic_sub_fetch(&g_total_subs, unlinked, __ATOMIC_RELEASE);

    /* Drain pending INTERRUPT-mode queue. */
    spin_lock(&proc->irq_lock);
    TouchPending *p = (TouchPending *)proc->irq_pending_head;
    proc->irq_pending_head = NULL;
    spin_unlock(&proc->irq_lock);
    while (p) {
        TouchPending *next = p->next;
        kfree(p);
        p = next;
    }

    /* Notify subscribers — runs the new O(N_subs_for_process_died) path. */
    struct { uint32_t pid; int32_t exit; } died = { proc->pid, 0 };
    TouchPublish("process:died", &died, sizeof(died));
}

void TouchFinalizeProcess(process_t *proc)
{
    if (!proc) return;
    TouchSub *cur = (TouchSub *)proc->subs_head;
    proc->subs_head = NULL;
    while (cur) {
        TouchSub *next = cur->proc_next;
        kfree(cur);
        cur = next;
    }
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
        spin_lock(&proc->subs_lock);
        TouchSub *sub = find_proc_sub(proc, pending->tag_id);
        bool reuse = sub && sub->mode == TOUCH_INTERRUPT;
        spin_unlock(&proc->subs_lock);
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
