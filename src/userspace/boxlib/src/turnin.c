/*
 * turnin.c — the sleep a strand takes when there is nothing left to look at.
 *
 * See box/turnin.h for what this is and how to use it. What lives here is the
 * two decisions that are easy to get wrong.
 *
 * THE HOT LOOK. Parking is not free — a submit, a K-Core hop, a reschedule —
 * and a strand handling a burst of messages would pay it between every two of
 * them. So the first thing a turn-in does is NOT go to sleep: it looks, hot,
 * for a short spin. A burst never reaches the syscall; an idle strand pays one
 * short spin once and then sleeps for as long as it likes. This is a spin
 * BUDGET, not a deadline: nothing fails when it runs out, the strand simply
 * stops guessing that more work is coming and goes to bed.
 *
 * THE LOOP THAT RE-ARMS. `submit once, then spin until woken` is a livelock
 * waiting to happen: any wake that is not an arrival — a stale timer, a
 * scheduler nudge — leaves the strand running with nothing to do and nothing
 * left to put it back down. So every turn round the loop asks AGAIN. In the
 * settled case that is exactly one round: ask, give up the core, sleep, wake
 * with the mark moved, leave. A spurious wake costs one more round instead of
 * a core. And because the request is re-issued rather than counted, there is no
 * magic number anywhere in it.
 *
 * ‼ ONE ASK AT A TIME, and this is not tidiness. On more than one core a submit
 * does not run where it was made: it goes to a K-Core, and the asking strand
 * returns to userspace immediately and keeps going. Ask on every turn of the
 * loop and a strand pumps request after request into its pocket ring for as
 * long as the K-Core takes to reach the first one — each of which the K-Core
 * then performs, in full, on a cursor that has not moved. MEASURED on an
 * earlier attempt at this sleep: 5505 parks for the display daemon and 21 for
 * the shell against NINE deliveries, and a machine that stopped dead. An empty
 * pocket ring is the exact, free question "has my last ask been taken yet" —
 * the ring is this strand's own, and nobody else pushes into it.
 */

#include "box/turnin.h"
#include "box/core/result.h"
#include "box/core/touch_ring.h"
#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/pocket.h"   /* pocket_ring_is_empty — one ask at a time */
#include "box/string.h"   /* memcpy — params are packed by hand */
#include "box/system.h"
#include "boxos_decks.h"

void yield(void);   /* boxlib (notify.c) — declared here to avoid pulling sync.h */

/* Long enough that a strand in a conversation never parks between two replies,
 * short enough to be invisible next to the K-Core round trip it is trying to
 * avoid. Same class as brook.c's BROOK_SPIN_BUDGET and print.c's
 * LANE_PUSH_SPIN_MIN — a microarchitectural quantity, not a clock. */
#define TURN_IN_HOT_LOOK  4096u

TurnInMark box_mark(void)
{
    TurnInMark m;
    ResultRing *rr = result_ring();
    TouchRing  *tr = touch_ring();
    m.result = rr ? __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE) : 0;
    m.touch  = tr ? __atomic_load_n(&tr->hdr.tail, __ATOMIC_ACQUIRE) : 0;
    return m;
}

bool box_mark_moved(TurnInMark seen)
{
    TurnInMark now = box_mark();
    return now.result != seen.result || now.touch != seen.touch;
}

/* Ask the kernel for the sleep. Fire-and-forget: this op answers nothing (see
 * turnin_ops.c), so there is no reply to wait for and no cloakroom token to
 * carry — the arrival IS the completion. */
static void turn_in_submit(TurnInMark seen)
{
    uint8_t         mbuf[128];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return;

    uint8_t params[16];
    memcpy(params,     &seen.touch,  sizeof(uint64_t));
    memcpy(params + 8, &seen.result, sizeof(uint64_t));

    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_TURN_IN, 0,
                             CRATE_INDEX_NONE, CRATE_INDEX_NONE,
                             params, sizeof(params)) != 0) return;
    if (ManifestBuilderFinalize(&mb) != 0) return;

    /* The Manifest is small enough to ride inside the ring slot, so this frame
     * may die the moment the push returns — the enclosure rule in manifest.c. */
    (void)ManifestSubmitNoWait((const Manifest *)mbuf, NULL, 0, 0);
}

void box_turn_in(TurnInMark seen)
{
    for (uint32_t spin = 0; spin < TURN_IN_HOT_LOOK; spin++) {
        if (box_mark_moved(seen)) return;
        __asm__ volatile("pause" ::: "memory");
    }

    while (!box_mark_moved(seen)) {
        /* Ask only when the last ask has been taken — see the file header. */
        if (pocket_ring_is_empty(pocket_ring())) turn_in_submit(seen);

        /* Give the core away. If the kernel has taken the request, this does
         * not come back until something arrives; if it has not yet, the
         * scheduler runs whoever else is ready and we come round and look
         * again. Either way the strand stops burning the core it was holding. */
        yield();
    }
}
