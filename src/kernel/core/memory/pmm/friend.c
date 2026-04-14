#include "friend.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"

// Zone boundary constants
#define FRIEND_DMA32_START  0x0000000000000000ULL
#define FRIEND_DMA32_END    0x0000000040000000ULL   // 1 GB
#define FRIEND_USER_START   0x0000000040000000ULL   // 1 GB
#define FRIEND_USER_END     0x0000000100000000ULL   // 4 GB
#define FRIEND_HIGH_START   0x0000000100000000ULL   // 4 GB
// HIGH end is set dynamically from buddy zone maximum

static PhysZone g_zones[PHYS_TAG_COUNT];
static bool     g_friend_initialized = false;

static inline int pages_to_order_friend(size_t pages) {
    int order = 0;
    size_t n = 1;
    while (n < pages) { n <<= 1; order++; }
    return order;
}

static inline size_t order_to_pages_friend(int order) {
    return (size_t)1 << order;
}

void FriendInit(void) {
    if (g_friend_initialized) return;

    BuddyZone* buddy = pmm_get_buddy_zone();

    uintptr_t buddy_end = buddy->base + buddy->total_pages * BUDDY_PAGE_SIZE;

    // DMA32 zone: physical 0 → 1 GB
    g_zones[PHYS_TAG_DMA32].tag        = PHYS_TAG_DMA32;
    g_zones[PHYS_TAG_DMA32].zone_start = FRIEND_DMA32_START;
    g_zones[PHYS_TAG_DMA32].zone_end   = FRIEND_DMA32_END;
    g_zones[PHYS_TAG_DMA32].cached_pages = 0;
    spinlock_init(&g_zones[PHYS_TAG_DMA32].lock);
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        g_zones[PHYS_TAG_DMA32].free_lists[o].head  = NULL;
        g_zones[PHYS_TAG_DMA32].free_lists[o].count = 0;
    }

    // USER zone: physical 1 GB → 4 GB
    g_zones[PHYS_TAG_USER].tag        = PHYS_TAG_USER;
    g_zones[PHYS_TAG_USER].zone_start = FRIEND_USER_START;
    g_zones[PHYS_TAG_USER].zone_end   = FRIEND_USER_END;
    g_zones[PHYS_TAG_USER].cached_pages = 0;
    spinlock_init(&g_zones[PHYS_TAG_USER].lock);
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        g_zones[PHYS_TAG_USER].free_lists[o].head  = NULL;
        g_zones[PHYS_TAG_USER].free_lists[o].count = 0;
    }

    // HIGH zone: physical 4 GB → buddy end
    g_zones[PHYS_TAG_HIGH].tag        = PHYS_TAG_HIGH;
    g_zones[PHYS_TAG_HIGH].zone_start = FRIEND_HIGH_START;
    g_zones[PHYS_TAG_HIGH].zone_end   = (buddy_end > FRIEND_HIGH_START) ? buddy_end : FRIEND_HIGH_START;
    g_zones[PHYS_TAG_HIGH].cached_pages = 0;
    spinlock_init(&g_zones[PHYS_TAG_HIGH].lock);
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        g_zones[PHYS_TAG_HIGH].free_lists[o].head  = NULL;
        g_zones[PHYS_TAG_HIGH].free_lists[o].count = 0;
    }

    g_friend_initialized = true;

    debug_printf("[FRIEND] Initialized: DMA32=[0x%lx-0x%lx) USER=[0x%lx-0x%lx) HIGH=[0x%lx-0x%lx)\n",
                 g_zones[PHYS_TAG_DMA32].zone_start, g_zones[PHYS_TAG_DMA32].zone_end,
                 g_zones[PHYS_TAG_USER].zone_start,  g_zones[PHYS_TAG_USER].zone_end,
                 g_zones[PHYS_TAG_HIGH].zone_start,  g_zones[PHYS_TAG_HIGH].zone_end);
}

PhysTag FriendClassifyAddr(uintptr_t phys_addr) {
    if (phys_addr < FRIEND_DMA32_END) return PHYS_TAG_DMA32;
    if (phys_addr < FRIEND_USER_END)  return PHYS_TAG_USER;
    return PHYS_TAG_HIGH;
}

const char* PhysTagName(PhysTag tag) {
    switch (tag) {
        case PHYS_TAG_DMA32: return "DMA32";
        case PHYS_TAG_USER:  return "USER";
        case PHYS_TAG_HIGH:  return "HIGH";
        default:             return "UNKNOWN";
    }
}

void* FriendAlloc(size_t pages, PhysTag tag) {
    if (!g_friend_initialized || !pages || tag >= PHYS_TAG_COUNT) return NULL;

    int order = pages_to_order_friend(pages);
    if (order > BUDDY_MAX_ORDER) return NULL;

    PhysZone* zone = &g_zones[tag];

    spin_lock(&zone->lock);

    // Check zone cache first: pop from the free list at this order
    if (zone->free_lists[order].head != NULL) {
        BuddyFreeNode* node = zone->free_lists[order].head;
        zone->free_lists[order].head = node->next;
        if (node->next) node->next->prev = NULL;
        zone->free_lists[order].count--;
        size_t block_pages = order_to_pages_friend(order);
        zone->cached_pages -= block_pages;
        spin_unlock(&zone->lock);

        uintptr_t phys = vmm_virt_to_phys_direct((void*)node);
        return (void*)phys;
    }

    spin_unlock(&zone->lock);

    // Cache miss: allocate from the global buddy within zone range
    BuddyZone* buddy = pmm_get_buddy_zone();
    void* phys = buddy_alloc_range(buddy, pages, zone->zone_start, zone->zone_end);

    return phys;
}

void FriendFree(void* addr, size_t pages) {
    if (!addr || !pages || !g_friend_initialized) return;

    uintptr_t phys = (uintptr_t)addr;
    PhysTag tag = FriendClassifyAddr(phys);
    PhysZone* zone = &g_zones[tag];

    int order = pages_to_order_friend(pages);
    if (order > BUDDY_MAX_ORDER) return;

    // Map physical address to a virtual pointer to embed the intrusive node
    BuddyFreeNode* node = (BuddyFreeNode*)vmm_phys_to_virt(phys);
    node->order = (uint8_t)order;

    spin_lock(&zone->lock);

    node->prev = NULL;
    node->next = zone->free_lists[order].head;
    if (zone->free_lists[order].head) {
        zone->free_lists[order].head->prev = node;
    }
    zone->free_lists[order].head = node;
    zone->free_lists[order].count++;
    zone->cached_pages += order_to_pages_friend(order);

    spin_unlock(&zone->lock);
}
