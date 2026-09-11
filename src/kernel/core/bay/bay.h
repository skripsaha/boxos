#ifndef BAY_H
#define BAY_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
#include "atomics.h"


#define BAY_OPEN        0x00u
#define BAY_CREATE      0x01u
#define BAY_RO          0x02u

#define BAY_ENCRYPTED   0x04u

#define TME_QUOTA_PER_PROC  8u

#define BAY_FLAGS_MASK   (BAY_CREATE | BAY_RO | BAY_ENCRYPTED)

#define BAY_MAX_OPEN_SIZE   (16ULL * 1024 * 1024 * 1024)

typedef struct BayClaim BayClaim;
typedef struct BayObject BayObject;
struct process_t;
struct cabin_t;

struct BayObject {
    BayObject  *bucket_next;
    uint16_t    tag_id;
    uint16_t    page_class;
    uint16_t    tme_keyid;
    uint16_t    _pad0;
    uint64_t    total_size;
    uint64_t    user_size;
    uint64_t    chunk_size;
    uint32_t    chunk_count;
    uint32_t    flags;
    uint64_t   *chunks;
    uint32_t    ref_count;
    uint32_t    _pad1;
    uint64_t    create_pid;
    uint64_t    create_tsc;
};

struct BayClaim {
    BayObject  *bay;
    BayClaim   *cabin_next;
    uint64_t    user_va_base;
    uint32_t    flags;
    uint32_t    _pad;
};

void BayInit(void);

error_t BayOpenInternal(struct process_t *proc,
                        const char *tag,
                        uint64_t requested_size,
                        uint32_t flags,
                        uint64_t *out_user_va,
                        uint64_t *out_actual_size);

error_t BayReleaseInternal(struct process_t *proc, uint64_t user_va);

uint64_t BaySizeInternal(struct process_t *proc, uint64_t user_va);

void BayCleanupCabin(struct cabin_t *cabin);

void BayStatsSnapshot(uint64_t out[4]);

#endif