#include "loose_leaves.h"
#include "tagfs_volume.h"

uint64_t LooseLeaves(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->loose_leaves.loose, __ATOMIC_ACQUIRE);
}

void LooseLeavesMounted(TagFSVolume* vol, uint64_t n)
{
    __atomic_store_n(&vol->loose_leaves.loose, n, __ATOMIC_RELEASE);
}

void LooseLeavesSettled(TagFSVolume* vol, const DocketTaken* t)
{
    uint64_t n = __atomic_load_n(&vol->loose_leaves.loose, __ATOMIC_ACQUIRE);
    for (const Jot* j = t->first; j; j = j->next[0]) {
        if (!j->carried)
            continue;
        bool was = j->was && j->was->slip, now = j->now && j->now->slip;
        if (now && !was)
            n++;
        else if (was && !now && n > 0)
            n--;
    }
    __atomic_store_n(&vol->loose_leaves.loose, n, __ATOMIC_RELEASE);
}
