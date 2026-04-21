#ifndef BOX_NOTIFY_H
#define BOX_NOTIFY_H

#include "types.h"

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

STATIC_ASSERT(sizeof(Pocket) == 128, "Pocket must be 128 bytes");

// PocketRing: SPSC ring buffer at CABIN_POCKET_RING_ADDR (0x2000)
// Userspace is the producer (writes Pockets, advances tail).
// Kernel is the consumer (reads Pockets, advances head).
// Layout: 8 bytes header + 31 * 128 = 3968 bytes slots = 3976 bytes total (fits in one page).
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

// Submit a Pocket: push to PocketRing and notify the kernel.
// Returns 0 on success.
int pocket_submit(Pocket* p);

// Yield: hint to scheduler without submitting a pocket
void yield(void);

// Send a single-prefix Pocket: prepare, set data, add prefix, submit.
int pocket_send(uint8_t deck_id, uint8_t opcode, void* data, uint32_t length);

/* =========================================================================
 * Batch API — push multiple Pockets, one SYSCALL for all.
 *
 * The Guide already drains the entire PocketRing per SYSCALL.  These
 * functions let userspace fill the ring first, then trigger processing
 * with a single kernel entry — eliminating N-1 round-trips.
 *
 * IMPORTANT: each queued Pocket's data_addr must point to a SEPARATE
 * buffer that remains valid until pocket_flush / pocket_flush_wait
 * completes.  The kernel reads data in-place; shared buffers will
 * be corrupted by earlier handlers before later ones run.
 *
 * Example (5 VGA ops in 1 SYSCALL instead of 5):
 *
 *   uint8_t color_buf  = GREEN;
 *   uint8_t str_buf[192];  // ... fill putstring packet ...
 *   uint8_t nl_buf[4] = {0};
 *
 *   pocket_queue(DECK_HARDWARE, 0x77, &color_buf, 1);
 *   pocket_queue(DECK_HARDWARE, 0x71, str_buf, len);
 *   pocket_queue(DECK_HARDWARE, 0x7A, nl_buf, 4);
 *
 *   Result results[3];
 *   int got = pocket_flush_wait(results, 3, 100000);
 * ========================================================================= */

// Queue a Pocket without triggering SYSCALL.
// Data buffer must remain valid until flush completes.
// Returns 0 on success, -1 if PocketRing is full.
int pocket_queue(uint8_t deck_id, uint8_t opcode, void* data, uint32_t length);

// Trigger one SYSCALL to process all queued Pockets.
void pocket_flush(void);

// Flush + wait for exactly `count` results (Result from box/result.h).
// Returns number of results successfully received.
// Caller must include box/result.h and pass a Result[] array.
int pocket_flush_wait(void* results, int count, uint32_t timeout_ms);

#endif // BOX_NOTIFY_H
