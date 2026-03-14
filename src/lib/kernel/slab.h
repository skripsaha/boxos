#ifndef SLAB_H
#define SLAB_H

#include "ktypes.h"
#include "klib.h"

#define SLAB_NUM_CLASSES     8
#define SLAB_PAGE_SIZE       4096
#define SLAB_LARGE_THRESHOLD 2048

// Size classes: 16, 32, 64, 128, 256, 512, 1024, 2048
// 4096 removed: obj_size == page_size leaves 0 slots after header.
// Each SlabPage header sits at offset 0 of a PMM page.
// Free slots use uint16_t offsets from page start (no rebasing needed).

#define SLAB_FREE_END 0xFFFF  // sentinel: end of free list

typedef struct SlabPage {
    uint16_t       free_head;    // offset of first free slot (or SLAB_FREE_END)
    uint16_t       free_count;   // slots free in this page
    uint16_t       total_slots;  // total slots per page
    uint16_t       obj_size;     // object size for this page
    uintptr_t      page_phys;    // physical address of this PMM page
    struct SlabPage* next;       // next slab page in class chain
} SlabPage;

typedef struct {
    SlabPage*  partial;          // pages with free slots
    SlabPage*  full;             // pages with no free slots
    size_t     obj_size;         // size of each object in this class
    uint16_t   slots_per_page;   // precomputed
    spinlock_t lock;
} SlabClass;

typedef struct {
    SlabClass  classes[SLAB_NUM_CLASSES];
    bool       initialized;
} SlabAllocator;

void  slab_init(void);
void* slab_alloc(size_t size);
void  slab_free(void* ptr);
void  slab_activate_pull_map(void);
bool  slab_owns(void* ptr);

#endif // SLAB_H
