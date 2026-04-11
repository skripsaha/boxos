#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"

static SlabAllocator g_slab;

static const size_t SLAB_SIZES[SLAB_NUM_CLASSES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

// Find size class index for a given size. Returns -1 if too large.
static int slab_size_class(size_t size) {
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        if (size <= SLAB_SIZES[i]) return i;
    }
    return -1;
}

// Initialize a slab page: place header at start, build free list with uint16_t offsets
static void slab_init_page(SlabPage* page, size_t obj_size) {
    uint16_t header_size = (uint16_t)ALIGN_UP(sizeof(SlabPage), 16);
    uint16_t slots = (SLAB_PAGE_SIZE - header_size) / (uint16_t)obj_size;

    page->obj_size = (uint16_t)obj_size;
    page->total_slots = slots;
    page->free_count = slots;
    page->next = NULL;

    // Build intrusive free list using uint16_t offsets from page start.
    // Each free slot stores the offset of the next free slot at its start.
    uint16_t first_offset = header_size;
    page->free_head = first_offset;

    uint8_t* base = (uint8_t*)page;
    for (uint16_t i = 0; i < slots; i++) {
        uint16_t slot_offset = header_size + (uint16_t)(i * obj_size);
        uint16_t next_offset = (i + 1 < slots)
            ? (uint16_t)(header_size + (i + 1) * obj_size)
            : SLAB_FREE_END;
        *(uint16_t*)(base + slot_offset) = next_offset;
    }
}

// Allocate a new slab page from PMM
static SlabPage* slab_alloc_page(SlabClass* cls) {
    void* raw = pmm_alloc(1);
    if (!raw) return NULL;
    uintptr_t phys = (uintptr_t)raw;

    // Get virtual address (Pull Map if active, identity if early boot)
    SlabPage* page = (SlabPage*)vmm_phys_to_virt(phys);
    memset(page, 0, SLAB_PAGE_SIZE);

    page->page_phys = phys;
    slab_init_page(page, cls->obj_size);

    return page;
}

void slab_init(void) {
    memset(&g_slab, 0, sizeof(g_slab));

    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        SlabClass* cls = &g_slab.classes[i];
        cls->obj_size = SLAB_SIZES[i];
        uint16_t header_size = (uint16_t)ALIGN_UP(sizeof(SlabPage), 16);
        cls->slots_per_page = (SLAB_PAGE_SIZE - header_size) / (uint16_t)SLAB_SIZES[i];
        cls->partial = NULL;
        cls->full = NULL;
        spinlock_init(&cls->lock);
    }

    g_slab.initialized = true;
    debug_printf("[SLAB] Initialized %d size classes:", SLAB_NUM_CLASSES);
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        debug_printf(" %zu(%u/pg)", SLAB_SIZES[i], g_slab.classes[i].slots_per_page);
    }
    debug_printf("\n");
}

void* slab_alloc(size_t size) {
    if (!g_slab.initialized || size == 0) return NULL;

    int idx = slab_size_class(size);
    if (idx < 0) return NULL;  // too large for slab

    SlabClass* cls = &g_slab.classes[idx];
    spin_lock(&cls->lock);

    // Get a partial page (or allocate new one)
    SlabPage* page = cls->partial;
    if (!page) {
        page = slab_alloc_page(cls);
        if (!page) {
            spin_unlock(&cls->lock);
            return NULL;
        }
        cls->partial = page;
    }

    // Pop a free slot — O(1)
    uint16_t slot_offset = page->free_head;
    uint8_t* base = (uint8_t*)page;
    uint16_t next_offset = *(uint16_t*)(base + slot_offset);
    page->free_head = next_offset;
    page->free_count--;

    // If page is now full, move to full list
    if (page->free_count == 0) {
        cls->partial = page->next;
        page->next = cls->full;
        cls->full = page;
    }

    spin_unlock(&cls->lock);

    void* ptr = base + slot_offset;
    memset(ptr, 0, cls->obj_size);
    return ptr;
}

void slab_free(void* ptr) {
    if (!ptr || !g_slab.initialized) return;

    // Find the SlabPage header at the start of the 4KB page
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t page_base = addr & ~(uintptr_t)(SLAB_PAGE_SIZE - 1);
    SlabPage* page = (SlabPage*)page_base;

    // Validate this looks like a slab page
    if (page->obj_size == 0 || page->obj_size > SLAB_LARGE_THRESHOLD) return;

    int idx = slab_size_class(page->obj_size);
    if (idx < 0) return;

    SlabClass* cls = &g_slab.classes[idx];
    spin_lock(&cls->lock);

    bool was_full = (page->free_count == 0);

    // Push slot back onto free list — O(1)
    uint16_t slot_offset = (uint16_t)(addr - page_base);
    uint8_t* base = (uint8_t*)page;
    *(uint16_t*)(base + slot_offset) = page->free_head;
    page->free_head = slot_offset;
    page->free_count++;

    // If page was full, move it back to partial list
    if (was_full) {
        // Remove from full list
        SlabPage** prev = &cls->full;
        while (*prev && *prev != page) prev = &(*prev)->next;
        if (*prev == page) *prev = page->next;

        // Add to partial list
        page->next = cls->partial;
        cls->partial = page;
    }

    // If page is completely free, optionally return to PMM
    if (page->free_count == page->total_slots && cls->partial != page) {
        // Only return if there's another partial page (keep at least one cached)
        SlabPage** prev = &cls->partial;
        while (*prev && *prev != page) prev = &(*prev)->next;
        if (*prev == page) {
            *prev = page->next;
            pmm_free((void*)page->page_phys, 1);
        }
    }

    spin_unlock(&cls->lock);
}

bool slab_owns(void* ptr) {
    if (!ptr || !g_slab.initialized) return false;

    // Check if the page header looks like a valid slab page
    uintptr_t page_base = (uintptr_t)ptr & ~(uintptr_t)(SLAB_PAGE_SIZE - 1);
    SlabPage* page = (SlabPage*)page_base;

    // Quick sanity: obj_size must be one of our known sizes
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        if (page->obj_size == SLAB_SIZES[i]) return true;
    }
    return false;
}

void slab_activate_pull_map(void) {
    // After Pull Map activation, all slab page virtual addresses need rebasing.
    // Identity mapping is GONE — can't dereference identity pointers anymore.
    // Since identity: virt == phys, use vmm_phys_to_virt(identity_addr) to access.

    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        SlabClass* cls = &g_slab.classes[i];

        // Rebase partial list
        uintptr_t page_phys = (uintptr_t)cls->partial;  // identity addr = phys addr
        SlabPage* new_head = NULL;
        SlabPage* new_tail = NULL;

        while (page_phys) {
            // Access the page via Pull Map (identity is gone)
            SlabPage* rebased = (SlabPage*)vmm_phys_to_virt(page_phys);

            // Save next identity pointer before we modify it
            uintptr_t next_phys = (uintptr_t)rebased->next;

            rebased->next = NULL;

            if (!new_head) {
                new_head = rebased;
                new_tail = rebased;
            } else {
                new_tail->next = rebased;
                new_tail = rebased;
            }

            page_phys = next_phys;
        }
        cls->partial = new_head;

        // Rebase full list (same logic)
        page_phys = (uintptr_t)cls->full;
        new_head = NULL;
        new_tail = NULL;

        while (page_phys) {
            SlabPage* rebased = (SlabPage*)vmm_phys_to_virt(page_phys);
            uintptr_t next_phys = (uintptr_t)rebased->next;

            rebased->next = NULL;

            if (!new_head) {
                new_head = rebased;
                new_tail = rebased;
            } else {
                new_tail->next = rebased;
                new_tail = rebased;
            }

            page_phys = next_phys;
        }
        cls->full = new_head;
    }
}
