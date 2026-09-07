/*
 * turnin_ops.c — SYSTEM_OP_TURN_IN: the strand with nothing left to do lies down.
 *
 * WHAT THIS IS FOR. A strand does not wait on a ring. It waits for anything at
 * all addressed to it — an answer, an event, a message — and until this op
 * existed there was no way to say that. touch_await says "wake me on my Touch
 * ring"; a submit's result_wait says "wake me on my answer". A strand whose work
 * can arrive on EITHER had to keep both eyes open, which on this machine meant a
 * yield loop: the display daemon and the shell's readline between them burned
 * roughly one whole core with the box doing nothing at all.
 *
 * WHY A CURSOR AND NOT "THE RING IS EMPTY". Emptiness cannot tell "nothing came"
 * from "it came and I already took it". The caller takes its mark, looks at
 * everything, finds nothing, and asks. Between the mark and this handler running
 * — on a DIFFERENT core, after a K-Core queue — an arrival can both land and be
 * drained by the caller's own loop; head catches tail again, and an emptiness
 * test would bed down a strand holding the very work it asked to sleep through.
 * A cursor cannot be fooled that way: the tail is monotonic, so `tail != seen`
 * is the fact "something arrived after I looked", and it stays true no matter
 * who drained it. The cursor is not the whole answer either — a slot claimed
 * before the mark and released after the look is invisible to it — so the
 * slot at each ring's head is judged as well; see turnin_arrival_pending.
 *
 * WHY IT HAS NO DEADLINE. There is no honest deadline for "until something
 * happens" — a shell prompt may wait for a keystroke for a week. A guessed one
 * would be a wake-up with nothing to do, and parking BOUNDED waits was measured
 * to break sixteen-core runs (a child's _Exit racing SYS_PROC_KILL, process:died
 * lost for three seconds). So this op arms no timer at all: no TouchQueueWakeAfter,
 * no park_seq, nothing to go stale and land on a later sleep. Bounded waiters do
 * not turn in; they keep their old spin, and their deadline stays their own.
 *
 * WHY IT ANSWERS NOTHING. A reply is a delivery, and a delivery wakes the strand
 * the reply was meant to put to sleep. SysTouchAwait learned this the hard way —
 * its park was undone by its own acknowledgement, every time, invisibly, because
 * the ack is filtered on error_code == 9 at the consumer. Setting
 * *async_owns_crates tells the guide to push nothing; the completion IS the
 * arrival, and the arrival already flips PROC_WAITING -> PROC_WORKING and rings
 * the IPI (kring.c step 9, touch_ring.c step 9).
 *
 * Lock ordering: nothing is taken here beyond process_set_state's own state_lock.
 */

#include "system_deck.h"
#include "chit.h"        /* ChitGive — the only writer of the async flag */
#include "turnin_ops.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_decks.h"
#include "process.h"
#include "vmm.h"
#include "kring.h"        /* KResultPush — a bell is a cursor movement */
#include "result.h"
#include "result_ring.h"
#include "touch_ring.h"
#include "error.h"
#include "klib.h"

/* The two tails, read through the kernel's own mapping of the ring header
 * pages. Absent ring (a strand torn down mid-flight) reads as 0, which can only
 * make the comparison below refuse the sleep — the safe direction. */
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

/* -------------------------------------------------------------------------
 * A strand may lie down only when nothing addressed to it is waiting. Two
 * facts decide that, and each covers what the other cannot see:
 *
 *   the CURSORS — each ring's `tail` against the mark the caller took before
 *   its last look. If a tail has moved, something arrived after the look, or
 *   the ask is a stale one from a look already superseded; either way it must
 *   not put the strand to bed. Emptiness could not say this: a stale ask can
 *   meet an empty ring in front of a strand that is busy elsewhere and park it
 *   in the middle of its work.
 *
 *   the HEAD SLOT — published and unconsumed. A producer takes `tail` first
 *   and releases the slot's seq later, so a strand can take its mark with the
 *   tail already moved, look while the seq is still unreleased (nothing to
 *   pop), and ask to sleep with a mark that already covers the arrival. The
 *   cursors pass; the slot is released a moment later; nobody releases it
 *   twice. MEASURED on BIOS 16c (2026-09-06): the console daemon refused its
 *   ask at result 35 (a lane request had just claimed 35), looked, found the
 *   slot unreleased, and parked at 36 — the request was released right after,
 *   and the child that sent it waited for its lane for the rest of the run.
 *   A claim still in flight at the head is deliberately NOT a reason to
 *   refuse: its producer releases the seq and then reads this strand's state,
 *   so a park committed across it is undone by that producer (kring.c and
 *   touch_ring.c, the wake below the publish). Only the slot already released
 *   is the one no one comes back for.
 * ------------------------------------------------------------------------- */
static bool turnin_arrival_pending(process_t *proc,
                                   uint64_t touch_seen, uint64_t result_seen)
{
    return turnin_touch_tail(proc)  != touch_seen  ||
           turnin_result_tail(proc) != result_seen ||
           KResultRingHasUnreadAtHead(proc)        ||
           KTouchRingHasUnreadAtHead(proc);
}

/* -------------------------------------------------------------------------
 * SysTurnIn — params: [u64 touch_tail_seen][u64 result_tail_seen]  (16 bytes)
 * ------------------------------------------------------------------------- */
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

    /* Look before lying down. Cheap, and it keeps a strand that was overtaken
     * between its own mark and this handler from paying for a park it is about
     * to undo. Not load-bearing — the re-check after the park is. */
    if (turnin_arrival_pending(proc, touch_seen, result_seen)) {
        ChitGive(ctx, "system.turn.in", 0);
        return ERR_WOULD_BLOCK;
    }

    process_set_state(proc, PROC_WAITING);

    /* ‼ THE FENCE. Everything else here is arithmetic; this is the primitive.
     *
     * Store(state=WAITING) then Load(tail) is the one reordering x86 permits
     * (SDM 3A 9.2.3.4: loads may pass earlier stores to different locations),
     * and process_set_state leaves through a plain-store spin_unlock, so the
     * state can still be sitting in this core's store buffer while the load of
     * `tail` below executes. The other side of the handshake — a K-Core in
     * KResultPush or KTouchPush — does Store(tail) then Load(state) and is
     * fenced for free, because both publish their tail with a LOCKed
     * instruction (lock cmpxchg on the Result ring's CAS claim, lock xadd on
     * the Touch ring's reservation), and read the state under a lock taken
     * with another one after releasing the slot.
     *
     * Without this fence both sides may read stale: the pusher sees a strand
     * still PROC_WORKING and skips the wake, the sleeper sees a tail that has
     * not moved and beds down. That is Dekker's, and it ends with a machine
     * standing perfectly still holding a message it has already been given.
     * One mfence per SLEEP — not per message — is the whole price. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (turnin_arrival_pending(proc, touch_seen, result_seen)) {
        process_set_state(proc, PROC_WORKING);
    }

    /* An answer that is OWED is an answer. Events the kernel accepted but could
     * not fit in the ring are promised, ordered and waiting for a door — and the
     * door is this strand's own next syscall, which it will not make while
     * asleep. Same reasoning, same words, as SysTouchAwait. */
    else if (__atomic_load_n(&proc->owed_count, __ATOMIC_RELAXED) != 0) {
        process_set_state(proc, PROC_WORKING);
    }

    /* Answer nothing — see the file header. Turn In is submitted without a
     * token, so ChitGive raises the flag and leaves no chit. */
    ChitGive(ctx, "system.turn.in", 0);
    return ERR_WOULD_BLOCK;
}

/* -------------------------------------------------------------------------
 * SysBell — params: [u32 pid]  (4 bytes). Move that strand's Result cursor.
 *
 * The record carries nothing and is meant to: no sender, no cloakroom token,
 * ERR_WOULD_BLOCK. Every consumer in boxlib has discarded that shape since the
 * async-park ack existed, so a bell can never be mistaken for a message or for
 * somebody's answer — and the cursor it moves is exactly what a turned-in
 * strand is watching.
 *
 * Refusable, and that is correct: KResultPush keeps the tail of a reply ring
 * for token-carrying ANSWERS, so a bell is dropped when the ring is crowded
 * with unsolicited traffic. A strand whose ring is that full has plenty to
 * wake up for already.
 *
 * A pid nobody answers to is not an error. The ringer is a peer acting on a
 * bell it took from a shared page; the strand that hung it may have gone in
 * the meantime, and telling the ringer so would give it nothing to do.
 *
 * ‼ IT ANSWERS NOTHING, and that is the point of a doorbell. The first version
 * replied like any other op, so the ringer waited for it — a full synchronous
 * round trip, taken from inside printf, every time a printing strand found the
 * console asleep. A ring is a favour done for somebody else; making the ringer
 * block on it puts one strand's liveness inside another's, on the hottest path
 * in the system, for an answer that says nothing it could act on. Nothing is
 * lost by not answering: a ring that fails to land leaves the sleeper's bell
 * marked and its peer will hang a fresh one on its next trip down, and a ring
 * to a departed strand was always a no-op. Same discipline as TURN_IN — the
 * handler owns the completion, and there is no completion. */
static int SysBell(const ManifestOp *op, Crate *crates,
                   uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    /* Past this point nothing is answered, so every exit must say so — a
     * silent op that replies on one path is worse than one that always does. */
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
