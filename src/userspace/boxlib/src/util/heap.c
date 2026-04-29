// Prevent variadic malloc macro from expanding internal calls
#ifdef malloc
#undef malloc
#endif

#include "box/heap.h"
#include "box/sync.h"
#include "box/string.h"
#include "box/notify.h"
#include "box/io.h"
#include "cabin_layout.h"

// ---------------------------------------------------------------------------
// Thread-safe first-fit heap allocator with tag support and diagnostics
// ---------------------------------------------------------------------------

#define HEAP_MAGIC      0xA110CA7EU
#define HEAP_ALIGN      16
#define HEAP_MIN_SPLIT  (sizeof(block_t) + HEAP_ALIGN)

typedef struct block {
    size_t        size;       // payload size (excluding header)
    uint32_t      magic;
    uint32_t      free;       // 1 = free, 0 = used
    struct block* next;
    uint8_t       tag;        // HEAP_TAG_NONE (0xFF) = untagged
    uint8_t       _pad[7];   // keep struct at 32 bytes
} block_t;

// Verify struct layout at compile time: 8+4+4+8+1+7 = 32 bytes
typedef char _block_size_check[sizeof(block_t) == 32 ? 1 : -1];

#define BLOCK_HDR_SIZE  ((sizeof(block_t) + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1))

// Verify BLOCK_HDR_SIZE is still 32 (no heap layout change)
typedef char _hdr_size_check[BLOCK_HDR_SIZE == 32 ? 1 : -1];

// ---- tag registry (all access under heap_lock) ----------------------------

static char    heap_tag_names[HEAP_TAG_CAP][HEAP_TAG_NAME_MAX];
static uint8_t heap_tag_count = 0;

// ---- internal state (all access under heap_lock) --------------------------

static umutex_t  heap_lock    = UMUTEX_INIT;
static block_t*  free_list    = NULL;
static uintptr_t heap_base    = 0;
static uintptr_t heap_current = 0;
static uintptr_t heap_max     = 0;
static int       initialized  = 0;

// ---- diagnostics ----------------------------------------------------------

error_t  heap_last_error = OK;

static uint32_t stat_malloc_calls = 0;
static uint32_t stat_free_calls   = 0;

// ---- helpers (called with lock held) --------------------------------------

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
            curr->next->magic == HEAP_MAGIC && curr->next->free) {
            curr->size += BLOCK_HDR_SIZE + curr->next->size;
            curr->next  = curr->next->next;
            continue;
        }
        curr = curr->next;
    }
}

// Look up or insert a tag name. Returns HEAP_TAG_NONE if registry full.
// Must be called with heap_lock held.
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

// Core allocation logic. tag_id must already be resolved.
// Called with heap_lock held. Returns payload pointer or NULL.
static void* alloc_locked(size_t size, uint8_t tag_id) {
    if (!initialized) heap_init_locked();

    stat_malloc_calls++;

    size_t orig_size = size;
    size = align_up(size, HEAP_ALIGN);
    if (size < orig_size) {
        heap_last_error = ERR_NO_MEMORY;
        return NULL;
    }

    // First-fit search
    block_t* curr = free_list;
    block_t* prev = NULL;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            heap_last_error = ERR_CORRUPTED;
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
            heap_last_error = OK;
            return (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
        }
        prev = curr;
        curr = curr->next;
    }

    // Grow the heap
    size_t total = BLOCK_HDR_SIZE + size;
    if (total < size) {
        heap_last_error = ERR_NO_MEMORY;
        return NULL;
    }

    void* mem = sbrk_locked(total);
    if (!mem) {
        heap_last_error = ERR_HEAP_EXHAUSTED;
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

    heap_last_error = OK;
    return (void*)((uint8_t*)block + BLOCK_HDR_SIZE);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void* _malloc_impl(size_t size) {
    if (size == 0) return NULL;

    umutex_lock(&heap_lock);
    void* ptr = alloc_locked(size, HEAP_TAG_NONE);
    umutex_unlock(&heap_lock);
    return ptr;
}

void* malloc_tagged(size_t size, const char *tag) {
    if (size == 0) return NULL;

    umutex_lock(&heap_lock);
    uint8_t tag_id = get_or_create_tag_locked(tag);
    void* ptr = alloc_locked(size, tag_id);
    umutex_unlock(&heap_lock);
    return ptr;
}

void free(void* ptr) {
    if (!ptr) return;

    umutex_lock(&heap_lock);

    stat_free_calls++;

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);

    if (block->magic != HEAP_MAGIC) {
        heap_last_error = ERR_CORRUPTED;
        umutex_unlock(&heap_lock);
        return;
    }

    if (block->free) {
        heap_last_error = ERR_INVALID_ADDRESS;
        umutex_unlock(&heap_lock);
        return;
    }

    block->free = 1;
    block->tag  = HEAP_TAG_NONE;
    coalesce_locked();

    heap_last_error = OK;
    umutex_unlock(&heap_lock);
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    size_t total = nmemb * size;
    if (total / nmemb != size) {
        heap_last_error = ERR_NO_MEMORY;
        return NULL;
    }

    void* ptr = _malloc_impl(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return _malloc_impl(size);
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

    size_t aligned = align_up(size, HEAP_ALIGN);

    // Shrink in-place
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
        heap_last_error = OK;
        umutex_unlock(&heap_lock);
        return ptr;
    }

    // Try to absorb the next block if it's free
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
            heap_last_error = OK;
            umutex_unlock(&heap_lock);
            return ptr;
        }
    }

    // Must relocate — save old tag and size, then drop lock
    size_t old_size = block->size;
    uint8_t old_tag = block->tag;
    umutex_unlock(&heap_lock);

    void* new_ptr = _malloc_impl(size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, old_size);
    free(ptr);

    // Restore tag on the new block
    if (old_tag != HEAP_TAG_NONE) {
        umutex_lock(&heap_lock);
        block_t* new_block = (block_t*)((uint8_t*)new_ptr - BLOCK_HDR_SIZE);
        if (new_block->magic == HEAP_MAGIC) {
            new_block->tag = old_tag;
        }
        umutex_unlock(&heap_lock);
    }

    return new_ptr;
}

// ---------------------------------------------------------------------------
// Tag registry query API
// ---------------------------------------------------------------------------

uint8_t heap_register_tag(const char *name) {
    umutex_lock(&heap_lock);
    uint8_t id = get_or_create_tag_locked(name);
    umutex_unlock(&heap_lock);
    return id;
}

uint8_t heap_lookup_tag(const char *name) {
    if (!name) return HEAP_TAG_NONE;

    umutex_lock(&heap_lock);
    uint8_t result = HEAP_TAG_NONE;
    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strncmp(heap_tag_names[i], name, HEAP_TAG_NAME_MAX - 1) == 0) {
            result = i;
            break;
        }
    }
    umutex_unlock(&heap_lock);
    return result;
}

const char *heap_tag_name(uint8_t id) {
    if (id == HEAP_TAG_NONE || id >= HEAP_TAG_CAP) return NULL;

    umutex_lock(&heap_lock);
    const char *name = (id < heap_tag_count) ? heap_tag_names[id] : NULL;
    umutex_unlock(&heap_lock);
    return name;
}

size_t heap_count_tag(const char *tag) {
    if (!tag) return 0;

    umutex_lock(&heap_lock);

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

    umutex_unlock(&heap_lock);
    return count;
}

void heap_iterate_tag(const char *tag, HeapTagCallback cb, void *userdata) {
    if (!tag || !cb) return;

    umutex_lock(&heap_lock);

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

    umutex_unlock(&heap_lock);
}

void heap_iterate_all_tagged(HeapTagCallback cb, void *userdata) {
    if (!cb) return;

    umutex_lock(&heap_lock);

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

    umutex_unlock(&heap_lock);
}

void heap_dump_tags(void) {
    umutex_lock(&heap_lock);

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

    umutex_unlock(&heap_lock);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

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
