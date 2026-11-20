#include "system_deck.h"
#include "chit.h"       /* ChitGive — the only writer of the async flag */
#include "touch.h"
#include "touch_queue.h"
#include "touch_ring.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "crate_io.h"
#include "kresult.h"
#include "kring.h"
#include "result.h"
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
 * All hot ops take a TouchTag (uint32_t) directly — no string parsing on
 * the hot path. The single string-form op is SYSTEM_OP_TOUCH_INTERN, which
 * userspace calls ONCE per tag at process init to obtain the handle.
 * ──────────────────────────────────────────────────────────────────────── */

/* SYSTEM_OP_TOUCH_INTERN
 *   in_crate:  tag string
 *   out_crate: 8 bytes — [u32 full_id][u32 bare_id]
 *
 * Resolves the tag string via TagFS registry, interning if necessary.
 * Called once per tag at process init. The returned handles are then
 * passed to CLAIM/RELEASE/SEND/AWAIT/REGISTER/ACK in their u32 params. */
static int SysTouchIntern(const ManifestOp *op, Crate *crates,
                          uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->in_crate  == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_INVALID_ARGUMENT;

    /* The tag whole, spelled in place in the crate's copy: its width is
     * judged on the crate's size before anything is copied, and a name is
     * never cut to fit, so a subscriber never hears a shorter one. */
    const Crate *src = &crates[op->in_crate];
    if (src->size == 0)             return ERR_TAGFS_INVALID_TAG;
    if (src->size > TAG_TEXT_WIDTH) return ERR_TAGFS_TAG_TOO_LONG;
    char *tag = crate_in_buf(src, ctx);
    if (!tag) return ERR_INVALID_ADDRESS;

    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    error_t  rc   = TouchTagResolve(tag, src->size, &full, &bare);
    crate_buf_free(tag);
    if (rc != OK) return rc;

    /* Page-walked write of the two handle ids; crate_write sets out->size = 8
     * on success and fails closed (ERR_INVALID_ADDRESS) on a bad address. */
    uint32_t blob[2] = { full, bare };
    return crate_write(out, ctx, blob, sizeof(blob));
}

/* Pull a TouchTag from params[0..3]. */
static inline TouchTag params_tag(const ManifestOp *op)
{
    if (op->param_size < sizeof(TouchTag)) return TOUCH_TAG_INVALID;
    TouchTag v;
    memcpy(&v, op->params, sizeof(TouchTag));
    return v;
}

/* SYSTEM_OP_TOUCH_CLAIM
 *   params: [u32 tag_id][u8 mode][... mode-specific ...]
 *     REST      total 5 bytes  : tag + mode
 *     REACT     total 13 bytes : tag + mode + [u64 manifest_handle]
 *     INTERRUPT total 21 bytes : tag + mode + [u64 handler_addr][u64 stack_top]
 */
static int SysTouchClaim(const ManifestOp *op, Crate *crates,
                         uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < 5)  return ERR_INVALID_ARGUMENT;

    TouchTag tag = params_tag(op);
    if (tag == TOUCH_TAG_INVALID) return ERR_INVALID_ARGUMENT;

    uint8_t mode = op->params[4];
    if (mode > TOUCH_INTERRUPT) return ERR_INVALID_ARGUMENT;

    ManifestHandle manifest = MANIFEST_HANDLE_INVALID;
    uint64_t handler_addr = 0, stack_top = 0;

    if (mode == TOUCH_REACT) {
        if (op->param_size < 13) return ERR_INVALID_ARGUMENT;
        memcpy(&manifest, op->params + 5, sizeof(uint64_t));
    } else if (mode == TOUCH_INTERRUPT) {
        if (op->param_size < 21) return ERR_INVALID_ARGUMENT;
        memcpy(&handler_addr, op->params + 5,  sizeof(uint64_t));
        memcpy(&stack_top,    op->params + 13, sizeof(uint64_t));
    }

    return TouchClaimSet(ctx->proc, tag, (TouchMode)mode,
                         manifest, handler_addr, stack_top);
}

/* SYSTEM_OP_TOUCH_RELEASE
 *   params: [u32 tag_id]
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
 *   params:    [u32 full_id][u32 bare_id][u32 after_ms]  (12 bytes)
 *   out_crate (optional, 4 bytes): u32 — how many subscribers were handed
 *              the event (0 for a delayed send: nobody has heard it yet).
 *
 * Caller publishes to BOTH full_id and bare_id buckets so wildcard
 * subscribers see the event. Either may be TOUCH_TAG_INVALID.
 */
static int SysTouchSend(const ManifestOp *op, Crate *crates,
                        uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 12) return ERR_INVALID_ARGUMENT;

    TouchTag full_id, bare_id;
    uint32_t after_ms;
    memcpy(&full_id,  op->params,     sizeof(TouchTag));
    memcpy(&bare_id,  op->params + 4, sizeof(TouchTag));
    memcpy(&after_ms, op->params + 8, sizeof(uint32_t));

    if (full_id == TOUCH_TAG_INVALID && bare_id == TOUCH_TAG_INVALID)
        return ERR_INVALID_ARGUMENT;

    /* Bounded snapshot of the payload into a stack buffer. The page-walked
     * crate_read copies a straddling payload across every backing frame, and
     * the BOXOS_TOUCH_PAYLOAD_MAX (96) cap closes a DoS: src->size is
     * attacker-controlled up to UINT32_MAX and every consumer reads at most 96
     * bytes (TouchPublishPair truncates, and a delayed node carries what this
     * snapshot holds), so an unbounded size must never drive a kmalloc/copy. */
    uint8_t     payload_buf[BOXOS_TOUCH_PAYLOAD_MAX];
    const void *payload = NULL;
    uint32_t    plen    = 0;
    if (op->in_crate != CRATE_INDEX_NONE) {
        Crate *src = &crates[op->in_crate];
        if (src->size > 0) {
            uint64_t n = src->size < BOXOS_TOUCH_PAYLOAD_MAX
                         ? src->size : BOXOS_TOUCH_PAYLOAD_MAX;
            error_t rc = crate_read(src, ctx, payload_buf, n);
            if (rc != OK) return rc;
            payload = payload_buf;
            plen    = (uint32_t)n;
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
        /* How many heard it, for a sender that asked (a 4-byte out crate).
         * A delayed send says 0: nobody has heard it yet. */
        uint32_t handed = TouchPublishPair(full_id, bare_id, payload, plen,
                                           ctx->proc->pid, TOUCH_FLAG_USER);
        if (op->out_crate != CRATE_INDEX_NONE) {
            Crate *out = &crates[op->out_crate];
            if (out->capacity >= sizeof(uint32_t))
                (void)crate_write(out, ctx, &handed, sizeof(uint32_t));
        }
    } else {
        /* A moment of the touch queue's one clock. */
        uint64_t fire_at = pit_get_uptime_us() + (uint64_t)after_ms * 1000u;
        /* Both nodes made before either is armed: a send the heap has no room
         * for is refused whole, never half heard. */
        bool            two  = bare_id != TOUCH_TAG_INVALID && bare_id != full_id;
        TouchQueueNode *full = full_id != TOUCH_TAG_INVALID
                                   ? TouchQueuePublishNode(full_id, payload, plen,
                                                           ctx->proc->pid, TOUCH_FLAG_USER)
                                   : NULL;
        TouchQueueNode *bare = two ? TouchQueuePublishNode(bare_id, payload, plen,
                                                           ctx->proc->pid, TOUCH_FLAG_USER)
                                   : NULL;
        if ((full_id != TOUCH_TAG_INVALID && !full) || (two && !bare)) {
            kfree(full);
            kfree(bare);
            return ERR_NO_MEMORY;
        }
        if (full)
            TouchQueueArm(full, fire_at);
        if (bare)
            TouchQueueArm(bare, fire_at);
    }
    return OK;
}

/* SYSTEM_OP_TOUCH_AWAIT
 *   params: [u32 tag_id][u32 timeout_ms][u64 taken] (16 bytes) — `taken` is
 *   the caller's ring head when it decided to sleep (boxlib touch.c)
 *
 * Ensures a REST claim on tag_id, applies LEVEL synthetic-touch sync,
 * arms a TouchQueueArmWake timeout, parks the caller (PROC_WAITING).
 * Result lands in ResultRing via TouchRestDeliver.
 */
static int SysTouchAwait(const ManifestOp *op, Crate *crates,
                         uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16) return ERR_INVALID_ARGUMENT;

    TouchTag tag;
    uint32_t timeout_ms;
    uint64_t taken;
    memcpy(&tag,        op->params,     sizeof(TouchTag));
    memcpy(&timeout_ms, op->params + 4, sizeof(uint32_t));
    memcpy(&taken,      op->params + 8, sizeof(uint64_t));
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

    /* This sleep's number, claimed before anything is armed for it — the wake
     * the queue fires reschedules only while the strand is still in THIS park.
     * An await that ends early (the ring re-check below, or an event) leaves
     * its wake armed, and without the number that wake would land on whatever
     * park came next. See process_t.park_seq. */
    uint32_t park_seq = __atomic_add_fetch(&ctx->proc->park_seq, 1,
                                           __ATOMIC_ACQ_REL);

    if (timeout_ms > 0) {
        uint64_t fire_at = pit_get_uptime_us() + (uint64_t)timeout_ms * 1000u;
        /* owes_result=0: touch_await registers no addr_wait entry and is
         * owed no Result on expiry — only the in-IRQ reschedule (PROC_WORKING)
         * applies, and no deadline baton is passed. The await's own ring path
         * then reports count=0 on the timeout. */
        TouchQueueArmWake(ctx->proc, fire_at, 0, park_seq);
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
     * cacheline.
     *
     * ‼ The RELEASE of process_set_state's spin_unlock is NOT enough, and this
     * comment used to claim it was. RELEASE orders the state store after what
     * precedes it; it does nothing about the load that FOLLOWS. Store-then-load
     * to different locations is exactly the reordering x86 permits (SDM 3A
     * 9.2.3.4), so the state can sit in this core's store buffer while the load
     * of `tail` below already executes — and then the publisher reads
     * PROC_WORKING, skips the wake, while we read an unmoved tail and sleep on
     * a slot already in the ring. The publisher needs no fence of its own (its
     * tail bump is a lock xadd, a full barrier); the sleeper does. One mfence
     * per park.
     *
     * ‼ AND A RING THAT LOOKS EMPTY IS NOT A STRAND THAT IS WAITING. The
     * re-check above sees only what is still in the ring. An event that landed
     * in the window and was ALREADY TAKEN by the caller's wait in userspace
     * leaves tail == head, the strand has gone on past touch_await, and parking
     * it here put it to sleep with nothing left to wake it. MEASURED: the hub's
     * prompt parked for keeper:settled, its keeping strand said it, and the
     * prompt never woke — 6 boots in 80, UEFI 4 cores, two K-Cores handling
     * the send and the await at once; every CPU idle, the shell never shown.
     * So the caller says how far it had taken its ring when it decided to
     * sleep (`taken`), and a head that has moved since means it is no longer
     * waiting — the futex rule, as addr_park keeps it.
     *
     * ERR_WOULD_BLOCK is returned in both branches, and nothing is pushed
     * for it — see the note below on why. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (ctx->proc->touch_ring_phys) {
        TouchRing *rr = (TouchRing *)vmm_phys_to_virt(ctx->proc->touch_ring_phys);
        if (rr) {
            uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
            uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
            if (tail != head || head != taken) {
                process_set_state(ctx->proc, PROC_WORKING);
            }
        }
    }

    /* An answer that is OWED is an answer. The ring is not the whole story
     * once the kernel holds events it could not fit (touch.c, Owed): those
     * are accepted, ordered and waiting for a door, and this strand parking
     * on an empty ring would be parking on an event it has already been
     * promised. The wake that would free it is the hand-over at its own next
     * syscall — which it will not make while parked. So do not park: end the
     * await, let it come round again, and the door at the syscall gate hands
     * the events over on the way in.
     *
     * Reachable whenever a hand-over on another core holds the drain token and
     * then fails, and unconditionally in the mutation oracle that routes every
     * delivery through the queue — which is how it was found. */
    if (__atomic_load_n(&ctx->proc->owed_count, __ATOMIC_RELAXED) != 0)
        process_set_state(ctx->proc, PROC_WORKING);

    /* Answer nothing. This used to fall through without the flag, so the guide
     * pushed the transient ERR_WOULD_BLOCK ack as a reply — into the caller's
     * ResultRing, where KResultPush's last step reads "target is PROC_WAITING"
     * and makes it PROC_WORKING again. The park above was undone by its own
     * acknowledgement, every time, and the strand went back to spinning out its
     * wait in userspace. Nobody noticed because the ack is filtered on
     * error_code == 9 at the consumer: it was invisible as a reply and
     * load-bearing as a wake. addr_park has always set this flag, which is why
     * its park is the one that holds. The op stages no crates (touch.c submits
     * it with none), so the flag carries only its other meaning here — the
     * handler owns the completion, and the completion is the event itself.
     * No chit is left: the await is submitted without a token, so nobody is
     * holding out for a Result to it. */
    ChitGive(ctx, "system.touch.await", 0);
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
 *   params: [u32 tag_id][u8 policy][u8 capability]  (6 bytes)
 */
static int SysTouchRegister(const ManifestOp *op, Crate *crates,
                            uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 6) return ERR_INVALID_ARGUMENT;

    TouchTag tag = params_tag(op);
    if (tag == TOUCH_TAG_INVALID) return ERR_INVALID_ARGUMENT;
    TouchPolicy     policy = (TouchPolicy)op->params[4];
    TouchCapability cap    = (TouchCapability)op->params[5];
    return TouchPolicySet(tag, policy, cap);
}

/* SYSTEM_OP_TOUCH_ACK
 *   params: [u32 tag_id]
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
