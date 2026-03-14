#include "buddy.h"
#include "vmm.h"
#include "klib.h"

// --- Helpers ---

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

// Mark a range of pages in alloc_map
static void alloc_map_mark_range(BuddyZone* zone, size_t start_idx, size_t count, bool allocated) {
    for (size_t i = 0; i < count; i++) {
        if (allocated)
            alloc_map_set(zone, start_idx + i);
        else
            alloc_map_clear(zone, start_idx + i);
    }
}

// Get a virtual pointer to access a physical page (works before and after DPM)
static inline BuddyFreeNode* phys_to_node(uintptr_t phys) {
    return (BuddyFreeNode*)vmm_phys_to_virt(phys);
}

// Get physical address from a node pointer
static inline uintptr_t node_to_phys(BuddyFreeNode* node) {
    return vmm_virt_to_phys_direct((void*)node);
}

// --- Free list operations (doubly-linked, intrusive) ---

static void buddy_list_insert(BuddyFreeList* list, BuddyFreeNode* node, int order) {
    node->order = (uint8_t)order;
    node->prev = NULL;
    node->next = list->head;
    if (list->head) {
        list->head->prev = node;
    }
    list->head = node;
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
    }
    node->prev = NULL;
    node->next = NULL;
    list->count--;
}

// --- Buddy core ---

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

    // Initialize all free lists to empty
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        zone->free_lists[o].head = NULL;
        zone->free_lists[o].count = 0;
    }

    spinlock_init(&zone->lock);

    // Mark all pages as allocated initially
    memset(alloc_map, 0xFF, alloc_map_size);

    zone->initialized = true;
}

// Add a range of pages to the buddy free lists.
// Decomposes [start, end) into naturally-aligned power-of-2 blocks.
void buddy_free_range(BuddyZone* zone, uintptr_t start, uintptr_t end) {
    // Align to page boundaries within zone
    if (start < zone->base) start = zone->base;
    uintptr_t zone_end = zone->base + zone->total_pages * BUDDY_PAGE_SIZE;
    if (end > zone_end) end = zone_end;
    if (start >= end) return;

    // Page-align
    start = ALIGN_UP(start, BUDDY_PAGE_SIZE);
    end = ALIGN_DOWN(end, BUDDY_PAGE_SIZE);
    if (start >= end) return;

    uintptr_t addr = start;
    while (addr < end) {
        // Find max order: block must be naturally aligned AND fit within [addr, end)
        int order = 0;
        while (order < BUDDY_MAX_ORDER) {
            size_t next_pages = order_to_pages(order + 1);
            uintptr_t offset = addr - zone->base;
            // Check alignment for next order
            if (offset % (next_pages * BUDDY_PAGE_SIZE) != 0) break;
            // Check fit
            if (addr + next_pages * BUDDY_PAGE_SIZE > end) break;
            order++;
        }

        size_t block_pages = order_to_pages(order);
        size_t idx = page_index(zone, addr);

        // Mark pages free in alloc_map
        alloc_map_mark_range(zone, idx, block_pages, false);
        zone->free_count += block_pages;

        // Insert into free list
        BuddyFreeNode* node = phys_to_node(addr);
        buddy_list_insert(&zone->free_lists[order], node, order);

        addr += block_pages * BUDDY_PAGE_SIZE;
    }
}

// Reserve a range by marking pages as allocated and removing from free lists.
// This works by allocating individual pages that fall within the range.
void buddy_reserve_range(BuddyZone* zone, uintptr_t start, uintptr_t end) {
    start = ALIGN_DOWN(start, BUDDY_PAGE_SIZE);
    end = ALIGN_UP(end, BUDDY_PAGE_SIZE);

    if (start < zone->base) start = zone->base;
    uintptr_t zone_end = zone->base + zone->total_pages * BUDDY_PAGE_SIZE;
    if (end > zone_end) end = zone_end;
    if (start >= end) return;

    // For each page in the range, if it's free, we need to split blocks
    // down to order 0 and mark the page as allocated.
    // Simpler approach: walk all free lists and remove any block that overlaps.
    // Then re-add the non-overlapping portions.

    for (int o = BUDDY_MAX_ORDER; o >= 0; o--) {
        BuddyFreeNode* node = zone->free_lists[o].head;
        while (node) {
            uintptr_t block_phys = node_to_phys(node);
            size_t block_pages = order_to_pages(o);
            uintptr_t block_end = block_phys + block_pages * BUDDY_PAGE_SIZE;
            BuddyFreeNode* next = node->next;

            // Check if block overlaps with reserved range
            if (block_phys < end && block_end > start) {
                // Remove from free list
                buddy_list_remove(&zone->free_lists[o], node);
                zone->free_count -= block_pages;
                alloc_map_mark_range(zone, page_index(zone, block_phys), block_pages, true);

                // Re-add non-overlapping portions
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

    // Find smallest order with a free block
    int o;
    for (o = order; o <= BUDDY_MAX_ORDER; o++) {
        if (zone->free_lists[o].head != NULL) break;
    }

    if (o > BUDDY_MAX_ORDER) {
        spin_unlock(&zone->lock);
        return NULL;
    }

    // Pop block from free list
    BuddyFreeNode* block_node = zone->free_lists[o].head;
    uintptr_t block_phys = node_to_phys(block_node);
    buddy_list_remove(&zone->free_lists[o], block_node);

    // Split down to target order
    while (o > order) {
        o--;
        uintptr_t buddy_phys = block_phys + order_to_pages(o) * BUDDY_PAGE_SIZE;

        // Insert upper half (buddy) into free list at order o
        BuddyFreeNode* buddy_node = phys_to_node(buddy_phys);
        buddy_list_insert(&zone->free_lists[o], buddy_node, o);
    }

    // Mark allocated in alloc_map
    size_t block_pages = order_to_pages(order);
    size_t idx = page_index(zone, block_phys);
    alloc_map_mark_range(zone, idx, block_pages, true);
    zone->free_count -= block_pages;

    spin_unlock(&zone->lock);

    return (void*)block_phys;
}

void buddy_free(BuddyZone* zone, void* addr, size_t pages) {
    if (!addr || !pages || !zone->initialized) return;

    uintptr_t phys = (uintptr_t)addr;
    int order = pages_to_order(pages);

    if (order > BUDDY_MAX_ORDER) {
        debug_printf("[BUDDY] ERROR: free order %d exceeds MAX_ORDER %d\n", order, BUDDY_MAX_ORDER);
        return;
    }

    size_t idx = page_index(zone, phys);
    size_t block_pages = order_to_pages(order);

    spin_lock(&zone->lock);

    // Double-free check
    if (!alloc_map_test(zone, idx)) {
        spin_unlock(&zone->lock);
        panic("[BUDDY] Double free at phys=0x%lx pages=%lu order=%d", phys, pages, order);
    }

    // Mark pages free
    alloc_map_mark_range(zone, idx, block_pages, false);
    zone->free_count += block_pages;

    // Coalesce with buddy
    uintptr_t block = phys;
    while (order < BUDDY_MAX_ORDER) {
        uintptr_t buddy_phys = buddy_address(zone, block, order);

        // Buddy must be within zone
        if (buddy_phys < zone->base ||
            buddy_phys + order_to_pages(order) * BUDDY_PAGE_SIZE >
            zone->base + zone->total_pages * BUDDY_PAGE_SIZE) {
            break;
        }

        size_t buddy_idx = page_index(zone, buddy_phys);

        // Buddy's first page must be free
        if (alloc_map_test(zone, buddy_idx)) break;

        // Buddy must be free at the SAME order (check the order field)
        BuddyFreeNode* buddy_node = phys_to_node(buddy_phys);
        if (buddy_node->order != (uint8_t)order) break;

        // Remove buddy from its free list
        buddy_list_remove(&zone->free_lists[order], buddy_node);

        // Merged block starts at min(block, buddy)
        if (buddy_phys < block) block = buddy_phys;
        order++;
    }

    // Insert coalesced block
    BuddyFreeNode* node = phys_to_node(block);
    buddy_list_insert(&zone->free_lists[order], node, order);

    spin_unlock(&zone->lock);
}

void buddy_activate_pull_map(BuddyZone* zone) {
    // Rebase alloc_map pointer
    zone->alloc_map = (uint8_t*)vmm_phys_to_virt(zone->alloc_map_phys);

    // Rebase all free list pointers from identity to DPM
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        BuddyFreeList* list = &zone->free_lists[o];
        if (!list->head) continue;

        // Rebase head pointer (currently identity = physical)
        uintptr_t head_phys = (uintptr_t)list->head;
        list->head = (BuddyFreeNode*)vmm_phys_to_virt(head_phys);

        // Walk and rebase all node pointers
        BuddyFreeNode* node = list->head;
        while (node) {
            if (node->next) {
                uintptr_t next_phys = (uintptr_t)node->next;
                node->next = (BuddyFreeNode*)vmm_phys_to_virt(next_phys);
            }
            if (node->prev) {
                uintptr_t prev_phys = (uintptr_t)node->prev;
                node->prev = (BuddyFreeNode*)vmm_phys_to_virt(prev_phys);
            }
            node = node->next;
        }
    }

    debug_printf("[BUDDY] Pull Map activated: alloc_map rebased to %p\n", zone->alloc_map);
}
