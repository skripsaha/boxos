#ifndef BOX_RESULT_H
#define BOX_RESULT_H

#include "types.h"
#include "error.h"

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

// ResultRing: SPSC ring buffer at CABIN_RESULT_RING_ADDR (0x3000)
// Kernel is the producer (writes Results, advances tail).
// Userspace is the consumer (reads Results, advances head).
typedef struct PACKED {
    volatile uint32_t head;
    volatile uint32_t tail;
    Result slots[RESULT_RING_CAPACITY];
} ResultRing;

STATIC_ASSERT(sizeof(ResultRing) <= CABIN_RESULT_RING_SIZE, "ResultRing must fit in result ring pages");

INLINE ResultRing* result_ring(void) {
    return (ResultRing*)RESULT_RING_VADDR;
}

bool result_available(void);
uint32_t result_count(void);
bool result_pop(Result* out);

// Uses UMONITOR/UMWAIT on CPUs with WAITPKG; falls back to cooperative yield.
bool result_wait(Result* out, uint32_t timeout_ms);

bool result_pop_non_ipc(Result* out);
bool result_pop_ipc(Result* out);
uint32_t result_ipc_stash_count(void);

#endif // BOX_RESULT_H
