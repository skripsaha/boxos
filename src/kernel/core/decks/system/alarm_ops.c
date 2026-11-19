/*
 * alarm_ops.c — SYSTEM_OP_ALARM: time, as an event.
 *
 * A strand sets the moment it wants to be told of (uptime in µs, the
 * ClockBoard's) and the kernel puts an ordinary Touch — tag "alarm", an
 * AlarmEvent — in that strand's own TouchRing when it comes. That is all a
 * turned-in strand needs: the Touch moves the cursor Turn In watches, so the
 * strand wakes with no deadline on its sleep and no helper strand to keep one
 * (a strand of the display would be a second carrier of its tag, and
 * system.ask refuses an ambiguous carrier).
 *
 * The deadline is kept by the machinery every timed wait already uses, and no
 * second timer exists: a TouchQueueNode embedded in the process
 * (process_t.alarm_node), fired at its moment like a park's deadline. One
 * strand has one alarm — setting again moves the same node, a moment of 0
 * takes it out — so the cost of an alarm is a node that is already there.
 *
 * What is pending is ONE word, process_t.alarm_moment: the moment, or 0. A
 * delivery CLAIMS it (compare-exchange to 0) and delivers only what it
 * claimed, so a moment is delivered once, and one that was replaced or
 * cancelled after the claim's read is not delivered at all: the exchange
 * loses to the new value.
 *
 * The fire delivers nothing itself. The Touch is written into a ring in
 * memory the cabin maps, and a timer interrupt does no such work (a
 * park's timeout Result starved cores of TLB-shootdown acks). So the fire
 * passes the strand's alarm_baton (baton.h) — embedded, never dropped — to a
 * K-Core, which runs AlarmDeliver. A fire that finds the baton still queued
 * folds into that pass: the pass judges by alarm_moment as it stands when it
 * runs, which is the point of one word.
 *
 * Lock ordering: SysAlarm and AlarmDeliver take no lock of their own. Arming
 * takes the touch queue's lock (TouchQueueArmAlarm), a leaf held only to
 * link a node — never with another held, and with interrupts off, as the
 * fire takes it. The fire takes that lock alone, lets it go, and only then
 * calls: AlarmFire is a compare-exchange and a baton pass, lock-free.
 * TouchRestDeliver runs on the K-Core with nothing held, and takes the locks
 * it always does (owed_lock, a leaf, only on a full ring). An alarm ends with
 * its strand: process_destroy disarms the node after setting `destroying`,
 * and a fire or a pass that meets `destroying` delivers nothing.
 */

#include "alarm_ops.h"
#include "system_deck.h"
#include "chit.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_decks.h"
#include "boxos_alarm.h"
#include "process.h"
#include "baton.h"
#include "touch.h"
#include "touch_queue.h"
#include "pit.h"
#include "error.h"
#include "klib.h"

static TouchTag g_alarm_tag = TOUCH_TAG_INVALID;

static void AlarmDeliver(void *ctx);

/* -------------------------------------------------------------------------
 * SysAlarm — params: [u64 moment_us]  (8 bytes); 0 takes the alarm back.
 * Answers nothing, as Turn In: see SYSTEM_OP_ALARM.
 * ------------------------------------------------------------------------- */
static int SysAlarm(const ManifestOp *op, Crate *crates,
                    uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    ChitGive(ctx, "system.alarm", 0);

    uint64_t moment;
    memcpy(&moment, op->params, sizeof(moment));
    process_t *proc = ctx->proc;

    /* The moment first, then the node: a delivery that reads the word after
     * this store finds the new moment, and one that read the old moment loses
     * its claim. A cancel takes the word to 0 before the node goes, so a pass
     * already on its way delivers nothing. */
    __atomic_store_n(&proc->alarm_moment, moment, __ATOMIC_SEQ_CST);
    if (moment == 0)
        TouchQueueDisarm(&proc->alarm_node);
    else
        TouchQueueArmAlarm(proc, moment);
    return ERR_WOULD_BLOCK;
}

void AlarmFire(process_t *target)
{
    if (__atomic_load_n(&target->destroying, __ATOMIC_ACQUIRE)) return;

    uint8_t idle = 0;
    if (!__atomic_compare_exchange_n(&target->alarm_passed, &idle, 1u, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return;                             /* the pass already queued will judge */

    process_ref_inc(target);                /* released by AlarmDeliver */
    target->alarm_baton.run = AlarmDeliver;
    target->alarm_baton.ctx = target;
    BatonPass(&target->alarm_baton);
}

/* The pass, on a K-Core. The gate is cleared FIRST, so a fire that comes while
 * this runs passes a baton of its own and its moment is judged again. */
static void AlarmDeliver(void *ctx)
{
    process_t *target = (process_t *)ctx;

    __atomic_store_n(&target->alarm_passed, 0u, __ATOMIC_SEQ_CST);

    uint64_t moment = __atomic_load_n(&target->alarm_moment, __ATOMIC_SEQ_CST);
    if (moment != 0 && !__atomic_load_n(&target->destroying, __ATOMIC_ACQUIRE)) {
        if (pit_get_uptime_us() < moment) {
            /* Not yet: the alarm was moved later after this fire left the
             * queue. */
            TouchQueueArmAlarm(target, moment);
        } else if (__atomic_compare_exchange_n(&target->alarm_moment, &moment, 0, false,
                                               __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            AlarmEvent event = { .moment_us = moment };
            TouchRestDeliver(target, g_alarm_tag, &event, sizeof(event), 0, 0);
        }
    }

    process_ref_dec(target);                /* the pass's ref */
}

error_t AlarmOpsRegister(void)
{
    TouchTag full, bare;
    error_t rc = TouchTagResolve(ALARM_TOUCH_TAG, sizeof(ALARM_TOUCH_TAG) - 1, &full, &bare);
    if (rc != OK) {
        kprintf("[Alarm] the tag could not be resolved: %s\n", ErrorString(rc));
        return rc;
    }
    g_alarm_tag = bare;

    rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, SYSTEM_OP_ALARM), SysAlarm, OP_AUTH_APP,
                            "system.alarm");
    if (rc != OK && rc != ERR_ALREADY_EXISTS) {
        kprintf("[Alarm] register system.alarm failed: %s\n", ErrorString(rc));
        return rc;
    }
    debug_printf("[Alarm] registered system.alarm\n");
    return OK;
}
