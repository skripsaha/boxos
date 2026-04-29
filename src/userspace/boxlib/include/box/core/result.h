#ifndef BOX_CORE_RESULT_H
#define BOX_CORE_RESULT_H

#include "box/types.h"
#include "box/error.h"

// Result: syscall response from kernel to userspace.
// Data is NOT inline — data_addr points to cabin heap.
typedef struct PACKED {
    uint32_t error_code;
    uint32_t data_length;
    uint64_t data_addr;      // virtual address of data in cabin heap
    uint32_t sender_pid;     // 0 = kernel, != 0 = IPC sender
    uint32_t _reserved;
} Result;

STATIC_ASSERT(sizeof(Result) == 24, "Result must be 24 bytes");

/* ResultRing — Phase 11 lazy-growable, monotonic-index SPSC.
 *
 * Header at CABIN_RESULT_RING_ADDR (0x3000); slots at CABIN_RESULT_SLOTS_BASE.
 * The kernel maps slot pages eagerly when it pushes (see kring.c).
 */
typedef struct PACKED {
    volatile uint64_t head;             /* userspace cursor */
    volatile uint64_t tail;             /* kernel cursor */
    uint64_t          slots_base;       /* user vaddr of slot 0 */
    uint32_t          slot_size;        /* RESULT_SLOT_SIZE (32) */
    uint32_t          slot_count_max;
    uint64_t          magic;
    uint8_t           _pad[24];
} ResultRingHeader;

STATIC_ASSERT(sizeof(ResultRingHeader) == 64, "ResultRingHeader must be 64 bytes");

typedef struct PACKED {
    ResultRingHeader hdr;
    uint8_t          _page_pad[4096 - sizeof(ResultRingHeader)];
} ResultRing;

STATIC_ASSERT(sizeof(ResultRing) == 4096, "ResultRing header must be one page");

INLINE ResultRing* result_ring(void) {
    return (ResultRing*)RESULT_RING_VADDR;
}

INLINE bool result_ring_is_empty(const ResultRing* ring) {
    return ring->hdr.head == ring->hdr.tail;
}

bool result_available(void);
uint32_t result_count(void);
bool result_pop(Result* out);

// Uses UMONITOR/UMWAIT on CPUs with WAITPKG; falls back to cooperative yield.
bool result_wait(Result* out, uint32_t timeout_ms);

bool result_pop_non_ipc(Result* out);
bool result_pop_ipc(Result* out);
uint32_t result_ipc_stash_count(void);

// Block until ANY result arrives (no filtering).
// For IPC servers (display daemon, etc.) that receive both IPC and kernel results.
bool result_wait_any(Result* out, uint32_t timeout_ms);

#endif // BOX_RESULT_H
