#ifndef BOX_NOTIFY_H
#define BOX_NOTIFY_H

#include "types.h"

/* Pocket flag bits (mirror kernel src/kernel/core/ipc/pocket.h). */
#define POCKET_FLAG_YIELD     0x80
#define POCKET_FLAG_MANIFEST  0x40

// BoxOS notify — signal the kernel that PocketRing has work for the Guide.
// Wraps the x86-64 fast entry instruction. INT 0x80 remains as fallback.
#define __notify() __asm__ volatile("syscall" ::: "memory", "rcx", "r11")

// CabinInfo: read-only metadata at CABIN_INFO_VADDR (0x1000)
// Kernel fills this at process creation. Userspace reads pid, heap_base, etc.
typedef struct PACKED {
    uint32_t magic;           // CABIN_INFO_MAGIC ("CABN")
    uint32_t pid;
    uint32_t spawner_pid;
    uint32_t reserved;
    uint64_t heap_base;
    uint64_t heap_max_size;
    uint64_t buf_heap_base;
    uint64_t stack_top;
} CabinInfo;

STATIC_ASSERT(sizeof(CabinInfo) == 48, "CabinInfo header must be 48 bytes");

INLINE CabinInfo* cabin_info(void) {
    return (CabinInfo*)CABIN_INFO_VADDR;
}

/* Pocket — Manifest-only envelope (Phase 12). The fields below mirror the
 * kernel's pocket.h layout exactly. */
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

/* PocketRing — Phase 11 lazy-growable, monotonic-index SPSC.
 *
 * The header lives at CABIN_POCKET_RING_ADDR (0x2000) — one fixed page.
 * Slots live at CABIN_POCKET_SLOTS_BASE in a 1 MiB virtual reservation; the
 * kernel maps slot pages on demand the first time userspace touches them.
 *
 * Indices are 64-bit and never wrap. Slot lookup: slots_base + (idx % cap)*stride.
 */
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

/* Push a Pocket. Writes to slots_base + (tail % cap)*slot_size — the slot page
 * faults in if necessary; the kernel's #PF handler maps a fresh phys page and
 * returns. The ABI guarantees slot_size == sizeof(Pocket) for PocketRing. */
INLINE bool pocket_ring_push(PocketRing* ring, const Pocket* p) {
    if (pocket_ring_is_full(ring)) return false;
    uint64_t idx  = ring->hdr.tail;
    Pocket  *slot = (Pocket *)(uintptr_t)
        (ring->hdr.slots_base + (idx % ring->hdr.slot_count_max) * ring->hdr.slot_size);
    *slot = *p;
    __sync_synchronize();
    ring->hdr.tail = idx + 1;
    return true;
}

// Prepare a fresh Pocket for a new syscall
void pocket_prepare(Pocket* p);

// Submit a Pocket to the PocketRing and notify the kernel. Used internally
// by box/manifest.c (ManifestSubmit). Userspace code should call MfCall1 or
// the higher-level wrappers instead of building Pockets directly.
int pocket_submit(Pocket* p);

// Yield: hint to scheduler. Pushes a YIELD-flagged Pocket and returns.
void yield(void);

#endif // BOX_NOTIFY_H
