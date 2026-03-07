#ifndef BOXOS_SIZES_H
#define BOXOS_SIZES_H

#define PAGE_SIZE              4096

// Pocket: syscall request structure
#define POCKET_MAX_PREFIXES    16    // Max prefix chain length
#define POCKET_ROUTE_TAG_SIZE  32    // Route tag string size

// Result: syscall response structure (kept minimal for high ring density)
#define RESULT_RING_CAPACITY   1535  // Fits in 36KB ResultRing (9 pages)

// PocketRing capacity (fits in 4KB page with header)
#define POCKET_RING_CAPACITY   42    // (4096 - 8) / 96

// ReadyQueue
#define READY_QUEUE_CAPACITY   256   // Max processes waiting for Guide

#ifndef BOXOS_SIZES_NO_ASSERT
_Static_assert(POCKET_MAX_PREFIXES == 16, "Max 16 prefixes");
#endif

#endif // BOXOS_SIZES_H
