#ifndef MANIFEST_STAGE_H
#define MANIFEST_STAGE_H

#include "ktypes.h"
#include "error.h"


#define MANIFEST_STAGE_TIER_SCRATCH  0u
#define MANIFEST_STAGE_TIER_KMALLOC  1u
#define MANIFEST_STAGE_TIER_INVALID  0xFFu

#define MANIFEST_STAGE_SCRATCH_DEFAULT_SIZE  (64u * 1024u)
#define MANIFEST_STAGE_SCRATCH_FLOOR_SIZE    (16u * 1024u)

typedef struct ManifestStageGrant {
    void    *kbuf;
    uint32_t bytes;
    uint8_t  tier;
    uint8_t  _pad[3];
} ManifestStageGrant;

_Static_assert(sizeof(ManifestStageGrant) == 16,
               "ManifestStageGrant must stay 16 bytes (half cacheline)");

typedef struct ManifestStageStats {
    uint64_t scratch_acquires;
    uint64_t kmalloc_acquires;
    uint64_t reentry_fallbacks;
    uint64_t reject_too_large;
    uint64_t reject_alloc_failed;
    uint32_t max_bytes_seen;
    uint32_t scratch_capacity;
} ManifestStageStats;

typedef struct ManifestStage ManifestStage;

error_t ManifestStageInitAll(void);

ManifestStage *ManifestStageCurrent(void);

error_t ManifestStageAcquire(ManifestStage *st, uint32_t bytes,
                              ManifestStageGrant *out);

void    ManifestStageRelease(ManifestStage *st, ManifestStageGrant *grant);

error_t ManifestStageStatsGet(uint8_t kcore_id, ManifestStageStats *out);

void    ManifestStageDumpAll(void);

#endif