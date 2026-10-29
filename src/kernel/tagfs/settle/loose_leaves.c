#include "loose_leaves.h"

/* LooseLeaves (loose_leaves.h): one count, its writers one at a time. */
static volatile uint64_t s_loose;

uint64_t LooseLeaves(void)
{
    return __atomic_load_n(&s_loose, __ATOMIC_ACQUIRE);
}

void LooseLeavesMounted(uint64_t n)
{
    __atomic_store_n(&s_loose, n, __ATOMIC_RELEASE);
}

void LooseLeavesSettled(const DocketTaken* t)
{
    uint64_t n = __atomic_load_n(&s_loose, __ATOMIC_ACQUIRE);
    for (const Jot* j = t->first; j; j = j->next[0]) {
        if (!j->carried)
            continue;
        bool was = j->was && j->was->slip, now = j->now && j->now->slip;
        if (now && !was)
            n++;
        else if (was && !now && n > 0)
            n--;
    }
    __atomic_store_n(&s_loose, n, __ATOMIC_RELEASE);
}
