#include "buddy.h"
#include "vmm.h"
#include "klib.h"


static inline int pages_to_order(size_t pages) {
    int order = 0;
    size_t n = 1;
    while (n < pages) { n <<= 1; order++; }
    return order;
}

static inline size_t order_to_pages(int order) {
    return (size_t)1 << order;
}

static inline size_t page_index(BuddyZone* zone, uintptr_t phys) {
    return (phys - zone->base) / BUDDY_PAGE_SIZE;
}

static inline uintptr_t index_to_phys(BuddyZone* zone, size_t idx) {
    return zone->base + idx * BUDDY_PAGE_SIZE;
}

static inline void alloc_map_set(BuddyZone* zone, size_t idx) {
    zone->alloc_map[idx / 8] |= (1 << (idx % 8));
}

static inline void alloc_map_clear(BuddyZone* zone, size_t idx) {
    zone->alloc_map[idx / 8] &= ~(1 << (idx % 8));
}

static inline bool alloc_map_test(BuddyZone* zone, size_t idx) {
    return (zone->alloc_map[idx / 8] & (1 << (idx % 8))) != 0;
}

static void alloc_map_mark_range(BuddyZone* zone, size_t start_idx, size_t count, bool allocated) {
    for (size_t i = 0; i < count; i++) {
        if (allocated)
            alloc_map_set(zone, start_idx + i);
        else
            alloc_map_clear(zone, start_idx + i);
    }
}

static inline BuddyFreeNode* phys_to_node(uintptr_t phys) {
    return (BuddyFreeNode*)vmm_phys_to_virt(phys);
}

static inline uintptr_t node_to_phys(BuddyFreeNode* node) {
    return vmm_virt_to_phys_direct((void*)node);
}


static void buddy_list_insert(BuddyFreeList* list, BuddyFreeNode* node, int order) {
    node->order = (uint8_t)order;
    node->next = NULL;
    node->prev = list->tail;
    if (list->tail) {
        list->tail->next = node;
    } else {
        list->head = node;
    }
    list->tail = node;
    list->count++;
}

static void buddy_list_remove(BuddyFreeList* list, BuddyFreeNode* node) {
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        list->head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        list->tail = node->prev;
    }
    node->prev = NULL;
    node->next = NULL;
    list->count--;
}


static uintptr_t buddy_address(BuddyZone* zone, uintptr_t block_phys, int order) {
    uintptr_t offset = block_phys - zone->base;
    uintptr_t buddy_offset = offset ^ (order_to_pages(order) * BUDDY_PAGE_SIZE);
    return zone->base + buddy_offset;
}

void buddy_init(BuddyZone* zone, uintptr_t base, size_t total_pages,
                uint8_t* alloc_map, uintptr_t alloc_map_phys, size_t alloc_map_size) {
    zone->base = base;
    zone->total_pages = total_pages;
    zone->alloc_map = alloc_map;
    zone->alloc_map_phys = alloc_map_phys;
    zone->alloc_map_size = alloc_map_size;
    zone->free_count = 0;
    zone->initialized = false;

    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        zone->free_lists[o].head = NULL;
        zone->free_lists[o].tail = NULL;
        zone->free_lists[o].count = 0;
    }

    spinlock_init(&zone->lock);

    memset(alloc_map, 0xFF, alloc_map_size);

    zone->initialized = true;
}

void buddy_free_range(BuddyZone* zone, uintptr_t start, uintptr_t end) {
    if (start < zone->base) start = zone->base;
    uintptr_t zone_end = zone->base + zone->total_pages * BUDDY_PAGE_SIZE;
    if (end > zone_end) end = zone_end;
    if (start >= end) return;

    start = ALIGN_UP(start, BUDDY_PAGE_SIZE);
    end = ALIGN_DOWN(end, BUDDY_PAGE_SIZE);
    if (start >= end) return;

    uintptr_t addr = start;
    while (addr < end) {
        int order = 0;
        while (order < BUDDY_MAX_ORDER) {
            size_t next_pages = order_to_pages(order + 1);
            uintptr_t offset = addr - zone->base;
            if (offset % (next_pages * BUDDY_PAGE_SIZE) != 0) break;
            if (addr + next_pages * BUDDY_PAGE_SIZE > end) break;
            order++;
        }

        size_t block_pages = order_to_pages(order);
        size_t idx = page_index(zone, addr);

        alloc_map_mark_range(zone, idx, block_pages, false);
        zone->free_count += block_pages;

        BuddyFreeNode* node = phys_to_node(addr);
        buddy_list_insert(&zone->free_lists[order], node, order);

        addr += block_pages * BUDDY_PAGE_SIZE;
    }
}

void buddy_reserve_range(BuddyZone* zone, uintptr_t start, uintptr_t end) {
    start = ALIGN_DOWN(start, BUDDY_PAGE_SIZE);
    end = ALIGN_UP(end, BUDDY_PAGE_SIZE);

    if (start < zone->base) start = zone->base;
    uintptr_t zone_end = zone->base + zone->total_pages * BUDDY_PAGE_SIZE;
    if (end > zone_end) end = zone_end;
    if (start >= end) return;


    for (int o = BUDDY_MAX_ORDER; o >= 0; o--) {
        BuddyFreeNode* node = zone->free_lists[o].head;
        while (node) {
            uintptr_t block_phys = node_to_phys(node);
            size_t block_pages = order_to_pages(o);
            uintptr_t block_end = block_phys + block_pages * BUDDY_PAGE_SIZE;
            BuddyFreeNode* next = node->next;

            if (block_phys < end && block_end > start) {
                buddy_list_remove(&zone->free_lists[o], node);
                zone->free_count -= block_pages;
                alloc_map_mark_range(zone, page_index(zone, block_phys), block_pages, true);

                if (block_phys < start) {
                    buddy_free_range(zone, block_phys, start);
                }
                if (block_end > end) {
                    buddy_free_range(zone, end, block_end);
                }
            }

            node = next;
        }
    }
}

void* buddy_alloc(BuddyZone* zone, size_t pages) {
    if (!pages || !zone->initialized) return NULL;

    int order = pages_to_order(pages);
    if (order > BUDDY_MAX_ORDER) return NULL;

    spin_lock(&zone->lock);

    int o;
    for (o = order; o <= BUDDY_MAX_ORDER; o++) {
        if (zone->free_lists[o].head != NULL) break;
    }

    if (o > BUDDY_MAX_ORDER) {
        spin_unlock(&zone->lock);
        return NULL;
    }

    BuddyFreeNode* block_node = zone->free_lists[o].head;
    uintptr_t block_phys = node_to_phys(block_node);
    buddy_list_remove(&zone->free_lists[o], block_node);

    while (o > order) {
        o--;
        uintptr_t buddy_phys = block_phys + order_to_pages(o) * BUDDY_PAGE_SIZE;

        BuddyFreeNode* buddy_node = phys_to_node(buddy_phys);
        buddy_list_insert(&zone->free_lists[o], buddy_node, o);
    }

    size_t block_pages = order_to_pages(order);
    size_t idx = page_index(zone, block_phys);
    alloc_map_mark_range(zone, idx, block_pages, true);
    zone->free_count -= block_pages;

    spin_unlock(&zone->lock);

    return (void*)block_phys;
}

void* buddy_alloc_range(BuddyZone* zone, size_t pages, uintptr_t min_phys, uintptr_t max_phys) {
    if (!pages || !zone->initialized) return NULL;

    int order = pages_to_order(pages);
    if (order > BUDDY_MAX_ORDER) return NULL;

    uintptr_t zone_end = zone->base + zone->total_pages * BUDDY_PAGE_SIZE;
    if (min_phys < zone->base) min_phys = zone->base;
    if (max_phys > zone_end)   max_phys = zone_end;
    if (min_phys >= max_phys)  return NULL;

    spin_lock(&zone->lock);

    for (int o = order; o <= BUDDY_MAX_ORDER; o++) {
        BuddyFreeList* list = &zone->free_lists[o];
        BuddyFreeNode* node = list->head;
        while (node) {
            uintptr_t block_phys = node_to_phys(node);
            uintptr_t block_end  = block_phys + ((uintptr_t)order_to_pages(o) * BUDDY_PAGE_SIZE);

            if (block_phys >= max_phys || block_end <= min_phys) {
                node = node->next;
                continue;
            }

            if (block_phys >= min_phys && block_end <= max_phys) {
                buddy_list_remove(list, node);

                int cur = o;
                while (cur > order) {
                    cur--;
                    uintptr_t split_buddy = block_phys + (uintptr_t)order_to_pages(cur) * BUDDY_PAGE_SIZE;
                    BuddyFreeNode* split_node = phys_to_node(split_buddy);
                    buddy_list_insert(&zone->free_lists[cur], split_node, cur);
                }

                size_t block_pages = order_to_pages(order);
                size_t idx = page_index(zone, block_phys);
                alloc_map_mark_range(zone, idx, block_pages, true);
                zone->free_count -= block_pages;

                spin_unlock(&zone->lock);
                return (void*)block_phys;
            }
            node = node->next;
        }
    }

    spin_unlock(&zone->lock);
    return NULL;
}

void buddy_free(BuddyZone* zone, void* addr, size_t pages) {
    if (!addr || !pages || !zone->initialized) return;

    uintptr_t phys = (uintptr_t)addr;
    int order = pages_to_order(pages);

    if (order > BUDDY_MAX_ORDER) {
        debug_printf("[BUDDY] ERROR: free order %d exceeds MAX_ORDER %d\n", order, BUDDY_MAX_ORDER);
        return;
    }

    uintptr_t zone_end = zone->base + zone->total_pages * BUDDY_PAGE_SIZE;
    if (phys < zone->base || phys >= zone_end) {
        debug_printf("[BUDDY] ERROR: free address 0x%lx out of zone [0x%lx, 0x%lx)\n",
                     phys, zone->base, zone_end);
        return;
    }

    size_t idx = page_index(zone, phys);
    size_t block_pages = order_to_pages(order);

    spin_lock(&zone->lock);

    for (size_t i = 0; i < block_pages; i++) {
        if (!alloc_map_test(zone, idx + i)) {
            panic("[BUDDY] Double free at phys=0x%lx pages=%lu order=%d (page +%lu already free)",
                  phys, pages, order, (unsigned long)i);
        }
    }

    alloc_map_mark_range(zone, idx, block_pages, false);
    zone->free_count += block_pages;

    uintptr_t block = phys;
    while (order < BUDDY_MAX_ORDER) {
        uintptr_t buddy_phys = buddy_address(zone, block, order);

        if (buddy_phys < zone->base ||
            buddy_phys + order_to_pages(order) * BUDDY_PAGE_SIZE >
            zone->base + zone->total_pages * BUDDY_PAGE_SIZE) {
            break;
        }

        size_t buddy_idx = page_index(zone, buddy_phys);

        if (alloc_map_test(zone, buddy_idx)) break;

        BuddyFreeNode* buddy_node = phys_to_node(buddy_phys);
        if (buddy_node->order != (uint8_t)order) break;

        buddy_list_remove(&zone->free_lists[order], buddy_node);

        if (buddy_phys < block) block = buddy_phys;
        order++;
    }

    BuddyFreeNode* node = phys_to_node(block);
    buddy_list_insert(&zone->free_lists[order], node, order);

    spin_unlock(&zone->lock);
}

void buddy_activate_pull_map(BuddyZone* zone) {
    zone->alloc_map = (uint8_t*)vmm_phys_to_virt(zone->alloc_map_phys);

    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        BuddyFreeList* list = &zone->free_lists[o];
        if (!list->head) continue;

        uintptr_t head_phys = (uintptr_t)list->head;
        list->head = (BuddyFreeNode*)vmm_phys_to_virt(head_phys);

        BuddyFreeNode* node = list->head;
        BuddyFreeNode* last = NULL;
        while (node) {
            if (node->next) {
                uintptr_t next_phys = (uintptr_t)node->next;
                node->next = (BuddyFreeNode*)vmm_phys_to_virt(next_phys);
            }
            if (node->prev) {
                uintptr_t prev_phys = (uintptr_t)node->prev;
                node->prev = (BuddyFreeNode*)vmm_phys_to_virt(prev_phys);
            }
            last = node;
            node = node->next;
        }
        list->tail = last;
    }

    debug_printf("[BUDDY] Pull Map activated: alloc_map rebased to %p\n", zone->alloc_map);
}