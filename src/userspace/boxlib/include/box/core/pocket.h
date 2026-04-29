#ifndef BOX_CORE_POCKET_H
#define BOX_CORE_POCKET_H

#include "box/types.h"

/*
 * Pocket — Manifest-only envelope (Phase 12). The fields below mirror the
 * kernel's pocket.h layout exactly.
 *
 * PocketRing — Phase 11 lazy-growable, monotonic-index SPSC. The header
 * lives at CABIN_POCKET_RING_ADDR (0x2000) — one fixed page. Slots live
 * at CABIN_POCKET_SLOTS_BASE in a 1 MiB virtual reservation; the kernel
 * maps slot pages on demand on first touch.
 *
 * Indices are 64-bit and never wrap. Slot lookup: slots_base + (idx % cap)*stride.
 */

#define POCKET_FLAG_YIELD     0x80
#define POCKET_FLAG_MANIFEST  0x40

typedef struct PACKED {
    uint32_t pid;                /* kernel overwrites (security) */
    uint32_t target_pid;         /* 0 = self, != 0 = IPC route */
    uint32_t error_code;
    uint8_t  flags;              /* POCKET_FLAG_YIELD | POCKET_FLAG_MANIFEST */
    uint8_t  _reserved[3];
    uint32_t data_length;        /* manifest size */
    uint64_t data_addr;          /* manifest user vaddr */
    char     route_tag[32];      /* manifest mode: crates_addr/count/pier_id */
    uint8_t  _pad[68];           /* pad to 128 bytes for PocketRing slot stride */
} Pocket;

STATIC_ASSERT(sizeof(Pocket) == 128, "Pocket must be 128 bytes");

typedef struct PACKED {
    volatile uint64_t head;             /* kernel cursor */
    volatile uint64_t tail;             /* userspace cursor */
    uint64_t          slots_base;       /* user vaddr of slot 0 */
    uint32_t          slot_size;        /* POCKET_SLOT_SIZE (128) */
    uint32_t          slot_count_max;   /* ring capacity */
    uint64_t          magic;
    uint8_t           _pad[24];
} PocketRingHeader;

STATIC_ASSERT(sizeof(PocketRingHeader) == 64, "PocketRingHeader must be 64 bytes");

typedef struct PACKED {
    PocketRingHeader hdr;
    uint8_t          _page_pad[4096 - sizeof(PocketRingHeader)];
} PocketRing;

STATIC_ASSERT(sizeof(PocketRing) == 4096, "PocketRing header must be one page");

INLINE PocketRing* pocket_ring(void) {
    return (PocketRing*)POCKET_RING_VADDR;
}

INLINE bool pocket_ring_is_empty(const PocketRing* ring) {
    return ring->hdr.head == ring->hdr.tail;
}

INLINE bool pocket_ring_is_full(const PocketRing* ring) {
    return (ring->hdr.tail - ring->hdr.head) >= ring->hdr.slot_count_max;
}

INLINE uint32_t pocket_ring_count(const PocketRing* ring) {
    uint64_t n = ring->hdr.tail - ring->hdr.head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

/*
 * Push a Pocket. Writes to slots_base + (tail % cap)*slot_size — the slot page
 * faults in if necessary; the kernel's #PF handler maps a fresh phys page and
 * returns. ABI guarantees slot_size == sizeof(Pocket) for PocketRing.
 */
INLINE bool pocket_ring_push(PocketRing* ring, const Pocket* p) {
    if (pocket_ring_is_full(ring)) return false;

    /* Sanity check the header before the slot store. If slots_base or
     * slot_count_max is zero (uninitialised header, corrupted page, etc.),
     * the slot pointer below would land somewhere meaningless — typically
     * inside the process's .text segment, where the resulting write fault
     * is hard to debug. Refusing the push here makes the failure mode
     * recoverable instead of crashing. */
    if (ring->hdr.slot_count_max == 0) return false;
    if (ring->hdr.slots_base   == 0)   return false;
    if (ring->hdr.slot_size    == 0)   return false;

    uint64_t idx  = ring->hdr.tail;
    Pocket  *slot = (Pocket *)(uintptr_t)
        (ring->hdr.slots_base + (idx % ring->hdr.slot_count_max) * ring->hdr.slot_size);
    *slot = *p;
    __sync_synchronize();
    ring->hdr.tail = idx + 1;
    return true;
}

#endif /* BOX_CORE_POCKET_H */
