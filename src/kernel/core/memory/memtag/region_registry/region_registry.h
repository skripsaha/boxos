
#ifndef MEMTAG_REGION_REGISTRY_H
#define MEMTAG_REGION_REGISTRY_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"
#include "tag_registry.h"

#define MEMTAG_INVALID_REGION_ID    0xFFFFFFFFu
#define MEMTAG_REGION_INITIAL_CAP   1024u
#define MEMTAG_REGION_MAX_CAP       (1u << 20)
#define MEMTAG_REGION_BUCKETS       256u
#define MEMTAG_REGION_TAGS_INITIAL  4u

#define MEMTAG_REGION_FLAG_ACTIVE       0x0001
#define MEMTAG_REGION_FLAG_PINNED       0x0002
#define MEMTAG_REGION_FLAG_KERNEL       0x0004
#define MEMTAG_REGION_FLAG_CABIN        0x0008
#define MEMTAG_REGION_FLAG_PHYSICAL     0x0010
#define MEMTAG_REGION_FLAG_VIRTUAL      0x0020

#define MEMTAG_ATTACH_ACTIVE     0u
#define MEMTAG_ATTACH_REVOKED    1u

#define MEMTAG_ATTACH_CLASS_4K   0u
#define MEMTAG_ATTACH_CLASS_2M   1u

typedef struct MemRegionAttach {
    void                     *ctx;
    uintptr_t                 va_base;
    uint32_t                  region_id;
    uint32_t                  pages;
    uint64_t                  orig_flags;
    uint8_t                   page_class;
    uint8_t                   state;
    uint16_t                  reserved16;
    uint32_t                  reserved32;
    struct MemRegionAttach   *next;
} MemRegionAttach;

typedef struct MemRegion {
    uintptr_t        base_phys;
    uintptr_t        base_virt;
    void            *ctx;
    size_t           pages;
    uint16_t        *tag_ids;
    uint16_t         tag_count;
    uint16_t         tag_cap;
    uint16_t         flags;
    uint32_t         generation;
    MemRegionAttach *attach_head;
} MemRegion;

typedef struct MemRegionRegistry {
    MemRegion       *slots;
    uint32_t         slot_cap;
    uint32_t         slot_count;

    uint32_t        *free_stack;
    uint32_t         free_top;
    uint32_t         free_cap;

    uint32_t        *id_by_page;
    size_t           page_count;

    volatile uint64_t generation;

    spinlock_t       lock;
    spinlock_t       bucket_locks[MEMTAG_REGION_BUCKETS];
} MemRegionRegistry;


error_t          MemRegionRegistryInit(MemRegionRegistry *reg, size_t mem_end);
void             MemRegionRegistryShutdown(MemRegionRegistry *reg);


uint32_t         MemRegionRegistryCreate(MemRegionRegistry *reg,
                                          uintptr_t base_phys,
                                          uintptr_t base_virt,
                                          void *ctx,
                                          size_t pages,
                                          uint16_t flags);

void             MemRegionRegistryDestroy(MemRegionRegistry *reg, uint32_t region_id);


bool             MemRegionRegistryIsActive(MemRegionRegistry *reg, uint32_t region_id);

uint32_t         MemRegionRegistryFromPhys(MemRegionRegistry *reg, uintptr_t phys);

MemRegion       *MemRegionRegistrySlot(MemRegionRegistry *reg, uint32_t region_id);

typedef struct {
    uintptr_t   base_phys;
    uintptr_t   base_virt;
    size_t      pages;
    uint16_t    tag_count;
    uint16_t    flags;
    uint32_t    generation;
} MemRegionSnapshot;

error_t          MemRegionRegistrySnapshot(MemRegionRegistry *reg,
                                            uint32_t region_id,
                                            MemRegionSnapshot *out);


error_t          MemRegionRegistryAddTag(MemRegionRegistry *reg,
                                          uint32_t region_id, uint16_t tag_id);
error_t          MemRegionRegistryRemoveTag(MemRegionRegistry *reg,
                                             uint32_t region_id, uint16_t tag_id);
bool             MemRegionRegistryHasTag(MemRegionRegistry *reg,
                                          uint32_t region_id, uint16_t tag_id);

size_t           MemRegionRegistryListTags(MemRegionRegistry *reg,
                                            uint32_t region_id,
                                            uint16_t *out, size_t max);


error_t          MemRegionRegistryAttach(MemRegionRegistry *reg,
                                          uint32_t region_id,
                                          void *ctx,
                                          uintptr_t va_base,
                                          uint32_t pages,
                                          uint8_t page_class,
                                          uint64_t orig_flags);

error_t          MemRegionRegistryDetach(MemRegionRegistry *reg,
                                          uint32_t region_id,
                                          void *ctx,
                                          uintptr_t va_base);

size_t           MemRegionRegistrySnapshotAttachs(MemRegionRegistry *reg,
                                                   uint32_t region_id,
                                                   MemRegionAttach *out,
                                                   size_t max);

error_t          MemRegionRegistrySetAttachState(MemRegionRegistry *reg,
                                                  uint32_t region_id,
                                                  void *ctx,
                                                  uintptr_t va_base,
                                                  uint8_t new_state);

size_t           MemRegionRegistryDetachAllForCtx(MemRegionRegistry *reg,
                                                   void *ctx);


uint32_t         MemRegionRegistryActiveCount(MemRegionRegistry *reg);
uint64_t         MemRegionRegistryGeneration(MemRegionRegistry *reg);
void             MemRegionRegistryDump(MemRegionRegistry *reg,
                                        MemTagRegistry *tag_reg);

#endif