#include "system_deck.h"
#include "touch.h"
#include "touch_queue.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
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

static const void *touch_crate_read(const Crate *c, const OpContext *ctx)
{
    if (!c || c->size == 0) return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin)
        return vmm_translate_user_addr(ctx->proc->cabin, (uintptr_t)c->addr, (size_t)c->size);
    return (const void *)(uintptr_t)c->addr;
}

static error_t touch_crate_string(const Crate *c, const OpContext *ctx,
                                   char *dst, size_t dst_size)
{
    if (!c || c->size == 0 || dst_size == 0) return ERR_INVALID_ARGUMENT;
    const char *src = touch_crate_read(c, ctx);
    if (!src) return ERR_INVALID_ADDRESS;
    size_t copy = c->size < dst_size - 1 ? c->size : dst_size - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
    if (dst[0] == '\0') return ERR_INVALID_ARGUMENT;
    return OK;
}

/* Resolve into one or two ids:
 *   *out_full = (key, value) when string carries an explicit non-wildcard value
 *   *out_bare = (key, NULL)  — key-only id, also serves wildcard `key:...`
 * Caller decides which one(s) to use (claim → bare for wildcard, full for
 * exact; send → both, so wildcard subscribers receive `key:value` events). */
static void touch_resolve_pair(const char *tag, uint16_t *out_full, uint16_t *out_bare)
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
    if (*out_bare == TAGFS_INVALID_TAG_ID)
        *out_bare = tag_registry_intern(fs->registry, key, NULL);

    if (has_value && !is_wildcard) {
        *out_full = tag_registry_lookup(fs->registry, key, value);
        if (*out_full == TAGFS_INVALID_TAG_ID)
            *out_full = tag_registry_intern(fs->registry, key, value);
    }
}

static uint16_t touch_resolve_tag(const char *tag)
{
    uint16_t full, bare;
    touch_resolve_pair(tag, &full, &bare);
    return (full != TAGFS_INVALID_TAG_ID) ? full : bare;
}

/* SYSTEM_OP_TOUCH_CLAIM
 *   in_crate: tag string
 *   params:   [u8 mode][u64 handler_addr][u64 stack_top] (17 bytes for INTERRUPT)
 *             [u8 mode][u64 manifest_handle]              (9 bytes for REACT)
 *             [u8 mode]                                   (1 byte for REST)
 */
static int SysTouchClaim(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < 1)                return ERR_INVALID_ARGUMENT;

    char tag[256];
    error_t rc = touch_crate_string(&crates[op->in_crate], ctx, tag, sizeof(tag));
    if (rc != OK) return rc;

    uint16_t tag_id = touch_resolve_tag(tag);
    if (tag_id == TAGFS_INVALID_TAG_ID) {
        TagFSState *fs = tagfs_get_state();
        if (!fs || !fs->registry) return ERR_INVALID_ARGUMENT;
        tag_id = tag_registry_intern(fs->registry, tag, NULL);
        if (tag_id == TAGFS_INVALID_TAG_ID) return ERR_INVALID_ARGUMENT;
    }

    uint8_t mode = op->params[0];
    if (mode > TOUCH_INTERRUPT) return ERR_INVALID_ARGUMENT;

    ManifestHandle manifest = MANIFEST_HANDLE_INVALID;
    uint64_t handler_addr = 0, stack_top = 0;

    if (mode == TOUCH_REACT) {
        if (op->param_size < 9) return ERR_INVALID_ARGUMENT;
        memcpy(&manifest, op->params + 1, sizeof(uint64_t));
    } else if (mode == TOUCH_INTERRUPT) {
        if (op->param_size < 17) return ERR_INVALID_ARGUMENT;
        memcpy(&handler_addr, op->params + 1, sizeof(uint64_t));
        memcpy(&stack_top,    op->params + 9, sizeof(uint64_t));
    }

    return TouchClaimSet(ctx->proc, tag_id, (TouchMode)mode,
                           manifest, handler_addr, stack_top);
}

/* SYSTEM_OP_TOUCH_RELEASE
 *   in_crate: tag string
 */
static int SysTouchRelease(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    char tag[256];
    error_t rc = touch_crate_string(&crates[op->in_crate], ctx, tag, sizeof(tag));
    if (rc != OK) return rc;

    uint16_t tag_id = touch_resolve_tag(tag);
    if (tag_id == TAGFS_INVALID_TAG_ID) return ERR_INVALID_ARGUMENT;

    return TouchClaimClear(ctx->proc, tag_id);
}

/* SYSTEM_OP_TOUCH_SEND
 *   in_crate:  tag string
 *   out_crate: payload (optional, reused as second input)
 *   params:    [u32 after_ms] (0 = immediate)
 */
static int SysTouchSend(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    char tag[256];
    error_t rc = touch_crate_string(&crates[op->in_crate], ctx, tag, sizeof(tag));
    if (rc != OK) return rc;

    uint16_t tag_id = touch_resolve_tag(tag);
    if (tag_id == TAGFS_INVALID_TAG_ID) {
        TagFSState *fs = tagfs_get_state();
        if (!fs || !fs->registry) return ERR_INVALID_ARGUMENT;
        tag_id = tag_registry_intern(fs->registry, tag, NULL);
        if (tag_id == TAGFS_INVALID_TAG_ID) return ERR_INVALID_ARGUMENT;
    }

    const void *payload = NULL;
    uint32_t    plen    = 0;
    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *src = &crates[op->out_crate];
        if (src->size > 0) {
            payload = touch_crate_read(src, ctx);
            if (!payload) return ERR_INVALID_ADDRESS;
            plen = (uint32_t)(src->size > UINT32_MAX ? UINT32_MAX : src->size);
        }
    }

    uint32_t after_ms = 0;
    if (op->param_size >= 4)
        memcpy(&after_ms, op->params, sizeof(uint32_t));

    /* Capability pre-check so we return ERR_ACCESS_DENIED to the caller. */
    {
        TouchPolicy     policy = TOUCH_POLICY_EDGE;
        TouchCapability cap    = TOUCH_CAP_OPEN;
        TouchPolicyGet(tag_id, &policy, &cap);
        if (cap == TOUCH_CAP_KERNEL_ONLY)
            return ERR_ACCESS_DENIED;
        if (cap == TOUCH_CAP_OWNERS && !process_has_tag_id(ctx->proc, tag_id))
            return ERR_ACCESS_DENIED;
    }

    /* Publish to BOTH the full (key:value) bucket and the bare-key bucket
     * so wildcard subscribers (`key:...`) and exact ones both receive. */
    uint16_t full_id, bare_id;
    touch_resolve_pair(tag, &full_id, &bare_id);

    if (after_ms == 0) {
        if (full_id != TAGFS_INVALID_TAG_ID)
            TouchPublishId(full_id, payload, plen, ctx->proc->pid, TOUCH_FLAG_USER);
        if (bare_id != TAGFS_INVALID_TAG_ID && bare_id != full_id)
            TouchPublishId(bare_id, payload, plen, ctx->proc->pid, TOUCH_FLAG_USER);
    } else {
        uint32_t freq = pit_get_frequency();
        uint64_t delay_ticks = (freq > 0) ? ((uint64_t)after_ms * freq / 1000ULL) : (uint64_t)after_ms;
        uint64_t fire_at = pit_get_ticks() + delay_ticks;
        if (full_id != TAGFS_INVALID_TAG_ID)
            TouchQueueEnqueue(full_id, payload, plen, fire_at, ctx->proc->pid, TOUCH_FLAG_USER);
        if (bare_id != TAGFS_INVALID_TAG_ID && bare_id != full_id)
            TouchQueueEnqueue(bare_id, payload, plen, fire_at, ctx->proc->pid, TOUCH_FLAG_USER);
    }
    return OK;
}

/* SYSTEM_OP_TOUCH_AWAIT
 *   in_crate:  tag string
 *   out_crate: Touch struct (32 bytes) + optional payload
 *   params:    [u32 timeout_ms] (optional)
 *
 * Sets process to PROC_WAITING. When KResultPush wakes it, the Result
 * with context=KCTX_TOUCH lands in its result ring.
 */
static int SysTouchAwait(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    char tag[256];
    error_t rc = touch_crate_string(&crates[op->in_crate], ctx, tag, sizeof(tag));
    if (rc != OK) return rc;

    uint16_t tag_id = touch_resolve_tag(tag);
    if (tag_id == TAGFS_INVALID_TAG_ID) {
        TagFSState *fs = tagfs_get_state();
        if (!fs || !fs->registry) return ERR_INVALID_ARGUMENT;
        tag_id = tag_registry_intern(fs->registry, tag, NULL);
        if (tag_id == TAGFS_INVALID_TAG_ID) return ERR_INVALID_ARGUMENT;
    }

    /* Ensure a REST claim exists so delivery finds this process */
    TouchClaim *existing = NULL;
    TouchClaim *table = (TouchClaim *)ctx->proc->claim_table;
    for (uint16_t i = 0; i < ctx->proc->claim_count; i++) {
        if (table && table[i].tag_id == tag_id) { existing = &table[i]; break; }
    }
    if (!existing) {
        rc = TouchClaimSet(ctx->proc, tag_id, TOUCH_REST,
                             MANIFEST_HANDLE_INVALID, 0, 0);
        if (rc != OK) return rc;
    }

    /* LEVEL policy: deliver synthetic touch reflecting current state before
     * parking. This is the place the touch lands cleanly in the ring without
     * being eaten by another op's reply (the SysTouchAwait reply that follows
     * has error_code=ERR_WOULD_BLOCK; userspace touch_await skips that). */
    {
        TouchPolicy policy = TOUCH_POLICY_EDGE;
        if (TouchPolicyGet(tag_id, &policy, NULL) && policy == TOUCH_POLICY_LEVEL) {
            uint8_t state = TouchPolicyLevelState(tag_id);
            if (state != 0) {
                TouchRestDeliver(ctx->proc, tag_id, &state, 1, 0, TOUCH_FLAG_KERNEL);
            }
        }
    }

    /* Optional timeout via params[0..3] — kernel-side wake the caller after
     * `timeout_ms` PIT ticks. Without this, scheduler-parked WAITING procs
     * cannot enforce their own timeout (user code does not run). */
    uint32_t timeout_ms = 0;
    if (op->param_size >= 4) memcpy(&timeout_ms, op->params, sizeof(uint32_t));
    if (timeout_ms > 0) {
        uint32_t freq = pit_get_frequency();
        uint64_t delay = (freq > 0) ? ((uint64_t)timeout_ms * freq / 1000ULL) : 1ULL;
        if (delay == 0) delay = 1;
        TouchQueueWakeAfter(ctx->proc->pid, pit_get_ticks() + delay);
    }

    process_set_state(ctx->proc, PROC_WAITING);
    return ERR_WOULD_BLOCK;
}

/* SYSTEM_OP_TOUCH_IRQ_RETURN — restore context after INTERRUPT handler */
static int SysTouchIrqReturn(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                              const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    TouchIrqReturn(ctx->proc);
    return OK;
}

/* SYSTEM_OP_TOUCH_REGISTER
 *   in_crate: tag string
 *   params:   [u8 policy][u8 capability]
 */
static int SysTouchRegister(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                             const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < 2)                return ERR_INVALID_ARGUMENT;

    char tag[256];
    error_t rc = touch_crate_string(&crates[op->in_crate], ctx, tag, sizeof(tag));
    if (rc != OK) return rc;

    uint16_t tag_id = touch_resolve_tag(tag);
    if (tag_id == TAGFS_INVALID_TAG_ID) {
        TagFSState *fs = tagfs_get_state();
        if (!fs || !fs->registry) return ERR_INVALID_ARGUMENT;
        tag_id = tag_registry_intern(fs->registry, tag, NULL);
        if (tag_id == TAGFS_INVALID_TAG_ID) return ERR_INVALID_ARGUMENT;
    }

    TouchPolicy     policy = (TouchPolicy)op->params[0];
    TouchCapability cap    = (TouchCapability)op->params[1];
    return TouchPolicySet(tag_id, policy, cap);
}

/* SYSTEM_OP_TOUCH_ACK
 *   in_crate: tag string
 * Clears the has_pending latched slot for the caller's claim.
 */
static int SysTouchAck(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    char tag[256];
    error_t rc = touch_crate_string(&crates[op->in_crate], ctx, tag, sizeof(tag));
    if (rc != OK) return rc;

    uint16_t tag_id = touch_resolve_tag(tag);
    if (tag_id == TAGFS_INVALID_TAG_ID) return ERR_TAG_NOT_FOUND;

    TouchClaim *table = (TouchClaim *)ctx->proc->claim_table;
    for (uint16_t i = 0; i < ctx->proc->claim_count; i++) {
        if (table[i].tag_id == tag_id) {
            table[i].has_pending  = 0;
            table[i].pending_plen = 0;
            return OK;
        }
    }
    return ERR_TAG_NOT_FOUND;
}

error_t TouchOpsRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
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
    debug_printf("[TouchOps] registered %zu touch ops\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
