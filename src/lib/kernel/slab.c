#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"

static SlabAllocator g_slab;

/*
 * Slab page registry — one bit per physical page, set while the page belongs
 * to the slab and cleared when it returns to the PMM.
 *
 * This replaces the old membership HEURISTIC: slab_owns() used to read two
 * bytes at (ptr & ~0xFFF) and call the page a slab page whenever they matched
 * one of the eight class sizes. Any page whose CONTENT began with such a
 * word passed — including pages in the middle of multi-page pool blocks
 * holding bytes read from disk. A kfree() of such a block was then routed to
 * slab_free, which recruited the pool's LIVE page into a class free list:
 * the class handed the page out as blocks while its real owner kept using
 * it — double ownership of live memory, surfacing later as destroyed heap
 * canaries and lost wakeups (the 0xffff880004471000 corpse). Ownership is a
 * question only the allocator may answer, and the bitmap is that answer:
 * exact in both directions, O(1), sized by the machine.
 */
static uint8_t  *g_slab_page_bits;
static uintptr_t g_slab_page_bits_phys;
static size_t    g_slab_page_count;      /* bits tracked = phys pages to mem_end */

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
    slab_bit_set(phys);   /* the page is a slab page from this moment on */

    return page;
}

void slab_init(void) {
    memset(&g_slab, 0, sizeof(g_slab));

    /* The page registry covers every physical page the buddy can ever hand
     * out (mem_end, not total_pages — holes in the map must still index).
     * 1 bit per 4 KiB = 32 KiB of registry per 1 GiB of RAM. A machine that
     * cannot spare 0.001% of itself for knowing what memory IS cannot run;
     * the allocator's identity is load-bearing, so failure here is fatal. */
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

    /* Free-list integrity — the page this class is about to hand a block
     * from must actually be one of its own. A foreign page recruited into
     * the list (a look-alike header, a stale pointer, an overwrite) shows
     * up here as an offset that cannot exist on a genuine slab page: below
     * the header, past the page, or off the object grid. Handing such a
     * "block" out is how a page-aligned pointer with no guard header ends
     * in kfree's canary check — dying loudly HERE names the corruption at
     * its source instead of one owner later. */
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

    /* A genuine slab block sits ON the object grid of its page: at or past
     * the header, and an exact multiple of obj_size from it. A pointer that
     * reaches here off-grid is NOT a slab block — its page merely wears a
     * plausible obj_size (kfree's slab_owns is a two-byte heuristic). The
     * old code pushed such a pointer into the class free list anyway,
     * silently recruiting a foreign LIVE page into the allocator: the page
     * then got handed out as blocks while its real owner kept writing —
     * double ownership of live memory, surfacing one owner later as a
     * destroyed kfree canary (see the 0xffff880004471000 corpse). Die HERE,
     * naming the caller, instead. */
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

    /* Same recruitment gate under the lock: freeing onto a page whose
     * accounting says "already all free" means this pointer's page was
     * never a member of this class (or was double-freed page-wide). */
    if (page->free_count >= page->total_slots ||
        page->total_slots == 0 || page->obj_size != cls->obj_size) {
        spin_unlock(&cls->lock);
        panic("[SLAB] free of %p rejects its page %p: obj_size=%u "
              "free_count=%u total=%u — impostor page or double free\n",
              ptr, (void *)page, page->obj_size,
              page->free_count, page->total_slots);
    }

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
            slab_bit_clear(page->page_phys);   /* ...and stops being one here */
            pmm_free((void*)page->page_phys, 1);
        }
    }

    spin_unlock(&cls->lock);
}

bool slab_owns(void* ptr) {
    if (!ptr || !g_slab.initialized) return false;

    /* Exact membership from the page registry — the bit set when the page
     * was carved for a class and cleared when it went back to the PMM.
     * The old two-byte look at the page header called any page whose
     * CONTENT resembled a class size a slab page; pages of pool blocks
     * full of disk bytes qualified, and their kfree was mis-routed here,
     * recruiting live pool pages into class free lists. Ownership is the
     * allocator's own record, never a guess about what memory contains. */
    uintptr_t phys = vmm_virt_to_phys_direct(ptr);
    return slab_bit_test(phys & ~(uintptr_t)(SLAB_PAGE_SIZE - 1));
}

void slab_activate_pull_map(void) {
    // After Pull Map activation, all slab page virtual addresses need rebasing.
    // Identity mapping is GONE — can't dereference identity pointers anymore.
    // Since identity: virt == phys, use vmm_phys_to_virt(identity_addr) to access.

    /* The page registry itself moves with everything else. */
    g_slab_page_bits = (uint8_t *)vmm_phys_to_virt(g_slab_page_bits_phys);

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

/*
 * slab_identity_selftest — boot-time proof that slab membership is the
 * allocator's RECORD, not a guess about page content.
 *
 * Forge the exact page that used to fool the old heuristic: a multi-page
 * pool block whose interior page begins with a class-sized word (disk data
 * looked like this). Membership must still answer "not slab" — the answer
 * that keeps kfree of such blocks on the pool path. Under the old two-byte
 * heuristic this forgery passed, kfree was mis-routed to slab_free, and the
 * pool's LIVE page was recruited into a class free list (double ownership
 * of live memory; the 0xffff880004471000 corpse). Deterministic, runs at
 * every boot, and panics the moment anyone reintroduces a content guess.
 */
void slab_identity_selftest(void) {
    uint8_t *buf = (uint8_t *)kmalloc(3 * SLAB_PAGE_SIZE);
    if (!buf) {
        panic("[SLAB] identity self-test: kmalloc(3 pages) failed at boot");
    }
    uint8_t *interior = (uint8_t *)(((uintptr_t)buf + SLAB_PAGE_SIZE) &
                                    ~(uintptr_t)(SLAB_PAGE_SIZE - 1));
    /* Forge the REAL field a content-guess would read: SlabPage.obj_size
     * (it sits after page_phys, not at byte 0 — exactly where disk data
     * happened to land it in the live corruption). */
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
