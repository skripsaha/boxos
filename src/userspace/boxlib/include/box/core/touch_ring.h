#ifndef BOX_CORE_TOUCH_RING_H
#define BOX_CORE_TOUCH_RING_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/core/strand_self.h"


#define BOXOS_TOUCH_PAYLOAD_MAX  96u

typedef struct PACKED {
    volatile uint64_t head;
    uint64_t          slots_base;
    uint32_t          slot_size;
    uint32_t          slot_count_max;
    uint64_t          magic;
    uint8_t           _pad_line0[32];

    volatile uint64_t tail;
    volatile uint64_t owed;
    uint8_t           _pad_line1[48];
} TouchRingHeader;

STATIC_ASSERT(sizeof(TouchRingHeader) == 128,
              "TouchRingHeader must be 128 bytes (two cachelines)");
STATIC_ASSERT(OFFSETOF(TouchRingHeader, owed) == 72,
              "TouchRingHeader.owed must share cacheline 1 with tail");

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


bool touch_ring_pop_slot(TouchSlot *slot_out);
bool touch_ring_peek_slot(TouchSlot *slot_out);

bool touch_ring_published_at_head(void);

void touch_ring_pop_stats(uint64_t out[8]);

#ifdef __cplusplus
}
#endif

#endif