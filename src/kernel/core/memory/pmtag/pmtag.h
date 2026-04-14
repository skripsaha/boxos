#ifndef PMTAG_H
#define PMTAG_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"

// Bits 0-15 reserved for kernel use. Bits 16-63 available for user/driver tags.
#define PHYS_TAG_DMA32    (1ULL <<  0)
#define PHYS_TAG_USER     (1ULL <<  1)
#define PHYS_TAG_HIGH     (1ULL <<  2)
#define PHYS_TAG_KERNEL   (1ULL <<  3)
#define PHYS_TAG_MMIO     (1ULL <<  4)

typedef struct {
    uint64_t  *tag_map;
    uint64_t  *bands[64];
    size_t     page_count;
    uintptr_t  base_phys;
    uintptr_t  mem_end;
    size_t     band_words;
    spinlock_t lock;
    bool       initialized;
} PhysTagTable;

extern PhysTagTable g_pmtag;

error_t  PhysTagInit(void);

void     PhysTagSet(uintptr_t start, uintptr_t end, uint64_t tags);
void     PhysTagClear(uintptr_t start, uintptr_t end, uint64_t tags);
uint64_t PhysTagGet(uintptr_t phys_addr);

void*    PhysAllocTagged(size_t pages, uint64_t required_tags);
void     PhysAllocTaggedFree(void *addr, size_t pages);

bool     PhysTagFindSpan(uint64_t required_tags, uintptr_t *out_start, uintptr_t *out_end);

void     PhysTagDump(void);

#endif // PMTAG_H
