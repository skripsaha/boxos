#ifndef TOUCH_H
#define TOUCH_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
#include "manifest.h"

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

typedef struct {
    uint16_t   tag_id;
    uint8_t    mode;
    uint8_t    _pad;
    union {
        ManifestHandle manifest;
        struct {
            uint64_t handler_addr;
            uint64_t stack_top;
        } irq;
        uint64_t _raw;
    } u;
    /* LATCHED policy: pending unack'd slot */
    uint8_t  has_pending;
    uint8_t  pending_payload[64];
    uint32_t pending_plen;
} TouchClaim;

typedef struct TouchPending {
    uint16_t            tag_id;
    uint16_t            flags;
    uint32_t            source_pid;
    uint8_t             payload[64];
    uint32_t            plen;
    struct TouchPending *next;
} TouchPending;

struct process_t;

/* Per-tag policy/capability entry. */
typedef struct {
    uint16_t        tag_id;
    uint8_t         policy;
    uint8_t         capability;
    uint8_t         level_state;  /* For LEVEL policy: current state byte (0 or 1). */
    uint8_t         _pad[3];
} TouchPolicyEntry;

/* Global policy table — sparse hash, 256 buckets. */
#define TOUCH_POLICY_TABLE_SIZE 256

typedef struct {
    TouchPolicyEntry entries[TOUCH_POLICY_TABLE_SIZE];
    uint8_t          used[TOUCH_POLICY_TABLE_SIZE];
} TouchPolicyTable;

/* Fast test: returns true if any process is listening to any touch tag. */
bool TouchHasAnyListeners(void);

/* Diagnostic: snapshot all 5 publish-path counters into out[].
 *   out[0] = TouchPublishId calls with valid ear_presence
 *   out[1] = subscribers passed all gates → deliver_to_subscriber
 *   out[2] = TouchRestDeliver succeeded (KResultPush returned true)
 *   out[3] = TouchRestDeliver dropped because touch_emit_payload returned 0
 *   out[4] = TouchRestDeliver exhausted retries / target destroyed mid-deliver
 * Sums: subscribers_visited == delivered + emit_fail + push_fail. */
void TouchStatsSnapshot(uint64_t out[5]);

void   TouchInit(void);

error_t TouchPolicySet(uint16_t tag_id, TouchPolicy policy, TouchCapability capability);
bool    TouchPolicyGet(uint16_t tag_id, TouchPolicy *out_policy, TouchCapability *out_cap);
uint8_t TouchPolicyLevelState(uint16_t tag_id);
void    TouchPolicySetLevelState(uint16_t tag_id, uint8_t state);

void   TouchPublish(const char *tag, const void *kpayload, uint32_t plen);
void   TouchRestDeliver(struct process_t *target, uint16_t tag_id,
                          const void *kpayload, uint32_t plen,
                          uint32_t source_pid, uint16_t flags);

void   TouchPublishId(uint16_t tag_id, const void *kpayload, uint32_t plen,
                        uint32_t source_pid, uint16_t flags);

error_t TouchClaimSet(struct process_t *proc, uint16_t tag_id,
                        TouchMode mode, ManifestHandle manifest,
                        uint64_t handler_addr, uint64_t stack_top);
error_t TouchClaimClear(struct process_t *proc, uint16_t tag_id);

void   TouchCleanupProcess(struct process_t *proc);
void   TouchIrqReturn(struct process_t *proc);

#endif /* TOUCH_H */
