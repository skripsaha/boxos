#ifndef BUDDY_H
#define BUDDY_H

#include "ktypes.h"
#include "klib.h"

#define BUDDY_MAX_ORDER     11      // 2^11 = 2048 pages = 8MB max block
#define BUDDY_PAGE_SIZE     4096

// Intrusive free node — lives inside the first 32 bytes of each free block
typedef struct BuddyFreeNode {
    struct BuddyFreeNode* next;
    struct BuddyFreeNode* prev;
    uint8_t  order;        // which free list this node belongs to
    uint8_t  _pad[7];
} BuddyFreeNode;

typedef struct {
    BuddyFreeNode* head;
    size_t         count;
} BuddyFreeList;

typedef struct {
    uintptr_t      base;            // first managed physical address (page-aligned)
    size_t         total_pages;     // total pages in this zone
    uint8_t*       alloc_map;       // 1 bit per page: 1=allocated, 0=free
    uintptr_t      alloc_map_phys;  // physical address (for DPM rebase)
    size_t         alloc_map_size;  // size in bytes
    BuddyFreeList  free_lists[BUDDY_MAX_ORDER + 1];
    spinlock_t     lock;
    size_t         free_count;      // total free pages across all orders
    bool           initialized;
} BuddyZone;

// Internal API — called from pmm.c
void   buddy_init(BuddyZone* zone, uintptr_t base, size_t total_pages,
                  uint8_t* alloc_map, uintptr_t alloc_map_phys, size_t alloc_map_size);
void*  buddy_alloc(BuddyZone* zone, size_t pages);
// Allocate 'pages' from buddy within physical range [min_phys, max_phys).
// Same as buddy_alloc but constrains result to the specified range.
// Returns NULL if no block in range is available. Acquires zone->lock internally.
void*  buddy_alloc_range(BuddyZone* zone, size_t pages, uintptr_t min_phys, uintptr_t max_phys);
void   buddy_free(BuddyZone* zone, void* addr, size_t pages);
void   buddy_activate_pull_map(BuddyZone* zone);
void   buddy_reserve_range(BuddyZone* zone, uintptr_t start, uintptr_t end);
void   buddy_free_range(BuddyZone* zone, uintptr_t start, uintptr_t end);

#endif // BUDDY_H
