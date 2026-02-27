#ifndef BOXOS_SIZES_H
#define BOXOS_SIZES_H

// ============================================================================
// BOXOS SIZE CONSTANTS
// ============================================================================
// Shared size definitions for event/notify/result structures

// Page sizes
#define BOXOS_PAGE_SIZE          4096

// Event system sizes
#define BOXOS_EVENT_DATA_SIZE    256   // Inline data in Event/Notify
#define BOXOS_EVENT_MAX_PREFIXES 16    // Max prefix chain length
#define BOXOS_RESULT_PAYLOAD_SIZE 244  // Result entry payload
#define BOXOS_RESULT_RING_SIZE   15    // Result ring buffer slots

// Compile-time assertions (use in headers that include this)
#ifndef BOXOS_SIZES_NO_ASSERT
_Static_assert(BOXOS_EVENT_DATA_SIZE == 256, "Event data must be 256 bytes");
_Static_assert(BOXOS_EVENT_MAX_PREFIXES == 16, "Max 16 prefixes");
_Static_assert(BOXOS_RESULT_RING_SIZE == 15, "Result ring must be 15 slots");
#endif

#endif // BOXOS_SIZES_H
