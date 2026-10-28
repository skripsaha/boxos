#ifndef DOG_EAR_HOST_H
#define DOG_EAR_HOST_H

/*
 * Forced into every unit of dog_ear_host ahead of its first line (-include):
 * what parcel_host.h stands in for — the kernel's types, its heap counted —
 * and what settle/dog_ear.c reaches past the file map: a TagFS lock, which on
 * one host thread holds nothing (counted, so a take left held is seen), and
 * the Binder's one door it knocks at, a Tally demanded.
 */

#include "parcel_host.h"

#define CONFIG_VOLUME_TESTS 1

#define TAGFS_LOCK_RANK_H
#define LOCK_RANK_LEAF 2
typedef struct {
    const char* name;
    int         held;
} TagFSLock;
#define TAGFS_LOCK_INIT(n, rank) { .name = (n), .held = 0 }
static inline void TagFSLockTake(TagFSLock* l)
{
    l->held++;
}
static inline void TagFSLockDrop(TagFSLock* l)
{
    l->held--;
}

#define TAGFS_SETTLE_BINDER_H
void BinderDemanded(void);

#endif /* DOG_EAR_HOST_H */
