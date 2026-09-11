#ifndef TOUCH_RING_H
#define TOUCH_RING_H

#include "ktypes.h"
#include "cabin_layout.h"


typedef struct __packed {
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

_Static_assert(sizeof(TouchRingHeader) == 128,
               "TouchRingHeader must be exactly 128 bytes (two cachelines)");
_Static_assert(__builtin_offsetof(TouchRingHeader, head) == 0,
               "TouchRingHeader.head must start at offset 0");
_Static_assert(__builtin_offsetof(TouchRingHeader, tail) == 64,
               "TouchRingHeader.tail must start at cacheline 1 (offset 64)");
_Static_assert(__builtin_offsetof(TouchRingHeader, owed) == 72,
               "TouchRingHeader.owed must share cacheline 1 with tail — the "
               "consumer's UMONITOR watches that line and nothing else");

typedef struct __packed {
    uint16_t tag_id;
    uint16_t flags;
    uint32_t source_pid;
    uint32_t payload_len;
    uint32_t _reserved;
    uint64_t timestamp_tsc;
    uint8_t  payload[BOXOS_TOUCH_PAYLOAD_MAX];
    uint64_t seq;
} TouchSlot;

_Static_assert(sizeof(TouchSlot) == 128,
               "TouchSlot must be exactly 128 bytes (TOUCH_SLOT_SIZE)");
_Static_assert(sizeof(TouchSlot) == TOUCH_SLOT_SIZE,
               "TouchSlot stride mismatch with cabin_layout.h TOUCH_SLOT_SIZE");
_Static_assert((TOUCH_SLOT_SIZE & (TOUCH_SLOT_SIZE - 1)) == 0,
               "TOUCH_SLOT_SIZE must be power-of-2 for clean modulo");
_Static_assert(4096 % TOUCH_SLOT_SIZE == 0,
               "TOUCH_SLOT_SIZE must divide a 4 KiB page");
_Static_assert((CABIN_TOUCH_SLOTS_BASE & 0xFFFULL) == 0,
               "CABIN_TOUCH_SLOTS_BASE must be page-aligned");
_Static_assert(__builtin_offsetof(TouchSlot, seq) ==
               TOUCH_SLOT_SIZE - sizeof(uint64_t),
               "TouchSlot.seq must be the LAST field (consumer reads metadata + payload first)");

typedef struct __packed {
    TouchRingHeader hdr;
    uint8_t         _page_pad[4096 - sizeof(TouchRingHeader)];
} TouchRing;

_Static_assert(sizeof(TouchRing) == 4096,
               "TouchRing header page must be exactly one page");


static inline bool touch_ring_is_empty(const TouchRing *r)
{
    return r->hdr.head == r->hdr.tail;
}

static inline bool touch_ring_is_full(const TouchRing *r)
{
    return (r->hdr.tail - r->hdr.head) >= r->hdr.slot_count_max;
}

static inline uint32_t touch_ring_count(const TouchRing *r)
{
    uint64_t n = r->hdr.tail - r->hdr.head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

static inline uintptr_t touch_ring_slot_uvaddr(const TouchRing *r, uint64_t idx)
{
    return (uintptr_t)(r->hdr.slots_base
                       + (idx % r->hdr.slot_count_max) * r->hdr.slot_size);
}

typedef struct process_t process_t;

void KTouchRingInit(TouchRing *hdr);
void KTouchRingInitAt(TouchRing *hdr, uint64_t slots_base, uint32_t slot_count_max);

bool KTouchPush(process_t *target,
                uint16_t tag_id, uint16_t flags, uint32_t source_pid,
                const void *payload, uint32_t payload_len);

bool KTouchRingHasUnreadAtHead(process_t *proc);

void KTouchPushStats(uint64_t out[7]);

#endif