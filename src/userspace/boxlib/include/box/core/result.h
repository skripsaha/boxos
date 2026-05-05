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

/* ResultRing — Phase 11 lazy-growable, monotonic-index, MPSC kernel
 * producers / SP userspace consumer.
 *
 * Header at CABIN_RESULT_RING_ADDR (0x3000); slots at CABIN_RESULT_SLOTS_BASE,
 * lazily mapped by the kernel on first KResultPush touch.
 *
 * Each slot carries a Vyukov generation counter (slot.seq) used to gate
 * producer/consumer access:
 *   seq == 2*round           — slot empty, producer for `round` may write
 *   seq == 2*round + 1       — producer wrote, consumer for `round` may read
 *   seq == 2*(round + 1)     — consumer read, slot ready for next round
 * where round = pos / slot_count_max. Zero-init pages naturally satisfy
 * round-0 producers; no eager init is required.
 */
typedef struct PACKED {
    Result   r;                          /* 24 bytes payload */
    uint64_t seq;                        /* 8  bytes generation counter */
} ResultSlot;

STATIC_ASSERT(sizeof(ResultSlot) == 32, "ResultSlot must be 32 bytes");

typedef struct PACKED {
    volatile uint64_t head;             /* userspace cursor */
    volatile uint64_t tail;             /* kernel reservation cursor (MPSC) */
    uint64_t          slots_base;       /* user vaddr of slot 0 */
    uint32_t          slot_size;        /* sizeof(ResultSlot) == 32 */
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

// Diagnostic: snapshot result_pop counters
//   out[0]=calls, out[1]=empty(head==tail), out[2]=seq_mismatch, out[3]=success
//   out[4]=last_seq_seen, out[5]=last_expected, out[6]=last_pos, out[7]=last_tail
void result_pop_stats(uint64_t out[8]);


#endif // BOX_RESULT_H
