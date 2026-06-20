#include "system_deck.h"
#include "touch.h"
#include "touch_queue.h"
#include "touch_ring.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "kresult.h"
#include "kring.h"
#include "result.h"
#include "scheduler.h"          /* g_global_tick + SCHEDULER_DEFAULT_TICK_HZ */
#include "result_ring.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "tagfs.h"
#include "error.h"
#include "pit.h"

/* ────────────────────────────────────────────────────────────────────────
 * Handle-based Touch syscalls (real-HW audit 2026-05-30).
 *
 * All hot ops take a TouchTag (uint16_t) directly — no string parsing on
 * the hot path. The single string-form op is SYSTEM_OP_TOUCH_INTERN, which
 * userspace calls ONCE per tag at process init to obtain the handle.
 * ──────────────────────────────────────────────────────────────────────── */

static const void *crate_read(const Crate *c, const OpContext *ctx)
{
    if (!c || c->size == 0) return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin)
        return vmm_translate_user_addr(ctx->proc->cabin->vmm,
                                       (uintptr_t)c->addr, (size_t)c->size);
    return (const void *)(uintptr_t)c->addr;
}

static error_t crate_string(const Crate *c, const OpContext *ctx,
                            char *dst, size_t dst_size)
{
    if (!c || c->size == 0 || dst_size == 0) return ERR_INVALID_ARGUMENT;
    const char *src = crate_read(c, ctx);
    if (!src) return ERR_INVALID_ADDRESS;
    size_t copy = c->size < dst_size - 1 ? c->size : dst_size - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
    if (dst[0] == '\0') return ERR_INVALID_ARGUMENT;
    return OK;
}

/* SYSTEM_OP_TOUCH_INTERN
 *   in_crate:  tag string
 *   out_crate: 4 bytes — [u16 full_id][u16 bare_id]
 *
 * Resolves the tag string via TagFS registry, interning if necessary.
 * Called once per tag at process init. The returned handles are then
 * passed to CLAIM/RELEASE/SEND/AWAIT/REGISTER/ACK in their u16 params. */
static int SysTouchIntern(const ManifestOp *op, Crate *crates,
                          uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->in_crate  == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    char tag[256];
    error_t rc = crate_string(&crates[op->in_crate], ctx, tag, sizeof(tag));
    if (rc != OK) return rc;

    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    TouchTagResolve(tag, &full, &bare);
    if (full == TOUCH_TAG_INVALID && bare == TOUCH_TAG_INVALID)
        return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 4) return ERR_INVALID_ARGUMENT;
    void *dst = (void *)(uintptr_t)out->addr;
    if (ctx->proc->cabin)
        dst = vmm_translate_user_addr(ctx->proc->cabin->vmm,
                                      (uintptr_t)out->addr, 4);
    if (!dst) return ERR_INVALID_ADDRESS;

    uint16_t buf[2] = { full, bare };
    memcpy(dst, buf, sizeof(buf));
    out->size = 4;
    return OK;
}

/* Pull a TouchTag from params[0..1]. */
static inline TouchTag params_tag(const ManifestOp *op)
{
    if (op->param_size < 2) return TOUCH_TAG_INVALID;
    uint16_t v;
    memcpy(&v, op->params, sizeof(uint16_t));
    return (TouchTag)v;
}

/* SYSTEM_OP_TOUCH_CLAIM
 *   params: [u16 tag_id][u8 mode][... mode-specific ...]
 *     REST      total 3 bytes  : tag + mode
 *     REACT     total 11 bytes : tag + mode + [u64 manifest_handle]
 *     INTERRUPT total 19 bytes : tag + mode + [u64 handler_addr][u64 stack_top]
 */
static int SysTouchClaim(const ManifestOp *op, Crate *crates,
                         uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < 3)  return ERR_INVALID_ARGUMENT;

    TouchTag tag = params_tag(op);
    if (tag == TOUCH_TAG_INVALID) return ERR_INVALID_ARGUMENT;

    uint8_t mode = op->params[2];
    if (mode > TOUCH_INTERRUPT) return ERR_INVALID_ARGUMENT;

    ManifestHandle manifest = MANIFEST_HANDLE_INVALID;
    uint64_t handler_addr = 0, stack_top = 0;

    if (mode == TOUCH_REACT) {
        if (op->param_size < 11) return ERR_INVALID_ARGUMENT;
        memcpy(&manifest, op->params + 3, sizeof(uint64_t));
    } else if (mode == TOUCH_INTERRUPT) {
        if (op->param_size < 19) return ERR_INVALID_ARGUMENT;
        memcpy(&handler_addr, op->params + 3,  sizeof(uint64_t));
        memcpy(&stack_top,    op->params + 11, sizeof(uint64_t));
    }

    return TouchClaimSet(ctx->proc, tag, (TouchMode)mode,
                         manifest, handler_addr, stack_top);
}

/* SYSTEM_OP_TOUCH_RELEASE
 *   params: [u16 tag_id]
 */
static int SysTouchRelease(const ManifestOp *op, Crate *crates,
                           uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;

    TouchTag tag = params_tag(op);
    if (tag == TOUCH_TAG_INVALID) return ERR_INVALID_ARGUMENT;
    return TouchClaimClear(ctx->proc, tag);
}

/* SYSTEM_OP_TOUCH_SEND
 *   in_crate:  payload (optional, NONE = no payload)
 *   params:    [u16 full_id][u16 bare_id][u32 after_ms]  (8 bytes)
 *
 * Caller publishes to BOTH full_id and bare_id buckets so wildcard
 * subscribers see the event. Either may be TOUCH_TAG_INVALID.
 */
static int SysTouchSend(const ManifestOp *op, Crate *crates,
                        uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    TouchTag full_id, bare_id;
    uint32_t after_ms;
    memcpy(&full_id,  op->params,     sizeof(uint16_t));
    memcpy(&bare_id,  op->params + 2, sizeof(uint16_t));
    memcpy(&after_ms, op->params + 4, sizeof(uint32_t));

    if (full_id == TOUCH_TAG_INVALID && bare_id == TOUCH_TAG_INVALID)
        return ERR_INVALID_ARGUMENT;

    const void *payload = NULL;
    uint32_t    plen    = 0;
    if (op->in_crate != CRATE_INDEX_NONE) {
        Crate *src = &crates[op->in_crate];
        if (src->size > 0) {
            payload = crate_read(src, ctx);
            if (!payload) return ERR_INVALID_ADDRESS;
            plen = (uint32_t)(src->size > UINT32_MAX ? UINT32_MAX : src->size);
        }
    }

    /* Capability pre-check using whichever id is registered. */
    TouchTag check_id = (full_id != TOUCH_TAG_INVALID) ? full_id : bare_id;
    {
        TouchPolicy     policy = TOUCH_POLICY_EDGE;
        TouchCapability cap    = TOUCH_CAP_OPEN;
        TouchPolicyGet(check_id, &policy, &cap);
        if (cap == TOUCH_CAP_KERNEL_ONLY) return ERR_ACCESS_DENIED;
        if (cap == TOUCH_CAP_OWNERS &&
            !process_has_tag_id(ctx->proc, check_id))
            return ERR_ACCESS_DENIED;
    }

    if (after_ms == 0) {
        TouchPublishPair(full_id, bare_id, payload, plen,
                         ctx->proc->pid, TOUCH_FLAG_USER);
    } else {
        /* Clock-domain rule: dispatcher reads g_global_tick (250 Hz fixed
         * derived from hpet_now_us), NOT pit_get_ticks (dynamic PIT freq +
         * HPET phase). Enqueue on the same clock or after_ms collapses. */
        uint64_t delay_ticks = ((uint64_t)after_ms * SCHEDULER_DEFAULT_TICK_HZ)
                               / 1000ULL;
        if (delay_ticks == 0) delay_ticks = 1;
        uint64_t fire_at = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED)
                           + delay_ticks;
        if (full_id != TOUCH_TAG_INVALID)
            TouchQueueEnqueue(full_id, payload, plen, fire_at,
                              ctx->proc->pid, TOUCH_FLAG_USER);
        if (bare_id != TOUCH_TAG_INVALID && bare_id != full_id)
            TouchQueueEnqueue(bare_id, payload, plen, fire_at,
                              ctx->proc->pid, TOUCH_FLAG_USER);
    }
    return OK;
}

/* SYSTEM_OP_TOUCH_AWAIT
 *   params: [u16 tag_id][u16 _pad][u32 timeout_ms] (8 bytes)
 *
 * Ensures a REST claim on tag_id, applies LEVEL synthetic-touch sync,
 * arms a TouchQueueWakeAfter timeout, parks the caller (PROC_WAITING).
 * Result lands in ResultRing via TouchRestDeliver.
 */
static int SysTouchAwait(const ManifestOp *op, Crate *crates,
                         uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    TouchTag tag;
    uint32_t timeout_ms;
    memcpy(&tag,        op->params,     sizeof(uint16_t));
    memcpy(&timeout_ms, op->params + 4, sizeof(uint32_t));
    if (tag == TOUCH_TAG_INVALID) return ERR_INVALID_ARGUMENT;

    /* Ensure a REST claim exists. */
    error_t rc = TouchClaimSet(ctx->proc, tag, TOUCH_REST,
                               MANIFEST_HANDLE_INVALID, 0, 0);
    if (rc != OK) return rc;

    /* LEVEL + LATCHED on-claim sync. */
    {
        TouchPolicy policy = TOUCH_POLICY_EDGE;
        if (TouchPolicyGet(tag, &policy, NULL)) {
            if (policy == TOUCH_POLICY_LEVEL) {
                uint8_t state = TouchPolicyLevelState(tag);
                if (state != 0)
                    TouchRestDeliver(ctx->proc, tag, &state, 1, 0,
                                     TOUCH_FLAG_KERNEL);
            } else if (policy == TOUCH_POLICY_LATCHED) {
                /* If a publish happened before our claim and no ack has
                 * cleared the bucket, deliver the latched payload now so
                 * the awaiter never misses a one-shot value. Matches the
                 * "first publish queued until ack" intuition for the
                 * late-joiner case. The 96 B stack buffer is safe — the
                 * bucket-side payload is capped at BOXOS_TOUCH_PAYLOAD_MAX. */
                uint8_t buf[BOXOS_TOUCH_PAYLOAD_MAX];
                uint32_t plen = TouchPolicyLatchedSnapshot(tag, buf);
                if (plen > 0) {
                    TouchRestDeliver(ctx->proc, tag, buf, plen, 0,
                                     TOUCH_FLAG_KERNEL);
                }
            }
        }
    }

    if (timeout_ms > 0) {
        uint64_t delay = ((uint64_t)timeout_ms * SCHEDULER_DEFAULT_TICK_HZ)
                         / 1000ULL;
        if (delay == 0) delay = 1;
        uint64_t fire_at = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED)
                           + delay;
        TouchQueueWakeAfter(ctx->proc->pid, fire_at);
    }

    process_set_state(ctx->proc, PROC_WAITING);

    /* Lost-wakeup guard — closes the touch_stress S3 ~4% flake on UEFI
     * STRICT 16c (see project memory touch_stress_s3_race_2026_06_02).
     *
     * The AWAIT pocket is dispatched async (kcore_submit) on multi-core,
     * so this handler runs after a variable latency on the K-Core that
     * popped the calling process. During that window the publisher may
     * have already pushed slots into ctx->proc->TouchRing — KTouchPush
     * step (9) only flips PROC_WAITING→PROC_WORKING, and at publish
     * time we were still PROC_WORKING (the AWAIT had not yet been
     * dequeued by K-Core), so its wake was skipped. The userspace
     * UMWAIT then wakes from the cacheline write on `tail` and the
     * caller fast-pops slots through touch_await's fast path. Parking
     * unconditionally HERE would strand the caller in PROC_WAITING
     * after it had already moved past the touch_await call; with no
     * further publishes coming (parent's burst is over) the listener
     * times out the next touch_await and reports count = 0.
     *
     * Re-check the ring under ACQUIRE. If the publisher already
     * advanced `tail` past `head`, undo the park so the next scheduler
     * tick re-runs the caller and lets it drain the slot(s). Memory
     * ordering pairs with KTouchPush's __atomic_fetch_add(tail,
     * ACQ_REL) at touch_ring.c:180; both sides serialise on the same
     * cacheline. The state write above is RELEASE (via spin_unlock
     * inside process_set_state), so KTouchPush observing PROC_WAITING
     * is guaranteed to be sequenced after our state set, and our load
     * of `tail` here either sees the publisher's update (we undo) or
     * the publisher sees our PROC_WAITING and wakes us (it wins). No
     * permanently lost wakeup.
     *
     * ERR_WOULD_BLOCK is returned in both branches; if state was
     * undone, guide.c pushes a Result that result_pop_non_ipc filters
     * on error_code == 9 (see result.c:151), so the orphan never
     * surfaces as a stale reply to a subsequent ManifestSubmitFull. */
    if (ctx->proc->touch_ring_phys) {
        TouchRing *rr = (TouchRing *)vmm_phys_to_virt(ctx->proc->touch_ring_phys);
        if (rr) {
            uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
            uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
            if (tail != head) {
                process_set_state(ctx->proc, PROC_WORKING);
            }
        }
    }

    return ERR_WOULD_BLOCK;
}

/* SYSTEM_OP_TOUCH_IRQ_RETURN — restore context after INTERRUPT handler. */
static int SysTouchIrqReturn(const ManifestOp *op, Crate *crates,
                             uint16_t crate_count, const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    TouchIrqReturn(ctx->proc);
    return OK;
}

/* SYSTEM_OP_TOUCH_REGISTER
 *   params: [u16 tag_id][u8 policy][u8 capability]  (4 bytes)
 */
static int SysTouchRegister(const ManifestOp *op, Crate *crates,
                            uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    TouchTag tag = params_tag(op);
    if (tag == TOUCH_TAG_INVALID) return ERR_INVALID_ARGUMENT;
    TouchPolicy     policy = (TouchPolicy)op->params[2];
    TouchCapability cap    = (TouchCapability)op->params[3];
    return TouchPolicySet(tag, policy, cap);
}

/* SYSTEM_OP_TOUCH_ACK
 *   params: [u16 tag_id]
 */
static int SysTouchAck(const ManifestOp *op, Crate *crates,
                       uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;

    TouchTag tag = params_tag(op);
    if (tag == TOUCH_TAG_INVALID) return ERR_INVALID_ARGUMENT;
    return TouchClaimAck(ctx->proc, tag);
}

error_t TouchOpsRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { SYSTEM_OP_TOUCH_INTERN,     SysTouchIntern,    OP_AUTH_APP,  "system.touch.intern"     },
        { SYSTEM_OP_TOUCH_CLAIM,      SysTouchClaim,     OP_AUTH_APP,  "system.touch.claim"      },
        { SYSTEM_OP_TOUCH_RELEASE,    SysTouchRelease,   OP_AUTH_APP,  "system.touch.release"    },
        { SYSTEM_OP_TOUCH_SEND,       SysTouchSend,      OP_AUTH_APP,  "system.touch.send"       },
        { SYSTEM_OP_TOUCH_AWAIT,      SysTouchAwait,     OP_AUTH_APP,  "system.touch.await"      },
        { SYSTEM_OP_TOUCH_IRQ_RETURN, SysTouchIrqReturn, OP_AUTH_APP,  "system.touch.irq_return" },
        { SYSTEM_OP_TOUCH_REGISTER,   SysTouchRegister,  OP_AUTH_APP,  "system.touch.register"   },
        { SYSTEM_OP_TOUCH_ACK,        SysTouchAck,       OP_AUTH_APP,  "system.touch.ack"        },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[TouchOps] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[TouchOps] registered %zu touch ops (handle-based ABI)\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
