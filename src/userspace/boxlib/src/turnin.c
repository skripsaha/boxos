
#include "box/turnin.h"
#include "box/core/result.h"
#include "box/core/touch_ring.h"
#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/pocket.h"
#include "box/string.h"
#include "box/system.h"
#include "boxos_decks.h"

void yield(void);

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

    (void)ManifestSubmitNoWait((const Manifest *)mbuf, NULL, 0, 0);
}

static bool turn_in_arrival(TurnInMark seen)
{
    return box_mark_moved(seen) ||
           result_published_at_head() ||
           touch_ring_published_at_head();
}

void box_turn_in(TurnInMark seen)
{
    for (uint32_t spin = 0; spin < TURN_IN_HOT_LOOK; spin++) {
        if (turn_in_arrival(seen)) return;
        __asm__ volatile("pause" ::: "memory");
    }

    while (!turn_in_arrival(seen)) {
        if (pocket_ring_is_empty(pocket_ring())) turn_in_submit(seen);

        yield();
    }
}