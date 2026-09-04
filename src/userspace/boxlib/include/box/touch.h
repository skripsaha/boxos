#ifndef BOX_TOUCH_H
#define BOX_TOUCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"

/*
 * Touch — tag-multicast event API (handle-based ABI, real-HW audit 2026-05-30).
 *
 * The legacy string-form syscalls were removed. Every call now takes a
 * TouchTag (uint16_t handle) returned by touch_intern. Cache the handle in
 * a static so hot loops never round-trip the registry.
 *
 *   TouchTagPair T_KB = touch_intern("keyboard");
 *   touch_claim(T_KB.bare, TOUCH_REST, 0, 0);
 *   touch_send(T_KB, &ev, sizeof(ev), 0);
 *   touch_await(T_KB.bare, &t, 1000);
 *
 * intern returns BOTH the (key:value) full_id and the (key) bare_id so
 * publishers can hit both buckets in one syscall and wildcard subscribers
 * (`key:...`) can subscribe to the bare_id alone.
 */

typedef uint16_t TouchTag;
#define TOUCH_TAG_INVALID  ((TouchTag)0xFFFF)

/* Canonical Touch tag names. Convention is `key:value` (TagFS bucket
 * format) — pass these to TOUCH_TAG_PAIR(...) / TOUCH_TAG_ID(...) so the
 * resolved handle is cached statically at the call site. Keep these in
 * lock-step with the kernel's TouchPublish() call sites. */
#define TOUCH_TAG_KEYBOARD          "keyboard"
#define TOUCH_TAG_PROCESS_DIED      "process:died"
#define TOUCH_TAG_PROCESS_SPAWNED   "process:spawned"
#define TOUCH_TAG_SYSTEM_SHUTDOWN   "system:shutdown"
#define TOUCH_TAG_SYSTEM_REBOOT     "system:reboot"
/* Two levels, and the difference matters. connect/disconnect are about a
 * socket: something changed at port 5, and at that instant nobody knows what.
 * arrived/left are about a device — addressed, configured, and carrying its
 * vendor, product and class, so a subscriber never has to go and ask. */
#define TOUCH_TAG_USB_CONNECT       "usb:connect"
#define TOUCH_TAG_USB_DISCONNECT    "usb:disconnect"
#define TOUCH_TAG_USB_ARRIVED       "usb:arrived"
#define TOUCH_TAG_USB_LEFT          "usb:left"

/* Payload of usb:arrived and usb:left. Kept in lock-step with the kernel's
 * xhci_touch_device_t. */
typedef struct {
    uint8_t  port;
    uint8_t  slot_id;
    uint8_t  speed;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t usb_version;       /* bcdUSB, so 0x0300 says SuperSpeed */
    uint32_t reserved;
} __attribute__((packed)) TouchUsbDevice;

typedef struct {
    TouchTag full;   /* (key, value) id — TOUCH_TAG_INVALID for wildcard strings */
    TouchTag bare;   /* (key, NULL) id  — also serves wildcard subscribers */
} TouchTagPair;

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

/* Touch — the userspace-facing copy of one TouchRing slot.
 *
 * Layout (2026-06-01 TouchRing migration, payload_addr removed for
 * production safety — see "Why no payload_addr" below):
 *
 *   tag_id, flags, source_pid, payload_len, _reserved, timestamp_tsc,
 *   payload[BOXOS_TOUCH_PAYLOAD_MAX]
 *
 * Total 120 bytes; payload is inline, accessible as `t.payload[]` or via
 * a cast on `t.payload` (decays to `uint8_t *`):
 *
 *     const Foo *p = (const Foo *)t.payload;   // OK — t.payload is in-struct
 *
 * Why no payload_addr:
 * Earlier drafts of this struct carried a self-pointer field
 * `payload_addr = (uint64_t)&payload[0]` for source-level back-compat
 * with the pre-TouchRing API where payload lived in a separate cabin VA.
 * That self-pointer is a footgun: copying a `Touch` to a different
 * storage location leaves payload_addr pointing into the ORIGINAL
 * location's payload bytes — silently broken once the original goes out
 * of scope or is overwritten. Removing the field forces every caller to
 * use `t.payload` (which is always in-struct, hence always correct
 * relative to whichever copy of the Touch they hold). The downside is a
 * one-time refactor of existing call sites — applied in the same
 * commit. */
#define BOXOS_TOUCH_PAYLOAD_MAX  96u

typedef struct __attribute__((packed)) {
    uint16_t tag_id;
    uint16_t flags;
    uint32_t source_pid;
    uint32_t payload_len;                    /* bytes valid in payload[] */
    uint32_t _reserved;                      /* alignment + future use */
    uint64_t timestamp_tsc;
    uint8_t  payload[BOXOS_TOUCH_PAYLOAD_MAX];
} Touch;

STATIC_ASSERT(sizeof(Touch) == 120, "Touch must be 120 bytes (24 hdr + 96 payload)");

typedef struct __attribute__((packed)) {
    uint32_t pid;          /* @0 — the dead process's pid                       */
    int32_t  exit_code;    /* @4 — disposition (proc_exit.h): >=0 / -1 / -2     */
    uint32_t generation;   /* @8 — pid-allocator generation of THIS incarnation.
                            *      A recycled pid reads a different generation,
                            *      so (pid, generation) names the exact death.
                            *      16-bit value widened to u32 (future-proof;
                            *      generation wrap at 2^16 reuses = accepted LOW). */
} TouchProcessDied;

STATIC_ASSERT(sizeof(TouchProcessDied) == 12,
              "TouchProcessDied must be 12 bytes (pid@0, exit_code@4, generation@8) "
              "— pid/exit_code offsets are frozen for all existing readers");

typedef struct __attribute__((packed)) {
    uint32_t pid;
    uint32_t parent_pid;
} TouchProcessSpawned;

typedef struct __attribute__((packed)) {
    uint32_t reason;
    uint32_t grace_ms;
} TouchSystemHalt;

typedef struct __attribute__((packed)) {
    uint8_t  port;
    uint8_t  speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t _reserved;
} TouchUsbConnect;

/* Resolve a tag string into a TouchTagPair. Interns the string if not
 * already present in the registry. One syscall round-trip; cache the
 * result in a static. Returns {TOUCH_TAG_INVALID, TOUCH_TAG_INVALID} on
 * failure (registry exhausted or no TagFS state). */
TouchTagPair touch_intern(const char *tag);

/* Convenience: pick the more specific id from a pair. Returns full when
 * present, otherwise bare. Use for claim/await/release/register/ack. */
static inline TouchTag touch_pair_choose(TouchTagPair p) {
    return (p.full != TOUCH_TAG_INVALID) ? p.full : p.bare;
}

int touch_claim(TouchTag tag, TouchMode mode, uint64_t manifest_or_handler,
                uint64_t stack_top);
int touch_release(TouchTag tag);

/* Publish to both ids in the pair (wildcard fan-out). Either may be
 * TOUCH_TAG_INVALID — that side is skipped by the kernel. */
int touch_send(TouchTagPair pair, const void *payload, uint32_t plen,
               uint32_t after_ms);

int touch_await(TouchTag tag, Touch *out, uint32_t timeout_ms);
int touch_irq_return(void);
int touch_register(TouchTag tag, TouchPolicy policy, TouchCapability capability);
int touch_ack(TouchTag tag);

/* Non-blocking pop from TouchRing.
 *
 * Reads ONE Touch event published by the kernel for this cabin and copies
 * it (metadata + inline payload bytes) into `*out`. Payload is accessed
 * directly via `out->payload[]` — no separate pointer, no lifetime hazard
 * across struct copies.
 *
 * Returns true on success (Touch delivered), false if the ring is
 * currently empty or no fresh slot has published yet. Non-blocking;
 * suitable for hot polling loops and as the inner step of touch_wait. */
bool touch_pop(Touch *out);

/* Block until a Touch arrives or `timeout_ms` elapses (0 = wait forever).
 *
 * Implementation: cooperative spin with WAITPKG UMWAIT fallback when
 * supported (Intel SDM Vol 2A). The kernel KTouchPush flips us from
 * PROC_WAITING to PROC_WORKING the moment a slot is published, so the
 * wait latency is bounded by the producer's release-store + IPI hop. */
bool touch_wait(Touch *out, uint32_t timeout_ms);

/* True if TouchRing has at least one published slot waiting to be
 * consumed. Non-blocking; useful as a fast "should I drain?" predicate
 * in display-like loops that mix TouchRing and ResultRing consumption. */
bool touch_available(void);

/* Tag-selective consume.
 *
 * The TouchRing is cabin-wide: the kernel delivery-filters (only tags this
 * cabin claimed are pushed here), but touch_pop / touch_wait are FIFO across
 * ALL claimed tags. A cabin with several distinct claims (e.g. one box::touch
 * subscription per tag) needs to pull the NEXT event of ONE tag while leaving
 * the others intact for their own consumers. These two calls do that: they
 * hand back the first event whose tag_id == `tag`, parking each non-matching
 * event in a small per-cabin stash so a later call for ITS tag still finds it
 * (FIFO order within a tag is preserved — the stash is consulted before the
 * ring). Pure userspace; no new kernel op.
 *
 * touch_try_pop_tag: non-blocking. Returns true if a matching event was
 *   delivered into `*out`, false if none is currently available.
 * touch_wait_tag:    blocks up to timeout_ms (0 = forever) for a matching
 *   event; returns false on timeout (or, for a forever wait, only under a
 *   flood of unconsumed other-tag events that fills the stash — drain your
 *   claimed tags).
 *
 * Per-strand, no locking: the stash is private to the calling strand (Ф21),
 * not shared across the cabin. touch_pop already routes per strand — each
 * strand drains its OWN TouchRing — and each strand parks its non-matching
 * events into its OWN stash (the main strand keeps the static stash; a spawned
 * strand a heap one cached in its StrandInfo). A strand is the sole writer of
 * its own stash, so two strands consuming Touch concurrently never share state
 * and still need no lock. */
bool touch_try_pop_tag(TouchTag tag, Touch *out);
bool touch_wait_tag(TouchTag tag, Touch *out, uint32_t timeout_ms);

/* Release the calling strand's per-strand Touch stash. Called from strand_exit
 * for a spawned strand (mirrors strand_pool_flush_self); idempotent, and a
 * no-op on the main strand or a strand that never consumed a tag. */
void touch_stash_free_self(void);

/* Diagnostic: TouchRing consumer-side counters (mirror of touch_ring_pop_stats
 * for users that only see box/touch.h). */
void touch_pop_stats(uint64_t out[8]);

/* How many events the kernel accepted for this strand's Touch ring but could
 * not fit in it. Non-zero means the ring is not the whole story: the kernel is
 * holding them in order and will hand them over at the next pocket this strand
 * submits. The waiters below act on it themselves; read it directly only for
 * diagnostics. */
uint64_t touch_owed(void);

void touch_await_stats(uint32_t out[7]);

/* Caching macros — for COMPILE-TIME CONSTANT tag strings only.
 *
 *   touch_send(TOUCH_TAG_PAIR("keyboard"), &ev, sizeof(ev), 0);
 *   touch_claim(TOUCH_TAG_ID("storage:ata:error"), TOUCH_REST, 0, 0);
 *
 * The pair is resolved once per syntactic call site and cached in a static.
 * Do NOT use these with dynamic strings (e.g. loop-generated tag names) —
 * the cache pins the first resolved value. Use touch_intern() directly there. */
#define TOUCH_TAG_PAIR(str) \
    ({ static TouchTagPair _tp = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID }; \
       if (_tp.full == TOUCH_TAG_INVALID && _tp.bare == TOUCH_TAG_INVALID) { \
           _tp = touch_intern(str); \
       } _tp; })

#define TOUCH_TAG_ID(str)  (touch_pair_choose(TOUCH_TAG_PAIR(str)))

#ifdef __cplusplus
}
#endif

#endif /* BOX_TOUCH_H */
