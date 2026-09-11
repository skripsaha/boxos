#ifndef BUDDY_H
#define BUDDY_H

#include "ktypes.h"
#include "klib.h"

#define BUDDY_MAX_ORDER     14
#define BUDDY_PAGE_SIZE     4096

typedef struct BuddyFreeNode {
    struct BuddyFreeNode* next;
    struct BuddyFreeNode* prev;
    uint8_t  order;
    uint8_t  _pad[7];
} BuddyFreeNode;

typedef struct {
    BuddyFreeNode* head;
    BuddyFreeNode* tail;
    size_t         count;
} BuddyFreeList;

typedef struct {
    uintptr_t      base;
    size_t         total_pages;
    uint8_t*       alloc_map;
    uintptr_t      alloc_map_phys;
    size_t         alloc_map_size;
    BuddyFreeList  free_lists[BUDDY_MAX_ORDER + 1];
    spinlock_t     lock;
    size_t         free_count;
    bool           initialized;
} BuddyZone;

void   buddy_init(BuddyZone* zone, uintptr_t base, size_t total_pages,
                  uint8_t* alloc_map, uintptr_t alloc_map_phys, size_t alloc_map_size);
void*  buddy_alloc(BuddyZone* zone, size_t pages);
void*  buddy_alloc_range(BuddyZone* zone, size_t pages, uintptr_t min_phys, uintptr_t max_phys);
void   buddy_free(BuddyZone* zone, void* addr, size_t pages);
void   buddy_activate_pull_map(BuddyZone* zone);
void   buddy_reserve_range(BuddyZone* zone, uintptr_t start, uintptr_t end);
void   buddy_free_range(BuddyZone* zone, uintptr_t start, uintptr_t end);

#endif