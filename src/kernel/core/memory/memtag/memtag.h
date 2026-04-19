#ifndef MEMTAG_H
#define MEMTAG_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"

#define MEMTAG_NAME_MAX         48
#define MEMTAG_REGISTRY_CAP     128
#define MEMTAG_REGION_CAP       2048
#define MEMTAG_QUERY_RESULT_MAX 512
#define MEMTAG_ID_NONE          0xFF

// Words needed to cover MEMTAG_REGISTRY_CAP (128) tags as a bitmap: 128/64 = 2
#define MEMTAG_TAG_WORDS        (MEMTAG_REGISTRY_CAP / 64)
// Words needed to cover MEMTAG_REGION_CAP (2048) slots as a bitmap: 2048/64 = 32
#define MEMTAG_REG_WORDS        (MEMTAG_REGION_CAP / 64)

typedef uint8_t memtag_id_t;

typedef struct {
    uintptr_t   base_phys;
    size_t      pages;
    uint64_t    tag_bits[MEMTAG_TAG_WORDS];   // bit N set ↔ tag N present
    bool        active;
} MemTagRegion;

typedef struct {
    MemTagRegion *regions[MEMTAG_QUERY_RESULT_MAX];
    size_t        count;
} MemTagResult;

error_t      MemTagInit(void);

memtag_id_t  MemTagRegister(const char *name);
memtag_id_t  MemTagLookup(const char *name);
const char  *MemTagName(memtag_id_t id);

// Register a physical region with NULL-terminated variadic string tags.
error_t      MemTagAdd(uintptr_t base_phys, size_t pages, ...);

error_t      MemTagAddToRegion(uintptr_t base_phys, const char *tag);
error_t      MemTagRemoveFromRegion(uintptr_t base_phys, const char *tag);
void         MemTagRemoveRegion(uintptr_t base_phys);
MemTagRegion *MemTagFindRegion(uintptr_t phys_addr);

// Queries — arrays must be NULL-terminated.
MemTagResult MemTagQueryAnd(const char *const *tags);
MemTagResult MemTagQueryOr(const char *const *tags);
MemTagResult MemTagQueryMixed(const char *const *required,
                               const char *const *any,
                               const char *const *excluded);

// Convenience macros — build NULL-terminated arrays as compound literals on stack.
#define MemTagAnd(...) MemTagQueryAnd((const char *const[]){__VA_ARGS__, NULL})
#define MemTagOr(...)  MemTagQueryOr((const char *const[]){__VA_ARGS__, NULL})

// Allocation helper — called from pmm_alloc(pages, "string") path.
void *_pmm_alloc_memtag(size_t pages, const char *tag);

void MemTagDump(void);

void MemTagStressTest(void);

#endif // MEMTAG_H
