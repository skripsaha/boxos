#include "box/timeouts.h"
#include "box/touch.h"
#include "box/core/manifest.h"
#include "box/core/result.h"
#include "box/core/touch_ring.h"
#include "box/core/notify.h"
#include "box/core/pocket.h"
#include "box/core/strand_self.h"
#include "box/cpu.h"
#include "box/clock.h"
#include "box/string.h"
#include "box/memory.h"
#include "box/core/stash.h"
#include "box/debug.h"
#include "box/error.h"
#include "boxos_decks.h"

TouchTagPair touch_intern(const char *tag)
{
    TouchTagPair pair = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID };
    if (!tag) return pair;

    uint16_t out[2] = { TOUCH_TAG_INVALID, TOUCH_TAG_INVALID };
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_INTERN,
                     NULL, 0,
                     tag, (uint32_t)(strlen(tag) + 1),
                     out, sizeof(out), NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return pair;
    pair.full = (TouchTag)out[0];
    pair.bare = (TouchTag)out[1];
    return pair;
}

int touch_claim(TouchTag tag, TouchMode mode, uint64_t manifest_or_handler,
                uint64_t stack_top)
{
    if (tag == TOUCH_TAG_INVALID) return -ERR_INVALID_ARGS;

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
                   BOX_ANSWER_GUARANTEED, NULL);
}

int touch_release(TouchTag tag)
{
    if (tag == TOUCH_TAG_INVALID) return -ERR_INVALID_ARGS;
    uint8_t params[2];
    memcpy(params, &tag, sizeof(uint16_t));
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_RELEASE,
                   params, 2,
                   NULL, 0, NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

int touch_send(TouchTagPair pair, const void *payload, uint32_t plen,
               uint32_t after_ms)
{
    if (pair.full == TOUCH_TAG_INVALID && pair.bare == TOUCH_TAG_INVALID)
        return -ERR_INVALID_ARGS;

    uint8_t mbuf[300];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return -ERR_INVALID_ARGS;

    Crate crates[2];
    uint16_t cc = 0;
    uint16_t payload_idx = CRATE_INDEX_NONE;
    if (payload && plen > 0) {
        CrateSetInOut(&crates[cc], (void *)payload, plen, plen);
        payload_idx = cc++;
    }
    uint32_t handed = 0;
    CrateSetOutput(&crates[cc], &handed, sizeof(handed));
    uint16_t handed_idx = cc++;

    uint8_t params[8];
    memcpy(params,     &pair.full, sizeof(uint16_t));
    memcpy(params + 2, &pair.bare, sizeof(uint16_t));
    memcpy(params + 4, &after_ms,  sizeof(uint32_t));

    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_TOUCH_SEND, 0,
                             payload_idx, handed_idx, params, 8) != 0)
        return -ERR_INVALID_ARGS;
    if (ManifestBuilderFinalize(&mb) != 0) return -ERR_INVALID_ARGS;

    Result r;
    int rc = ManifestSubmitTimeout((Manifest *)mbuf, crates, cc, &r, BOX_ANSWER_GUARANTEED);
    if (rc != 0) return box_fail(rc);
    return (int)handed;
}

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
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
    return tail != head;
}

void touch_pop_stats(uint64_t out[8])
{
    touch_ring_pop_stats(out);
}

uint64_t touch_owed(void)
{
    TouchRing *rr = touch_ring();
    if (!rr) return 0;
    return __atomic_load_n(&rr->hdr.owed, __ATOMIC_ACQUIRE);
}

static inline uint64_t touch_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void yield(void);

static bool touch_wait_umwait(Touch *out, uint32_t timeout_ms)
{
    TouchRing *rr = touch_ring();
    volatile uint64_t *tail_addr = (volatile uint64_t *)
        ((uintptr_t)rr + OFFSETOF(TouchRing, hdr.tail));

    uint64_t owed_deadline = timeout_ms
                             ? touch_rdtsc() + cpu_ms_to_tsc(timeout_ms) : 0;

    while (1) {
        __sync_synchronize();
        if (touch_pop(out)) return true;

        umonitor((volatile void *)tail_addr);
        __sync_synchronize();

        if (touch_available()) continue;

        if (touch_owed() != 0) {
            if (owed_deadline != 0 && touch_rdtsc() >= owed_deadline) return false;
            yield();
            continue;
        }

        uint64_t deadline_tsc;
        if (timeout_ms == 0) {
            deadline_tsc = 0xFFFFFFFFFFFFFFFFULL;
        } else {
            deadline_tsc = touch_rdtsc() + cpu_ms_to_tsc(timeout_ms);
        }

        int wake_reason = umwait(0, deadline_tsc);

        if (wake_reason == 1 && timeout_ms > 0) {
            __sync_synchronize();
            if (!touch_available()) return false;
        }
    }
}

#define TOUCH_SPIN_BUDGET 2048u

static bool touch_wait_pause(Touch *out, uint32_t timeout_ms)
{
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = touch_rdtsc() + cpu_ms_to_tsc(timeout_ms);
    }

    uint32_t spin = 0;
    while (1) {
        __sync_synchronize();
        if (touch_pop(out)) return true;

        if (timeout_ms > 0 && touch_rdtsc() >= deadline) return false;
        if (touch_owed() != 0) {
            spin = 0;
            yield();
        } else if (++spin < TOUCH_SPIN_BUDGET) {
            __asm__ volatile("pause");
        } else {
            spin = 0;
            yield();
        }
    }
}

bool touch_wait(Touch *out, uint32_t timeout_ms)
{
    if (!out) return false;
    if (touch_pop(out)) return true;

    if (cpu_has_waitpkg()) return touch_wait_umwait(out, timeout_ms);
    return touch_wait_pause(out, timeout_ms);
}

static Stash g_touch_stash;

static Stash *touch_stash_self(bool create)
{
    StrandInfo *si = strand_info_or_null();
    Stash *s = &g_touch_stash;
    if (si) {
        s = (Stash *)(uintptr_t)si->touch_stash_ptr;
        if (!s) {
            if (!create) return NULL;
            s = (Stash *)malloc(sizeof(Stash));
            if (!s) return NULL;
            memset(s, 0, sizeof(*s));
            si->touch_stash_ptr = (uint64_t)(uintptr_t)s;
        }
    }
    if (s->entry_size == 0) {
        TouchRing *rr = touch_ring();
        uint32_t cap = rr ? __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED) : 0;
        stash_init(s, sizeof(Touch), cap);
    }
    return s;
}

static bool touch_tag_is(const void *entry, const void *key)
{
    return ((const Touch *)entry)->tag_id == *(const TouchTag *)key;
}

static bool g_touch_no_room_said;

static void touch_no_room(void)
{
    if (g_touch_no_room_said) return;
    g_touch_no_room_said = true;
    kdbg_nowait("[touch] DEFECT: no memory to keep an event of another tag - it stays in the ring");
}

bool touch_try_pop_tag(TouchTag tag, Touch *out)
{
    if (tag == TOUCH_TAG_INVALID || !out) return false;
    Stash *st = touch_stash_self(false);
    if (stash_take_where(st, touch_tag_is, &tag, out)) return true;

    TouchSlot slot;
    while (touch_ring_peek_slot(&slot)) {
        if (slot.tag_id == tag) {
            if (!touch_ring_pop_slot(&slot)) return false;
            touch_from_slot(&slot, out);
            return true;
        }
        if (!st) st = touch_stash_self(true);
        if (!st || !stash_reserve(st)) { touch_no_room(); return false; }
        if (!touch_ring_pop_slot(&slot)) return false;
        Touch kept;
        touch_from_slot(&slot, &kept);
        stash_put(st, &kept);
    }
    return false;
}

bool touch_wait_tag(TouchTag tag, Touch *out, uint32_t timeout_ms)
{
    if (tag == TOUCH_TAG_INVALID || !out) return false;

    uint64_t deadline = timeout_ms ? clock_uptime_ms() + timeout_ms : 0;

    for (;;) {
        if (touch_try_pop_tag(tag, out)) return true;

        Stash *st = touch_stash_self(true);
        if (!st || !stash_reserve(st)) { touch_no_room(); return false; }

        uint32_t slice = 0;
        if (timeout_ms) {
            uint64_t now = clock_uptime_ms();
            if (now >= deadline) return false;
            uint64_t rem = deadline - now;
            slice = rem > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)rem;
            if (slice == 0) slice = 1;
        }

        Touch tmp;
        if (!touch_wait(&tmp, slice)) {
            if (timeout_ms && clock_uptime_ms() >= deadline) return false;
            continue;
        }
        if (tmp.tag_id == tag) { *out = tmp; return true; }
        stash_put(st, &tmp);
    }
}

void touch_stash_free_self(void)
{
    StrandInfo *si = strand_info_or_null();
    if (!si || si->touch_stash_ptr == 0) return;
    Stash *s = (Stash *)(uintptr_t)si->touch_stash_ptr;
    stash_free(s);
    free(s);
    si->touch_stash_ptr = 0;
}

int touch_await(TouchTag tag, Touch *out, uint32_t timeout_ms)
{
    if (tag == TOUCH_TAG_INVALID || !out) return -ERR_INVALID_ARGS;

    if (touch_try_pop_tag(tag, out)) {
        __atomic_add_fetch(&g_ta_entries_popped,   1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_ta_touches_returned, 1, __ATOMIC_RELAXED);
        return 0;
    }

    uint8_t mbuf[200];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return -ERR_INVALID_ARGS;

    uint8_t params[8];
    memset(params, 0, sizeof(params));
    memcpy(params,     &tag,        sizeof(uint16_t));
    memcpy(params + 4, &timeout_ms, sizeof(uint32_t));

    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_TOUCH_AWAIT, 0,
                             CRATE_INDEX_NONE, CRATE_INDEX_NONE, params, 8) != 0)
        return -ERR_INVALID_ARGS;
    if (ManifestBuilderFinalize(&mb) != 0) return -ERR_INVALID_ARGS;

    uint64_t began = touch_rdtsc();

    int push_rc = ManifestSubmitNoWait((Manifest *)mbuf, NULL, 0, 0);
    if (push_rc != OK) return push_rc;

    uint32_t left = 0;
    if (timeout_ms != 0) {
        uint64_t spent = cpu_tsc_to_ms(touch_rdtsc() - began);
        if (spent >= (uint64_t)timeout_ms) {
            __atomic_add_fetch(&g_ta_timeouts, 1, __ATOMIC_RELAXED);
            return -ERR_TIMEOUT;
        }
        left = (uint32_t)((uint64_t)timeout_ms - spent);
    }

    if (!touch_wait_tag(tag, out, left)) {
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
                   BOX_ANSWER_GUARANTEED, NULL);
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
                   BOX_ANSWER_GUARANTEED, NULL);
}

int touch_ack(TouchTag tag)
{
    if (tag == TOUCH_TAG_INVALID) return -ERR_INVALID_ARGS;
    uint8_t params[2];
    memcpy(params, &tag, sizeof(uint16_t));
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_TOUCH_ACK,
                   params, 2,
                   NULL, 0, NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}