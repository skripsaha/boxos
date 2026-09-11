#ifndef TOUCH_H
#define TOUCH_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
#include "atomics.h"
#include "manifest.h"


typedef uint16_t TouchTag;
#define TOUCH_TAG_INVALID  ((TouchTag)0xFFFF)

typedef enum {
    TOUCH_REST      = 0,
    TOUCH_REACT     = 1,
    TOUCH_INTERRUPT = 2,
} TouchMode;

typedef enum {
    TOUCH_POLICY_EDGE    = 0,
    TOUCH_POLICY_LEVEL   = 1,
    TOUCH_POLICY_LATCHED = 2,
} TouchPolicy;

typedef enum {
    TOUCH_CAP_OPEN        = 0,
    TOUCH_CAP_OWNERS      = 1,
    TOUCH_CAP_KERNEL_ONLY = 2,
} TouchCapability;

#define TOUCH_FLAG_KERNEL  0x0001u
#define TOUCH_FLAG_USER    0x0002u
#define TOUCH_FLAG_TAGFS   0x0004u

typedef struct __packed {
    uint16_t tag_id;
    uint16_t flags;
    uint32_t source_pid;
    uint64_t payload_addr;
    uint32_t payload_len;
    uint64_t timestamp_tsc;
    uint32_t reserved;
} Touch;
_Static_assert(sizeof(Touch) == 32, "Touch must be 32 bytes");

struct process_t;
struct TouchBucket;

typedef struct TouchPending {
    uint16_t            tag_id;
    uint16_t            flags;
    uint32_t            source_pid;
    uint8_t             payload[64];
    uint32_t            plen;
    struct TouchPending *next;
} TouchPending;

typedef struct TouchSub {
    struct TouchSub *bucket_next;
    struct TouchSub *bucket_prev;
    struct TouchSub *proc_next;
    struct TouchSub *proc_prev;
    struct process_t *proc;
    struct TouchBucket *bucket;
    atomic_u32_t ref;
    uint16_t   tag_id;
    uint8_t    mode;
    uint8_t    has_pending;
    union {
        ManifestHandle manifest;
        struct {
            uint64_t handler_addr;
            uint64_t stack_top;
        } irq;
        uint64_t _raw;
    } u;
    uint32_t   pending_plen;
    uint8_t    pending_payload[64];
} TouchSub;

typedef struct TouchBucket {
    spinlock_t       lock;
    TouchSub        *head;
    volatile uint32_t sub_count;
    uint8_t          policy;
    uint8_t          capability;
    uint8_t          level_state;
    uint8_t          flags;
    uint16_t         tag_id;
    uint16_t         latched_plen;
    volatile uint32_t watch_count;
    uint8_t         *latched_payload;
    struct TouchWatch *watch_head;
} TouchBucket;
_Static_assert(sizeof(TouchBucket) <= 64, "TouchBucket must fit one cache line pair");

#define TOUCH_BUCKET_FLAG_REGISTERED  0x01u

#define TOUCH_BUCKET_L1_ENTRIES  256u
#define TOUCH_BUCKET_L2_ENTRIES  256u

bool TouchHasAnyListenersForTag(TouchTag tag_id);

bool TouchHasAnyListeners(void);

void TouchStatsSnapshot(uint64_t out[5]);

void TouchInit(void);

void    TouchTagResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare);

error_t TouchPolicySet(TouchTag tag_id, TouchPolicy policy, TouchCapability capability);
bool    TouchPolicyGet(TouchTag tag_id, TouchPolicy *out_policy, TouchCapability *out_cap);
uint8_t TouchPolicyLevelState(TouchTag tag_id);
void    TouchPolicySetLevelState(TouchTag tag_id, uint8_t state);

uint32_t TouchPolicyLatchedSnapshot(TouchTag tag_id, uint8_t *out_buf);

uint32_t TouchPublishId(TouchTag tag_id, const void *kpayload, uint32_t plen,
                        uint32_t source_pid, uint16_t flags);

uint32_t TouchPublishPair(TouchTag full_id, TouchTag bare_id,
                          const void *kpayload, uint32_t plen,
                          uint32_t source_pid, uint16_t flags);

void   TouchPublish(const char *tag, const void *kpayload, uint32_t plen);

void   TouchPublishIrqPair(TouchTag full_id, TouchTag bare_id,
                           const void *payload, uint16_t plen,
                           uint32_t source_pid, uint16_t flags);

void   TouchIrqRingAccount(void);

typedef void (*TouchWatchFn)(TouchTag tag_id, const void *payload,
                             uint32_t plen, uint32_t source_pid, void *ctx);

typedef struct TouchWatch TouchWatch;

TouchWatch *TouchWatchSet(TouchTag tag_id, TouchWatchFn fn, void *ctx);

void        TouchWatchClear(TouchWatch *w);

uint32_t    TouchWatchCount(TouchTag tag_id);

void   TouchRestDeliver(struct process_t *target, TouchTag tag_id,
                        const void *kpayload, uint32_t plen,
                        uint32_t source_pid, uint16_t flags);

void   TouchOwedHandOver(struct process_t *proc);

void   TouchOwedRelease(struct process_t *proc);

error_t TouchClaimSet(struct process_t *proc, TouchTag tag_id,
                      TouchMode mode, ManifestHandle manifest,
                      uint64_t handler_addr, uint64_t stack_top);
error_t TouchClaimClear(struct process_t *proc, TouchTag tag_id);

error_t TouchClaimAck(struct process_t *proc, TouchTag tag_id);

void   TouchCleanupProcess(struct process_t *proc, int32_t exit_code);

void   TouchIrqReturn(struct process_t *proc);

#endif