#ifndef FRIEND_H
#define FRIEND_H

#include "ktypes.h"
#include "buddy.h"

// Physical address zone tags for the Friend Allocator.
// DMA32: < 1GB (safe for 32-bit DMA)
// USER:  1GB - 4GB (general purpose)
// HIGH:  > 4GB (activated after pull-map via deferred regions)
typedef enum {
    PHYS_TAG_DMA32 = 0,
    PHYS_TAG_USER  = 1,
    PHYS_TAG_HIGH  = 2,
    PHYS_TAG_COUNT = 3
} PhysTag;

typedef struct {
    PhysTag        tag;
    uintptr_t      zone_start;
    uintptr_t      zone_end;
    BuddyFreeList  free_lists[BUDDY_MAX_ORDER + 1];
    spinlock_t     lock;
    size_t         cached_pages;
} PhysZone;

void        FriendInit(void);
void*       FriendAlloc(size_t pages, PhysTag tag);
void        FriendFree(void* addr, size_t pages);
PhysTag     FriendClassifyAddr(uintptr_t phys_addr);
const char* PhysTagName(PhysTag tag);

#endif // FRIEND_H
