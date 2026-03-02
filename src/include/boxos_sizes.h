#ifndef BOXOS_SIZES_H
#define BOXOS_SIZES_H

#define PAGE_SIZE          4096

#define EVENT_DATA_SIZE    256   // Inline data in Event/Notify
#define EVENT_MAX_PREFIXES 16    // Max prefix chain length
#define RESULT_PAYLOAD_SIZE 244  // Result entry payload
#define RESULT_RING_SIZE   111   // Result ring buffer slots (fits in 28KB result region)

#ifndef BOXOS_SIZES_NO_ASSERT
_Static_assert(EVENT_DATA_SIZE == 256, "Event data must be 256 bytes");
_Static_assert(EVENT_MAX_PREFIXES == 16, "Max 16 prefixes");
_Static_assert(RESULT_RING_SIZE == 111, "Result ring must be 111 slots");
#endif

#endif // BOXOS_SIZES_H
