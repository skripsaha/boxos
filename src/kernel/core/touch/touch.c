#include "touch.h"
#include "sync_ops.h"   /* ProcessGoneDeliver — a death pays its named waiters */
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

/* Per-core REACT-delivery nesting depth — bounds synchronous recursion
 * (touch_react_deliver → ManifestExecute → publish → touch_react_deliver).
 * Per-core is exact: kernel ops run to completion on one core's stack
 * (syscalls IF=0; scheduler switches only outside REACT chains). */
static uint32_t          g_react_depth[CONFIG_MAX_CORES];
static volatile uint64_t g_react_depth_drops;
/* Rate limit for the drop announcement below — one line per burst. */
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

    /* The kernel's own book first. A name the kernel has already spoken is
     * an occurrence, and a process asking by that name has to land on the id
     * the kernel publishes to — not on a second, private id in the volume.
     * Lookup only: a process may hear what the kernel named, never add to it.
     * A bare hit with no full hit is the honest answer for a value the kernel
     * never used, so it ends the search either way. */
    TouchLogbookLookup(tag, out_full, out_bare);
    if (*out_bare != TOUCH_TAG_INVALID) return;

    char key[256], value[256];
    tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));

    bool has_value   = (value[0] != '\0');
    bool is_wildcard = has_value && value[0] == '.' && value[1] == '.' &&
                                    value[2] == '.' && value[3] == '\0';

    /* Asked of TagFS by name rather than through a pointer to its registry:
     * this runs on every publish, and the volume can be dropped and read again
     * underneath it. */
    *out_bare = tagfs_tag_intern(key);
    if (*out_bare == TAGFS_INVALID_TAG_ID) *out_bare = TOUCH_TAG_INVALID;

    if (has_value && !is_wildcard) {
        char full[512];
        tagfs_format_tag(full, sizeof(full), key, value);
        *out_full = tagfs_tag_intern(full);
        if (*out_full == TAGFS_INVALID_TAG_ID) *out_full = TOUCH_TAG_INVALID;
    }
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
    /* A kernel ear counts. Answering "nobody" while a watch is open is how a
     * caller talks itself out of a publish someone is waiting on. */
    return __atomic_load_n(&b->sub_count,   __ATOMIC_ACQUIRE) > 0 ||
           __atomic_load_n(&b->watch_count, __ATOMIC_ACQUIRE) > 0;
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

    /* Write the record through the page-walked primitive rather than
     * single-page-translating the whole `total` range. pmm_alloc hands out
     * non-contiguous frames, so a record that crosses a page boundary
     * (sizeof(Touch) + plen exceeding the bytes left in the first frame) would,
     * under the old vmm_translate_user_addr + memcpy, spill the payload tail
     * into whatever physical frame followed the first — not the second mapped
     * page. commit_out walks each page, delivering the header and payload to
     * their real frames regardless of plen or base alignment. */
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

/* ────────────────────────────────────────────────────────────────────────
 * Owed — what a full ring refused, and did not lose.
 *
 * A claim is a promise. A TouchRing that has no free slot used to end the
 * story: the publish was retried, then thrown away and counted. For an event
 * that happens once in a strand's life — process:died above all — a throw-away
 * is a supervisor waiting forever for something that already happened.
 *
 * So an event the ring refuses is HELD instead, in the order the ring would
 * have carried it, and handed over when the ring has room. Three decisions
 * make that safe:
 *
 *   WHERE IT HANGS. On the ring's owner (process_t), never on a subscription.
 *   It is the RING that is full — one ring is one stream with one order, and a
 *   queue per subscription would have to be found, locked and ref-counted on
 *   every hand-over, putting the subscription lock in the publish and yield
 *   paths. Hanging it on the ring costs a pointer chase and nothing else.
 *
 *   WHICH LOCK. owed_lock, and it is a LEAF: held only to link or unlink one
 *   node and to publish the depth into the ring's slip. KTouchPush is never
 *   called under it — that call walks VMM and spins on the Vyukov gate, and a
 *   publisher on another core must not queue behind it for a lock whose whole
 *   job is to move a pointer.
 *
 *   WHERE THE BOUND COMES FROM. The ring itself: a queue may grow to one more
 *   ring's worth, no further. A subscriber that has fallen a whole ring behind
 *   has consumed nothing at all for that entire window — that is not congestion
 *   but a stopped consumer, and kernel memory is not its to grow. Below that
 *   line nothing is lost. Above it the loss is loud and named.
 *
 * The hand-over happens at the door: the syscall gate (idt.c), which every
 * entry into the kernel passes through — sync dispatch, K-Core submit, and
 * the YIELD short-circuit that never reaches the guide at all. And because a
 * consumer asleep in UMWAIT on the
 * ring's `tail` would never learn it should come to that door — a full ring's
 * tail does not move — the depth is mirrored into TouchRingHeader.owed, which
 * shares that cacheline. Writing the slip wakes the sleeper.
 * ──────────────────────────────────────────────────────────────────────── */

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

/* The bound: this strand's own ring capacity (64 for a Hammock-carved strand,
 * TOUCH_RING_SLOT_MAX for a cabin's main ring). Taken from the ring, never
 * invented. */
static uint32_t touch_owed_bound(process_t *proc)
{
    TouchRing *rr = touch_owed_ring(proc);
    return rr ? rr->hdr.slot_count_max : 0u;
}

/* Publish the depth into the ring header's slip. Called only with owed_lock
 * held: two writers racing here could leave 0 behind while an event is still
 * owed, and a consumer reading that 0 would go back to sleep on a tail that
 * cannot move. */
static void touch_owed_slip_locked(process_t *proc, uint32_t depth)
{
    TouchRing *rr = touch_owed_ring(proc);
    if (rr) __atomic_store_n(&rr->hdr.owed, (uint64_t)depth, __ATOMIC_RELEASE);
}

/* Take the event onto the queue's tail. Returns false only past the bound (or
 * with no ring / no memory) — the one case that is a real loss. */
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

    /* Wake exactly as a successful push would (KTouchPush step 9). An event
     * has been ACCEPTED for this strand; that it is in the queue rather than
     * in the ring is the kernel's business, not the sleeper's. Without this a
     * strand parked in touch_await with an empty ring — reachable when a
     * drainer on another core holds the token and its own push then fails —
     * would sleep on a tail no one is going to move. It wakes, finds nothing
     * in the ring, sees the slip, and comes to the door.
     *
     * A consumer in userspace UMWAIT needs no IPI at all: the slip store above
     * writes the very cacheline its UMONITOR is armed on. */
    if (process_get_state(proc) == PROC_WAITING)
        process_set_state(proc, PROC_WORKING);
    touch_wake_remote(proc);
    return true;
}

void TouchOwedHandOver(process_t *proc)
{
    if (!proc) return;
    if (__atomic_load_n(&proc->owed_count, __ATOMIC_RELAXED) == 0) return;

    /* One hand-over at a time. Order is the whole value of the queue, and two
     * drainers popping concurrently would interleave it. A caller that loses
     * the token does not wait for it: its own event goes behind the drainer's
     * on the tail, which is the same order either way. */
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

        /* owed_count is NOT decremented here. It counts what is still owed —
         * the queue plus the one node in this drainer's hand — so a publisher
         * reading it can never see 0 while an event is still undelivered and
         * jump the queue into the ring. */
        if (!KTouchPush(proc, n->tag_id, n->flags, n->source_pid,
                        n->plen ? n->payload : NULL, n->plen)) {
            /* Still no room. Back to the head it goes, exactly where it was. */
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

/* The ring refused and the queue is past its bound: this event is GONE.
 *
 * A counter nobody reads is not diagnostics. A dropped event can strand a
 * subscriber forever — process:died most of all, since a supervisor blocking
 * on it has nothing else to wake it — so the drop says so, names the victim,
 * and does it once per burst rather than once per event so a saturated ring
 * cannot drown the console it is trying to warn. */
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
    /* REST mode now publishes directly into the target's TouchRing.
     * The payload travels inline in the TouchSlot — no separate
     * `buf_heap_next` allocation, no per-publish vmm_map_page in the
     * hot path. Vyukov seq gating ties payload lifetime to the slot
     * round (consumer must read before releasing seq, see
     * touch_ring.h). This closes the buf_heap_next physical-RAM leak
     * the multiplexed-ResultRing design suffered from. */
    if (!target) return;

    /* A dying strand's ring is nobody's business any more, and a promise made
     * to a strand that is already gone is not a promise — TouchCleanupProcess
     * is on its way to free the queue this would join. Counted, not announced:
     * this drop harms no one. Checked once, here, because the retry loop that
     * used to re-check it every turn is gone. */
    if (__atomic_load_n(&target->destroying, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_touch_push_fail, 1, __ATOMIC_RELAXED);
        return;
    }

    /* Anything already owed to this ring comes first: the queue IS the ring's
     * order, and an event that went round it into a slot the queue was waiting
     * for would arrive before events accepted earlier. Settle the debt, and if
     * it does not settle, join its tail. */
    if (__atomic_load_n(&target->owed_count, __ATOMIC_RELAXED) != 0) {
        TouchOwedHandOver(target);
        if (__atomic_load_n(&target->owed_count, __ATOMIC_RELAXED) != 0) {
            if (!touch_owed_append(target, tag_id, flags, source_pid,
                                   kpayload, plen))
                touch_drop_announce(target, tag_id);
            return;
        }
    }

    /* One honest attempt, and no retry.
     *
     * There used to be 64 of them, 32 PAUSEs apart, because a full ring used
     * to be answered by a guess and the only way to get a real answer was to
     * ask again. It is answered truthfully now, at once (touch_ring.c): the
     * claim is taken only when the ring provably has room, so "no" means no.
     *
     * Retrying would be worse than useless. The only thing that can make room
     * is the consumer, and the consumer makes room by running — while this
     * K-Core spins with the event still in its hand, having not yet written
     * the slip that is what tells the consumer to come to the door at all. The
     * old loop spent 2048 PAUSEs delaying the very mechanism that resolves the
     * situation it was waiting on. What the ring refuses is owed instead, and
     * the slip goes out immediately. */
    if (KTouchPush(target, tag_id, flags, source_pid, kpayload, plen)) {
        __atomic_add_fetch(&g_touch_delivered, 1, __ATOMIC_RELAXED);
        touch_wake_remote(target);
        return;
    }

    /* A claim is a promise: hold the event for this ring and hand it over at
     * the strand's next syscall. */
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

/* Bound synchronous re-entry on THIS core — a delivery that publishes, whose
 * delivery publishes again. Measured against ACTUAL kernel-stack headroom
 * rather than a nesting count: frame-size AND stack-size proof. floor/top
 * describe the stack currently in use (set together on every dispatch); rsp in
 * (floor, top] confirms we are on that stack, and if the geometry is unknown
 * (stale, or the pre-userspace boot stack) we fall back to counting.
 *
 * Returns false when there is no room to descend — the caller must not.
 * Every true must be paired with touch_depth_leave. Shared by REACT delivery
 * into a process and by TouchWatch delivery into the kernel, because both run
 * on the publisher's stack and both can loop back into publish. */
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
        if ((n & (n - 1)) == 0)   /* power-of-2 throttle */
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

    /*
     * Stage the Crate descriptor in a kmalloc'd buffer so async-capable
     * ops (storage.read, write_job, …) can pin the descriptor pointer
     * into their async_ctx without it disappearing the moment this
     * function returns. The on-stack Crate of the previous design was a
     * latent UAF for any REACT manifest that included an async op.
     *
     * Ownership protocol — mirrors guide.c:
     *   sync handler  → async_owned stays false → we kfree at exit
     *   async-park    → handler sets *async_owned = true via
     *                   ctx->async_owns_crates → handler's completion
     *                   path owns the kbuf and frees via
     *                   crate_stage_commit_and_release. We skip the kfree.
     *
     * crates_uaddr = 0 is the kernel-internal sentinel: the subscriber's
     * manifest never asked for a user write-back of the Crate descriptor,
     * so crate_stage_commit_and_release will recognise the 0 sentinel
     * and skip its vmm_user_buf_commit_out step — pure kfree.
     */
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
        .crates_uaddr      = 0,             /* kernel-internal — see crate_stage_commit_and_release */
        .async_owns_crates = &async_owned,  /* race-safe ownership transfer */
    };
    ManifestExecResult exec_result;
    ManifestExecute(sub->u.manifest, crates_kbuf, 1, &ctx, &exec_result);

    if (!async_owned) {
        kfree(crates_kbuf);
    }
    /* else: handler kfrees via crate_stage_commit_and_release at I/O completion. */

out_dec:
    touch_depth_leave(core);
}

/* ────────────────────────────────────────────────────────────────────────
 * TouchWatch — kernel-side listeners.
 *
 * Same shape as TouchSub and for the same reason: the publisher snapshots
 * under the bucket lock and calls outside it, so a watch cleared mid-publish
 * must stay alive until the call it is already inside of returns. Base ref = 1
 * held by bucket membership; +1 per in-flight publisher.
 * ──────────────────────────────────────────────────────────────────────── */

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

    /* The coarse "does anybody listen at all?" gate that storage_ops and
     * write_job consult before serializing tag snapshots has to see kernel
     * ears too, or a publish meant for one of them is skipped wholesale. */
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

/* Drop one reference on a TouchSub and free it on the last drop. The base ref
 * is held by proc-list membership (TouchClaimSet); each in-flight publisher
 * snapshot holds one more (TouchPublishId). Callers MUST already have unlinked
 * the sub from its bucket and proc list and released+zeroed its REACT manifest
 * before dropping the base ref; the manifest check here is a backstop for any
 * path that didn't. No touch lock may be held when calling this. */
static void touch_sub_release(TouchSub *sub)
{
    if (__atomic_sub_fetch(&sub->ref, 1, __ATOMIC_ACQ_REL) != 0) return;
    if (sub->mode == TOUCH_REACT && sub->u.manifest != MANIFEST_HANDLE_INVALID)
        ManifestRelease(sub->u.manifest);
    kfree(sub);
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
 * proc*). Under the bucket lock we take BOTH a proc ref (protects e->proc in
 * Phase 2) and a sub ref (protects e->sub). After we release the lock a
 * concurrent owner-drop path may unlink the sub and try to free it, but the
 * sub ref we hold keeps it alive until Phase 2 delivery finishes and we call
 * touch_sub_release.
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

        /* Bucket-level snapshot for on-claim sync. A subscriber that
         * joins AFTER this publish but BEFORE any ack picks up the
         * latched payload from the bucket via TouchPolicyLatchedSnapshot
         * in SysTouchAwait — mirrors the LEVEL on-claim sync but for
         * one-shot latched values.
         *
         * Allocated lazily so tags that never use LATCHED never pay the
         * 96 B. Reused on subsequent first-of-cycle publishes (the
         * intervening ack clears the buffer below). Bucket lock guards
         * both pointer and bytes; callers of TouchPolicyLatchedSnapshot
         * take the same lock. */
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

/* Snapshot-then-call, exactly as the process side does it. The bucket lock is
 * released before any callback runs, so a watch may publish, and a watch on
 * another tag may be set or cleared from inside one. */
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

    uint32_t n_subs   = __atomic_load_n(&b->sub_count,   __ATOMIC_ACQUIRE);
    uint32_t n_watches = __atomic_load_n(&b->watch_count, __ATOMIC_ACQUIRE);
    if (n_subs == 0 && n_watches == 0) return;

    __atomic_add_fetch(&g_touch_publish_calls, 1, __ATOMIC_RELAXED);

    if (n_watches != 0) touch_watch_deliver(b, tag_id, kpayload, plen, source_pid);
    if (n_subs == 0) return;

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
        __atomic_add_fetch(&s->ref, 1, __ATOMIC_RELAXED);
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

    for (uint32_t i = 0; i < snap_n; i++) {
        touch_sub_release(snap[i].sub);
        process_ref_dec(snap[i].proc);
    }
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
    TouchLogbookResolve(tag, &full, &bare);
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
    /* Base reference. Set before the sub becomes visible on the bucket list so
     * any publisher snapshot increments from a live count of at least 1. */
    __atomic_store_n(&sub->ref, 1, __ATOMIC_RELAXED);

    /* Link into bucket (publish visibility) and into proc list. Bucket lock
     * acquired BEFORE proc lock — established ordering, never reversed. */
    spin_lock(&b->lock);
    sub->bucket_next = b->head;
    sub->bucket_prev = NULL;
    if (b->head) b->head->bucket_prev = sub;
    b->head = sub;
    __atomic_add_fetch(&b->sub_count, 1, __ATOMIC_RELEASE);
    spin_unlock(&b->lock);

    spin_lock(&proc->cabin->subs_lock);
    /* Re-check under the proc lock before linking into the proc list:
     *  (a) duplicate — a concurrent TouchClaimSet on the same (proc, tag_id)
     *      may have inserted; or
     *  (b) teardown — TouchCleanupProcess already set touch_cleaned and walked
     *      the proc list, missing this sub (bucket-linked above but not yet
     *      proc-linked). Linking now would orphan it: touch_cleaned latches so
     *      cleanup never runs again, and at process_destroy the bucket would
     *      still hold a sub with a dangling sub->proc → cross-core UAF on the
     *      freed process_t (+ leak). subs_lock serialises this read against
     *      cleanup's splice: if the walk could miss us, its touch_cleaned store
     *      (before its subs_lock) is visible here (after ours).
     * Either case: undo our bucket link and drop the base ref. */
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

    /* Clear bucket-level LATCHED state so a subsequent first-of-cycle
     * publish establishes the new latched payload, and so a future
     * subscriber that claims AFTER this ack but BEFORE any new publish
     * does NOT receive the now-acked stale value via on-claim sync.
     *
     * Multi-sub LATCHED case: any single ack clears the bucket for ALL
     * future joiners — simpler than reference-counting acks across subs
     * and consistent with the "first publish queued until ack" intuition
     * (the latch is a one-shot per cycle, not per-sub-cycle). Subs that
     * already received the latched delivery still hold their own
     * has_pending until they ack individually.
     *
     * Bucket lock is taken outside subs_lock to maintain the established
     * bucket→proc ordering (see TouchClaimSet at line 720 commentary). */
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
    /* Exactly-once claim. The two death-sites can reach here on different
     * K-Cores at once (SysProcKill on one, the reaper's process_destroy on
     * another); a plain check-then-store let both pass and double-publish
     * process:died with conflicting exit codes (and a data race on the byte).
     * The atomic exchange makes exactly one caller the winner. ACQ_REL is
     * strictly stronger than the prior plain store, so the touch_cleaned read
     * in TouchClaimSet (serialised by subs_lock) keeps its happens-before. */
    if (__atomic_exchange_n(&proc->touch_cleaned, 1, __ATOMIC_ACQ_REL)) return;

    /* Subscriptions are keyed by sub->proc.  With multi-strand cabins the
     * proc-list (cabin->subs_head) is shared by every strand, so we must
     * splice out only THIS strand's subs under subs_lock — sibling strands
     * mutate the same list concurrently via TouchClaimSet/Clear.  Only
     * pointer surgery happens under subs_lock: the bucket unlink takes the
     * bucket lock, and the established order is bucket→subs (TouchClaimSet),
     * so taking the bucket lock here while holding subs_lock would invert it.
     * We therefore detach first, unlock, then unlink buckets.  For a
     * single-strand cabin every sub matches, so the end state is identical
     * to the old wholesale teardown. */
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
            cur->proc_next = mine;   /* reuse proc_next as the detached-list link */
            mine = cur;
        }
        cur = pnext;
    }
    spin_unlock(&proc->cabin->subs_lock);

    /* Now unlink each detached sub from its bucket (publishers stop seeing
     * this strand), release its REACT manifest, then drop the base ref.  The
     * sub is freed here unless an in-flight publisher snapshot still holds a
     * ref — that publisher frees it on completion via touch_sub_release.
     * Capture proc_next before the release: touch_sub_release may free s. */
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

    /* Release anything still owed to this strand's ring. Its consumer is gone,
     * so the promise has no one left to keep it for; the slip goes to 0 with
     * it. A publisher that appends after this point (it may still hold a proc
     * ref) leaves a node behind — TouchOwedRelease runs once more in the final
     * teardown, where no reference can exist. */
    TouchOwedRelease(proc);

    /* Drain pending INTERRUPT-mode queue (per-strand). */
    spin_lock(&proc->irq_lock);
    TouchPending *p = (TouchPending *)proc->irq_pending_head;
    proc->irq_pending_head = NULL;
    spin_unlock(&proc->irq_lock);
    while (p) {
        TouchPending *next = p->next;
        kfree(p);
        p = next;
    }

    /* Notify subscribers — runs the new O(N_subs_for_process_died) path.
     * exit_code carries the disposition (proc_exit.h); it is snapshotted into
     * the immutable Touch payload by TouchPublish on this same core, so
     * cross-core subscribers read the ring copy, never a live field. */
    /* generation rides alongside pid+exit so a supervisor can match a death to
     * the exact incarnation (pid, generation) and drop the stale death of a
     * recycled pid. Read AFTER the touch_cleaned claim above — generation is
     * set once at creation and never changes, so this neither races nor weakens
     * the exactly-once death-publish. */
    struct __attribute__((packed)) {
        uint32_t pid;
        int32_t  exit;
        uint32_t gen;
    } died = { proc->pid, exit_code, proc->generation };
    _Static_assert(sizeof(died) == 12,
                   "process:died wire payload must stay 12 bytes "
                   "(box/touch.h TouchProcessDied: pid@0, exit_code@4, generation@8)");
    TouchPublish("process:died", &died, sizeof(died));

    /* Answer everyone parked on this incarnation being gone.
     *
     * This runs AFTER the touch_cleaned claim above, which is the single
     * commit point of a death — so a waiter that reads that flag under
     * gone_lock either sees the death (and never parks) or is already on the
     * list this drains. There is no third interleaving, and therefore no way
     * to sleep past the answer.
     *
     * Deliberately NOT the same thing as the publish above. process:died is a
     * multicast announcement to whoever happens to listen, and a multicast may
     * be dropped; this is a debt owed to a named waiter, and a debt may not.
     * The list is spliced out under the lock and delivered outside it, so no
     * cabin VMM work happens with the lock held. */
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
