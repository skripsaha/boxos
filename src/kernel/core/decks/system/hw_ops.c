/*
 * HW — Manifest opcode handlers for per-process real-HW state.
 *
 * Per-process knobs (LAM mode now; CET SSP / TME context KeyID later)
 * that don't belong under MemTag's region-centric model. Each op acts
 * on the CALLING process's VM context — set returns OK / err, get
 * returns the current value in the out crate.
 *
 *   LAM_GET   no params; out_crate = [u8 lam_mode]
 *   LAM_SET   params = [u8 lam_mode 0=NONE,1=U48,2=U57]
 *
 * Auth model: every op is unprivileged for self-targeted state.
 * Cross-process mutation would require "system" — not added here yet.
 */

#include "system_deck.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "manifest_auth.h"
#include "process.h"
#include "vmm.h"
#include "tme.h"
#include "klib.h"
#include "error.h"

/* ─── SYSTEM_OP_HW_LAM_GET ─────────────────────────────────────────── */
static int SysHwLamGet(const ManifestOp *op, Crate *crates,
                        uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    vmm_context_t *vmm = (vmm_context_t *)ctx->proc->cabin;
    uint8_t mode = vmm ? (uint8_t)vmm->lam_mode : 0u;

    Crate *out = &crates[op->out_crate];
    if (out->size < sizeof(uint8_t)) return ERR_INVALID_ARGUMENT;

    uint8_t *kbuf = (uint8_t *)vmm_user_buf_alloc_out(out->size);
    if (!kbuf) return ERR_NO_MEMORY;
    kbuf[0] = mode;
    int rc = vmm_user_buf_commit_out(ctx->proc->cabin,
                                      (uintptr_t)out->addr,
                                      kbuf, sizeof(uint8_t));
    vmm_user_buf_free(kbuf);
    return rc;
}

/* ─── SYSTEM_OP_HW_TME_STATE ───────────────────────────────────────── */
typedef struct {
    uint8_t   tme_active;
    uint8_t   mk_active;
    uint8_t   num_keyid_bits;
    uint8_t   activated_alg;
    uint16_t  max_keyid;
    uint16_t  pool_programmed;
    uint16_t  in_use;
    uint16_t  per_proc_quota;
    uint8_t   reduced_maxphyaddr;
    uint8_t   _pad[3];
    uint16_t  this_proc_held;
} HwTmeState;

static int SysHwTmeState(const ManifestOp *op, Crate *crates,
                          uint16_t crate_count, const OpContext *ctx) {
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->size < sizeof(HwTmeState)) return ERR_INVALID_ARGUMENT;

    HwTmeState *kbuf = (HwTmeState *)vmm_user_buf_alloc_out(out->size);
    if (!kbuf) return ERR_NO_MEMORY;
    memset(kbuf, 0, sizeof(*kbuf));

    extern TmeState g_tme;
    kbuf->tme_active        = (uint8_t)g_tme.tme_active;
    kbuf->mk_active         = (uint8_t)g_tme.mk_active;
    kbuf->num_keyid_bits    = g_tme.num_keyid_bits;
    kbuf->activated_alg     = g_tme.activated_alg;
    kbuf->max_keyid         = g_tme.max_keyid;
    kbuf->pool_programmed   = (uint16_t)g_tme.pool_programmed;
    kbuf->in_use            = (uint16_t)g_tme.in_use;
    kbuf->per_proc_quota    = 8u;  /* TME_QUOTA_PER_PROC (bay.h) — keep in sync */
    kbuf->reduced_maxphyaddr = g_tme.reduced_maxphyaddr;
    kbuf->this_proc_held    = ctx->proc->tme_keyids_held;

    int rc = vmm_user_buf_commit_out(ctx->proc->cabin,
                                      (uintptr_t)out->addr,
                                      kbuf, sizeof(*kbuf));
    vmm_user_buf_free(kbuf);
    return rc;
}

/* ─── SYSTEM_OP_HW_LAM_SET ─────────────────────────────────────────── */
static int SysHwLamSet(const ManifestOp *op, Crate *crates,
                        uint16_t crate_count, const OpContext *ctx) {
    (void)crates;
    (void)crate_count;
    if (!ctx || !ctx->proc)        return ERR_INVALID_ARGUMENT;
    if (op->param_size < 1)        return ERR_INVALID_ARGUMENT;

    uint8_t mode = op->params[0];
    if (mode > (uint8_t)VMM_LAM_U57) return ERR_INVALID_ARGUMENT;

    vmm_context_t *vmm = (vmm_context_t *)ctx->proc->cabin;
    if (!vmm) return ERR_INVALID_STATE;

    /* vmm_set_user_lam validates has_lam + 5-level paging requirements
     * and updates ctx->lam_mode. The next CR3 reload (scheduler tick or
     * vmm_switch_context) picks up the new bits. */
    return vmm_set_user_lam(vmm, (vmm_lam_mode_t)mode);
}

error_t HwOpsRegister(void) {
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { SYSTEM_OP_HW_LAM_GET,   SysHwLamGet,   OP_AUTH_APP, "system.hw.lam_get"   },
        { SYSTEM_OP_HW_LAM_SET,   SysHwLamSet,   OP_AUTH_APP, "system.hw.lam_set"   },
        { SYSTEM_OP_HW_TME_STATE, SysHwTmeState, OP_AUTH_APP, "system.hw.tme_state" },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[HwOps] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[HwOps] registered %zu hw ops\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
