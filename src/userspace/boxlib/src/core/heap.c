#include "box/heap.h"
#include "box/sync.h"
#include "box/string.h"
#include "box/notify.h"
#include "cabin_layout.h"

// ---------------------------------------------------------------------------
// Thread-safe first-fit heap allocator with diagnostics
//
// All public functions (malloc, free, calloc, realloc, heap_get_stats)
// are protected by a single umutex.  On the current uniprocessor BoxOS
// without threads this is a no-op (uncontended fast-path), but it
// guarantees correctness when threads or SMP arrive.
// ---------------------------------------------------------------------------

#define HEAP_MAGIC      0xA110CA7EU
#define HEAP_ALIGN      16
#define HEAP_MIN_SPLIT  (sizeof(block_t) + HEAP_ALIGN)

typedef struct block {
    size_t        size;   // payload size (excluding header)
    uint32_t      magic;
    uint32_t      free;   // 1 = free, 0 = used
    struct block* next;
} block_t;

#define BLOCK_HDR_SIZE  ((sizeof(block_t) + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1))

// ---- internal state (all access under heap_lock) -------------------------

static umutex_t  heap_lock   = UMUTEX_INIT;
static block_t*  free_list    = NULL;
static uintptr_t heap_base    = 0;
static uintptr_t heap_current = 0;
static uintptr_t heap_max     = 0;
static int       initialized  = 0;

// ---- diagnostics ---------------------------------------------------------

error_t  heap_last_error = OK;

static uint32_t stat_malloc_calls = 0;
static uint32_t stat_free_calls   = 0;

// ---- helpers (called with lock held) -------------------------------------

static void heap_init_locked(void) {
    // ASLR: read actual heap base from notify page (set by kernel)
    notify_page_t* np = notify_page();
    if (np->cabin_heap_base != 0 && np->cabin_heap_max_size != 0) {
        heap_base = np->cabin_heap_base;
        heap_max  = heap_base + np->cabin_heap_max_size;
    } else {
        // fallback for legacy/pre-ASLR kernels
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

// Coalesce adjacent free blocks starting from the head of free_list.
// Must be called with heap_lock held.
static void coalesce_locked(void) {
    block_t* curr = free_list;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) break;
        if (curr->free && curr->next &&
            curr->next->magic == HEAP_MAGIC && curr->next->free) {
            curr->size += BLOCK_HDR_SIZE + curr->next->size;
            curr->next  = curr->next->next;
            continue;   // re-check: the new next might also be free
        }
        curr = curr->next;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void* malloc(size_t size) {
    if (size == 0) return NULL;

    umutex_lock(&heap_lock);

    if (!initialized) heap_init_locked();

    stat_malloc_calls++;

    size_t orig_size = size;
    size = align_up(size, HEAP_ALIGN);
    if (size < orig_size) {                 // overflow in align_up
        heap_last_error = ERR_NO_MEMORY;
        umutex_unlock(&heap_lock);
        return NULL;
    }

    // First-fit search through free list
    block_t* curr = free_list;
    block_t* prev = NULL;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            heap_last_error = ERR_CORRUPTED;
            break;                          // corrupted — stop walking
        }
        if (curr->free && curr->size >= size) {
            // Split if remainder is large enough
            if (curr->size >= size + HEAP_MIN_SPLIT) {
                block_t* split = (block_t*)((uint8_t*)curr + BLOCK_HDR_SIZE + size);
                split->size  = curr->size - size - BLOCK_HDR_SIZE;
                split->magic = HEAP_MAGIC;
                split->free  = 1;
                split->next  = curr->next;
                curr->size   = size;
                curr->next   = split;
            }
            curr->free = 0;
            heap_last_error = OK;
            umutex_unlock(&heap_lock);
            return (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
        }
        prev = curr;
        curr = curr->next;
    }

    // No suitable free block — grow the heap
    size_t total = BLOCK_HDR_SIZE + size;
    if (total < size) {                     // overflow
        heap_last_error = ERR_NO_MEMORY;
        umutex_unlock(&heap_lock);
        return NULL;
    }

    void* mem = sbrk_locked(total);
    if (!mem) {
        heap_last_error = ERR_HEAP_EXHAUSTED;
        umutex_unlock(&heap_lock);
        return NULL;
    }

    block_t* block = (block_t*)mem;
    block->size  = size;
    block->magic = HEAP_MAGIC;
    block->free  = 0;
    block->next  = NULL;

    if (prev) {
        prev->next = block;
    } else {
        free_list = block;
    }

    heap_last_error = OK;
    umutex_unlock(&heap_lock);
    return (void*)((uint8_t*)block + BLOCK_HDR_SIZE);
}

void free(void* ptr) {
    if (!ptr) return;

    umutex_lock(&heap_lock);

    stat_free_calls++;

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);

    if (block->magic != HEAP_MAGIC) {
        heap_last_error = ERR_CORRUPTED;    // corruption or wild pointer
        umutex_unlock(&heap_lock);
        return;
    }

    if (block->free) {
        heap_last_error = ERR_INVALID_ADDRESS;  // double-free
        umutex_unlock(&heap_lock);
        return;
    }

    block->free = 1;
    coalesce_locked();

    heap_last_error = OK;
    umutex_unlock(&heap_lock);
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    size_t total = nmemb * size;
    if (total / nmemb != size) {
        heap_last_error = ERR_NO_MEMORY;    // overflow
        return NULL;
    }

    void* ptr = malloc(total);              // malloc sets heap_last_error
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

    umutex_lock(&heap_lock);

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);
    if (block->magic != HEAP_MAGIC) {
        heap_last_error = ERR_CORRUPTED;
        umutex_unlock(&heap_lock);
        return NULL;
    }

    size = align_up(size, HEAP_ALIGN);

    // Shrink in-place
    if (block->size >= size) {
        if (block->size >= size + HEAP_MIN_SPLIT) {
            block_t* split = (block_t*)((uint8_t*)block + BLOCK_HDR_SIZE + size);
            split->size  = block->size - size - BLOCK_HDR_SIZE;
            split->magic = HEAP_MAGIC;
            split->free  = 1;
            split->next  = block->next;
            block->size  = size;
            block->next  = split;
        }
        heap_last_error = OK;
        umutex_unlock(&heap_lock);
        return ptr;
    }

    // Try to absorb the next block if it's free
    if (block->next && block->next->magic == HEAP_MAGIC && block->next->free) {
        size_t combined = block->size + BLOCK_HDR_SIZE + block->next->size;
        if (combined >= size) {
            block->size = combined;
            block->next = block->next->next;
            if (block->size >= size + HEAP_MIN_SPLIT) {
                block_t* split = (block_t*)((uint8_t*)block + BLOCK_HDR_SIZE + size);
                split->size  = block->size - size - BLOCK_HDR_SIZE;
                split->magic = HEAP_MAGIC;
                split->free  = 1;
                split->next  = block->next;
                block->size  = size;
                block->next  = split;
            }
            heap_last_error = OK;
            umutex_unlock(&heap_lock);
            return ptr;
        }
    }

    // Must relocate — drop lock, use malloc+free (they take their own locks)
    size_t old_size = block->size;
    umutex_unlock(&heap_lock);

    void* new_ptr = malloc(size);
    if (!new_ptr) return NULL;              // heap_last_error set by malloc

    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}

void heap_get_stats(heap_stats_t *out) {
    if (!out) return;

    umutex_lock(&heap_lock);

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

    umutex_unlock(&heap_lock);
}
