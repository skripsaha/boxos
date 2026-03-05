#ifndef ASLR_H
#define ASLR_H

#include "ktypes.h"

// Address Space Layout Randomization for BoxOS
//
// Randomizes per-process: stack top, heap base, buffer heap base.
// Code (flat binary) stays at CABIN_CODE_START_ADDR — position-dependent.
// IPC pages (notify/result) stay at fixed addresses — part of the ABI.
//
// Entropy source: RDRAND (hardware RNG on modern x86-64).
// Fallback: TSC-based PRNG for older CPUs without RDRAND.
//
// Randomization ranges (page-aligned):
//   Stack:    offset 0 .. ASLR_STACK_RANGE  below nominal USER_STACK_TOP
//   Heap:     offset 0 .. ASLR_HEAP_RANGE   above nominal CABIN_HEAP_BASE
//   BufHeap:  offset 0 .. ASLR_BUF_RANGE    above nominal CABIN_BUF_HEAP_START

#define ASLR_STACK_RANGE    (8ULL * 1024 * 1024)     // 8MB — 2048 possible positions
#define ASLR_HEAP_RANGE     (16ULL * 1024 * 1024)    // 16MB — 4096 possible positions
#define ASLR_BUF_RANGE      (16ULL * 1024 * 1024)    // 16MB — 4096 possible positions

#define ASLR_PAGE_SIZE      4096

typedef struct {
    uint64_t stack_offset;      // page-aligned random offset for stack
    uint64_t heap_offset;       // page-aligned random offset for heap
    uint64_t buf_heap_offset;   // page-aligned random offset for buffer heap
} aslr_offsets_t;

void aslr_init(void);

// Generate per-process random offsets
aslr_offsets_t aslr_generate(void);

// Get a page-aligned random value in range [0, max_bytes)
uint64_t aslr_random_offset(uint64_t max_bytes);

#endif // ASLR_H
