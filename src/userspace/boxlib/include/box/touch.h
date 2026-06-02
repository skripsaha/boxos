#ifndef BOX_TOUCH_H
#define BOX_TOUCH_H

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
#define TOUCH_TAG_USB_CONNECT       "usb:connect"
#define TOUCH_TAG_USB_DISCONNECT    "usb:disconnect"

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
    uint32_t pid;
    int32_t  exit_code;
} TouchProcessDied;

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

/* Diagnostic: TouchRing consumer-side counters (mirror of touch_ring_pop_stats
 * for users that only see box/touch.h). */
void touch_pop_stats(uint64_t out[8]);

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
       if (_tp.full == TOUCH_TAG_INVALID && _tp.bare == TOUCH_TAG_INVALID) \
           _tp = touch_intern(str); _tp; })

#define TOUCH_TAG_ID(str)  (touch_pair_choose(TOUCH_TAG_PAIR(str)))

#endif /* BOX_TOUCH_H */
