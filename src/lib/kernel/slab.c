#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"

static SlabAllocator g_slab;

static uint8_t  *g_slab_page_bits;
static uintptr_t g_slab_page_bits_phys;
static size_t    g_slab_page_count;

static inline void slab_bit_set(uintptr_t phys)
{
    size_t idx = phys / SLAB_PAGE_SIZE;
    if (idx >= g_slab_page_count) return;
    __atomic_fetch_or(&g_slab_page_bits[idx >> 3],
                      (uint8_t)(1u << (idx & 7u)), __ATOMIC_RELEASE);
}

static inline void slab_bit_clear(uintptr_t phys)
{
    size_t idx = phys / SLAB_PAGE_SIZE;
    if (idx >= g_slab_page_count) return;
    __atomic_fetch_and(&g_slab_page_bits[idx >> 3],
                       (uint8_t)~(1u << (idx & 7u)), __ATOMIC_RELEASE);
}

static inline bool slab_bit_test(uintptr_t phys)
{
    size_t idx = phys / SLAB_PAGE_SIZE;
    if (idx >= g_slab_page_count) return false;
    return (__atomic_load_n(&g_slab_page_bits[idx >> 3], __ATOMIC_ACQUIRE)
            >> (idx & 7u)) & 1u;
}

static const size_t SLAB_SIZES[SLAB_NUM_CLASSES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

static int slab_size_class(size_t size) {
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        if (size <= SLAB_SIZES[i]) return i;
    }
    return -1;
}

static void slab_init_page(SlabPage* page, size_t obj_size) {
    uint16_t header_size = (uint16_t)ALIGN_UP(sizeof(SlabPage), 16);
    uint16_t slots = (SLAB_PAGE_SIZE - header_size) / (uint16_t)obj_size;

    page->obj_size = (uint16_t)obj_size;
    page->total_slots = slots;
    page->free_count = slots;
    page->next = NULL;

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

static SlabPage* slab_alloc_page(SlabClass* cls) {
    void* raw = pmm_alloc(1);
    if (!raw) return NULL;
    uintptr_t phys = (uintptr_t)raw;

    SlabPage* page = (SlabPage*)vmm_phys_to_virt(phys);
    memset(page, 0, SLAB_PAGE_SIZE);

    page->page_phys = phys;
    slab_init_page(page, cls->obj_size);
    slab_bit_set(phys);

    return page;
}

void slab_init(void) {
    memset(&g_slab, 0, sizeof(g_slab));

    g_slab_page_count = (size_t)(pmm_get_mem_end() / SLAB_PAGE_SIZE) + 1;
    size_t bits_bytes = (g_slab_page_count + 7) / 8;
    size_t bits_pages = (bits_bytes + SLAB_PAGE_SIZE - 1) / SLAB_PAGE_SIZE;
    void *bits_phys = pmm_alloc_zero(bits_pages);
    if (!bits_phys)
        panic("[SLAB] cannot allocate the page registry (%zu pages)", bits_pages);
    g_slab_page_bits_phys = (uintptr_t)bits_phys;
    g_slab_page_bits      = (uint8_t *)vmm_phys_to_virt(g_slab_page_bits_phys);

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
    if (idx < 0) return NULL;

    SlabClass* cls = &g_slab.classes[idx];
    spin_lock(&cls->lock);

    SlabPage* page = cls->partial;
    if (!page) {
        page = slab_alloc_page(cls);
        if (!page) {
            spin_unlock(&cls->lock);
            return NULL;
        }
        cls->partial = page;
    }

    uint16_t slot_offset = page->free_head;
    uint8_t* base = (uint8_t*)page;

    {
        uint16_t header_size = (uint16_t)ALIGN_UP(sizeof(SlabPage), 16);
        if (slot_offset == SLAB_FREE_END ||
            slot_offset < header_size ||
            (uint32_t)slot_offset + page->obj_size > SLAB_PAGE_SIZE ||
            ((uint32_t)(slot_offset - header_size) % page->obj_size) != 0 ||
            page->obj_size != cls->obj_size ||
            page->free_count == 0 || page->free_count > page->total_slots) {
            panic("[SLAB] class %zu free-list corrupt: page=%p obj_size=%u "
                  "free_head=%u free_count=%u total=%u — a foreign page was "
                  "recruited into this class or its header was overwritten\n",
                  cls->obj_size, (void *)page, page->obj_size,
                  slot_offset, page->free_count, page->total_slots);
        }
    }

    uint16_t next_offset = *(uint16_t*)(base + slot_offset);
    page->free_head = next_offset;
    page->free_count--;

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

    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t page_base = addr & ~(uintptr_t)(SLAB_PAGE_SIZE - 1);
    SlabPage* page = (SlabPage*)page_base;

    if (page->obj_size == 0 || page->obj_size > SLAB_LARGE_THRESHOLD) return;

    int idx = slab_size_class(page->obj_size);
    if (idx < 0) return;

    {
        uint16_t header_size = (uint16_t)ALIGN_UP(sizeof(SlabPage), 16);
        uint16_t off = (uint16_t)(addr - page_base);
        if (off < header_size ||
            ((uint32_t)(off - header_size) % page->obj_size) != 0 ||
            (uint32_t)off + page->obj_size > SLAB_PAGE_SIZE) {
            panic("[SLAB] free of non-slab pointer %p: off-grid for page %p "
                  "(obj_size=%u header=%u free=%u/%u bit=%d) caller=%p — "
                  "double free across a page reincarnation, or a foreign "
                  "pointer routed here\n",
                  ptr, (void *)page, page->obj_size, header_size,
                  page->free_count, page->total_slots,
                  (int)slab_bit_test(vmm_virt_to_phys_direct(ptr) & ~(uintptr_t)(SLAB_PAGE_SIZE-1)),
                  __builtin_return_address(0));
        }
    }

    SlabClass* cls = &g_slab.classes[idx];
    spin_lock(&cls->lock);

    bool was_full = (page->free_count == 0);

    if (page->free_count >= page->total_slots ||
        page->total_slots == 0 || page->obj_size != cls->obj_size) {
        spin_unlock(&cls->lock);
        panic("[SLAB] free of %p rejects its page %p: obj_size=%u "
              "free_count=%u total=%u — impostor page or double free\n",
              ptr, (void *)page, page->obj_size,
              page->free_count, page->total_slots);
    }

    uint16_t slot_offset = (uint16_t)(addr - page_base);
    uint8_t* base = (uint8_t*)page;
    *(uint16_t*)(base + slot_offset) = page->free_head;
    page->free_head = slot_offset;
    page->free_count++;

    if (was_full) {
        SlabPage** prev = &cls->full;
        while (*prev && *prev != page) prev = &(*prev)->next;
        if (*prev == page) *prev = page->next;

        page->next = cls->partial;
        cls->partial = page;
    }

    if (page->free_count == page->total_slots && cls->partial != page) {
        SlabPage** prev = &cls->partial;
        while (*prev && *prev != page) prev = &(*prev)->next;
        if (*prev == page) {
            *prev = page->next;
            slab_bit_clear(page->page_phys);
            pmm_free((void*)page->page_phys, 1);
        }
    }

    spin_unlock(&cls->lock);
}

bool slab_owns(void* ptr) {
    if (!ptr || !g_slab.initialized) return false;

    uintptr_t phys = vmm_virt_to_phys_direct(ptr);
    return slab_bit_test(phys & ~(uintptr_t)(SLAB_PAGE_SIZE - 1));
}

void slab_activate_pull_map(void) {

    g_slab_page_bits = (uint8_t *)vmm_phys_to_virt(g_slab_page_bits_phys);

    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        SlabClass* cls = &g_slab.classes[i];

        uintptr_t page_phys = (uintptr_t)cls->partial;
        SlabPage* new_head = NULL;
        SlabPage* new_tail = NULL;

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
        cls->partial = new_head;

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

void slab_identity_selftest(void) {
    uint8_t *buf = (uint8_t *)kmalloc(3 * SLAB_PAGE_SIZE);
    if (!buf) {
        panic("[SLAB] identity self-test: kmalloc(3 pages) failed at boot");
    }
    uint8_t *interior = (uint8_t *)(((uintptr_t)buf + SLAB_PAGE_SIZE) &
                                    ~(uintptr_t)(SLAB_PAGE_SIZE - 1));
    SlabPage *impostor = (SlabPage *)interior;
    uint16_t saved = impostor->obj_size;
    impostor->obj_size = 64;

    bool fooled = slab_owns(interior + 64);

    impostor->obj_size = saved;
    if (fooled) {
        panic("[SLAB] identity self-test FAILED: page content forged slab "
              "membership — ownership must come from the page registry, "
              "never from what a page happens to contain");
    }
    kfree(buf);
    kprintf("[SLAB] identity self-test passed — membership is the registry's answer\n");
}