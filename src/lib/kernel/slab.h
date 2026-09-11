#ifndef SLAB_H
#define SLAB_H

#include "ktypes.h"
#include "klib.h"

#define SLAB_NUM_CLASSES     8
#define SLAB_PAGE_SIZE       4096
#define SLAB_LARGE_THRESHOLD 2048


#define SLAB_FREE_END 0xFFFF

typedef struct SlabPage {
    uint16_t       free_head;
    uint16_t       free_count;
    uint16_t       total_slots;
    uint16_t       obj_size;
    uintptr_t      page_phys;
    struct SlabPage* next;
} SlabPage;

typedef struct {
    SlabPage*  partial;
    SlabPage*  full;
    size_t     obj_size;
    uint16_t   slots_per_page;
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
void  slab_identity_selftest(void);

#endif