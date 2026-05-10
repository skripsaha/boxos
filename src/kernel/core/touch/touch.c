#include "touch.h"
#include "touch_queue.h"
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
#include "manifest_exec.h"
#include "boxos_crate.h"
#include "op_registry.h"

/* Global bitmask: bit N set means at least one process has ear_bits bit N set.
 * Fast path: if 0, skip all process_list walking for tag_id < 64. */
static volatile uint64_t g_ear_presence;

/* Per-tag policy/capability table. */
static TouchPolicyTable g_policy_table;
static spinlock_t       g_policy_lock;

/* Diagnostic counters — bumped from publish path. Zero overhead in steady
 * state (one __atomic_add_fetch per event). Snapshot via TouchStatsSnapshot. */
static volatile uint64_t g_touch_publish_calls;
static volatile uint64_t g_touch_subscribers_visited;
static volatile uint64_t g_touch_delivered;
static volatile uint64_t g_touch_emit_fail;
static volatile uint64_t g_touch_push_fail;

void TouchStatsSnapshot(uint64_t out[5])
{
    out[0] = __atomic_load_n(&g_touch_publish_calls,        __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_touch_subscribers_visited,  __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_touch_delivered,            __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_touch_emit_fail,            __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&g_touch_push_fail,            __ATOMIC_RELAXED);
}

/* Listener counter — covers BOTH well-known (<64, tracked in
 * g_ear_presence too) and overflow (>=64). g_ear_presence alone is
 * a bitmap and can't represent overflow tags, so a TagFS write to a
 * file tagged with a user-defined tag (id >= 64) would be silently
 * skipped if we only checked the bitmap. The counter increments on
 * every claim and decrements on release. */
static volatile uint32_t g_listener_count = 0;

bool TouchHasAnyListeners(void)
{
    if (__atomic_load_n(&g_ear_presence, __ATOMIC_ACQUIRE) != 0) return true;
    return __atomic_load_n(&g_listener_count, __ATOMIC_ACQUIRE) != 0;
}

void TouchInit(void)
{
    g_ear_presence = 0;
    g_listener_count = 0;
    spinlock_init(&g_policy_lock);
    memset(&g_policy_table, 0, sizeof(g_policy_table));
    TouchQueueInit();
    debug_printf("[TOUCH] Touch event subsystem initialized\n");
}

error_t TouchPolicySet(uint16_t tag_id, TouchPolicy policy, TouchCapability capability)
{
    spin_lock(&g_policy_lock);
    uint16_t slot = (uint16_t)(tag_id % TOUCH_POLICY_TABLE_SIZE);
    /* Linear probe on collision. */
    for (uint16_t i = 0; i < TOUCH_POLICY_TABLE_SIZE; i++) {
        uint16_t idx = (uint16_t)((slot + i) % TOUCH_POLICY_TABLE_SIZE);
        if (!g_policy_table.used[idx] || g_policy_table.entries[idx].tag_id == tag_id) {
            g_policy_table.entries[idx].tag_id     = tag_id;
            g_policy_table.entries[idx].policy     = (uint8_t)policy;
            g_policy_table.entries[idx].capability = (uint8_t)capability;
            if (!g_policy_table.used[idx]) {
                g_policy_table.entries[idx].level_state = 0;
                g_policy_table.used[idx] = 1;
            }
            spin_unlock(&g_policy_lock);
            return OK;
        }
    }
    spin_unlock(&g_policy_lock);
    return ERR_NO_MEMORY;
}

bool TouchPolicyGet(uint16_t tag_id, TouchPolicy *out_policy, TouchCapability *out_cap)
{
    uint16_t slot = (uint16_t)(tag_id % TOUCH_POLICY_TABLE_SIZE);
    spin_lock(&g_policy_lock);
    for (uint16_t i = 0; i < TOUCH_POLICY_TABLE_SIZE; i++) {
        uint16_t idx = (uint16_t)((slot + i) % TOUCH_POLICY_TABLE_SIZE);
        if (!g_policy_table.used[idx]) break;
        if (g_policy_table.entries[idx].tag_id == tag_id) {
            if (out_policy) *out_policy = (TouchPolicy)g_policy_table.entries[idx].policy;
            if (out_cap)    *out_cap    = (TouchCapability)g_policy_table.entries[idx].capability;
            spin_unlock(&g_policy_lock);
            return true;
        }
    }
    spin_unlock(&g_policy_lock);
    return false;
}

uint8_t TouchPolicyLevelState(uint16_t tag_id)
{
    uint16_t slot = (uint16_t)(tag_id % TOUCH_POLICY_TABLE_SIZE);
    spin_lock(&g_policy_lock);
    for (uint16_t i = 0; i < TOUCH_POLICY_TABLE_SIZE; i++) {
        uint16_t idx = (uint16_t)((slot + i) % TOUCH_POLICY_TABLE_SIZE);
        if (!g_policy_table.used[idx]) break;
        if (g_policy_table.entries[idx].tag_id == tag_id) {
            uint8_t state = g_policy_table.entries[idx].level_state;
            spin_unlock(&g_policy_lock);
            return state;
        }
    }
    spin_unlock(&g_policy_lock);
    return 0;
}

void TouchPolicySetLevelState(uint16_t tag_id, uint8_t state)
{
    uint16_t slot = (uint16_t)(tag_id % TOUCH_POLICY_TABLE_SIZE);
    spin_lock(&g_policy_lock);
    for (uint16_t i = 0; i < TOUCH_POLICY_TABLE_SIZE; i++) {
        uint16_t idx = (uint16_t)((slot + i) % TOUCH_POLICY_TABLE_SIZE);
        if (!g_policy_table.used[idx]) break;
        if (g_policy_table.entries[idx].tag_id == tag_id) {
            g_policy_table.entries[idx].level_state = state;
            spin_unlock(&g_policy_lock);
            return;
        }
    }
    spin_unlock(&g_policy_lock);
}

/* Resolve a tag string into one or two tag_ids:
 *   - `out_full`: id of (key, value) when string has a value AND is not a
 *                 "..." wildcard; otherwise TAGFS_INVALID_TAG_ID.
 *   - `out_bare`: id of (key, NULL) — used both for plain labels and for
 *                 wildcard subscriptions (`key:...`).
 * Wildcard rule: a trailing `:...` or a bare `...` value disables out_full
 * and treats the tag as key-only. */
static void resolve_tag_pair(const char *tag, uint16_t *out_full, uint16_t *out_bare)
{
    *out_full = TAGFS_INVALID_TAG_ID;
    *out_bare = TAGFS_INVALID_TAG_ID;

    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry) return;

    char key[256], value[256];
    tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));

    bool has_value   = (value[0] != '\0');
    bool is_wildcard = has_value && value[0] == '.' && value[1] == '.' && value[2] == '.' && value[3] == '\0';

    *out_bare = tag_registry_lookup(fs->registry, key, NULL);
    if (has_value && !is_wildcard) {
        *out_full = tag_registry_lookup(fs->registry, key, value);
    }
}

/* Single-tag-id resolver (legacy callers + claim/send paths that target
 * exactly one bucket). For wildcard or bare-key strings, returns the
 * key-only id; otherwise the full (key, value) id. */
static uint16_t resolve_tag(const char *tag)
{
    uint16_t full, bare;
    resolve_tag_pair(tag, &full, &bare);
    return (full != TAGFS_INVALID_TAG_ID) ? full : bare;
}

/* Check if proc subscribes to tag_id (called under no lock; uses atomics). */
static bool proc_hears(process_t *proc, uint16_t tag_id)
{
    if (tag_id < 64) {
        return (__atomic_load_n(&proc->ear_bits, __ATOMIC_ACQUIRE) >> tag_id) & 1;
    }
    uint16_t count = __atomic_load_n(&proc->ear_overflow_count, __ATOMIC_ACQUIRE);
    uint16_t *ids  = __atomic_load_n(&proc->ear_overflow_ids, __ATOMIC_ACQUIRE);
    for (uint16_t i = 0; i < count; i++) {
        if (ids[i] == tag_id) return true;
    }
    return false;
}

/* Find the TouchClaim for tag_id in proc's claim_table. ACQUIRE-load both
 * pointer and count so we observe the full publish from a sibling K-Core's
 * TouchClaimSet — without it, a concurrent claim and find could see a new
 * count but a stale table entry, returning NULL for a tag just claimed. */
static TouchClaim *find_claim(process_t *proc, uint16_t tag_id)
{
    TouchClaim *table = (TouchClaim *)__atomic_load_n(&proc->claim_table, __ATOMIC_ACQUIRE);
    uint16_t count = __atomic_load_n(&proc->claim_count, __ATOMIC_ACQUIRE);
    for (uint16_t i = 0; i < count; i++) {
        if (table[i].tag_id == tag_id) return &table[i];
    }
    return NULL;
}

/* Allocate Touch struct + payload in target's cabin heap.
 * Returns user vaddr of the Touch on success, 0 on failure. Each record
 * gets its own page — wasteful but correct under heavy MPSC contention.
 * (Packing records into shared pages turned out to fault under SMP load;
 * this conservative one-page-per-record path is what the stress suite
 * exercises and what passes 11/11 + S2/S3 reliably.) */
static uint64_t touch_emit_payload(process_t *target, uint16_t tag_id,
                                   uint16_t flags, uint32_t source_pid,
                                   const void *kpayload, uint32_t plen)
{
    uint64_t total = sizeof(Touch) + plen;
    uint32_t pages = (uint32_t)((total + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    uint64_t bytes = (uint64_t)pages * PMM_PAGE_SIZE;
    uint64_t vaddr = __atomic_fetch_add(&target->buf_heap_next, bytes, __ATOMIC_ACQ_REL);

    for (uint32_t i = 0; i < pages; i++) {
        void *page = pmm_alloc(1);
        if (!page) return 0;
        uint64_t va = vaddr + (i * PMM_PAGE_SIZE);
        vmm_map_result_t r = vmm_map_page(target->cabin, va, (uint64_t)page, VMM_FLAGS_USER_RW);
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

/* Wake target's home App-Core via IPI when running SMP and target is on a
 * different core than current. Local-core target wakes via state-only. */
static void touch_wake_remote(process_t *target)
{
    if (g_amp.total_cores <= 1) return;
    uint8_t core = target->home_core;
    if (core < g_amp.total_cores && core != amp_get_core_index()) {
        lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
    }
}

void TouchRestDeliver(process_t *target, uint16_t tag_id,
                      const void *kpayload, uint32_t plen,
                      uint32_t source_pid, uint16_t flags)
{
    uint64_t vaddr = touch_emit_payload(target, tag_id, flags, source_pid, kpayload, plen);
    if (vaddr == 0) {
        __atomic_add_fetch(&g_touch_emit_fail, 1, __ATOMIC_RELAXED);
        return;
    }

    Result r = {
        .error_code  = OK,
        .data_length = (uint32_t)(sizeof(Touch) + plen),
        .data_addr   = vaddr,
        .sender_pid  = source_pid,
        .context     = KCTX_TOUCH,
    };

    /* Retry on transient ring contention. KResultPush returns false either
     * on overflow (ring full — synthetic ERR slot already written, drain
     * keeps moving) or on a Vyukov seq-mismatch spin-out. We back off via
     * cpu_pause() so the consumer can drain and try a small number of
     * times before giving up. This is the difference between 84% and 100%
     * delivery on 16-core stress. */
    enum { TOUCH_PUSH_RETRIES = 64 };
    for (int attempt = 0; attempt < TOUCH_PUSH_RETRIES; attempt++) {
        if (KResultPush(target, &r)) {
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
    /* Final fall-through wakes the consumer regardless so it drains the
     * synthetic-error slot and the ring keeps moving. */
    touch_wake_remote(target);
}

static void touch_interrupt_deliver(process_t *proc, const TouchClaim *claim,
                                    uint16_t tag_id, const void *kpayload,
                                    uint32_t plen, uint32_t source_pid, uint16_t flags)
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

    uint64_t touch_vaddr = touch_emit_payload(proc, tag_id, flags, source_pid, kpayload, plen);
    if (touch_vaddr == 0) {
        spin_unlock(&proc->irq_lock);
        return;
    }

    proc->irq_saved_rip    = proc->context.rip;
    proc->irq_saved_rsp    = proc->context.rsp;
    proc->irq_saved_rflags = proc->context.rflags;

    proc->context.rip = claim->u.irq.handler_addr;
    proc->context.rsp = claim->u.irq.stack_top;
    proc->context.rdi = touch_vaddr;  /* SysV ABI: first arg in RDI */
    proc->irq_active  = 1;

    spin_unlock(&proc->irq_lock);

    process_set_state(proc, PROC_WORKING);
    touch_wake_remote(proc);
}

/* REACT mode: kernel runs claim->manifest on behalf of `proc` with the
 * Touch payload as a single input Crate. ManifestExecute pins+releases the
 * handle internally; we don't need our own Resolve/Release pair. */
static void touch_react_deliver(process_t *proc, const TouchClaim *claim,
                                uint16_t tag_id, const void *kpayload,
                                uint32_t plen, uint32_t source_pid, uint16_t flags)
{
    if (claim->u.manifest == MANIFEST_HANDLE_INVALID) return;

    uint64_t vaddr = touch_emit_payload(proc, tag_id, flags, source_pid, kpayload, plen);
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
    ManifestExecute(claim->u.manifest, &crate, 1, &ctx, &exec_result);
}

/* Deliver one event to one subscriber, respecting LATCHED policy. */
static void deliver_to_subscriber(process_t *proc, TouchClaim *claim, uint16_t tag_id,
                                  const void *kpayload, uint32_t plen,
                                  uint32_t source_pid, uint16_t flags,
                                  TouchPolicy policy)
{
    if (policy == TOUCH_POLICY_LATCHED) {
        if (claim->has_pending) return; /* drop — keep existing pending */
        claim->has_pending  = 1;
        claim->pending_plen = plen > sizeof(claim->pending_payload) ? (uint32_t)sizeof(claim->pending_payload) : plen;
        if (claim->pending_plen > 0 && kpayload)
            memcpy(claim->pending_payload, kpayload, claim->pending_plen);
        /* Fall through to also push to result ring so process wakes. */
    }

    switch ((TouchMode)claim->mode) {
    case TOUCH_REST:
        TouchRestDeliver(proc, tag_id, kpayload, plen, source_pid, flags);
        break;
    case TOUCH_REACT:
        touch_react_deliver(proc, claim, tag_id, kpayload, plen, source_pid, flags);
        break;
    case TOUCH_INTERRUPT:
        touch_interrupt_deliver(proc, claim, tag_id, kpayload, plen, source_pid, flags);
        break;
    }
}

void TouchPublishId(uint16_t tag_id, const void *kpayload, uint32_t plen,
                      uint32_t source_pid, uint16_t flags)
{
    if (tag_id == TAGFS_INVALID_TAG_ID) return;

    if (tag_id < 64 && !(__atomic_load_n(&g_ear_presence, __ATOMIC_ACQUIRE) & ((uint64_t)1 << tag_id)))
        return;

    __atomic_add_fetch(&g_touch_publish_calls, 1, __ATOMIC_RELAXED);

    /* Resolve policy + capability. */
    TouchPolicy     policy    = TOUCH_POLICY_EDGE;
    TouchCapability cap_check = TOUCH_CAP_OPEN;
    TouchPolicyGet(tag_id, &policy, &cap_check);

    /* Capability check: kernel-only tags block user sends. */
    if (cap_check == TOUCH_CAP_KERNEL_ONLY && source_pid != 0) return;

    /* For LEVEL policy: update state and skip delivery if clearing (payload[0]==0). */
    if (policy == TOUCH_POLICY_LEVEL) {
        uint8_t new_state = (plen > 0 && kpayload) ? ((const uint8_t *)kpayload)[0] : 0;
        TouchPolicySetLevelState(tag_id, new_state);
        if (new_state == 0) return; /* Level cleared: no push to subscribers. */
    }

    /* Snapshot live PIDs. Hot path — avoid kmalloc when count fits a stack
     * buffer (covers PROCESS_MAX_COUNT for typical workloads). Falls back
     * to kmalloc for unusually large process counts to remain unbounded. */
    enum { TOUCH_PUB_STACK_SLOTS = 256 };
    uint32_t stack_buf[TOUCH_PUB_STACK_SLOTS];
    uint32_t *all_pids = stack_buf;
    uint32_t  pid_cap  = TOUCH_PUB_STACK_SLOTS;
    uint32_t  needed   = process_get_count() + 8; /* small slack for in-flight spawns */
    if (needed > pid_cap) {
        uint32_t *heap_buf = (uint32_t *)kmalloc(needed * sizeof(uint32_t));
        if (heap_buf) { all_pids = heap_buf; pid_cap = needed; }
    }
    uint32_t all_count = process_snapshot_pids(all_pids, pid_cap);

    for (uint32_t i = 0; i < all_count; i++) {
        process_t *proc = process_find_ref(all_pids[i]);
        if (!proc) continue;
        if (proc->destroying) { process_ref_dec(proc); continue; }
        process_state_t s = process_get_state(proc);
        if (s == PROC_CRASHED || s == PROC_DONE) { process_ref_dec(proc); continue; }

        if (!proc_hears(proc, tag_id)) { process_ref_dec(proc); continue; }

        /* OWNERS capability: verify sender has this tag. */
        if (cap_check == TOUCH_CAP_OWNERS && source_pid != 0) {
            process_t *sender = process_find_ref(source_pid);
            bool ok = sender && process_has_tag_id(sender, tag_id);
            if (sender) process_ref_dec(sender);
            if (!ok) { process_ref_dec(proc); continue; }
        }

        TouchClaim *claim = find_claim(proc, tag_id);
        if (!claim) { process_ref_dec(proc); continue; }

        __atomic_add_fetch(&g_touch_subscribers_visited, 1, __ATOMIC_RELAXED);
        deliver_to_subscriber(proc, claim, tag_id, kpayload, plen, source_pid, flags, policy);
        process_ref_dec(proc);
    }

    if (all_pids != stack_buf) kfree(all_pids);
}

void TouchPublish(const char *tag, const void *kpayload, uint32_t plen)
{
    uint16_t full, bare;
    resolve_tag_pair(tag, &full, &bare);

    /* Exact subscribers (full key:value match). */
    if (full != TAGFS_INVALID_TAG_ID)
        TouchPublishId(full, kpayload, plen, 0, TOUCH_FLAG_KERNEL);

    /* Wildcard / bare-key subscribers — receive every value under this key. */
    if (bare != TAGFS_INVALID_TAG_ID && bare != full)
        TouchPublishId(bare, kpayload, plen, 0, TOUCH_FLAG_KERNEL);
}

error_t TouchClaimSet(process_t *proc, uint16_t tag_id, TouchMode mode,
                        ManifestHandle manifest, uint64_t handler_addr, uint64_t stack_top)
{
    if (!proc) return ERR_NULL_POINTER;

    /* ACQUIRE-load to pair with RELEASE store on update — see find_claim. */
    TouchClaim *table = (TouchClaim *)__atomic_load_n(&proc->claim_table, __ATOMIC_ACQUIRE);
    uint16_t init_count = __atomic_load_n(&proc->claim_count, __ATOMIC_ACQUIRE);
    /* Check for existing claim to overwrite */
    for (uint16_t i = 0; i < init_count; i++) {
        if (table[i].tag_id == tag_id) {
            if (table[i].mode == TOUCH_REACT && table[i].u.manifest != MANIFEST_HANDLE_INVALID)
                ManifestRelease(table[i].u.manifest);
            table[i].mode  = (uint8_t)mode;
            table[i]._pad  = 0;
            if (mode == TOUCH_REACT) {
                ManifestRetain(manifest);
                table[i].u.manifest = manifest;
            } else if (mode == TOUCH_INTERRUPT) {
                table[i].u.irq.handler_addr = handler_addr;
                table[i].u.irq.stack_top    = stack_top;
            } else {
                table[i].u._raw = 0;
            }
            goto update_ears;
        }
    }

    /* New claim — grow table if needed */
    if (init_count >= proc->claim_capacity) {
        uint16_t new_cap = proc->claim_capacity == 0 ? 8 : (uint16_t)(proc->claim_capacity * 2);
        TouchClaim *new_table = kmalloc(sizeof(TouchClaim) * new_cap);
        if (!new_table) return ERR_NO_MEMORY;
        if (init_count > 0)
            memcpy(new_table, table, sizeof(TouchClaim) * init_count);
        TouchClaim *old = table;
        __atomic_store_n(&proc->claim_table, new_table, __ATOMIC_RELEASE);
        proc->claim_capacity = new_cap;
        if (old) kfree(old);
        table = new_table;
    }

    {
        uint16_t slot = init_count;
        table[slot].tag_id = tag_id;
        table[slot].mode   = (uint8_t)mode;
        table[slot]._pad   = 0;
        if (mode == TOUCH_REACT) {
            ManifestRetain(manifest);
            table[slot].u.manifest = manifest;
        } else if (mode == TOUCH_INTERRUPT) {
            table[slot].u.irq.handler_addr = handler_addr;
            table[slot].u.irq.stack_top    = stack_top;
        } else {
            table[slot].u._raw = 0;
        }
        /* RELEASE-store the new claim_count so a sibling K-Core (e.g. one
         * processing this same process's NEXT Pocket — touch_release) sees
         * the slot's writes before our claim_count bump. */
        __atomic_store_n(&proc->claim_count, (uint16_t)(slot + 1),
                         __ATOMIC_RELEASE);

        /* Bump global listener count — covers overflow tags (>=64) too,
         * so TouchHasAnyListeners() can short-circuit publish only when
         * truly nothing subscribes anywhere. */
        __atomic_add_fetch(&g_listener_count, 1, __ATOMIC_RELEASE);
    }

update_ears:
    if (tag_id < 64) {
        __atomic_or_fetch(&proc->ear_bits, (uint64_t)1 << tag_id, __ATOMIC_RELAXED);
        __atomic_or_fetch(&g_ear_presence, (uint64_t)1 << tag_id, __ATOMIC_RELEASE);
    } else {
        bool found = false;
        for (uint16_t i = 0; i < proc->ear_overflow_count; i++) {
            if (proc->ear_overflow_ids[i] == tag_id) { found = true; break; }
        }
        if (!found) {
            if (proc->ear_overflow_count >= proc->ear_overflow_capacity) {
                uint16_t new_cap = proc->ear_overflow_capacity == 0 ? 8
                                   : (uint16_t)(proc->ear_overflow_capacity * 2);
                uint16_t *new_ids = kmalloc(sizeof(uint16_t) * new_cap);
                if (!new_ids) return ERR_NO_MEMORY;
                if (proc->ear_overflow_count > 0)
                    memcpy(new_ids, proc->ear_overflow_ids,
                           sizeof(uint16_t) * proc->ear_overflow_count);
                uint16_t *old = proc->ear_overflow_ids;
                __atomic_store_n(&proc->ear_overflow_ids, new_ids, __ATOMIC_RELEASE);
                proc->ear_overflow_capacity = new_cap;
                if (old) kfree(old);
            }
            proc->ear_overflow_ids[proc->ear_overflow_count++] = tag_id;
        }
    }

    /* LEVEL policy: if state is already set when claiming, deliver synthetic touch. */
    /* LEVEL on-claim sync — deferred to Phase 3.
     * Inline TouchRestDeliver here is consumed by the caller's MfCall1
     * reply; TouchQueueEnqueue would broadcast to all subscribers, not
     * just this one. Proper fix: add a "synthetic_pending" flag to
     * TouchClaim that the next touch_await retrieves. */
    return OK;
}

error_t TouchClaimClear(process_t *proc, uint16_t tag_id)
{
    if (!proc) return ERR_NULL_POINTER;

    /* ACQUIRE-load pairs with the RELEASE store in TouchClaimSet so we
     * see all entries the sibling K-Core just wrote. */
    TouchClaim *table = (TouchClaim *)__atomic_load_n(&proc->claim_table, __ATOMIC_ACQUIRE);
    uint16_t count = __atomic_load_n(&proc->claim_count, __ATOMIC_ACQUIRE);
    for (uint16_t i = 0; i < count; i++) {
        if (table[i].tag_id != tag_id) continue;

        if (table[i].mode == TOUCH_REACT && table[i].u.manifest != MANIFEST_HANDLE_INVALID)
            ManifestRelease(table[i].u.manifest);

        /* Compact: move last entry here */
        uint16_t last = (uint16_t)(count - 1);
        if (i < last) table[i] = table[last];
        /* RELEASE-store new count so a sibling K-Core's next claim sees
         * our compaction. Mirrors the RELEASE in TouchClaimSet. */
        __atomic_store_n(&proc->claim_count, last, __ATOMIC_RELEASE);
        count = last;

        /* Mirror the listener-counter bump in TouchClaimSet. */
        __atomic_sub_fetch(&g_listener_count, 1, __ATOMIC_RELEASE);

        /* Clear ear bit / overflow entry */
        if (tag_id < 64) {
            /* Only clear if no other claim for this tag_id */
            bool still_needed = false;
            for (uint16_t j = 0; j < count; j++) {
                if (table[j].tag_id == tag_id) { still_needed = true; break; }
            }
            if (!still_needed)
                __atomic_and_fetch(&proc->ear_bits, ~((uint64_t)1 << tag_id), __ATOMIC_RELAXED);
        } else {
            bool still_needed = false;
            for (uint16_t j = 0; j < count; j++) {
                if (table[j].tag_id == tag_id) { still_needed = true; break; }
            }
            if (!still_needed) {
                for (uint16_t j = 0; j < proc->ear_overflow_count; j++) {
                    if (proc->ear_overflow_ids[j] == tag_id) {
                        uint16_t last_ov = proc->ear_overflow_count - 1;
                        if (j < last_ov)
                            proc->ear_overflow_ids[j] = proc->ear_overflow_ids[last_ov];
                        proc->ear_overflow_count--;
                        break;
                    }
                }
            }
        }
        return OK;
    }
    return ERR_TAG_NOT_FOUND;
}

void TouchCleanupProcess(process_t *proc)
{
    if (!proc) return;
    /* Idempotency guard: multiple callers (SysProcKill + process_destroy) are safe. */
    if (proc->touch_cleaned) return;
    proc->touch_cleaned = 1;

    /* Release all manifest handles from REACT claims; balance the
     * global listener counter for every claim we're tearing down. */
    TouchClaim *table = (TouchClaim *)proc->claim_table;
    if (proc->claim_count > 0) {
        __atomic_sub_fetch(&g_listener_count,
                           (uint32_t)proc->claim_count,
                           __ATOMIC_RELEASE);
    }
    for (uint16_t i = 0; i < proc->claim_count; i++) {
        if (table[i].mode == TOUCH_REACT && table[i].u.manifest != MANIFEST_HANDLE_INVALID)
            ManifestRelease(table[i].u.manifest);
    }
    if (proc->claim_table) {
        kfree(proc->claim_table);
        proc->claim_table    = NULL;
        proc->claim_count    = 0;
        proc->claim_capacity = 0;
    }

    if (proc->ear_overflow_ids) {
        kfree(proc->ear_overflow_ids);
        proc->ear_overflow_ids     = NULL;
        proc->ear_overflow_count   = 0;
        proc->ear_overflow_capacity = 0;
    }

    /* Free pending interrupt queue */
    spin_lock(&proc->irq_lock);
    TouchPending *p = (TouchPending *)proc->irq_pending_head;
    proc->irq_pending_head = NULL;
    spin_unlock(&proc->irq_lock);
    while (p) {
        TouchPending *next = p->next;
        kfree(p);
        p = next;
    }

    /* Publish process.died */
    struct { uint32_t pid; int32_t exit; } died = { proc->pid, 0 };
    TouchPublish("process:died", &died, sizeof(died));
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
        spin_unlock(&proc->irq_lock);

        /* Re-deliver the pending touch as an interrupt */
        TouchClaim *claim = find_claim(proc, pending->tag_id);
        if (claim && claim->mode == TOUCH_INTERRUPT) {
            touch_interrupt_deliver(proc, claim,
                                    pending->tag_id,
                                    pending->plen > 0 ? pending->payload : NULL,
                                    pending->plen,
                                    pending->source_pid,
                                    pending->flags);
        }
        kfree(pending);
        return;
    }
    spin_unlock(&proc->irq_lock);
}
