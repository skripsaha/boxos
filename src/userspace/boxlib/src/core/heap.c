#include "box/heap.h"
#include "box/string.h"
#include "cabin_layout.h"

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

static block_t*  free_list    = NULL;
static uintptr_t heap_base    = 0;
static uintptr_t heap_current = 0;
static uintptr_t heap_max     = 0;
static int       initialized  = 0;

static void heap_init(void) {
    heap_base    = CABIN_HEAP_BASE;
    heap_current = heap_base;
    heap_max     = heap_base + CABIN_HEAP_MAX_SIZE;
    free_list    = NULL;
    initialized  = 1;
}

static void* sbrk(size_t increment) {
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

void* malloc(size_t size) {
    if (size == 0) return NULL;
    if (!initialized) heap_init();

    size = align_up(size, HEAP_ALIGN);

    block_t* curr = free_list;
    block_t* prev = NULL;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) break;
        if (curr->free && curr->size >= size) {
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
            return (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
        }
        prev = curr;
        curr = curr->next;
    }

    size_t total = BLOCK_HDR_SIZE + size;
    void* mem = sbrk(total);
    if (!mem) return NULL;

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

    return (void*)((uint8_t*)block + BLOCK_HDR_SIZE);
}

void free(void* ptr) {
    if (!ptr) return;

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);

    if (block->magic != HEAP_MAGIC) return;
    if (block->free) return;

    block->free = 1;

    block_t* curr = free_list;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) break;
        if (curr->free && curr->next && curr->next->magic == HEAP_MAGIC && curr->next->free) {
            curr->size += BLOCK_HDR_SIZE + curr->next->size;
            curr->next  = curr->next->next;
            continue;
        }
        curr = curr->next;
    }
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    size_t total = nmemb * size;
    if (total / nmemb != size) return NULL;

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

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);
    if (block->magic != HEAP_MAGIC) return NULL;

    size = align_up(size, HEAP_ALIGN);

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
        return ptr;
    }

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
            return ptr;
        }
    }

    void* new_ptr = malloc(size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    return new_ptr;
}
