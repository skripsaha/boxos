#ifndef TOUCH_H
#define TOUCH_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
#include "atomics.h"
#include "manifest.h"

/*
 * Touch — tag-multicast event subsystem.
 *
 * Inverted-index design (2026-05-30 real-HW audit):
 *   tag_id → TouchBucket → doubly-linked list of TouchSub
 *
 * Publish walks ONLY the bucket for the target tag_id under a per-bucket
 * spinlock. No global process snapshot, no process_lock acquisition in the
 * hot path. Cost: O(N_subs_for_this_tag).
 *
 * Subscribe / unsubscribe are O(1): link/unlink a TouchSub on the bucket
 * list (and on the per-process list owned by proc).
 *
 * Buckets are addressed by a two-level table over the 16-bit tag_id space:
 *   g_bucket_l1[tag_id >> 8] → leaf[256]    (leaf lazily kmalloc'd)
 *   leaf[tag_id & 0xff]      → TouchBucket  (in-place, not pointer)
 * Always-resident root is 2 KiB; each populated leaf is 8 KiB. Cold tag_id
 * ranges keep their L2 pointer NULL → publish to those ids is one ACQUIRE
 * load + early-return.
 */

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

/* INTERRUPT-mode deferred event. While a process is already running an
 * IRQ handler, additional touches are queued here and replayed on
 * TouchIrqReturn. */
typedef struct TouchPending {
    uint16_t            tag_id;
    uint16_t            flags;
    uint32_t            source_pid;
    uint8_t             payload[64];
    uint32_t            plen;
    struct TouchPending *next;
} TouchPending;

/* One subscriber entry. Lives in two doubly-linked lists simultaneously:
 *   - bucket->head (publish walks this; ordered LIFO by subscribe time)
 *   - proc->subs_head (process cleanup walks this for O(N_my_claims) tear-down)
 *
 * Allocated by TouchClaimSet; freed only after the owning process drops to
 * ref_count==0 (so concurrent publishers walking the bucket list never
 * dereference a dead sub). TouchCleanupProcess unlinks subs from buckets
 * at process_destroy time; the kfree happens later in process_cleanup_immediate.
 */
typedef struct TouchSub {
    struct TouchSub *bucket_next;
    struct TouchSub *bucket_prev;
    struct TouchSub *proc_next;
    struct TouchSub *proc_prev;
    struct process_t *proc;
    struct TouchBucket *bucket;
    uint16_t   tag_id;
    uint8_t    mode;          /* TouchMode */
    uint8_t    has_pending;   /* LATCHED: 1 = pending slot occupied */
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

/* Per-tag publish/subscribe bucket. Locked individually — no global
 * serialization across tags. Holds the policy/cap/level inline so the
 * publish hot path needs no second table lookup.
 *
 * sub_count doubles as the O(1) "any subscriber?" gate: a load before
 * taking the lock skips publish entirely for tags nobody listens to.
 */
typedef struct TouchBucket {
    spinlock_t       lock;
    TouchSub        *head;
    volatile uint32_t sub_count;
    uint8_t          policy;       /* TouchPolicy */
    uint8_t          capability;   /* TouchCapability */
    uint8_t          level_state;  /* LEVEL: current state byte */
    uint8_t          flags;        /* bit 0: registered (policy/cap set) */
    uint16_t         tag_id;       /* mirror for diagnostics; == leaf index */
    uint16_t         _pad;
} TouchBucket;
_Static_assert(sizeof(TouchBucket) <= 64, "TouchBucket must fit one cache line pair");

#define TOUCH_BUCKET_FLAG_REGISTERED  0x01u

/* Number of distinct 256-id leaves in the two-level table. */
#define TOUCH_BUCKET_L1_ENTRIES  256u
#define TOUCH_BUCKET_L2_ENTRIES  256u

bool TouchHasAnyListenersForTag(TouchTag tag_id);

/* Global fast-test: any subscriber on any tag? Used by storage_ops /
 * write_job as a coarse gate before serializing tag_id snapshots for
 * publish — saves the snapshot work entirely when no one listens. */
bool TouchHasAnyListeners(void);

/* Diagnostic: snapshot publish-path counters.
 *   out[0] = publish calls hitting a live bucket
 *   out[1] = subscribers visited (post-snapshot, pre-deliver)
 *   out[2] = deliveries that succeeded (KResultPush ok)
 *   out[3] = touch_emit_payload returned 0 (no room in target cabin)
 *   out[4] = KResultPush exhausted retries / target destroyed
 */
void TouchStatsSnapshot(uint64_t out[5]);

void TouchInit(void);

/* Resolve a tag string into one or two TouchTags via the TagFS registry.
 * Interns the string if not yet present. Returns:
 *   *out_full = (key, value) id when string has explicit non-wildcard value
 *   *out_bare = (key, NULL)  id — also serves wildcard "key:..." subscribers
 * Either may be TOUCH_TAG_INVALID if the registry refuses interning.
 * Hot path callers should cache the returned ids in a static.
 */
void    TouchTagResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare);

/* Single-id convenience. Returns full_id if present, else bare_id, else
 * TOUCH_TAG_INVALID. Used by kernel publishers that don't care about wildcards. */
TouchTag TouchTagIntern(const char *tag);

/* Policy / capability management. */
error_t TouchPolicySet(TouchTag tag_id, TouchPolicy policy, TouchCapability capability);
bool    TouchPolicyGet(TouchTag tag_id, TouchPolicy *out_policy, TouchCapability *out_cap);
uint8_t TouchPolicyLevelState(TouchTag tag_id);
void    TouchPolicySetLevelState(TouchTag tag_id, uint8_t state);

/* Publish to a single tag_id. Hot path. */
void   TouchPublishId(TouchTag tag_id, const void *kpayload, uint32_t plen,
                      uint32_t source_pid, uint16_t flags);

/* Publish to BOTH full and bare ids in one shot (wildcard support).
 * Either may be TOUCH_TAG_INVALID — that side is skipped. */
void   TouchPublishPair(TouchTag full_id, TouchTag bare_id,
                        const void *kpayload, uint32_t plen,
                        uint32_t source_pid, uint16_t flags);

/* Kernel-side string convenience. Resolves once per call (no cache), so
 * cold paths use it freely; hot paths should cache via TouchTagIntern. */
void   TouchPublish(const char *tag, const void *kpayload, uint32_t plen);

/* IRQ-context Touch publisher.
 *
 * Drivers that run in IRQ context (PS/2 IRQ1, PIT IRQ0 repeat, xHCI MSI,
 * ATA INTRQ, etc.) MUST NOT call TouchPublish/TouchPublishId/TouchPublishPair
 * directly: those paths can take per-bucket spinlocks, kmalloc the L2 leaf
 * on cold tags, kmalloc a snapshot-growth buffer under contention, and
 * walk per-target VMM via vmm_ensure_user_page → pmm_alloc. All of those
 * acquire heap/PMM locks that are also held by non-IRQ kernel code on the
 * same core, which has historically deadlocked the kernel (see project
 * memory `irq_defer_done_2026_05_17` for the closed AHCI/SCI variant of
 * the same bug class).
 *
 * TouchPublishIrqPair copies the tag handles + a bounded payload into a
 * preallocated static slot, then enqueues the publish for K-Core
 * execution via irq_defer(). The slot ring is power-of-2 sized
 * (CONFIG_TOUCH_IRQ_RING_SIZE) and bump-allocated with circular
 * overwrite: extreme IRQ burst silently drops the OLDEST queued event.
 * That is the correct policy for events whose duty is best-effort
 * delivery (key-repeat ticks, USB port-change blips, ACPI GPE traffic).
 *
 * Bounded payload size is CONFIG_TOUCH_IRQ_PAYLOAD_MAX (small, fits the
 * largest in-kernel Touch event today). Larger payloads must be split
 * by the caller or queued via the regular K-Core publish path.
 *
 * Tag handles MUST be obtained outside IRQ context (via TouchTagResolve
 * during driver init) and cached. TouchTagResolve takes registry locks
 * and is NOT IRQ-safe; passing TOUCH_TAG_INVALID for both ids is a
 * harmless early-return (no publish). */
void   TouchPublishIrqPair(TouchTag full_id, TouchTag bare_id,
                           const void *payload, uint16_t plen,
                           uint32_t source_pid, uint16_t flags);

/* Diagnostic: number of times the static slot ring has wrapped past its
 * power-of-2 boundary. Each wrap is a STARTING POINT for potential drops
 * — a drop happens IFF the K-Core consumer has not yet drained the slot
 * that the producer just claimed. Counting the precise drop figure would
 * require per-slot generation atomics on the producer side, which adds
 * unwanted overhead to every IRQ-context publish; the wrap count is the
 * honest observable proxy and stays at 0 under realistic IRQ rates
 * (PS/2 ~30 Hz, xHCI ~Hz, ACPI rare-event) because irq_defer drains the
 * ring in microseconds while wraps take seconds. Snapshot only. */
uint64_t TouchPublishIrqWraps(void);

/* Push a Result to one specific subscriber. Used by REST mode. */
void   TouchRestDeliver(struct process_t *target, TouchTag tag_id,
                        const void *kpayload, uint32_t plen,
                        uint32_t source_pid, uint16_t flags);

/* Claim management — tag_id-only API. Kernel allocates TouchSub. */
error_t TouchClaimSet(struct process_t *proc, TouchTag tag_id,
                      TouchMode mode, ManifestHandle manifest,
                      uint64_t handler_addr, uint64_t stack_top);
error_t TouchClaimClear(struct process_t *proc, TouchTag tag_id);

/* LATCHED ack — clear the pending slot for proc's claim on tag_id. */
error_t TouchClaimAck(struct process_t *proc, TouchTag tag_id);

/* Tear down all of proc's subscriptions. Idempotent. Unlinks from buckets
 * immediately; sub structs are freed by TouchFinalizeProcess after ref_count
 * drops to 0. */
void   TouchCleanupProcess(struct process_t *proc);

/* Free per-proc TouchSub list. MUST be called only after ref_count == 0
 * (i.e. from process_cleanup_immediate) so no publisher still holds a
 * pointer to any of these subs. */
void   TouchFinalizeProcess(struct process_t *proc);

/* INTERRUPT mode return path. */
void   TouchIrqReturn(struct process_t *proc);

#endif /* TOUCH_H */
