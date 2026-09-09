/*
 * touch_ring.c — boxlib-side TouchRing consumer.
 *
 * Reads one slot at a time from the per-cabin TouchRing, gated by the
 * Vyukov seq counter. The slot is copied in its entirety BEFORE seq is
 * released — the inline payload bytes are reusable by the kernel
 * producer as soon as head advances, so any in-place read would race a
 * fast wraparound.
 *
 * Lifetime contract for callers: the TouchSlot returned by
 * touch_ring_pop_slot lives in caller-provided storage. Once the
 * function returns, the kernel may immediately wrap and overwrite the
 * underlying ring slot — touching the original ring vaddr after a pop
 * is undefined.
 */

#include "box/core/touch_ring.h"
#include "box/string.h"

/* Diagnostic counters — see touch_ring_pop_stats(). */
static volatile uint32_t g_tp_calls;
static volatile uint32_t g_tp_empty;
static volatile uint32_t g_tp_seq_mismatch;
static volatile uint32_t g_tp_success;
static volatile uint64_t g_tp_last_seq_seen;
static volatile uint64_t g_tp_last_expected;
static volatile uint64_t g_tp_last_pos;
static volatile uint64_t g_tp_last_tail;

void touch_ring_pop_stats(uint64_t out[8]) {
    out[0] = __atomic_load_n(&g_tp_calls,         __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_tp_empty,         __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_tp_seq_mismatch,  __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_tp_success,       __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&g_tp_last_seq_seen, __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&g_tp_last_expected, __ATOMIC_RELAXED);
    out[6] = __atomic_load_n(&g_tp_last_pos,      __ATOMIC_RELAXED);
    out[7] = __atomic_load_n(&g_tp_last_tail,     __ATOMIC_RELAXED);
}

/* The Touch twin of result_published_at_head (result.c): the slot at head is
 * released and unconsumed. Header sanity as in touch_ring_pop_slot; no side
 * effects. */
bool touch_ring_published_at_head(void) {
    TouchRing *rr = touch_ring();
    if (!rr) return false;
    uint32_t cap    = __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED);
    uint64_t base   = __atomic_load_n(&rr->hdr.slots_base,     __ATOMIC_RELAXED);
    uint32_t stride = __atomic_load_n(&rr->hdr.slot_size,      __ATOMIC_RELAXED);
    if (cap == 0 || base == 0 || stride == 0)        return false;
    if (base < 0x100000000ULL)                       return false;
    if (stride != sizeof(TouchSlot))                 return false;

    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t pos  = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
    if (pos == tail) return false;

    TouchSlot *slot   = (TouchSlot *)(uintptr_t)(base + (pos % cap) * stride);
    uint64_t expected = 2u * (pos / (uint64_t)cap) + 1u;
    return __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) == expected;
}

bool touch_ring_peek_slot(TouchSlot *slot_out) {
    TouchRing *rr = touch_ring();
    if (!rr || !slot_out) return false;
    uint32_t cap    = __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED);
    uint64_t base   = __atomic_load_n(&rr->hdr.slots_base,     __ATOMIC_RELAXED);
    uint32_t stride = __atomic_load_n(&rr->hdr.slot_size,      __ATOMIC_RELAXED);
    if (cap == 0 || base == 0 || stride == 0)        return false;
    if (base < 0x100000000ULL)                       return false;
    if (stride != sizeof(TouchSlot))                 return false;

    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t pos  = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
    if (pos == tail) return false;

    TouchSlot *slot   = (TouchSlot *)(uintptr_t)(base + (pos % cap) * stride);
    uint64_t expected = 2u * (pos / (uint64_t)cap) + 1u;
    if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != expected) return false;
    *slot_out = *slot;
    return true;
}

bool touch_ring_pop_slot(TouchSlot *slot_out) {
    TouchRing *rr = touch_ring();
    if (!rr || !slot_out) return false;
    __atomic_add_fetch(&g_tp_calls, 1, __ATOMIC_RELAXED);

    /* Header sanity — same defensive pattern as result.c result_pop.
     * Compiler-elided const-after-init checks have bitten us before
     * (memory `feedback_canonical_addr`); force runtime loads on every
     * pop. */
    uint32_t cap     = __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED);
    uint64_t base    = __atomic_load_n(&rr->hdr.slots_base,     __ATOMIC_RELAXED);
    uint32_t stride  = __atomic_load_n(&rr->hdr.slot_size,      __ATOMIC_RELAXED);
    if (cap == 0 || base == 0 || stride == 0)        return false;
    if (base < 0x100000000ULL)                       return false;
    if (stride != sizeof(TouchSlot))                 return false;

    /* Cheap drain probe. ACQUIRE on tail pairs with the kernel's ACQ_REL
     * fetch_add at KTouchPush step (3) — guarantees we never read a
     * stale seq for a slot the producer is about to write. */
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t pos  = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
    if (pos == tail) {
        __atomic_add_fetch(&g_tp_empty, 1, __ATOMIC_RELAXED);
        return false;
    }

    /* Vyukov consumer gate: slot is ready when seq == 2*round + 1. If
     * the producer holding `pos` has not yet release-stored, the slot's
     * seq is still `2*round` (this round's "free" marker) — we treat
     * the slot as empty and the caller retries on its next poll. */
    TouchSlot *slot   = (TouchSlot *)(uintptr_t)(base + (pos % cap) * stride);
    uint64_t expected = 2u * (pos / (uint64_t)cap) + 1u;
    uint64_t seq      = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if (seq != expected) {
        __atomic_store_n(&g_tp_last_seq_seen, seq, __ATOMIC_RELAXED);
        __atomic_store_n(&g_tp_last_expected, expected, __ATOMIC_RELAXED);
        __atomic_store_n(&g_tp_last_pos, pos, __ATOMIC_RELAXED);
        __atomic_store_n(&g_tp_last_tail, tail, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_tp_seq_mismatch, 1, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_add_fetch(&g_tp_success, 1, __ATOMIC_RELAXED);

    /* Copy the entire slot — metadata + inline payload — to caller storage.
     * MUST happen BEFORE the seq release below, since the kernel may
     * immediately wrap and overwrite this slot once seq advances. */
    *slot_out = *slot;

    /* Release the slot for the producer's next round at this index.
     * Producer for round R+1 expects seq == 2*(R+1) before it may write. */
    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);
    /* Advance head — kernel's ACQUIRE-load of head pairs with this
     * RELEASE so the seq release is visible before fullness probes. */
    __atomic_store_n(&rr->hdr.head, pos + 1u, __ATOMIC_RELEASE);
    return true;
}
