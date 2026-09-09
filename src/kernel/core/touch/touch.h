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
 * Allocated by TouchClaimSet with ref==1 — the base reference, owned by
 * proc-list membership. Each in-flight TouchPublishId snapshot takes one extra
 * ref under the bucket lock and drops it after delivery, so a publisher can
 * dereference the sub outside the lock without UAF. Whoever drops the last ref
 * (an owner-drop path or a lagging publisher) frees it via touch_sub_release.
 * Owner-drop paths (TouchClaimClear, the TouchClaimSet dup-undo,
 * TouchCleanupProcess) unlink the sub from its bucket and proc list first, then
 * drop the base ref.
 */
typedef struct TouchSub {
    struct TouchSub *bucket_next;
    struct TouchSub *bucket_prev;
    struct TouchSub *proc_next;
    struct TouchSub *proc_prev;
    struct process_t *proc;
    struct TouchBucket *bucket;
    /* Lifetime refcount mirroring process_t.ref_count: base ref = 1 held by
     * proc-list membership, +1 per in-flight publisher snapshot. Freed by
     * touch_sub_release when it reaches 0. */
    atomic_u32_t ref;
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
    uint16_t         latched_plen; /* LATCHED on-claim sync: byte count, 0 = no active latch */
    /* Kernel-side listeners on this tag (TouchWatch below). Counted
     * separately from sub_count because they are a different kind of
     * delivery — a call, not a Result — and the publish gate has to see
     * either. Lives in the padding the 8-byte-aligned pointer below left
     * behind, so the bucket does not grow for it. */
    volatile uint32_t watch_count;
    /* LATCHED on-claim sync: payload of the most recent first-of-cycle
     * publish, valid while latched_plen > 0. Heap-allocated (kmalloc) on
     * first publish, kfree'd when any subscriber acks. Pointer fits in
     * the bucket's remaining headroom — payload bytes themselves live
     * outside to keep TouchBucket within its cache-line budget. */
    uint8_t         *latched_payload;
    struct TouchWatch *watch_head;
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

/* Resolve a tag string into one or two TouchTags. THE READER DOOR: it looks
 * the name up in the kernel's Logbook first (logbook.h — occurrences the
 * kernel named), and only if that misses does it reach the mounted volume's
 * registry, interning there if needed. Returns:
 *   *out_full = (key, value) id when string has explicit non-wildcard value
 *   *out_bare = (key, NULL)  id — also serves wildcard "key:..." subscribers
 * Either may be TOUCH_TAG_INVALID if neither book will issue an id.
 * Hot path callers should cache the returned ids in a static.
 *
 * This is what SysTouchIntern gives a process, so a process subscribing by
 * name lands on the id the kernel publishes to. It never CREATES a Logbook
 * name: kernel code that means to name an occurrence calls
 * TouchLogbookResolve / TouchLogbookIntern instead.
 */
void    TouchTagResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare);

/* Policy / capability management. */
error_t TouchPolicySet(TouchTag tag_id, TouchPolicy policy, TouchCapability capability);
bool    TouchPolicyGet(TouchTag tag_id, TouchPolicy *out_policy, TouchCapability *out_cap);
uint8_t TouchPolicyLevelState(TouchTag tag_id);
void    TouchPolicySetLevelState(TouchTag tag_id, uint8_t state);

/*
 * LATCHED on-claim sync. Snapshot the currently-latched payload for `tag_id`
 * into `out_buf` (which must be ≥ BOXOS_TOUCH_PAYLOAD_MAX bytes) under the
 * bucket lock. Returns the byte count written (0 = no latched payload
 * active). Used by SysTouchAwait to deliver the latched value to a fresh
 * subscriber that joined after a publish but before any ack.
 */
uint32_t TouchPolicyLatchedSnapshot(TouchTag tag_id, uint8_t *out_buf);

/* Publish to a single tag_id. Hot path. Returns how many subscribers the
 * event was handed to — pushed into their ring, owed to it, or left latched
 * for them — so a sender can know that nobody heard: the display daemon
 * says each key again on the lane that listens, and a reader that died in
 * the middle of its reading leaves a lane nobody wears the tag of any more. */
uint32_t TouchPublishId(TouchTag tag_id, const void *kpayload, uint32_t plen,
                        uint32_t source_pid, uint16_t flags);

/* Publish to BOTH full and bare ids in one shot (wildcard support).
 * Either may be TOUCH_TAG_INVALID — that side is skipped. Returns the sum
 * of both sides' hand-overs. */
uint32_t TouchPublishPair(TouchTag full_id, TouchTag bare_id,
                          const void *kpayload, uint32_t plen,
                          uint32_t source_pid, uint16_t flags);

/* Kernel-side string convenience. Resolves once per call (no cache), so
 * cold paths use it freely; hot paths should cache via TouchLogbookIntern. */
void   TouchPublish(const char *tag, const void *kpayload, uint32_t plen);

/* IRQ-context Touch publisher.
 *
 * Drivers that run in IRQ context (PS/2 IRQ1, PIT IRQ0 repeat, xHCI MSI,
 * ATA INTRQ, etc.) MUST NOT call TouchPublish/TouchPublishId/TouchPublishPair
 * directly: those paths can take per-bucket spinlocks, kmalloc the L2 leaf
 * on cold tags, kmalloc a snapshot-growth buffer under contention, and
 * walk per-target VMM via vmm_ensure_user_page → pmm_alloc. All of those
 * acquire heap/PMM locks that are also held by non-IRQ kernel code on the
 * same core, which has deadlocked the kernel before (the AHCI/SCI variant
 * of the same bug class was the first one closed).
 *
 * TouchPublishIrqPair copies the tag handles + a bounded payload into a
 * preallocated static slot and knocks (baton.h, Knock): the K-Core that
 * owns the drain queue comes once per knock, reads every slot up to the
 * producers' cursor and publishes each in claim order. The slot ring is
 * power-of-2 sized (CONFIG_TOUCH_IRQ_RING_SIZE) and claimed by fetch_add:
 * an interrupt can neither wait for room nor allocate, so a burst that
 * outruns the K-Core by a whole ring overwrites the OLDEST queued events.
 * Every slot carries its generation and a done-mark, and the K-Core that
 * finds an event no longer there counts the loss exactly and says it on
 * the console — never in silence. The knock itself cannot be dropped.
 *
 * Bounded payload size is TOUCH_IRQ_PAYLOAD_MAX (small, fits the largest
 * in-kernel Touch event today). A larger payload is cut, and the K-Core
 * says so; split it at the caller or use the regular K-Core publish path.
 *
 * Tag handles MUST be obtained outside IRQ context (via TouchTagResolve
 * during driver init) and cached. TouchTagResolve takes registry locks
 * and is NOT IRQ-safe; passing TOUCH_TAG_INVALID for both ids is a
 * harmless early-return (no publish). */
void   TouchPublishIrqPair(TouchTag full_id, TouchTag bare_id,
                           const void *payload, uint16_t plen,
                           uint32_t source_pid, uint16_t flags);

/* The IRQ ring's account since boot — published, delivered, lost, still in
 * the ring — said once at a halt, where it can be checked against what the
 * listeners received. */
void   TouchIrqRingAccount(void);

/*
 * TouchWatch — the kernel's own ear.
 *
 * Touch could always be PUBLISHED by the kernel, but only LISTENED TO by a
 * process: every subscription path ends in a Result pushed into a cabin, a
 * REACT manifest, or a user IRQ handler, and all three need a process_t. The
 * kernel had no way to say "tell me when this tag is published", and paid for
 * the gap three separate times — the enumeration state machine, storage
 * completion, and the volume-arrival path — each time by building a private
 * one-off instead.
 *
 * What was missing was never delivery. Delivery the kernel already has
 * (the never-drop Baton for point-to-point hand-back, and a Knock on it
 * from IRQ context). What was missing is SUBSCRIPTION BY TAG, and that is all this
 * adds: a callback on a bucket, alongside the process subscribers, seen by the
 * same publish.
 *
 * A watch is not a subscription in the process sense and must not be used as
 * one. It is a multicast announcement, so it can have any number of listeners
 * and no listener is promised exclusivity. For a completion that exactly one
 * party is waiting for and whose loss hangs the waiter, use the storage
 * completion path instead: Touch is the room's public address system, not a
 * waiter bringing an order to one table.
 *
 * CONTRACT FOR THE CALLBACK:
 *   - Runs on the PUBLISHER's stack, on whichever core published, with no
 *     touch lock held. It may publish (bounded by the same kernel-stack
 *     headroom guard that bounds REACT re-entry) and it may call TouchWatch
 *     Set/Clear for OTHER tags.
 *   - It may run with interrupts disabled and with the publisher's own locks
 *     held, because a publisher is free to be inside either. Keep it short,
 *     take no lock that a slower path holds across I/O, and never block.
 *     Work that cannot honour that belongs on a K-Core: record the event and
 *     let the guide loop pick it up.
 *   - It must not clear its OWN watch from inside itself. Clearing waits for
 *     in-flight deliveries to finish, and one of them would be this call.
 */
typedef void (*TouchWatchFn)(TouchTag tag_id, const void *payload,
                             uint32_t plen, uint32_t source_pid, void *ctx);

typedef struct TouchWatch TouchWatch;

/* Listen for `tag_id` until cleared. Returns a handle, or NULL if the bucket
 * or the watch could not be allocated. Safe to call before any process
 * exists — this is the point, since the kernel listens during bring-up. */
TouchWatch *TouchWatchSet(TouchTag tag_id, TouchWatchFn fn, void *ctx);

/* Stop listening and free the handle. Unlinks first, so no publish started
 * after this point can see the watch; a publish already in flight holds a
 * reference and frees the watch when it finishes. Must not be called from
 * inside that watch's own callback. */
void        TouchWatchClear(TouchWatch *w);

/* Diagnostic: how many kernel ears are open on this tag. */
uint32_t    TouchWatchCount(TouchTag tag_id);

/* Push a Result to one specific subscriber. Used by REST mode. */
void   TouchRestDeliver(struct process_t *target, TouchTag tag_id,
                        const void *kpayload, uint32_t plen,
                        uint32_t source_pid, uint16_t flags);

/* Hand over what a full ring made this strand wait for.
 *
 * Called at the door — the syscall gate (idt.c syscall_handler) — and from
 * the publish path itself, so an event can never overtake one the ring
 * already refused. The gate, and not guide_process_pocket: the YIELD
 * short-circuit pops its pocket and reschedules without ever entering the
 * guide, and a yield is exactly how a consumer that has read the `owed` slip
 * comes back to ask for the rest. Pushes in FIFO order until
 * the ring refuses again; a refusal puts the node straight back at the head.
 * Takes no lock across KTouchPush and looks up no subscription: the queue
 * belongs to the ring, and the ring belongs to `proc`.
 *
 * Cheap to call unconditionally — it returns on a relaxed load when nothing
 * is owed, which is the case for every strand almost all of the time. */
void   TouchOwedHandOver(struct process_t *proc);

/* Free everything still owed to a dying strand and clear its slip. Called
 * from TouchCleanupProcess and again from the final teardown, once no
 * publisher can hold a reference. Idempotent. */
void   TouchOwedRelease(struct process_t *proc);

/* Claim management — tag_id-only API. Kernel allocates TouchSub. */
error_t TouchClaimSet(struct process_t *proc, TouchTag tag_id,
                      TouchMode mode, ManifestHandle manifest,
                      uint64_t handler_addr, uint64_t stack_top);
error_t TouchClaimClear(struct process_t *proc, TouchTag tag_id);

/* LATCHED ack — clear the pending slot for proc's claim on tag_id. */
error_t TouchClaimAck(struct process_t *proc, TouchTag tag_id);

/* Tear down all of proc's subscriptions, then publish process:died carrying
 * `exit_code` (the disposition — see proc_exit.h: >=0 clean / -1 killed /
 * -2 crashed). Idempotent on `proc->touch_cleaned`: only the FIRST call for a
 * strand publishes and decides the code; later calls no-op, so whichever
 * death-site cleans first sets the disposition. Unlinks each sub from its
 * bucket and proc list, then drops the base ref via touch_sub_release — the sub
 * is freed here unless a concurrent publisher still holds a snapshot ref, in
 * which case that publisher frees it on completion. */
void   TouchCleanupProcess(struct process_t *proc, int32_t exit_code);

/* INTERRUPT mode return path. */
void   TouchIrqReturn(struct process_t *proc);

#endif /* TOUCH_H */
