
#include "system_deck.h"
#include "chit.h"
#include "turnin_ops.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_decks.h"
#include "process.h"
#include "vmm.h"
#include "kring.h"
#include "result.h"
#include "result_ring.h"
#include "touch_ring.h"
#include "error.h"
#include "klib.h"

static uint64_t turnin_touch_tail(process_t *proc)
{
    if (!proc->touch_ring_phys) return 0;
    const TouchRing *tr = (const TouchRing *)vmm_phys_to_virt(proc->touch_ring_phys);
    if (!tr) return 0;
    return __atomic_load_n(&tr->hdr.tail, __ATOMIC_ACQUIRE);
}

static uint64_t turnin_result_tail(process_t *proc)
{
    if (!proc->result_ring_phys) return 0;
    const ResultRing *rr = (const ResultRing *)vmm_phys_to_virt(proc->result_ring_phys);
    if (!rr) return 0;
    return __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
}

static bool turnin_arrival_pending(process_t *proc,
                                   uint64_t touch_seen, uint64_t result_seen)
{
    return turnin_touch_tail(proc)  != touch_seen  ||
           turnin_result_tail(proc) != result_seen ||
           KResultRingHasUnreadAtHead(proc)        ||
           KTouchRingHasUnreadAtHead(proc);
}

static int SysTurnIn(const ManifestOp *op, Crate *crates,
                     uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16) return ERR_INVALID_ARGUMENT;

    uint64_t touch_seen, result_seen;
    memcpy(&touch_seen,  op->params,     sizeof(uint64_t));
    memcpy(&result_seen, op->params + 8, sizeof(uint64_t));

    process_t *proc = ctx->proc;

    if (turnin_arrival_pending(proc, touch_seen, result_seen)) {
        ChitGive(ctx, "system.turn.in", 0);
        return ERR_WOULD_BLOCK;
    }

    process_set_state(proc, PROC_WAITING);

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (turnin_arrival_pending(proc, touch_seen, result_seen)) {
        process_set_state(proc, PROC_WORKING);
    }

    else if (__atomic_load_n(&proc->owed_count, __ATOMIC_RELAXED) != 0) {
        process_set_state(proc, PROC_WORKING);
    }

    ChitGive(ctx, "system.turn.in", 0);
    return ERR_WOULD_BLOCK;
}

static int SysBell(const ManifestOp *op, Crate *crates,
                   uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    ChitGive(ctx, "system.bell", 0);

    uint32_t who;
    memcpy(&who, op->params, sizeof(uint32_t));
    if (who == 0) return ERR_WOULD_BLOCK;

    process_t *target = process_find_ref(who);
    if (!target) return ERR_WOULD_BLOCK;

    Result r;
    memset(&r, 0, sizeof(r));
    r.error_code = ERR_WOULD_BLOCK;
    (void)KResultPush(target, &r);
    process_ref_dec(target);
    return ERR_WOULD_BLOCK;
}

error_t TurnInOpsRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { SYSTEM_OP_TURN_IN, SysTurnIn, OP_AUTH_APP, "system.turn.in" },
        { SYSTEM_OP_BELL,    SysBell,   OP_AUTH_APP, "system.bell"    },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, table[i].opcode),
                                        table[i].handler, table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[TurnIn] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[TurnIn] registered system.turn.in + system.bell\n");
    return OK;
}