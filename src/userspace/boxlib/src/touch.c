#include "box/touch.h"
#include "box/core/manifest.h"
#include "box/core/result.h"
#include "box/core/notify.h"
#include "box/core/pocket.h"
#include "box/cpu.h"
#include "box/string.h"
#include "box/error.h"

static inline uint64_t touch_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Opcodes must match system_deck.h */
#define SYSTEM_OP_TOUCH_CLAIM      0x51
#define SYSTEM_OP_TOUCH_RELEASE    0x52
#define SYSTEM_OP_TOUCH_SEND       0x53
#define SYSTEM_OP_TOUCH_AWAIT      0x54
#define SYSTEM_OP_TOUCH_IRQ_RETURN 0x55
#define SYSTEM_OP_TOUCH_REGISTER   0x56
#define SYSTEM_OP_TOUCH_ACK        0x57

#define DECK_SYSTEM 0xFF

int touch_claim(const char *tag, TouchMode mode, uint64_t manifest_or_handler,
                uint64_t stack_top)
{
    if (!tag) return -ERR_INVALID_ARGS;

    uint8_t params[17];
    uint16_t param_size;
    params[0] = (uint8_t)mode;

    if (mode == TOUCH_REACT) {
        memcpy(params + 1, &manifest_or_handler, sizeof(uint64_t));
        param_size = 9;
    } else if (mode == TOUCH_INTERRUPT) {
        memcpy(params + 1, &manifest_or_handler, sizeof(uint64_t));
        memcpy(params + 9, &stack_top,           sizeof(uint64_t));
        param_size = 17;
    } else {
        param_size = 1;
    }

    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_CLAIM,
                   params, param_size,
                   tag, (uint32_t)(strlen(tag) + 1),
                   NULL, 0, NULL,
                   1000, NULL);
}

int touch_release(const char *tag)
{
    if (!tag) return -ERR_INVALID_ARGS;

    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_RELEASE,
                   NULL, 0,
                   tag, (uint32_t)(strlen(tag) + 1),
                   NULL, 0, NULL,
                   1000, NULL);
}

int touch_send(const char *tag, const void *payload, uint32_t plen, uint32_t after_ms)
{
    if (!tag) return -ERR_INVALID_ARGS;

    uint8_t mbuf[300];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return -ERR_INVALID_ARGS;

    Crate crates[2];
    uint16_t cc = 0;

    CrateSetInput(&crates[cc], (void *)tag, (uint32_t)(strlen(tag) + 1));
    uint16_t tag_idx = cc++;

    uint16_t payload_idx = CRATE_INDEX_NONE;
    if (payload && plen > 0) {
        CrateSetInOut(&crates[cc], (void *)payload, plen, plen);
        payload_idx = cc++;
    }

    uint8_t params[4];
    memcpy(params, &after_ms, sizeof(uint32_t));

    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_TOUCH_SEND, 0,
                             tag_idx, payload_idx, params, 4) != 0)
        return -ERR_INVALID_ARGS;
    if (ManifestBuilderFinalize(&mb) != 0) return -ERR_INVALID_ARGS;

    Result r;
    return ManifestSubmitTimeout((Manifest *)mbuf, crates, cc, &r, 1000);
}

/* Diagnostic counters — bumped from touch_await consumer path. */
static volatile uint32_t g_ta_entries_popped;     /* result_wait_any returned a Result */
static volatile uint32_t g_ta_touches_returned;
static volatile uint32_t g_ta_would_blocks_seen;
static volatile uint32_t g_ta_non_touch_ignored;  /* sender_pid!=0 but ctx != KCTX_TOUCH */
static volatile uint32_t g_ta_kernel_other;       /* sender_pid==0 ctx != GUIDE */
static volatile uint32_t g_ta_timeouts;
static volatile uint32_t g_ta_bad_payload;        /* kctx=touch but data_addr==0 etc */

void touch_await_stats(uint32_t out[7])
{
    out[0] = __atomic_load_n(&g_ta_entries_popped,    __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_ta_touches_returned,  __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_ta_would_blocks_seen, __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_ta_non_touch_ignored, __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&g_ta_kernel_other,      __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&g_ta_timeouts,          __ATOMIC_RELAXED);
    out[6] = __atomic_load_n(&g_ta_bad_payload,       __ATOMIC_RELAXED);
}

int touch_await(const char *tag, Touch *out, uint32_t timeout_ms)
{
    if (!tag || !out) return -ERR_INVALID_ARGS;

    /* Build a 1-op Manifest for SYSTEM_OP_TOUCH_AWAIT. */
    uint8_t mbuf[200];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return -ERR_INVALID_ARGS;

    Crate crates[1];
    CrateSetInput(&crates[0], (void *)tag, (uint32_t)(strlen(tag) + 1));

    /* Pass timeout_ms to kernel so it can wake us via TouchQueueWakeAfter
     * even when the scheduler has descheduled us (PROC_WAITING). */
    uint32_t to = (timeout_ms == 0) ? 30000 : timeout_ms;
    uint8_t params[4];
    memcpy(params, &to, sizeof(uint32_t));

    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_TOUCH_AWAIT, 0,
                             0, CRATE_INDEX_NONE, params, 4) != 0)
        return -ERR_INVALID_ARGS;
    if (ManifestBuilderFinalize(&mb) != 0) return -ERR_INVALID_ARGS;

    /* Encode a manifest-mode Pocket (mirrors ManifestSubmitFull internals). */
    Pocket p;
    pocket_prepare(&p);
    p.flags       = POCKET_FLAG_MANIFEST;
    p.target_pid  = 0;
    p.data_addr   = (uint64_t)(uintptr_t)mbuf;
    p.data_length = ((Manifest *)mbuf)->total_size;

    uint64_t crates_addr = (uint64_t)(uintptr_t)crates;
    uint16_t crate_count = 1;
    uint16_t pier_id     = 0;
    memcpy(p.route_tag + 0, &crates_addr,  sizeof(uint64_t));
    memcpy(p.route_tag + 8, &crate_count,  sizeof(uint16_t));
    memcpy(p.route_tag + 10, &pier_id,     sizeof(uint16_t));

    if (pocket_submit(&p) != 0) return -ERR_POCKET_RING_FULL;

    /* SysTouchAwait returns ERR_WOULD_BLOCK from the op (the await is parked
     * via PROC_WAITING and woken by KResultPush). Manifest dispatch still
     * pushes a Result with that error_code BEFORE the actual touch lands,
     * so we drain WOULD_BLOCK replies and keep waiting for a real touch. */
    /* Touch results carry the sender's pid (sender_pid != 0), so result_wait
     * (which stashes IPC results) would silently divert them. Use the _any
     * variant that drains BOTH IPC and non-IPC queues, then filter by context. */
    Result r;
    uint32_t wait_ms = (timeout_ms == 0) ? 30000 : timeout_ms;
    for (;;) {
        if (!result_wait_any(&r, wait_ms)) {
            __atomic_add_fetch(&g_ta_timeouts, 1, __ATOMIC_RELAXED);
            return -ERR_TIMEOUT;
        }
        __atomic_add_fetch(&g_ta_entries_popped, 1, __ATOMIC_RELAXED);
        if (r._reserved == 9 /* KCTX_TOUCH */) {
            __atomic_add_fetch(&g_ta_touches_returned, 1, __ATOMIC_RELAXED);
            break;
        }
        if (r.error_code == ERR_WOULD_BLOCK) {
            __atomic_add_fetch(&g_ta_would_blocks_seen, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (r.sender_pid != 0)
            __atomic_add_fetch(&g_ta_non_touch_ignored, 1, __ATOMIC_RELAXED);
        else
            __atomic_add_fetch(&g_ta_kernel_other, 1, __ATOMIC_RELAXED);
        /* Other reply: ignore. */
    }

    if (r.error_code == 0 && r.data_addr != 0 && r.data_length >= sizeof(Touch)) {
        memcpy(out, (const void *)(uintptr_t)r.data_addr, sizeof(Touch));
        return 0;
    }
    __atomic_add_fetch(&g_ta_bad_payload, 1, __ATOMIC_RELAXED);
    return r.error_code != 0 ? -(int)r.error_code : -ERR_INTERNAL;
}

int touch_irq_return(void)
{
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_IRQ_RETURN,
                   NULL, 0, NULL, 0, NULL, 0, NULL,
                   1000, NULL);
}

int touch_register(const char *tag, TouchPolicy policy, TouchCapability capability)
{
    if (!tag) return -ERR_INVALID_ARGS;

    uint8_t params[2];
    params[0] = (uint8_t)policy;
    params[1] = (uint8_t)capability;

    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_REGISTER,
                   params, 2,
                   tag, (uint32_t)(strlen(tag) + 1),
                   NULL, 0, NULL,
                   1000, NULL);
}

int touch_ack(const char *tag)
{
    if (!tag) return -ERR_INVALID_ARGS;

    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_ACK,
                   NULL, 0,
                   tag, (uint32_t)(strlen(tag) + 1),
                   NULL, 0, NULL,
                   1000, NULL);
}
