#include "roadstead.h"
#include "atomics.h"   /* cpu_pause */

/* The one place door_wanted is decided: it is a fact about the head, so it is
 * said again whenever the head changes. */
static inline void RoadsteadHeadChanged(Roadstead* r)
{
    r->door_wanted = r->head != NULL && r->head->want == BERTH_WHOLE;
}

void RoadsteadMoor(Roadstead* r, RoadsteadBerth* b)
{
    b->next = NULL;
    __atomic_store_n(&b->fate, (uint8_t)BERTH_MOORED, __ATOMIC_RELAXED);
    if (r->tail) {
        r->tail->next = b;
    } else {
        r->head = b;
    }
    r->tail = b;
    RoadsteadHeadChanged(r);
}

void RoadsteadMoorFirst(Roadstead* r, RoadsteadBerth* b)
{
    __atomic_store_n(&b->fate, (uint8_t)BERTH_MOORED, __ATOMIC_RELAXED);
    b->next = r->head;
    r->head = b;
    if (!r->tail) {
        r->tail = b;
    }
    RoadsteadHeadChanged(r);
}

void RoadsteadCastOff(Roadstead* r)
{
    RoadsteadBerth* b = r->head;
    if (!b) {
        return;
    }
    r->head = b->next;
    if (!r->head) {
        r->tail = NULL;
    }
    b->next = NULL;
    RoadsteadHeadChanged(r);
}

RoadsteadBerth* RoadsteadClear(Roadstead* r)
{
    RoadsteadBerth* all = r->head;
    r->head = NULL;
    r->tail = NULL;
    r->door_wanted = false;
    return all;
}

void RoadsteadCallIn(RoadsteadBerth* b, uint8_t slot)
{
    b->slot = slot;
    __atomic_store_n(&b->fate, (uint8_t)BERTH_CALLED, __ATOMIC_RELEASE);
}

void RoadsteadSendAway(RoadsteadBerth* b)
{
    __atomic_store_n(&b->fate, (uint8_t)BERTH_GONE, __ATOMIC_RELEASE);
}

RoadsteadFate RoadsteadStandBy(RoadsteadBerth* b, void (*turn)(void* ctx), void* ctx)
{
    for (;;) {
        uint8_t fate = __atomic_load_n(&b->fate, __ATOMIC_ACQUIRE);
        if (fate != BERTH_MOORED) {
            return (RoadsteadFate)fate;
        }
        turn(ctx);
        cpu_pause();
    }
}
