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
 *
 * Sanity rules against header corruption / uninitialised mappings — the
 * compiler used to optimise these checks away because the non-volatile
 * fields look const-after-init from its viewpoint (kernel writes them
 * before any user thread runs). Atomic-relaxed loads force a real memory
 * read on every push so a bad header never lets us blast garbage into
 * .text or NULL.
 */
INLINE bool pocket_ring_push(PocketRing* ring, const Pocket* p) {
    if (!ring || !p) return false;

    uint64_t magic   = __atomic_load_n(&ring->hdr.magic,          __ATOMIC_RELAXED);
    uint32_t cap     = __atomic_load_n(&ring->hdr.slot_count_max, __ATOMIC_RELAXED);
    uint64_t base    = __atomic_load_n(&ring->hdr.slots_base,     __ATOMIC_RELAXED);
    uint32_t stride  = __atomic_load_n(&ring->hdr.slot_size,      __ATOMIC_RELAXED);

    if (magic   != POCKET_RING_MAGIC) return false;
    if (cap     == 0)                 return false;
    if (base    == 0)                 return false;
    if (stride  == 0)                 return false;
    /* Guard against the slot region accidentally landing inside the user
     * binary mapping (low VA): Cabin pins POCKET_SLOTS at the 64 TB mark.
     * Anything below 4 GB cannot be a valid slots_base. */
    if (base    <  0x100000000ULL)    return false;

    if (pocket_ring_is_full(ring)) return false;

    uint64_t idx  = __atomic_load_n(&ring->hdr.tail, __ATOMIC_RELAXED);
    if ((idx - __atomic_load_n(&ring->hdr.head, __ATOMIC_ACQUIRE)) >= cap) return false;

    Pocket *slot = (Pocket *)(uintptr_t)(base + (idx % cap) * stride);
    *slot = *p;
    /* RELEASE pair with kernel ACQUIRE on tail — the consumer may not see
     * the new tail before the slot store is visible. */
    __atomic_store_n(&ring->hdr.tail, idx + 1, __ATOMIC_RELEASE);
    return true;
}

#endif /* BOX_CORE_POCKET_H */
