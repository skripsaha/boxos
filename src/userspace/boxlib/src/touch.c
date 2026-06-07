#include "box/touch.h"
#include "box/core/manifest.h"
#include "box/core/result.h"
#include "box/core/touch_ring.h"
#include "box/core/notify.h"
#include "box/core/pocket.h"
#include "box/cpu.h"
#include "box/string.h"
#include "box/error.h"
#include "arch/x86_64/cpu_wait.h"
#include "boxos_decks.h"  /* DECK_SYSTEM + SYSTEM_OP_TOUCH_* — single source */

TouchTagPair touch_intern(const char *tag)
{
    TouchTagPair pair = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID };
    if (!tag) return pair;

    uint16_t out[2] = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID };
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_INTERN,
                     NULL, 0,
                     tag, (uint32_t)(strlen(tag) + 1),
                     out, sizeof(out), NULL,
                     30000, NULL);
    if (rc != 0) return pair;
    pair.full = (TouchTag)out[0];
    pair.bare = (TouchTag)out[1];
    return pair;
}

int touch_claim(TouchTag tag, TouchMode mode, uint64_t manifest_or_handler,
                uint64_t stack_top)
{
    if (tag == TOUCH_TAG_INVALID) return -ERR_INVALID_ARGS;

    /* params: [u16 tag][u8 mode][mode-specific] */
    uint8_t  params[19];
    uint16_t param_size;
    memcpy(params, &tag, sizeof(uint16_t));
    params[2] = (uint8_t)mode;

    if (mode == TOUCH_REACT) {
        memcpy(params + 3, &manifest_or_handler, sizeof(uint64_t));
        param_size = 11;
    } else if (mode == TOUCH_INTERRUPT) {
        memcpy(params + 3,  &manifest_or_handler, sizeof(uint64_t));
        memcpy(params + 11, &stack_top,           sizeof(uint64_t));
        param_size = 19;
    } else {
        param_size = 3;
    }

    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_CLAIM,
                   params, param_size,
                   NULL, 0, NULL, 0, NULL,
                   30000, NULL);
}

int touch_release(TouchTag tag)
{
    if (tag == TOUCH_TAG_INVALID) return -ERR_INVALID_ARGS;
    uint8_t params[2];
    memcpy(params, &tag, sizeof(uint16_t));
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_RELEASE,
                   params, 2,
                   NULL, 0, NULL, 0, NULL,
                   30000, NULL);
}

int touch_send(TouchTagPair pair, const void *payload, uint32_t plen,
               uint32_t after_ms)
{
    if (pair.full == TOUCH_TAG_INVALID && pair.bare == TOUCH_TAG_INVALID)
        return -ERR_INVALID_ARGS;

    uint8_t mbuf[300];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return -ERR_INVALID_ARGS;

    Crate crates[1];
    uint16_t cc = 0;
    uint16_t payload_idx = CRATE_INDEX_NONE;
    if (payload && plen > 0) {
        CrateSetInOut(&crates[cc], (void *)payload, plen, plen);
        payload_idx = cc++;
    }

    /* params: [u16 full][u16 bare][u32 after_ms] */
    uint8_t params[8];
    memcpy(params,     &pair.full, sizeof(uint16_t));
    memcpy(params + 2, &pair.bare, sizeof(uint16_t));
    memcpy(params + 4, &after_ms,  sizeof(uint32_t));

    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_TOUCH_SEND, 0,
                             payload_idx, CRATE_INDEX_NONE, params, 8) != 0)
        return -ERR_INVALID_ARGS;
    if (ManifestBuilderFinalize(&mb) != 0) return -ERR_INVALID_ARGS;

    Result r;
    return ManifestSubmitTimeout((Manifest *)mbuf, crates, cc, &r, 1000);
}

/* Diagnostic counters — bumped from touch_await consumer path. */
static volatile uint32_t g_ta_entries_popped;
static volatile uint32_t g_ta_touches_returned;
static volatile uint32_t g_ta_would_blocks_seen;
static volatile uint32_t g_ta_non_touch_ignored;
static volatile uint32_t g_ta_kernel_other;
static volatile uint32_t g_ta_timeouts;
static volatile uint32_t g_ta_bad_payload;

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

/* Convert a raw TouchSlot into the userspace-facing Touch struct.
 *
 * Field-by-field copy (NOT memcpy of the slot) — Touch and TouchSlot
 * intentionally have different layouts: TouchSlot ends in a Vyukov seq
 * counter that has no userspace meaning, while Touch has no payload_addr
 * field (the self-pointer footgun, see box/touch.h rationale). The
 * out-payload-bytes are copied only up to slot->payload_len; the
 * remaining bytes of out->payload[] are left untouched (kernel
 * pre-zeroes the slot at cabin init, and producer always overwrites the
 * used prefix, so any leftover bytes are deterministic). */
static void touch_from_slot(const TouchSlot *slot, Touch *out)
{
    out->tag_id        = slot->tag_id;
    out->flags         = slot->flags;
    out->source_pid    = slot->source_pid;
    out->payload_len   = slot->payload_len;
    out->_reserved     = 0;
    out->timestamp_tsc = slot->timestamp_tsc;
    if (slot->payload_len > 0 && slot->payload_len <= BOXOS_TOUCH_PAYLOAD_MAX) {
        memcpy(out->payload, slot->payload, slot->payload_len);
    }
}

bool touch_pop(Touch *out)
{
    if (!out) return false;
    TouchSlot slot;
    if (!touch_ring_pop_slot(&slot)) return false;
    touch_from_slot(&slot, out);
    return true;
}

bool touch_available(void)
{
    TouchRing *rr = touch_ring();
    if (!rr) return false;
    /* ACQUIRE on tail pairs with kernel's ACQ_REL fetch_add — see
     * touch_ring.c consumer comment for the ordering rationale. */
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
    return tail != head;
}

void touch_pop_stats(uint64_t out[8])
{
    touch_ring_pop_stats(out);
}

static inline uint64_t touch_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool touch_wait_umwait(Touch *out, uint32_t timeout_ms)
{
    TouchRing *rr = touch_ring();
    /* UMONITOR arms a hardware monitor on the cacheline containing
     * the supplied address (Intel SDM Vol 2A — UMONITOR/UMWAIT,
     * granularity from CPUID.05H:EAX[15:0], typically 64 B). We
     * watch `tail` directly so any KTouchPush fetch_add wakes us. */
    volatile uint64_t *tail_addr = (volatile uint64_t *)
        ((uintptr_t)rr + OFFSETOF(TouchRing, hdr.tail));

    while (1) {
        __sync_synchronize();
        if (touch_pop(out)) return true;

        umonitor((volatile void *)tail_addr);
        __sync_synchronize();

        if (touch_available()) continue;

        uint64_t deadline_tsc;
        if (timeout_ms == 0) {
            deadline_tsc = 0xFFFFFFFFFFFFFFFFULL;
        } else {
            deadline_tsc = touch_rdtsc() + cpu_ms_to_tsc(timeout_ms);
        }

        int wake_reason = umwait(0, deadline_tsc);

        if (wake_reason == 1 && timeout_ms > 0) {
            __sync_synchronize();
            if (!touch_available()) return false;   /* deadline elapsed */
        }
    }
}

static bool touch_wait_pause(Touch *out, uint32_t timeout_ms)
{
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = touch_rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    while (1) {
        __sync_synchronize();
        if (touch_pop(out)) return true;

        if (timeout_ms > 0 && touch_rdtsc() >= deadline) return false;
        __asm__ volatile("pause");
    }
}

bool touch_wait(Touch *out, uint32_t timeout_ms)
{
    if (!out) return false;
    /* Fast initial drain — if a slot is already published, skip the
     * UMWAIT setup entirely. */
    if (touch_pop(out)) return true;

    if (cpu_has_waitpkg()) return touch_wait_umwait(out, timeout_ms);
    return touch_wait_pause(out, timeout_ms);
}

int touch_await(TouchTag tag, Touch *out, uint32_t timeout_ms)
{
    if (tag == TOUCH_TAG_INVALID || !out) return -ERR_INVALID_ARGS;

    /* Fast path — drain TouchRing for any already-queued event. The
     * caller's tag claim must already exist (or the kernel has been
     * publishing to a tag we didn't subscribe to, which is benign —
     * we just won't get any matching slots). */
    {
        Touch t;
        if (touch_pop(&t)) {
            __atomic_add_fetch(&g_ta_entries_popped,   1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_ta_touches_returned, 1, __ATOMIC_RELAXED);
            *out = t;
            return 0;
        }
    }

    /* Slow path: submit SYSTEM_OP_TOUCH_AWAIT — this both ensures the
     * REST claim and parks the process in PROC_WAITING with the kernel
     * timeout. KTouchPush flips us back to PROC_WORKING the moment a
     * slot is published into our TouchRing. */
    uint8_t mbuf[200];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return -ERR_INVALID_ARGS;

    /* params: [u16 tag][u16 _pad][u32 timeout_ms] */
    uint32_t to = (timeout_ms == 0) ? 30000 : timeout_ms;
    uint8_t params[8];
    memset(params, 0, sizeof(params));
    memcpy(params,     &tag, sizeof(uint16_t));
    memcpy(params + 4, &to,  sizeof(uint32_t));

    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_TOUCH_AWAIT, 0,
                             CRATE_INDEX_NONE, CRATE_INDEX_NONE, params, 8) != 0)
        return -ERR_INVALID_ARGS;
    if (ManifestBuilderFinalize(&mb) != 0) return -ERR_INVALID_ARGS;

    int push_rc = ManifestSubmitNoWait((Manifest *)mbuf, NULL, 0, 0);
    if (push_rc != OK) return push_rc;

    /* Wait on TouchRing for an event. The kernel will wake us via
     * KTouchPush's PROC_WAITING→PROC_WORKING flip; touch_wait covers
     * both the UMWAIT and pause-spin paths. */
    if (!touch_wait(out, to)) {
        __atomic_add_fetch(&g_ta_timeouts, 1, __ATOMIC_RELAXED);
        return -ERR_TIMEOUT;
    }
    __atomic_add_fetch(&g_ta_entries_popped,   1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_ta_touches_returned, 1, __ATOMIC_RELAXED);
    return 0;
}

int touch_irq_return(void)
{
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_IRQ_RETURN,
                   NULL, 0, NULL, 0, NULL, 0, NULL,
                   1000, NULL);
}

int touch_register(TouchTag tag, TouchPolicy policy, TouchCapability capability)
{
    if (tag == TOUCH_TAG_INVALID) return -ERR_INVALID_ARGS;
    uint8_t params[4];
    memcpy(params, &tag, sizeof(uint16_t));
    params[2] = (uint8_t)policy;
    params[3] = (uint8_t)capability;
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_REGISTER,
                   params, 4,
                   NULL, 0, NULL, 0, NULL,
                   1000, NULL);
}

int touch_ack(TouchTag tag)
{
    if (tag == TOUCH_TAG_INVALID) return -ERR_INVALID_ARGS;
    uint8_t params[2];
    memcpy(params, &tag, sizeof(uint16_t));
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_ACK,
                   params, 2,
                   NULL, 0, NULL, 0, NULL,
                   1000, NULL);
}
