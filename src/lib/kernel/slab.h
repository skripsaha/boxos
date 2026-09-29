#ifndef SLAB_H
#define SLAB_H

#include "ktypes.h"
#include "klib.h"
#include "kheap.h"

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
    uint8_t        charge;       // the KHeapCharge every object of this page is charged to
} SlabPage;

typedef struct {
    SlabPage*  partial;          // pages with free slots
    SlabPage*  full;             // pages with no free slots
    size_t     obj_size;         // size of each object in this class
    uint16_t   slots_per_page;   // precomputed
    spinlock_t lock;
    size_t     live_bytes;       // objects handed out and not back, at obj_size; under lock
} SlabClass;

/* One set of size classes per charge (kheap.h): a page carries the objects of
 * one class of cabins only, so what a class holds of the slab is whole pages
 * of its own, and a page goes back to the machine when its class lets go of
 * it — never kept alive by a stranger's object. */
typedef struct {
    SlabClass  classes[KHEAP_CHARGE_COUNT][SLAB_NUM_CLASSES];
    bool       initialized;
} SlabAllocator;

void  slab_init(void);
/* An object charged to whoever this core is serving, and admitted by the
 * share rule, as kmalloc charges and admits one (kheap.h). */
void* slab_alloc(size_t size);
void* SlabAllocFor(size_t size, KHeapCharge charge);
void  slab_free(void* ptr);
void  slab_activate_pull_map(void);
bool  slab_owns(void* ptr);
/* Bytes the slab holds for somebody right now: every object handed out and
 * not given back, at its class size. */
size_t SlabLiveBytes(void);
/* The pages one charge's size classes hold, in bytes. */
size_t SlabChargeBytes(KHeapCharge charge);
/* Boot-time proof that membership comes from the page registry, not page
 * content — panics if a content heuristic ever creeps back. */
void  slab_identity_selftest(void);

#endif // SLAB_H
