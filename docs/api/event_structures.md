# Event Structures - Authoritative Specification

This document defines the canonical structure sizes and limits for BoxOS event processing.

## Critical Constants

All code MUST use these exact values:

```c
EVENT_MAX_PREFIXES   = 16    // Maximum prefix chain length
EVENT_DATA_SIZE      = 256   // Event payload size
EVENT_TOTAL_SIZE     = 384   // sizeof(Event) - includes routing header

NOTIFY_MAX_PREFIXES  = 16    // MUST match EVENT_MAX_PREFIXES
NOTIFY_DATA_SIZE     = 256   // MUST match EVENT_DATA_SIZE
NOTIFY_PAGE_SIZE     = 4096  // sizeof(notify_page_t)

RESULT_RING_SIZE     = 15    // Result ring capacity
RESULT_ENTRY_SIZE    = 256   // sizeof(result_entry_t)
RESULT_PAGE_SIZE     = 4096  // sizeof(result_page_t)
```

## Event Structure

**Location:** `src/kernel/core/events.h`

```c
typedef struct __packed {
    uint32_t magic;                        // 4 bytes: 0x45565421 "EVT!"
    uint32_t pid;                          // 4 bytes
    uint32_t event_id;                     // 4 bytes
    uint8_t  current_prefix_idx;           // 1 byte
    uint8_t  prefix_count;                 // 1 byte
    uint8_t  state;                        // 1 byte
    uint8_t  error_deck_idx;               // 1 byte
    uint64_t timestamp;                    // 8 bytes
    boxos_error_t error_code;              // 4 bytes
    boxos_error_t first_error;             // 4 bytes
    // Routing header (IPC)
    uint32_t route_target;                 // 4 bytes: target PID (0 = self)
    uint32_t sender_pid;                   // 4 bytes: original sender (0 = kernel)
    uint8_t  route_flags;                  // 1 byte: ROUTE_SOURCE_*
    char     route_tag[32];                // 32 bytes: tag for fan-out routing
    uint8_t  _reserved_routing[23];        // 23 bytes: future expansion
    uint16_t prefixes[EVENT_MAX_PREFIXES]; // 32 bytes (16 * 2)
    uint8_t  data[EVENT_DATA_SIZE];        // 256 bytes
} Event;                                   // TOTAL: 384 bytes
```

## Notify Page Structure

**Location:** `src/kernel/core/notify_page.h`

```c
typedef struct __packed {
    uint32_t magic;                          // 4 bytes: 0x4E4F5449 "NOTI"
    uint8_t  prefix_count;                   // 1 byte
    uint8_t  flags;                          // 1 byte: NOTIFY_FLAG_CHECK_STATUS
    uint8_t  status;                         // 1 byte: NOTIFY_STATUS_OK/RING_FULL
    uint8_t  reserved1;                      // 1 byte: padding
    uint16_t prefixes[NOTIFY_MAX_PREFIXES];  // 32 bytes (16 * 2)
    uint8_t  data[NOTIFY_DATA_SIZE];         // 256 bytes
    volatile uint8_t event_ring_full;        // 1 byte: backpressure signal (set when EventRing >90% full)
    volatile uint8_t result_page_full;       // 1 byte: backpressure signal (set when Result Page full)
    uint32_t route_target;                   // 4 bytes: target PID for routing (IPC)
    char     route_tag[32];                  // 32 bytes: tag for fan-out routing (IPC)
    uint8_t  _reserved[3762];                // 3762 bytes: pad to 4096
} notify_page_t;                             // TOTAL: 4096 bytes
```

### Status Codes

```c
NOTIFY_STATUS_OK          = 0  // Success
NOTIFY_STATUS_RING_FULL   = 1  // Event Ring overflow
NOTIFY_STATUS_INVALID     = 2  // Invalid parameters
```

### Flags

```c
NOTIFY_FLAG_CHECK_STATUS  = 0x01  // Userspace must check status field
```

## Result Page Structure

**Location:** `src/kernel/core/result_page.h`

```c
typedef struct __packed {
    uint8_t  source;        // 1 byte: ROUTE_SOURCE_KERNEL/ROUTE/HARDWARE
    uint8_t  _reserved1;    // 1 byte
    uint16_t size;          // 2 bytes
    boxos_error_t error_code; // 4 bytes
    uint32_t sender_pid;    // 4 bytes: PID of sender (IPC)
    uint8_t  payload[244];  // 244 bytes
} result_entry_t;           // TOTAL: 256 bytes

typedef struct __packed {
    volatile uint32_t head __attribute__((aligned(64)));  // 64 bytes with padding
    uint8_t _pad1[60];
    volatile uint32_t tail __attribute__((aligned(64)));  // 64 bytes with padding
    uint8_t _pad2[60];
    result_entry_t entries[RESULT_RING_SIZE];             // 3840 bytes (15 * 256)
} result_ring_t;                                          // TOTAL: 3968 bytes

typedef struct __packed {
    uint32_t magic;           // 4 bytes: 0x52455355 "RESU"
    uint32_t _padding;        // 4 bytes
    result_ring_t ring;       // 3968 bytes
    uint8_t _reserved[120];   // 120 bytes: pad to 4096
} result_page_t;              // TOTAL: 4096 bytes
```

## Size Constraints

### Event Processing Limits

1. **Prefix Chain:** Maximum 16 prefixes per event
   - More than 16 will be truncated
   - Terminator 0x0000 must be within first 16

2. **Inline Data:** Maximum 256 bytes per event
   - Larger payloads require storage deck operations

3. **Result Ring:** Maximum 15 pending results
   - When full, kernel returns error or defers to Pending Queue

## Verification

All headers MUST include static assertions:

```c
STATIC_ASSERT(EVENT_MAX_PREFIXES == 16, "See docs/api/event_structures.md");
STATIC_ASSERT(EVENT_DATA_SIZE == 256, "See docs/api/event_structures.md");
STATIC_ASSERT(sizeof(Event) == 384, "Event size mismatch");

STATIC_ASSERT(NOTIFY_MAX_PREFIXES == EVENT_MAX_PREFIXES, "Prefix count mismatch");
STATIC_ASSERT(NOTIFY_DATA_SIZE == EVENT_DATA_SIZE, "Data size mismatch");
STATIC_ASSERT(sizeof(notify_page_t) == 4096, "Notify page size mismatch");

STATIC_ASSERT(RESULT_RING_SIZE == 15, "Result ring size mismatch");
STATIC_ASSERT(sizeof(result_entry_t) == 256, "Result entry size mismatch");
STATIC_ASSERT(sizeof(result_page_t) == 4096, "Result page size mismatch");
```

## References

- Event structure: `src/kernel/core/events.h`
- Notify page: `src/kernel/core/notify_page.h`
- Result page: `src/kernel/core/result_page.h`
- Userspace API: `src/userspace/boxlib/include/box/types.h`
- Guide dispatcher: `docs/core/notify.md`
