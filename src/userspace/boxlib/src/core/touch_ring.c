
#include "box/core/touch_ring.h"
#include "box/string.h"

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

    uint32_t cap     = __atomic_load_n(&rr->hdr.slot_count_max, __ATOMIC_RELAXED);
    uint64_t base    = __atomic_load_n(&rr->hdr.slots_base,     __ATOMIC_RELAXED);
    uint32_t stride  = __atomic_load_n(&rr->hdr.slot_size,      __ATOMIC_RELAXED);
    if (cap == 0 || base == 0 || stride == 0)        return false;
    if (base < 0x100000000ULL)                       return false;
    if (stride != sizeof(TouchSlot))                 return false;

    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t pos  = __atomic_load_n(&rr->hdr.head, __ATOMIC_RELAXED);
    if (pos == tail) {
        __atomic_add_fetch(&g_tp_empty, 1, __ATOMIC_RELAXED);
        return false;
    }

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

    *slot_out = *slot;

    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&rr->hdr.head, pos + 1u, __ATOMIC_RELEASE);
    return true;
}