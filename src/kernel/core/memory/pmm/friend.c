#include "friend.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "kernel_config.h"

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

static void zone_init(PhysZone* zone, PhysTag tag, uintptr_t start, uintptr_t end) {
    zone->tag          = tag;
    zone->zone_start   = start;
    zone->zone_end     = end;
    zone->cached_pages = 0;
    spinlock_init(&zone->lock);
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        zone->free_lists[o].head  = NULL;
        zone->free_lists[o].count = 0;
    }
}

void FriendInit(void) {
    if (g_friend_initialized) return;

    uintptr_t actual_ram_end = pmm_get_mem_end();

    uintptr_t dma32_end = CONFIG_PHYS_ZONE_DMA32_END;
    uintptr_t user_end  = CONFIG_PHYS_ZONE_USER_END;

    if (actual_ram_end <= dma32_end) {
        // Machine has less than 1GB RAM: DMA32 = all of RAM, USER and HIGH are empty
        dma32_end = actual_ram_end;
        user_end  = actual_ram_end;
    } else if (actual_ram_end <= user_end) {
        // Machine has between 1GB and 4GB RAM: USER = up to actual end, HIGH is empty
        user_end = actual_ram_end;
    }

    zone_init(&g_zones[PHYS_TAG_DMA32], PHYS_TAG_DMA32, 0,         dma32_end);
    zone_init(&g_zones[PHYS_TAG_USER],  PHYS_TAG_USER,  dma32_end, user_end);
    zone_init(&g_zones[PHYS_TAG_HIGH],  PHYS_TAG_HIGH,  user_end,  actual_ram_end);

    g_friend_initialized = true;

    debug_printf("[FRIEND] Initialized: DMA32=[0x%lx-0x%lx) USER=[0x%lx-0x%lx) HIGH=[0x%lx-0x%lx)\n",
                 g_zones[PHYS_TAG_DMA32].zone_start, g_zones[PHYS_TAG_DMA32].zone_end,
                 g_zones[PHYS_TAG_USER].zone_start,  g_zones[PHYS_TAG_USER].zone_end,
                 g_zones[PHYS_TAG_HIGH].zone_start,  g_zones[PHYS_TAG_HIGH].zone_end);
}

PhysTag FriendClassifyAddr(uintptr_t phys_addr) {
    for (int t = 0; t < PHYS_TAG_COUNT - 1; t++) {
        if (phys_addr < g_zones[t].zone_end) return (PhysTag)t;
    }
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

    spin_lock(&zone->lock);

    if (zone->cached_pages >= CONFIG_FRIEND_ZONE_CACHE_MAX_PAGES) {
        spin_unlock(&zone->lock);
        BuddyZone* buddy = pmm_get_buddy_zone();
        buddy_free(buddy, (void*)phys, pages);
        return;
    }

    BuddyFreeNode* node = (BuddyFreeNode*)vmm_phys_to_virt(phys);
    node->order = (uint8_t)order;
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
