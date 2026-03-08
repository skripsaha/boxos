#ifndef BOX_NOTIFY_H
#define BOX_NOTIFY_H

#include "types.h"

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

// Pocket: syscall request written to PocketRing
typedef struct PACKED {
    uint32_t pid;                // kernel overwrites (security)
    uint32_t target_pid;         // 0 = result to self, != 0 = IPC route
    uint32_t error_code;
    uint8_t  prefix_count;
    uint8_t  current_prefix_idx;
    uint8_t  flags;
    uint8_t  _reserved1;
    uint32_t data_length;        // bytes of data at data_addr
    uint64_t data_addr;          // virtual address of data in cabin heap
    char     route_tag[POCKET_ROUTE_TAG_SIZE];
    uint16_t prefixes[POCKET_MAX_PREFIXES];
    uint8_t  _pad[4];
} Pocket;

STATIC_ASSERT(sizeof(Pocket) == 96, "Pocket must be 96 bytes");

// PocketRing: SPSC ring buffer at CABIN_POCKET_RING_ADDR (0x2000)
// Userspace is the producer (writes Pockets, advances tail).
// Kernel is the consumer (reads Pockets, advances head).
typedef struct PACKED {
    volatile uint32_t head;
    volatile uint32_t tail;
    Pocket slots[POCKET_RING_CAPACITY];
} PocketRing;

STATIC_ASSERT(sizeof(PocketRing) <= 4096, "PocketRing must fit in one page");

INLINE PocketRing* pocket_ring(void) {
    return (PocketRing*)POCKET_RING_VADDR;
}

INLINE bool pocket_ring_is_full(const PocketRing* ring) {
    return ((ring->tail + 1) % POCKET_RING_CAPACITY) == ring->head;
}

// Push a Pocket to the ring. Returns true on success.
INLINE bool pocket_ring_push(PocketRing* ring, const Pocket* p) {
    if (pocket_ring_is_full(ring)) return false;
    uint32_t idx = ring->tail;
    ring->slots[idx] = *p;
    __sync_synchronize();
    ring->tail = (idx + 1) % POCKET_RING_CAPACITY;
    return true;
}

// Prepare a fresh Pocket for a new syscall
void pocket_prepare(Pocket* p);

// Add a prefix to the Pocket's chain
bool pocket_add_prefix(Pocket* p, uint8_t deck_id, uint8_t opcode);

// Set data buffer in the Pocket (points to cabin heap)
void pocket_set_data(Pocket* p, void* data, uint32_t length);

// Submit a Pocket: push to PocketRing and fire int 0x80.
// Returns 0 on success.
int pocket_submit(Pocket* p);

// Yield: hint to scheduler without submitting a pocket
void yield(void);

#endif // BOX_NOTIFY_H
