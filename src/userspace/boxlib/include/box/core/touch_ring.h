#ifndef BOX_CORE_TOUCH_RING_H
#define BOX_CORE_TOUCH_RING_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/core/strand_self.h"   /* strand_rings() — per-strand ring routing (P5a) */

/*
 * TouchRing — userspace mirror of the kernel's per-cabin Touch event ring.
 *
 * Layout MUST stay byte-identical to the kernel-side definition in
 * src/kernel/core/ipc/touch_ring.h. The header sits at the fixed VA
 * CABIN_TOUCH_RING_ADDR (0x5000); slots live at CABIN_TOUCH_SLOTS_BASE
 * (lazy-mapped on the kernel side).
 *
 * Consumer model (this side): single producer-pinned reader — the cabin
 * owner. Reads slot[head%cap], gates on seq == 2*round+1, copies the
 * entire 128-byte slot to caller's stack (because the slot payload's
 * lifetime ends as soon as we release seq), releases seq, advances
 * head.
 *
 * Lifetime contract: the inline payload bytes inside a TouchSlot are
 * valid ONLY until the consumer releases the slot's seq. After release,
 * a kernel-side producer may immediately wrap and overwrite the slot.
 * Therefore the boxlib API copies the entire slot into the caller's
 * `Touch` struct BEFORE the release — callers receive an independent
 * copy with no aliasing into ring memory.
 */

#define BOXOS_TOUCH_PAYLOAD_MAX  96u

typedef struct PACKED {
    /* Cacheline 0 — consumer cursor + read-only metadata. */
    volatile uint64_t head;
    uint64_t          slots_base;
    uint32_t          slot_size;
    uint32_t          slot_count_max;
    uint64_t          magic;
    uint8_t           _pad_line0[32];

    /* Cacheline 1 — producer cursor (kernel MPSC). */
    volatile uint64_t tail;
    /* Kernel-held events for this ring: accepted, promised, not yet fitted.
     * Kernel writes, we only read. It shares this cacheline with `tail` on
     * purpose — UMONITOR watches the line, so the kernel writing the slip
     * wakes a consumer that is asleep on a tail which cannot move. Non-zero
     * means: your ring is not the whole story, come to the door. */
    volatile uint64_t owed;
    uint8_t           _pad_line1[48];
} TouchRingHeader;

STATIC_ASSERT(sizeof(TouchRingHeader) == 128,
              "TouchRingHeader must be 128 bytes (two cachelines)");
STATIC_ASSERT(OFFSETOF(TouchRingHeader, owed) == 72,
              "TouchRingHeader.owed must share cacheline 1 with tail");

/* Per-slot envelope — metadata + inline payload + Vyukov gate.
 *
 * Layout must match the kernel-side struct exactly. Producer writes all
 * fields then release-stores seq. Consumer acquire-loads seq, copies
 * everything in one memcpy to caller-owned storage, then release-stores
 * the next-round seq value. */
typedef struct PACKED {
    uint16_t tag_id;
    uint16_t flags;
    uint32_t source_pid;
    uint32_t payload_len;
    uint32_t _reserved;
    uint64_t timestamp_tsc;
    uint8_t  payload[BOXOS_TOUCH_PAYLOAD_MAX];
    uint64_t seq;
} TouchSlot;

STATIC_ASSERT(sizeof(TouchSlot) == 128,
              "TouchSlot must be 128 bytes");

typedef struct PACKED {
    TouchRingHeader hdr;
    uint8_t         _page_pad[4096 - sizeof(TouchRingHeader)];
} TouchRing;

STATIC_ASSERT(sizeof(TouchRing) == 4096,
              "TouchRing header page must be exactly one page");

INLINE TouchRing *touch_ring(void) {
    return (TouchRing *)(uintptr_t)strand_rings().touch_va;
}

INLINE bool touch_ring_is_empty(const TouchRing *r) {
    return r->hdr.head == r->hdr.tail;
}

INLINE uint32_t touch_ring_count(const TouchRing *r) {
    uint64_t n = r->hdr.tail - r->hdr.head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

/* Public consumer helpers — internal to boxlib; the user-facing API lives
 * in box/touch.h (touch_pop/touch_wait/touch_available). */

/* Non-blocking pop into a TouchSlot-sized buffer. Returns true and fills
 * `slot_out` with a full 128-byte snapshot (metadata + inline payload)
 * if a slot was ready, false otherwise. The caller is responsible for
 * any further conversion (e.g. copying selected fields into the
 * user-facing `Touch` struct). Implementation: src/core/touch_ring.c. */
bool touch_ring_pop_slot(TouchSlot *slot_out);

/* The slot at head is released and unconsumed — poppable now. The fact a
 * sleep may end on (box_turn_in); a moved tail alone is a claim, not a slot. */
bool touch_ring_published_at_head(void);

/* Diagnostic: snapshot per-return-path counters.
 *   out[0] = calls
 *   out[1] = empty (head == tail)
 *   out[2] = seq mismatch (slot not yet published)
 *   out[3] = success
 *   out[4] = last_seq_seen
 *   out[5] = last_expected
 *   out[6] = last_pos
 *   out[7] = last_tail */
void touch_ring_pop_stats(uint64_t out[8]);

#ifdef __cplusplus
}
#endif

#endif /* BOX_CORE_TOUCH_RING_H */
