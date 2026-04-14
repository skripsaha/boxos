#ifndef BOXOS_SIZES_H
#define BOXOS_SIZES_H

#define PAGE_SIZE              4096

// Pocket: syscall request structure
// 128-byte layout: header(28) + route_tag(32) + prefixes[32](64) + pad(4)
#define POCKET_MAX_PREFIXES    32    // Max prefix chain length (doubled from 16)
#define POCKET_ROUTE_TAG_SIZE  32    // Route tag string size

// Result: syscall response structure (kept minimal for high ring density)
#define RESULT_RING_CAPACITY   1535  // Fits in 36KB ResultRing (9 pages)

// PocketRing capacity (fits in 4KB page with header): (4096 - 8) / 128 = 31
#define POCKET_RING_CAPACITY   31

// ReadyQueue is now an intrusive linked list — no static capacity limit.

#ifndef BOXOS_SIZES_NO_ASSERT
_Static_assert(POCKET_MAX_PREFIXES == 32, "Max 32 prefixes");
_Static_assert(POCKET_RING_CAPACITY == 31, "31 pockets per ring page");
#endif

#endif // BOXOS_SIZES_H
