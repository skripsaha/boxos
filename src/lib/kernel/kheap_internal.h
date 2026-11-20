#ifndef KHEAP_INTERNAL_H
#define KHEAP_INTERNAL_H

/*
 * The kernel heap's own parts, shared by the files it is written in and by
 * nobody else: klib_mem.c (kmalloc and kfree, HEAPFAIL, the guards, the
 * self-test), kheap_region.c (the regions and the large pieces),
 * kheap_charge.c (whom an allocation is charged to, and the share rule) and
 * slab.c (the small objects, one set of size classes per charge).
 */

#include "klib.h"
#include "kheap.h"

/* A pool block's header is 32 bytes and every payload a multiple of 32, so
 * every header and every payload in a region stays 32-aligned. */
#define KLIB_BLOCK_ALIGNMENT  32
#define KLIB_MAGIC_NUMBER     0xDEADBEEF
#define KLIB_MAGIC_FREE       0xFEEDFACE   /* set by kfree, checked by kfree to detect double-free in O(1) */

#define HEAP_CANARY_MAGIC     0xDEADBEEFCAFEBABEULL

/* A block of a pool region. `back` is the distance from the region's start
 * to this header, `below` the distance back to the block just below it in
 * its region (0 for the first) — distances, not pointers, so a block carries
 * nothing that has to be moved when the Pull Map takes over from the boot
 * identity mapping. A free block keeps its place on its region's free lists
 * in the first bytes of its payload (kheap_region.c), distances too. */
typedef struct mem_block {
    size_t   size;       /* payload bytes that follow this header */
    uint64_t below;
    uint64_t back;
    uint32_t magic;
    uint8_t  charge;     /* the KHeapCharge a live block is charged to */
    uint8_t  unused[3];
} mem_block_t;

_Static_assert(sizeof(mem_block_t) == KLIB_BLOCK_ALIGNMENT,
               "a pool block's header keeps every block 32-aligned");

/* A region: whole pages from the page allocator, reached through the Pull
 * Map, the header at its start and pool blocks after it. */
#define KHEAP_REGION_MAGIC    0x4B485247u   /* 'KHRG' */

typedef enum {
    KHEAP_REGION_BOOT = 0,     /* the first, before the Pull Map; kept for good */
    KHEAP_REGION_GROWN,        /* taken as the heap was asked; given back empty */
    KHEAP_REGION_RESERVE       /* the kernel's own paths'; kept for good */
} KHeapRegionKind;

/* A region's free blocks are kept by size: a list for each eighth of a power
 * of two — for each of the KHEAP_SIZE_STEPS steps of KHEAP_SIZE_CLASSES
 * powers of two of 32 bytes — two bitmaps saying which lists hold any, so a
 * block that fits is found, and one freed is merged with free neighbours, in
 * a time that does not depend on how many blocks the region holds. A list is
 * named by the distance of its first block from the region's start, 0 when
 * it is empty. Sixty-four MiB, the largest run the page allocator gives, is
 * 2^21 blocks of 32 bytes: twenty-four classes hold it. */
#define KHEAP_SIZE_CLASSES    24
#define KHEAP_SIZE_STEP_BITS  3
#define KHEAP_SIZE_STEPS      (1 << KHEAP_SIZE_STEP_BITS)

typedef struct KHeapRegion {
    uint32_t            magic;
    uint8_t             kind;     /* KHeapRegionKind */
    size_t              bytes;    /* the whole region, this header included */
    size_t              live;     /* bytes of live blocks, their headers included */
    uint32_t            classes;  /* bit c: some list of class c holds a block */
    uint8_t             steps[KHEAP_SIZE_CLASSES];   /* bit s: list s of the class holds one */
    uint32_t            lists[KHEAP_SIZE_CLASSES][KHEAP_SIZE_STEPS];
    struct KHeapRegion *next;     /* the next region of its list, oldest first */
} KHeapRegion;

#define KHEAP_REGION_HEAD     ALIGN_UP(sizeof(KHeapRegion), KLIB_BLOCK_ALIGNMENT)

/*
 * A large piece: an allocation of KHEAP_LARGE_MIN_PAGES pages or more is
 * whole pages of its own, taken from the page allocator and given back to it,
 * never carved from a region. Its head fills the first 64 bytes of its first
 * page; the payload follows, and an end canary after the payload.
 *
 * Eight pages is policy, and this is what it buys: rounding up to whole
 * pages then costs a piece less than one page in eight of what it takes,
 * while a piece that size carved from a region would keep the region from
 * going back for as long as it lived.
 */
#define KHEAP_LARGE_MIN_PAGES 8u
#define KHEAP_LARGE_MAGIC     0x4B48454150424947ULL

typedef struct {
    uint64_t magic;
    size_t   pages;
    size_t   size;       /* bytes asked */
    uint64_t charge;     /* KHeapCharge */
    uint64_t unused[3];
    uint64_t canary;     /* HEAP_CANARY_MAGIC, the word before the payload */
} KHeapLargeHead;

_Static_assert(sizeof(KHeapLargeHead) == 64, "a large piece's payload is 64-aligned");

/* kheap_region.c */
void   KHeapRegionsInit(void *boot, size_t bytes);
void   KHeapRegionsPullMap(void);
void  *KHeapPoolTake(size_t bytes, KHeapCharge charge, bool reserve);
void   KHeapPoolGive(void *payload);
bool   KHeapPoolOwns(const void *addr);
bool   KHeapIsLarge(size_t size);
void  *KHeapLargeTake(size_t size, KHeapCharge charge);
bool   KHeapLargeOwns(const void *ptr);
void   KHeapLargeGive(void *ptr);
/* The bytes a large piece holds for its owner: its size as asked, or as last
 * resized. */
size_t KHeapLargeSize(void *ptr);
/* The large piece made to hold `size` bytes in the pages it has: true when
 * that takes as many pages as it holds — its size and end canary moved, the
 * bytes before them as they were — false, nothing changed, when it would take
 * another count of pages. */
bool   KHeapLargeResize(void *ptr, size_t size);
size_t KHeapLiveOutsideSlab(void);
size_t KHeapRegionSlack(void);

/* kheap_charge.c */
KHeapScope KHeapScopeHere(void);
bool       KHeapChargeAdmits(KHeapCharge charge, uint32_t pid, size_t bytes);
void       KHeapChargeAdd(KHeapCharge charge, size_t bytes);
void       KHeapChargeSub(KHeapCharge charge, size_t bytes);

#endif /* KHEAP_INTERNAL_H */
