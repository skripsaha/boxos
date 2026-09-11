
#ifndef MEMTAG_H
#define MEMTAG_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"

#include "tag_registry.h"
#include "region_registry.h"
#include "tag_bitmap.h"


error_t       MemTagInit(void);
bool          MemTagIsInitialized(void);

void          MemTagEnableTouchPublish(void);


uint16_t      MemTagInternStr(const char *kv);
uint16_t      MemTagResolveStr(const char *kv);
uint16_t      MemTagIntern(const char *key, const char *value);
uint16_t      MemTagResolve(const char *key, const char *value);
const char   *MemTagKey(uint16_t tag_id);
const char   *MemTagValue(uint16_t tag_id);


uint32_t      MemRegionCreate(uintptr_t base_phys,
                               uintptr_t base_virt,
                               void *ctx,
                               size_t pages,
                               uint16_t flags);

void          MemRegionDestroy(uint32_t region_id);

error_t       MemRegionInfo(uint32_t region_id, MemRegionSnapshot *out);

bool          MemRegionIsActive(uint32_t region_id);

uint32_t      MemRegionFromPhys(uintptr_t phys);

uint32_t      MemRegionFromVirt(void *ctx, uintptr_t virt);


error_t       MemTagApply(uint32_t region_id, const char *tag_str);
error_t       MemTagApplyMany(uint32_t region_id, const char *const *tag_strs);

error_t       MemTagClear(uint32_t region_id, const char *tag_str);

bool          MemRegionHasTag(uint32_t region_id, uint16_t tag_id);
bool          MemRegionHasTagStr(uint32_t region_id, const char *tag_str);

error_t       MemTagApplyByPhys(uintptr_t base_phys, size_t pages,
                                 const char *tag_str);

error_t       MemTagClearByPhys(uintptr_t base_phys, const char *tag_str);

size_t        MemRegionListTags(uint32_t region_id, uint16_t *out, size_t max);


typedef struct {
    uint32_t    region_ids[512];
    size_t      count;
} MemTagResult;

MemTagResult  MemTagQueryAnd_(const char *const *tag_strs);

MemTagResult  MemTagQueryOr_(const char *const *tag_strs);

MemTagResult  MemTagQueryMixed_(const char *const *required,
                                 const char *const *any,
                                 const char *const *excluded);

#define MemTagAnd(...)  MemTagQueryAnd_((const char *const[]){__VA_ARGS__, NULL})
#define MemTagOr(...)   MemTagQueryOr_((const char *const[]){__VA_ARGS__, NULL})

void MemTagQueryAndInto_(const char *const *tag_strs, MemTagResult *out);
void MemTagQueryOrInto_(const char *const *tag_strs, MemTagResult *out);
void MemTagQueryMixedInto_(const char *const *required,
                            const char *const *any,
                            const char *const *excluded,
                            MemTagResult *out);

#define MemTagAndInto(out, ...) \
    MemTagQueryAndInto_((const char *const[]){__VA_ARGS__, NULL}, (out))
#define MemTagOrInto(out, ...)  \
    MemTagQueryOrInto_((const char *const[]){__VA_ARGS__, NULL}, (out))


void         *MemTagPmmAlloc(size_t pages, const char *tag_str);
void         *MemTagPmmAllocZero(size_t pages, const char *tag_str);

void          MemTagPmmFreed(uintptr_t base_phys, size_t pages);


error_t       MemTagSetGuard(const char *tag_str, bool guard);
bool          MemTagIsGuard(uint16_t tag_id);

error_t       MemCabinGrant(uint32_t pid, uint16_t tag_id);
error_t       MemCabinRevoke(uint32_t pid, uint16_t tag_id);
bool          MemCabinHolds(uint32_t pid, uint16_t tag_id);

size_t        MemCabinListTags(uint32_t pid, uint16_t *out, size_t max);

bool          MemRegionAccessAllowed(uint32_t pid, uint32_t region_id);

uint16_t      MemRegionFirstMissingGuard(uint32_t pid, uint32_t region_id);

bool          MemTagEnforce(uint32_t pid, uintptr_t va, uint32_t region_id);

bool          MemTagEnforcePhys(uint32_t pid, uintptr_t phys);


error_t       MemRegionAttachCabin(uint32_t region_id, void *ctx,
                                    uintptr_t va_base, uint32_t pages,
                                    uint8_t page_class, uint64_t orig_flags);

error_t       MemRegionDetachCabin(uint32_t region_id, void *ctx,
                                    uintptr_t va_base);

size_t        MemTagDetachAllForCabin(void *ctx);

size_t        MemCabinEnforceRevokePost(uint32_t pid, uint16_t tag_id);

size_t        MemCabinEnforceGrantPost(uint32_t pid, uint16_t tag_id);

size_t        MemTagSweepGuard(uint16_t tag_id, bool guard_on);


#define MEMTAG_PTE_REGION_SHIFT     52
#define MEMTAG_PTE_REGION_BITS      7
#define MEMTAG_PTE_REGION_MAX       ((1u << MEMTAG_PTE_REGION_BITS) - 1u)
#define MEMTAG_PTE_REGION_MASK      (((uint64_t)MEMTAG_PTE_REGION_MAX) \
                                     << MEMTAG_PTE_REGION_SHIFT)

uint32_t      MemRegionFromPte(uint64_t pte_value, uintptr_t phys);

bool          MemTagVerifyPteMetadataBits(void);


uint8_t       MemRegionEffectivePkey(uint32_t region_id);

error_t       MemTagApplyPkey(uint32_t region_id, uint8_t pkey);

size_t        MemTagSweepPkey(uint32_t region_id, uint8_t new_pkey);


bool          MemTagVerifyPatMsr(void);

void          MemTagDumpMtrrLayout(void);


typedef struct {
    uint32_t  tag_count;
    uint32_t  region_active;
    uint32_t  region_slot_count;
    uint32_t  region_slot_cap;
    uint64_t  registry_generation;
    uint64_t  region_generation;
    uint64_t  bitmap_generation;
    uint64_t  cache_hits;
    uint64_t  cache_misses;
} MemTagStats;

void          MemTagGetStats(MemTagStats *out);


void          MemTagDump(void);
void          MemTagStressTest(void);


MemTagRegistry      *MemTagGetRegistry(void);
MemRegionRegistry   *MemTagGetRegionRegistry(void);
MemTagBitmapIndex   *MemTagGetBitmap(void);

#endif