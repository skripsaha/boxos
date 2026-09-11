#ifdef malloc
#undef malloc
#endif

#include "box/timeouts.h"
#include "box/memory.h"
#include "box/sync.h"
#include "box/string.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"
#include "box/core/manifest.h"
#include "box/core/strand_self.h"
#include "box/print.h"
#include "cabin_layout.h"
#include "strand_pool_abi.h"

#include "boxos_sizes.h"
#include "boxos_decks.h"

#define HEAP_OP_PREFAULT         0x14u
#define HEAP_HUGE_THRESHOLD      LARGE_PAGE_2M_SIZE
#define HEAP_HUGE_MASK           LARGE_PAGE_2M_MASK


#define HEAP_MAGIC      0xA110CA7EU
#define HEAP_ALIGN      16
#define HEAP_MIN_SPLIT  (sizeof(block_t) + HEAP_ALIGN)

typedef struct block {
    size_t        size;
    uint32_t      magic;
    uint32_t      free;
    struct block* next;
    uint8_t       tag;
    uint8_t       _pad[7];
} block_t;

typedef char _block_size_check[sizeof(block_t) == 32 ? 1 : -1];

#define BLOCK_HDR_SIZE  ((sizeof(block_t) + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1))

typedef char _hdr_size_check[BLOCK_HDR_SIZE == 32 ? 1 : -1];


static char    heap_tag_names[HEAP_TAG_CAP][HEAP_TAG_NAME_MAX];
static uint8_t heap_tag_count = 0;


static uspin_t   heap_lock    = USPIN_INIT;
static block_t*  free_list    = NULL;
static uintptr_t heap_base    = 0;
static uintptr_t heap_current = 0;
static uintptr_t heap_max     = 0;
static int       initialized  = 0;


static error_t  heap_last_error = OK;

static uint32_t stat_malloc_calls = 0;
static uint32_t stat_free_calls   = 0;


static void heap_init_locked(void) {
    CabinInfo* ci = cabin_info();
    if (ci->heap_base != 0 && ci->heap_max_size != 0) {
        heap_base = ci->heap_base;
        heap_max  = heap_base + ci->heap_max_size;
    } else {
        heap_base = CABIN_HEAP_BASE;
        heap_max  = heap_base + CABIN_HEAP_MAX_SIZE;
    }
    heap_current = heap_base;
    free_list    = NULL;
    initialized  = 1;
}

static void* sbrk_locked(size_t increment) {
    if (!initialized) return NULL;

    uintptr_t new_end = heap_current + increment;
    if (new_end > heap_max) return NULL;

    uintptr_t old = heap_current;
    heap_current  = new_end;
    return (void*)old;
}

static size_t align_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

static void coalesce_locked(void) {
    block_t* curr = free_list;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) break;
        if (curr->free && curr->next &&
            curr->next->magic == HEAP_MAGIC && curr->next->free &&
            (uintptr_t)curr + BLOCK_HDR_SIZE + curr->size == (uintptr_t)curr->next) {
            curr->size += BLOCK_HDR_SIZE + curr->next->size;
            curr->next  = curr->next->next;
            continue;
        }
        curr = curr->next;
    }
}

static uint8_t get_or_create_tag_locked(const char *name) {
    if (!name) return HEAP_TAG_NONE;

    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strncmp(heap_tag_names[i], name, HEAP_TAG_NAME_MAX - 1) == 0)
            return i;
    }

    if (heap_tag_count >= HEAP_TAG_CAP) return HEAP_TAG_NONE;

    uint8_t id = heap_tag_count++;
    strncpy(heap_tag_names[id], name, HEAP_TAG_NAME_MAX - 1);
    heap_tag_names[id][HEAP_TAG_NAME_MAX - 1] = '\0';
    return id;
}

static int prefault_huge(uintptr_t va_base, uint64_t size_2m_aligned) {
    uint8_t params[16];
    uint64_t va64 = (uint64_t)va_base;
    memcpy(params,     &va64,           sizeof(uint64_t));
    memcpy(params + 8, &size_2m_aligned, sizeof(uint64_t));
    return MfCall1(DECK_SYSTEM, HEAP_OP_PREFAULT,
                   params, sizeof(params),
                   NULL, 0, NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

static void* alloc_locked(size_t size, uint8_t tag_id, error_t *errcell,
                          uintptr_t *huge_va, size_t *huge_len) {
    if (!initialized) heap_init_locked();

    stat_malloc_calls++;

    size_t orig_size = size;
    size = align_up(size, HEAP_ALIGN);
    if (size < orig_size) {
        *errcell = ERR_NO_MEMORY;
        return NULL;
    }

    block_t* curr = free_list;
    block_t* prev = NULL;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            *errcell = ERR_CORRUPTED;
            break;
        }
        if (curr->free && curr->size >= size) {
            if (curr->size >= size + HEAP_MIN_SPLIT) {
                block_t* split = (block_t*)((uint8_t*)curr + BLOCK_HDR_SIZE + size);
                split->size  = curr->size - size - BLOCK_HDR_SIZE;
                split->magic = HEAP_MAGIC;
                split->free  = 1;
                split->tag   = HEAP_TAG_NONE;
                split->next  = curr->next;
                curr->size   = size;
                curr->next   = split;
            }
            curr->free = 0;
            curr->tag  = tag_id;
            *errcell = OK;
            return (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
        }
        prev = curr;
        curr = curr->next;
    }

    size_t total = BLOCK_HDR_SIZE + size;
    if (total < size) {
        *errcell = ERR_NO_MEMORY;
        return NULL;
    }

    if (total >= HEAP_HUGE_THRESHOLD) {
        uintptr_t cur      = heap_current;
        uintptr_t aligned  = (cur + HEAP_HUGE_MASK) & ~HEAP_HUGE_MASK;
        size_t    pad_size = aligned - cur;
        size_t    huge_total   = (total + HEAP_HUGE_MASK) & ~HEAP_HUGE_MASK;
        size_t    grow_total   = pad_size + huge_total;

        if (heap_current + grow_total > heap_max) {
            *errcell = ERR_HEAP_EXHAUSTED;
            return NULL;
        }

        if (pad_size >= BLOCK_HDR_SIZE + HEAP_ALIGN) {
            block_t *pad = (block_t *)cur;
            pad->size  = pad_size - BLOCK_HDR_SIZE;
            pad->magic = HEAP_MAGIC;
            pad->free  = 1;
            pad->tag   = HEAP_TAG_NONE;
            pad->next  = NULL;
            if (prev) {
                prev->next = pad;
            } else {
                free_list = pad;
            }
        }

        if (huge_va)  *huge_va  = aligned;
        if (huge_len) *huge_len = huge_total;

        heap_current = aligned + huge_total;

        *errcell = OK;
        return (void *)(aligned + BLOCK_HDR_SIZE);
    }

    void* mem = sbrk_locked(total);
    if (!mem) {
        *errcell = ERR_HEAP_EXHAUSTED;
        return NULL;
    }

    block_t* block = (block_t*)mem;
    block->size  = size;
    block->magic = HEAP_MAGIC;
    block->free  = 0;
    block->tag   = tag_id;
    block->next  = NULL;

    if (prev) {
        prev->next = block;
    } else {
        free_list = block;
    }

    *errcell = OK;
    return (void*)((uint8_t*)block + BLOCK_HDR_SIZE);
}


static const size_t   StrandPoolClassSize[STRAND_POOL_CLASS_COUNT] =
    { 16, 32, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
static const uint16_t StrandPoolCapForClass[STRAND_POOL_CLASS_COUNT] =
    {  8,  8,  8,  8,   8,   8,   4,    4,    2,    2,    1 };

#define STRAND_POOL_REFILL_BATCH  4u
#define STRAND_POOL_NO_CLASS      STRAND_POOL_CLASS_COUNT

static StrandPool g_pool_slab[STRAND_POOL_SLAB_MAX];
static StrandPool g_main_pool;

static error_t g_pool_last_error[STRAND_POOL_SLAB_MAX];

static inline error_t *heap_err_cell_for(StrandPool *pool) {
    if (pool == NULL || pool == &g_main_pool) return &heap_last_error;
    return &g_pool_last_error[pool - g_pool_slab];
}

error_t heap_get_last_error(void) {
    StrandInfo *si = strand_info_or_null();
    if (si && si->strand_pool_ptr != 0)
        return *heap_err_cell_for((StrandPool *)(uintptr_t)si->strand_pool_ptr);
    return heap_last_error;
}

static volatile uint32_t g_strandpool_orphan_pending = 0;

static uint32_t g_pool_slab_hwm = 0;

static inline uint32_t pool_slab_hwm_clamped(void) {
    uint32_t hwm = g_pool_slab_hwm;
    return hwm > STRAND_POOL_SLAB_MAX ? STRAND_POOL_SLAB_MAX : hwm;
}

static unsigned StrandPoolSizeToClass(size_t n) {
    for (unsigned c = 0; c < STRAND_POOL_CLASS_COUNT; c++) {
        if (StrandPoolClassSize[c] >= n) return c;
    }
    return STRAND_POOL_NO_CLASS;
}

static unsigned StrandPoolClassFromBlockSize(size_t s) {
    if (s > STRAND_POOL_MAX_CLASS) return STRAND_POOL_NO_CLASS;
    unsigned cls = STRAND_POOL_NO_CLASS;
    for (unsigned c = 0; c < STRAND_POOL_CLASS_COUNT; c++) {
        if (StrandPoolClassSize[c] <= s) cls = c; else break;
    }
    return cls;
}

static void strand_pool_bind_kernel(StrandPool *pool, uint32_t gen) {
    uint8_t params[20];
    uint64_t va         = (uint64_t)(uintptr_t)pool;
    uint64_t pending_va = (uint64_t)(uintptr_t)&g_strandpool_orphan_pending;
    memcpy(params,      &va,         sizeof(uint64_t));
    memcpy(params + 8,  &gen,        sizeof(uint32_t));
    memcpy(params + 12, &pending_va, sizeof(uint64_t));
    (void)MfCall1(DECK_SYSTEM, SYSTEM_OP_STRAND_POOL_BIND,
                  params, sizeof(params),
                  NULL, 0, NULL, 0, NULL,
                  BOX_ANSWER_GUARANTEED, NULL);
}

static void reclaim_one_orphan_slot_locked(StrandPool *p, uint32_t word) {
    for (unsigned c = 0; c < STRAND_POOL_CLASS_COUNT; c++) {
        unsigned guard = p->Counts[c];
        void *node = p->Heads[c];
        while (node && guard--) {
            block_t *block = (block_t *)((uint8_t *)node - BLOCK_HDR_SIZE);
            if (block->magic != HEAP_MAGIC) break;
            void *next = *(void **)node;
            if (!block->free) {
                block->free = 1;
                block->tag  = HEAP_TAG_NONE;
            }
            node = next;
        }
        p->Heads[c]  = NULL;
        p->Counts[c] = 0;
    }
    coalesce_locked();
    p->OwnerPid = 0;
    __atomic_store_n(&p->GenState,
                     STRANDPOOL_PACK(STRANDPOOL_GEN(word) + 1, STRANDPOOL_FREE),
                     __ATOMIC_RELEASE);
}

static void reclaim_orphans_scan_locked(void) {
    if (__atomic_load_n(&g_strandpool_orphan_pending, __ATOMIC_ACQUIRE) == 0) return;
    __atomic_store_n(&g_strandpool_orphan_pending, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < pool_slab_hwm_clamped(); i++) {
        StrandPool *p = &g_pool_slab[i];
        uint32_t word = __atomic_load_n(&p->GenState, __ATOMIC_ACQUIRE);
        if (STRANDPOOL_STATE(word) != STRANDPOOL_ORPHANED) continue;
        reclaim_one_orphan_slot_locked(p, word);
    }
}

static StrandPool *pool_claim(StrandInfo *si) {
    if (!si) {
        uint32_t word = __atomic_load_n(&g_main_pool.GenState, __ATOMIC_ACQUIRE);
        if (STRANDPOOL_STATE(word) == STRANDPOOL_FREE) {
            g_main_pool.OwnerPid = strand_self();
            __atomic_store_n(&g_main_pool.GenState,
                             STRANDPOOL_PACK(STRANDPOOL_GEN(word) + 1, STRANDPOOL_LIVE),
                             __ATOMIC_RELEASE);
        }
        return &g_main_pool;
    }

    StrandPool *claimed = NULL;
    uint32_t    claimed_gen = 0;

    uspin_lock(&heap_lock);
    for (int pass = 0; pass < 2 && !claimed; pass++) {
        for (unsigned i = 0; i < STRAND_POOL_SLAB_MAX; i++) {
            StrandPool *p = &g_pool_slab[i];
            uint32_t word = __atomic_load_n(&p->GenState, __ATOMIC_RELAXED);
            if (STRANDPOOL_STATE(word) != STRANDPOOL_FREE) continue;
            uint32_t next = STRANDPOOL_PACK(STRANDPOOL_GEN(word) + 1, STRANDPOOL_LIVE);
            if (__atomic_compare_exchange_n(&p->GenState, &word, next, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                p->OwnerPid = strand_self();
                g_pool_last_error[i] = OK;
                claimed     = p;
                claimed_gen = STRANDPOOL_GEN(next);
                if (i + 1 > g_pool_slab_hwm) g_pool_slab_hwm = i + 1;
                break;
            }
        }
        if (!claimed && pass == 0) {
            for (unsigned j = 0; j < pool_slab_hwm_clamped(); j++) {
                StrandPool *p2 = &g_pool_slab[j];
                uint32_t word2 = __atomic_load_n(&p2->GenState, __ATOMIC_ACQUIRE);
                if (STRANDPOOL_STATE(word2) != STRANDPOOL_ORPHANED) continue;
                reclaim_one_orphan_slot_locked(p2, word2);
            }
            __atomic_store_n(&g_strandpool_orphan_pending, 0, __ATOMIC_RELEASE);
        }
    }
    uspin_unlock(&heap_lock);

    if (claimed) {
        si->strand_pool_ptr = (uint64_t)(uintptr_t)claimed;
        strand_pool_bind_kernel(claimed, claimed_gen);
    }
    return claimed;
}

static StrandPool *pool_self(void) {
    StrandInfo *si = strand_info_or_null();
    if (!si) {
        if (__atomic_load_n(&g_main_pool.GenState, __ATOMIC_ACQUIRE) == 0)
            return pool_claim(NULL);
        return &g_main_pool;
    }
    if (si->strand_pool_ptr != 0)
        return (StrandPool *)(uintptr_t)si->strand_pool_ptr;
    return pool_claim(si);
}

static void pool_refill_locked(StrandPool *pool, unsigned c) {
    error_t *errcell = heap_err_cell_for(pool);
    for (unsigned k = 0; k < STRAND_POOL_REFILL_BATCH; k++) {
        void *payload = alloc_locked(StrandPoolClassSize[c], HEAP_TAG_NONE, errcell, NULL, NULL);
        if (!payload) break;
        *(void **)payload = pool->Heads[c];
        pool->Heads[c]    = payload;
        pool->Counts[c]++;
    }
}

static void pool_flush_class_locked(StrandPool *pool, unsigned c, unsigned drop) {
    unsigned guard = pool->Counts[c];
    while (drop-- && guard-- && pool->Heads[c]) {
        void *payload = pool->Heads[c];
        block_t *block = (block_t *)((uint8_t *)payload - BLOCK_HDR_SIZE);
        if (block->magic != HEAP_MAGIC) break;
        pool->Heads[c] = *(void **)payload;
        pool->Counts[c]--;
        if (!block->free) {
            block->free = 1;
            block->tag  = HEAP_TAG_NONE;
        }
    }
}

void strand_pool_flush_self(void) {
    StrandInfo *si   = strand_info_or_null();
    StrandPool *pool = si ? (StrandPool *)(uintptr_t)si->strand_pool_ptr : &g_main_pool;
    if (!pool) return;
    if (si && si->strand_pool_ptr == 0) return;
    if (!si && __atomic_load_n(&g_main_pool.GenState, __ATOMIC_ACQUIRE) == 0) return;

    uspin_lock(&heap_lock);
    for (unsigned c = 0; c < STRAND_POOL_CLASS_COUNT; c++)
        pool_flush_class_locked(pool, c, (unsigned)-1);
    coalesce_locked();
    uint32_t word = __atomic_load_n(&pool->GenState, __ATOMIC_RELAXED);
    pool->OwnerPid = 0;
    __atomic_store_n(&pool->GenState,
                     STRANDPOOL_PACK(STRANDPOOL_GEN(word) + 1, STRANDPOOL_FREE),
                     __ATOMIC_RELEASE);
    uspin_unlock(&heap_lock);

    if (si) si->strand_pool_ptr = 0;
}

int strand_pool_test_orphan_reclaim(unsigned n_blocks) {
    const unsigned c = 3;
    if (n_blocks == 0 || n_blocks > StrandPoolCapForClass[c]) return 0;

    uspin_lock(&heap_lock);

    StrandPool *slot = NULL;
    for (int i = (int)STRAND_POOL_SLAB_MAX - 1; i >= 0; i--) {
        if (STRANDPOOL_STATE(g_pool_slab[i].GenState) == STRANDPOOL_FREE) {
            slot = &g_pool_slab[i];
            break;
        }
    }
    if (!slot) { uspin_unlock(&heap_lock); return 0; }

    uint32_t gen = STRANDPOOL_GEN(slot->GenState);

    error_t staging_err = OK;
    for (unsigned k = 0; k < n_blocks; k++) {
        void *payload = alloc_locked(StrandPoolClassSize[c], HEAP_TAG_NONE, &staging_err, NULL, NULL);
        if (!payload) break;
        *(void **)payload = slot->Heads[c];
        slot->Heads[c]    = payload;
        slot->Counts[c]++;
    }
    unsigned staged = slot->Counts[c];

    uint32_t live_before = 0;
    for (block_t *b = free_list; b && b->magic == HEAP_MAGIC; b = b->next)
        if (!b->free) live_before++;

    slot->OwnerPid = 0;
    __atomic_store_n(&slot->GenState,
                     STRANDPOOL_PACK(gen, STRANDPOOL_ORPHANED), __ATOMIC_RELEASE);

    unsigned slot_idx = (unsigned)(slot - g_pool_slab);
    if (slot_idx + 1 > g_pool_slab_hwm) g_pool_slab_hwm = slot_idx + 1;
    __atomic_store_n(&g_strandpool_orphan_pending, 1, __ATOMIC_RELEASE);
    reclaim_orphans_scan_locked();

    uint32_t live_after = 0;
    for (block_t *b = free_list; b && b->magic == HEAP_MAGIC; b = b->next)
        if (!b->free) live_after++;

    int ok = (STRANDPOOL_STATE(slot->GenState) == STRANDPOOL_FREE) &&
             (slot->Heads[c] == NULL) && (slot->Counts[c] == 0) &&
             (staged > 0) && (live_after + staged == live_before);

    uspin_unlock(&heap_lock);
    return ok;
}


static void link_block_locked(block_t *b) {
    block_t *prev = NULL;
    for (block_t *c = free_list; c && (uintptr_t)c < (uintptr_t)b; c = c->next) prev = c;
    if (prev) { b->next = prev->next; prev->next = b; }
    else      { b->next = free_list;  free_list  = b; }
}

static void* huge_backed(void* ptr, uintptr_t va, size_t len, uint8_t tag_id,
                         error_t *errcell) {
    if (!ptr || len == 0) return ptr;
    bool backed = prefault_huge(va, (uint64_t)len) == 0;

    uspin_lock(&heap_lock);
    block_t *block = (block_t *)va;
    block->size  = len - BLOCK_HDR_SIZE;
    block->magic = HEAP_MAGIC;
    block->free  = backed ? 0 : 1;
    block->tag   = backed ? tag_id : HEAP_TAG_NONE;
    link_block_locked(block);
    uspin_unlock(&heap_lock);

    if (backed) return ptr;
    *errcell = ERR_NO_MEMORY;
    return NULL;
}

void* malloc(size_t size) {
    if (size == 0) return NULL;

    StrandPool *pool    = pool_self();
    error_t    *errcell = heap_err_cell_for(pool);

    unsigned c = StrandPoolSizeToClass(size);
    if (c == STRAND_POOL_NO_CLASS || !pool) {
        uintptr_t huge_va = 0; size_t huge_len = 0;
        uspin_lock(&heap_lock);
        void* ptr = alloc_locked(size, HEAP_TAG_NONE, errcell, &huge_va, &huge_len);
        uspin_unlock(&heap_lock);
        return huge_backed(ptr, huge_va, huge_len, HEAP_TAG_NONE, errcell);
    }

    if (!pool->Heads[c]) {
        uspin_lock(&heap_lock);
        reclaim_orphans_scan_locked();
        pool_refill_locked(pool, c);
        uspin_unlock(&heap_lock);
        if (!pool->Heads[c]) {
            *errcell = ERR_HEAP_EXHAUSTED;
            return NULL;
        }
    }

    void *payload  = pool->Heads[c];
    pool->Heads[c] = *(void **)payload;
    pool->Counts[c]--;
    *errcell = OK;
    return payload;
}

void* aligned_alloc(size_t alignment, size_t size) {
    if (size == 0) return NULL;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return NULL;
    if (alignment <= HEAP_ALIGN) return malloc(size);

    size_t raw = size + alignment + BLOCK_HDR_SIZE + HEAP_ALIGN;
    if (raw < size) return NULL;

    StrandPool *pool    = pool_self();
    error_t    *errcell = heap_err_cell_for(pool);

    uintptr_t huge_va = 0; size_t huge_len = 0;
    uspin_lock(&heap_lock);
    void* p = alloc_locked(raw, HEAP_TAG_NONE, errcell, &huge_va, &huge_len);
    uspin_unlock(&heap_lock);
    p = huge_backed(p, huge_va, huge_len, HEAP_TAG_NONE, errcell);
    if (!p) return NULL;

    uspin_lock(&heap_lock);
    block_t*  b      = (block_t*)((uint8_t*)p - BLOCK_HDR_SIZE);
    uintptr_t target = align_up((uintptr_t)p + BLOCK_HDR_SIZE + HEAP_ALIGN,
                                alignment);
    block_t*  n      = (block_t*)(target - BLOCK_HDR_SIZE);
    size_t    lead   = (uintptr_t)n - (uintptr_t)p;

    n->size  = b->size - lead - BLOCK_HDR_SIZE;
    n->magic = HEAP_MAGIC;
    n->free  = 0;
    n->tag   = HEAP_TAG_NONE;
    n->next  = b->next;

    b->size  = lead;
    b->free  = 1;
    b->tag   = HEAP_TAG_NONE;
    b->next  = n;

    uspin_unlock(&heap_lock);
    return (void*)target;
}

void* malloc_tagged(size_t size, const char *tag) {
    if (size == 0) return NULL;

    error_t *errcell = heap_err_cell_for(pool_self());
    uintptr_t huge_va = 0; size_t huge_len = 0;
    uspin_lock(&heap_lock);
    uint8_t tag_id = get_or_create_tag_locked(tag);
    void* ptr = alloc_locked(size, tag_id, errcell, &huge_va, &huge_len);
    uspin_unlock(&heap_lock);
    return huge_backed(ptr, huge_va, huge_len, tag_id, errcell);
}

static void free_global(void* ptr) {
    error_t *errcell = heap_err_cell_for(pool_self());
    uspin_lock(&heap_lock);

    stat_free_calls++;

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);

    if (block->magic != HEAP_MAGIC) {
        *errcell = ERR_CORRUPTED;
        uspin_unlock(&heap_lock);
        return;
    }

    if (block->free) {
        *errcell = ERR_INVALID_ADDRESS;
        uspin_unlock(&heap_lock);
        return;
    }

    block->free = 1;
    block->tag  = HEAP_TAG_NONE;
    coalesce_locked();

    *errcell = OK;
    uspin_unlock(&heap_lock);
}

void free(void* ptr) {
    if (!ptr) return;

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);

    if (block->magic != HEAP_MAGIC) { free_global(ptr); return; }
    if (block->free)                { free_global(ptr); return; }
    if (block->tag != HEAP_TAG_NONE){ free_global(ptr); return; }

    unsigned c = StrandPoolClassFromBlockSize(block->size);
    if (c == STRAND_POOL_NO_CLASS) { free_global(ptr); return; }

    StrandPool *pool = pool_self();
    if (!pool) { free_global(ptr); return; }
    error_t *errcell = heap_err_cell_for(pool);

    unsigned guard = pool->Counts[c];
    for (void *node = pool->Heads[c]; node && guard--; ) {
        if (node == ptr) {
            *errcell = ERR_INVALID_ADDRESS;
            return;
        }
        block_t *b = (block_t *)((uint8_t *)node - BLOCK_HDR_SIZE);
        if (b->magic != HEAP_MAGIC) break;
        node = *(void **)node;
    }

    if (pool->Counts[c] >= StrandPoolCapForClass[c]) {
        uspin_lock(&heap_lock);
        reclaim_orphans_scan_locked();
        pool_flush_class_locked(pool, c, STRAND_POOL_REFILL_BATCH);
        coalesce_locked();
        uspin_unlock(&heap_lock);
    }

    *(void **)ptr  = pool->Heads[c];
    pool->Heads[c] = ptr;
    pool->Counts[c]++;
    *errcell = OK;
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    size_t total = nmemb * size;
    if (total / nmemb != size) {
        *heap_err_cell_for(pool_self()) = ERR_NO_MEMORY;
        return NULL;
    }

    void* ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    error_t *errcell = heap_err_cell_for(pool_self());
    uspin_lock(&heap_lock);

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);
    if (block->magic != HEAP_MAGIC) {
        *errcell = ERR_CORRUPTED;
        uspin_unlock(&heap_lock);
        return NULL;
    }

    size_t aligned = align_up(size, HEAP_ALIGN);

    if (block->size >= aligned) {
        if (block->size >= aligned + HEAP_MIN_SPLIT) {
            block_t* split = (block_t*)((uint8_t*)block + BLOCK_HDR_SIZE + aligned);
            split->size  = block->size - aligned - BLOCK_HDR_SIZE;
            split->magic = HEAP_MAGIC;
            split->free  = 1;
            split->tag   = HEAP_TAG_NONE;
            split->next  = block->next;
            block->size  = aligned;
            block->next  = split;
        }
        *errcell = OK;
        uspin_unlock(&heap_lock);
        return ptr;
    }

    if (block->next && block->next->magic == HEAP_MAGIC && block->next->free) {
        size_t combined = block->size + BLOCK_HDR_SIZE + block->next->size;
        if (combined >= aligned) {
            block->size = combined;
            block->next = block->next->next;
            if (block->size >= aligned + HEAP_MIN_SPLIT) {
                block_t* split = (block_t*)((uint8_t*)block + BLOCK_HDR_SIZE + aligned);
                split->size  = block->size - aligned - BLOCK_HDR_SIZE;
                split->magic = HEAP_MAGIC;
                split->free  = 1;
                split->tag   = HEAP_TAG_NONE;
                split->next  = block->next;
                block->size  = aligned;
                block->next  = split;
            }
            *errcell = OK;
            uspin_unlock(&heap_lock);
            return ptr;
        }
    }

    size_t old_size = block->size;
    uint8_t old_tag = block->tag;
    uspin_unlock(&heap_lock);

    void* new_ptr = malloc(size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, old_size);
    free(ptr);

    if (old_tag != HEAP_TAG_NONE) {
        uspin_lock(&heap_lock);
        block_t* new_block = (block_t*)((uint8_t*)new_ptr - BLOCK_HDR_SIZE);
        if (new_block->magic == HEAP_MAGIC) {
            new_block->tag = old_tag;
        }
        uspin_unlock(&heap_lock);
    }

    return new_ptr;
}


uint8_t heap_register_tag(const char *name) {
    uspin_lock(&heap_lock);
    uint8_t id = get_or_create_tag_locked(name);
    uspin_unlock(&heap_lock);
    return id;
}

uint8_t heap_lookup_tag(const char *name) {
    if (!name) return HEAP_TAG_NONE;

    uspin_lock(&heap_lock);
    uint8_t result = HEAP_TAG_NONE;
    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strncmp(heap_tag_names[i], name, HEAP_TAG_NAME_MAX - 1) == 0) {
            result = i;
            break;
        }
    }
    uspin_unlock(&heap_lock);
    return result;
}

const char *heap_tag_name(uint8_t id) {
    if (id == HEAP_TAG_NONE || id >= HEAP_TAG_CAP) return NULL;

    uspin_lock(&heap_lock);
    const char *name = (id < heap_tag_count) ? heap_tag_names[id] : NULL;
    uspin_unlock(&heap_lock);
    return name;
}

size_t heap_count_tag(const char *tag) {
    if (!tag) return 0;

    uspin_lock(&heap_lock);

    uint8_t tag_id = HEAP_TAG_NONE;
    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strncmp(heap_tag_names[i], tag, HEAP_TAG_NAME_MAX - 1) == 0) {
            tag_id = i;
            break;
        }
    }

    size_t count = 0;
    if (tag_id != HEAP_TAG_NONE) {
        block_t* curr = free_list;
        while (curr) {
            if (curr->magic != HEAP_MAGIC) break;
            if (!curr->free && curr->tag == tag_id) count++;
            curr = curr->next;
        }
    }

    uspin_unlock(&heap_lock);
    return count;
}

void heap_iterate_tag(const char *tag, HeapTagCallback cb, void *userdata) {
    if (!tag || !cb) return;

    uspin_lock(&heap_lock);

    uint8_t tag_id = HEAP_TAG_NONE;
    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strncmp(heap_tag_names[i], tag, HEAP_TAG_NAME_MAX - 1) == 0) {
            tag_id = i;
            break;
        }
    }

    if (tag_id != HEAP_TAG_NONE) {
        block_t* curr = free_list;
        while (curr) {
            if (curr->magic != HEAP_MAGIC) break;
            if (!curr->free && curr->tag == tag_id) {
                void* payload = (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
                cb(payload, curr->size, heap_tag_names[tag_id], userdata);
            }
            curr = curr->next;
        }
    }

    uspin_unlock(&heap_lock);
}

void heap_iterate_all_tagged(HeapTagCallback cb, void *userdata) {
    if (!cb) return;

    uspin_lock(&heap_lock);

    block_t* curr = free_list;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) break;
        if (!curr->free && curr->tag != HEAP_TAG_NONE) {
            void* payload = (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
            const char *name = (curr->tag < heap_tag_count)
                               ? heap_tag_names[curr->tag]
                               : NULL;
            cb(payload, curr->size, name, userdata);
        }
        curr = curr->next;
    }

    uspin_unlock(&heap_lock);
}

void heap_dump_tags(void) {
    uspin_lock(&heap_lock);

    printf("[heap] tagged live blocks:\n");

    block_t* curr = free_list;
    bool found_any = false;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            printf("[heap]   (corrupted block at 0x%x)\n", (uint32_t)(uintptr_t)curr);
            break;
        }
        if (!curr->free && curr->tag != HEAP_TAG_NONE) {
            const char *name = (curr->tag < heap_tag_count)
                               ? heap_tag_names[curr->tag]
                               : "(unknown)";
            void* payload = (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
            printf("[heap]   ptr=0x%x size=%u tag=%s\n",
                   (uint32_t)(uintptr_t)payload,
                   (uint32_t)curr->size,
                   name);
            found_any = true;
        }
        curr = curr->next;
    }

    if (!found_any) {
        printf("[heap]   (none)\n");
    }

    uspin_unlock(&heap_lock);
}


void heap_get_stats(heap_stats_t *out) {
    if (!out) return;

    uspin_lock(&heap_lock);

    out->total_allocated = 0;
    out->total_free      = 0;
    out->alloc_count     = 0;
    out->free_count      = 0;
    out->malloc_calls    = stat_malloc_calls;
    out->free_calls      = stat_free_calls;
    out->heap_used       = (initialized) ? (heap_current - heap_base) : 0;

    block_t* curr = free_list;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) break;
        if (curr->free) {
            out->total_free += curr->size;
            out->free_count++;
        } else {
            out->total_allocated += curr->size;
            out->alloc_count++;
        }
        curr = curr->next;
    }

    uspin_unlock(&heap_lock);
}