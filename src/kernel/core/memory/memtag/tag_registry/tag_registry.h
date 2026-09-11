
#ifndef MEMTAG_TAG_REGISTRY_H
#define MEMTAG_TAG_REGISTRY_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"

#define MEMTAG_INVALID_TAG_ID      0xFFFF
#define MEMTAG_MAX_TAG_ID          0xFFFE
#define MEMTAG_REG_BUCKETS         1024
#define MEMTAG_KEY_BUCKETS         256
#define MEMTAG_REG_INITIAL_BY_ID   64

#define MEMTAG_FLAG_HAS_VALUE         0x01
#define MEMTAG_FLAG_KERNEL_RESERVED   0x02
#define MEMTAG_FLAG_WILDCARD_ROOT     0x04
#define MEMTAG_FLAG_GUARD             0x08

typedef struct MemTagRegistryEntry {
    char       *key;
    char       *value;
    uint16_t    tag_id;
    uint8_t     flags;
    uint32_t    region_count;
} MemTagRegistryEntry;

typedef struct MemTagRegistryNode {
    uint16_t                       tag_id;
    struct MemTagRegistryNode     *next;
} MemTagRegistryNode;

typedef struct MemTagKeyGroup {
    char                       *key;
    uint16_t                   *tag_ids;
    uint32_t                    count;
    uint32_t                    capacity;
    struct MemTagKeyGroup      *next;
} MemTagKeyGroup;

typedef struct MemTagRegistry {
    MemTagRegistryNode    **buckets;
    uint32_t                bucket_count;

    MemTagRegistryEntry   **by_id;
    uint32_t                by_id_capacity;

    MemTagKeyGroup        **key_buckets;
    uint32_t                key_bucket_count;

    uint32_t                total_tags;
    uint16_t                next_id;

    volatile uint64_t       generation;

    spinlock_t              lock;
} MemTagRegistry;


error_t       MemTagRegistryInit(MemTagRegistry *reg);
void          MemTagRegistryShutdown(MemTagRegistry *reg);


uint16_t      MemTagRegistryIntern(MemTagRegistry *reg,
                                    const char *key, const char *value);

uint16_t      MemTagRegistryInternStr(MemTagRegistry *reg,
                                       const char *key_colon_value);

uint16_t      MemTagRegistryLookup(MemTagRegistry *reg,
                                    const char *key, const char *value);
uint16_t      MemTagRegistryLookupStr(MemTagRegistry *reg,
                                       const char *key_colon_value);


const char  *MemTagRegistryKey(MemTagRegistry *reg, uint16_t tag_id);
const char  *MemTagRegistryValue(MemTagRegistry *reg, uint16_t tag_id);

MemTagKeyGroup *MemTagRegistryKeyGroup(MemTagRegistry *reg, const char *key);

uint64_t      MemTagRegistryGeneration(MemTagRegistry *reg);
uint32_t      MemTagRegistryTotalTags(MemTagRegistry *reg);


void          MemTagRegistryRefInc(MemTagRegistry *reg, uint16_t tag_id);
void          MemTagRegistryRefDec(MemTagRegistry *reg, uint16_t tag_id);
uint32_t      MemTagRegistryRefCount(MemTagRegistry *reg, uint16_t tag_id);


void          MemTagRegistryMarkReserved(MemTagRegistry *reg, uint16_t tag_id);
bool          MemTagRegistryIsReserved(MemTagRegistry *reg, uint16_t tag_id);

void          MemTagRegistryMarkGuard(MemTagRegistry *reg, uint16_t tag_id);
void          MemTagRegistryClearGuard(MemTagRegistry *reg, uint16_t tag_id);
bool          MemTagRegistryIsGuard(MemTagRegistry *reg, uint16_t tag_id);


void          MemTagRegistryDump(MemTagRegistry *reg);

#endif