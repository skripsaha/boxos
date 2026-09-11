#include "system_deck.h"
#include "chit.h"
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
#include "scheduler.h"
#include "result_ring.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "tagfs.h"
#include "error.h"
#include "pit.h"


static error_t crate_string(const Crate *c, const OpContext *ctx,
                            char *dst, size_t dst_size)
{
    if (!c || c->size == 0 || dst_size == 0) return ERR_INVALID_ARGUMENT;
    size_t copy = c->size < dst_size - 1 ? c->size : dst_size - 1;
    error_t rc = crate_read(c, ctx, dst, copy);
    if (rc != OK) return rc;
    dst[copy] = '\0';
    if (dst[0] == '\0') return ERR_INVALID_ARGUMENT;
    return OK;
}

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

    uint16_t blob[2] = { full, bare };
    return crate_write(out, ctx, blob, sizeof(blob));
}

static inline TouchTag params_tag(const ManifestOp *op)
{
    if (op->param_size < 2) return TOUCH_TAG_INVALID;
    uint16_t v;
    memcpy(&v, op->params, sizeof(uint16_t));
    return (TouchTag)v;
}

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

static int SysTouchRelease(const ManifestOp *op, Crate *crates,
                           uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;

    TouchTag tag = params_tag(op);
    if (tag == TOUCH_TAG_INVALID) return ERR_INVALID_ARGUMENT;
    return TouchClaimClear(ctx->proc, tag);
}

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
        uint32_t handed = TouchPublishPair(full_id, bare_id, payload, plen,
                                           ctx->proc->pid, TOUCH_FLAG_USER);
        if (op->out_crate != CRATE_INDEX_NONE) {
            Crate *out = &crates[op->out_crate];
            if (out->capacity >= sizeof(uint32_t))
                (void)crate_write(out, ctx, &handed, sizeof(uint32_t));
        }
    } else {
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

    error_t rc = TouchClaimSet(ctx->proc, tag, TOUCH_REST,
                               MANIFEST_HANDLE_INVALID, 0, 0);
    if (rc != OK) return rc;

    {
        TouchPolicy policy = TOUCH_POLICY_EDGE;
        if (TouchPolicyGet(tag, &policy, NULL)) {
            if (policy == TOUCH_POLICY_LEVEL) {
                uint8_t state = TouchPolicyLevelState(tag);
                if (state != 0)
                    TouchRestDeliver(ctx->proc, tag, &state, 1, 0,
                                     TOUCH_FLAG_KERNEL);
            } else if (policy == TOUCH_POLICY_LATCHED) {
                uint8_t buf[BOXOS_TOUCH_PAYLOAD_MAX];
                uint32_t plen = TouchPolicyLatchedSnapshot(tag, buf);
                if (plen > 0) {
                    TouchRestDeliver(ctx->proc, tag, buf, plen, 0,
                                     TOUCH_FLAG_KERNEL);
                }
            }
        }
    }

    uint32_t park_seq = __atomic_add_fetch(&ctx->proc->park_seq, 1,
                                           __ATOMIC_ACQ_REL);

    if (timeout_ms > 0) {
        uint64_t delay = ((uint64_t)timeout_ms * SCHEDULER_DEFAULT_TICK_HZ)
                         / 1000ULL;
        if (delay == 0) delay = 1;
        uint64_t fire_at = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED)
                           + delay;
        TouchQueueWakeAfter(ctx->proc->pid, fire_at, 0, park_seq);
    }

    process_set_state(ctx->proc, PROC_WAITING);

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

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

    if (__atomic_load_n(&ctx->proc->owed_count, __ATOMIC_RELAXED) != 0)
        process_set_state(ctx->proc, PROC_WORKING);

    ChitGive(ctx, "system.touch.await", 0);
    return ERR_WOULD_BLOCK;
}

static int SysTouchIrqReturn(const ManifestOp *op, Crate *crates,
                             uint16_t crate_count, const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    TouchIrqReturn(ctx->proc);
    return OK;
}

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