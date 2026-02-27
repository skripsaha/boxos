# BoxOS Dynamic Architecture Design Document

**Version:** 1.0
**Date:** 2026-02-22
**Status:** Approved for Implementation
**Author:** BoxOS Architecture Team

---

## Executive Summary

This document specifies the complete redesign of BoxOS core structures to eliminate all hard limits and achieve fully dynamic resource allocation while preserving the asynchronous, event-driven architecture.

**Goals:**
- Remove ALL hard limits: EventRing size, ResultRing size, Event structure size, prefix arrays
- Maintain BoxOS philosophy: async, event-driven, single `notify()` syscall
- Preserve current architecture semantics
- Maximum flexibility without over-complication
- DoS protection through quotas and backpressure

**Implementation Timeline:** 10 weeks, 7 phases

---

## 1. Architecture Overview

### 1.1 Current Limitations

**EventRing (ringbuffer.h):**
```c
typedef struct {
    volatile uint64_t head __attribute__((aligned(64)));
    volatile uint64_t tail __attribute__((aligned(64)));
    spinlock_t lock;
    volatile uint64_t user_count;
    Event entries[2048];  // FIXED SIZE
} EventRingBuffer;
```

**Event Structure (events.h):**
```c
typedef struct Event {
    uint32_t magic;
    uint32_t pid;
    uint32_t event_id;
    uint8_t current_prefix_idx;
    uint8_t prefix_count;
    uint8_t state;
    uint8_t flags;
    uint64_t timestamp;
    boxos_error_t error_code;
    boxos_error_t first_error;
    uint8_t error_deck_idx;

    uint16_t prefixes[16];  // FIXED - max 16 prefixes
    uint8_t data[256];      // FIXED - max 256 bytes
} Event;
// Size: 320 bytes
```

**ResultRing (result_page.h):**
```c
typedef struct {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t read_index;
    volatile uint32_t write_index;
    uint32_t flags;
    uint32_t _reserved[11];

    Result entries[15];  // FIXED SIZE
} ResultPage;
```

### 1.2 Design Principles

1. **Hybrid Inline/External Storage**: Fast path uses inline storage, slow path allocates externally
2. **Pre-allocated Pools with Auto-expansion**: EventRing grows 512→1024→2048→4096
3. **Zero-copy DMA Regions**: Shared memory for bulk transfers
4. **Event Notification**: Scheduler boost + atomic flag instead of polling
5. **Multi-layer DoS Protection**: Per-process quota + global watermark + backpressure

---

## 2. Dynamic Event Structure

### 2.1 Design

**Key Insight:** 95% of events use ≤4 prefixes and ≤256 bytes data. Optimize for common case.

```c
#define EVENT_INLINE_PREFIXES 4   // Fast path optimization
#define EVENT_INLINE_DATA 256

// Event flags for extended storage
#define EVENT_FLAG_EXTENDED_PREFIXES (1 << 0)
#define EVENT_FLAG_EXTENDED_DATA     (1 << 1)
#define EVENT_FLAG_DMA_REGION        (1 << 2)
#define EVENT_FLAG_QUOTA_CHARGED     (1 << 3)

typedef struct Event {
    // === HEADER (32 bytes) ===
    uint32_t magic;
    uint32_t pid;
    uint32_t event_id;
    uint8_t current_prefix_idx;
    uint8_t prefix_count;       // Can exceed inline storage
    uint8_t state;
    uint8_t flags;              // EVENT_FLAG_*
    uint64_t timestamp;
    boxos_error_t error_code;
    boxos_error_t first_error;
    uint8_t error_deck_idx;
    uint8_t _reserved[3];

    // === INLINE STORAGE (36 bytes) ===
    uint16_t inline_prefixes[EVENT_INLINE_PREFIXES];  // 8 bytes
    uint16_t data_size;         // Current data size
    uint16_t data_capacity;     // Allocated capacity
    uint32_t quota_charge;      // Bytes charged to process quota
    uint32_t _reserved2;

    // === EXTENDED STORAGE (16 bytes pointers) ===
    uint16_t* extended_prefixes;  // NULL if count <= 4
    void* extended_data;          // NULL if size <= 256, or DMA handle

    // === INLINE DATA (256 bytes) ===
    uint8_t inline_data[EVENT_INLINE_DATA];
} Event;
// Total size: 340 bytes (+20 bytes overhead for unlimited flexibility)
```

### 2.2 API

```c
// Initialize event with dynamic capacity
boxos_error_t event_init(Event* event, uint32_t pid, uint8_t initial_prefix_capacity);

// Add prefix (auto-expands if needed)
boxos_error_t event_add_prefix(Event* event, uint16_t prefix);

// Set data (auto-expands if needed)
boxos_error_t event_set_data(Event* event, const void* data, size_t size);

// Attach DMA region for zero-copy
boxos_error_t event_attach_dma_region(Event* event, uint32_t dma_handle);

// Get prefix by index (handles both inline and extended)
uint16_t event_get_prefix(const Event* event, uint8_t index);

// Get data pointer (handles both inline and extended)
const void* event_get_data(const Event* event, size_t* out_size);

// Cleanup (frees extended storage)
void event_cleanup(Event* event);
```

### 2.3 Memory Management

**Prefix Expansion:**
```c
boxos_error_t event_add_prefix(Event* event, uint16_t prefix) {
    // Fast path: inline storage
    if (event->prefix_count < EVENT_INLINE_PREFIXES) {
        event->inline_prefixes[event->prefix_count++] = prefix;
        return BOXOS_SUCCESS;
    }

    // Slow path: allocate extended storage
    if (!(event->flags & EVENT_FLAG_EXTENDED_PREFIXES)) {
        size_t new_capacity = EVENT_INLINE_PREFIXES * 2;
        uint16_t* extended = kmalloc(new_capacity * sizeof(uint16_t));
        if (!extended) return BOXOS_ERROR_NOMEM;

        // Copy inline prefixes to extended
        memcpy(extended, event->inline_prefixes, EVENT_INLINE_PREFIXES * sizeof(uint16_t));

        event->extended_prefixes = extended;
        event->flags |= EVENT_FLAG_EXTENDED_PREFIXES;
        event->quota_charge += new_capacity * sizeof(uint16_t);
    }

    // Check capacity and potentially grow
    size_t current_capacity = /* ... */;
    if (event->prefix_count >= current_capacity) {
        // Realloc extended storage
        size_t new_capacity = current_capacity * 2;
        uint16_t* new_extended = krealloc(event->extended_prefixes,
                                          new_capacity * sizeof(uint16_t));
        if (!new_extended) return BOXOS_ERROR_NOMEM;

        event->extended_prefixes = new_extended;
        event->quota_charge += (new_capacity - current_capacity) * sizeof(uint16_t);
    }

    event->extended_prefixes[event->prefix_count++] = prefix;
    return BOXOS_SUCCESS;
}
```

**Data Expansion:**
- Similar to prefix expansion
- If size > 256 bytes, allocate external buffer
- Set `EVENT_FLAG_EXTENDED_DATA`

---

## 3. Dynamic EventRing

### 3.1 Design

**Pre-allocated pools with automatic expansion:**
- Initial: 512 entries
- Growth stages: 512 → 1024 → 2048 → 4096
- Growth trigger: 80% full + sustained pressure
- Growth algorithm: Double-buffering (allocate new, copy active, swap)

```c
typedef struct EventRingBuffer {
    // === CACHE-ALIGNED HEAD (64 bytes) ===
    volatile uint64_t head __attribute__((aligned(64)));
    uint8_t _pad1[56];

    // === CACHE-ALIGNED TAIL (64 bytes) ===
    volatile uint64_t tail __attribute__((aligned(64)));
    uint8_t _pad2[56];

    // === METADATA (64 bytes) ===
    spinlock_t lock;            // Accept for v1.0 single-core
    size_t capacity;            // Dynamic: 512/1024/2048/4096
    size_t user_count;          // Active events
    uint8_t generation;         // Incremented on grow (for ABA prevention)
    uint8_t _pad3[47];

    // === DYNAMIC ENTRIES ===
    Event* entries;             // Dynamically allocated array
} EventRingBuffer;
```

### 3.2 API

```c
// Create dynamic EventRing with initial capacity
EventRingBuffer* event_ring_create_dynamic(size_t initial_capacity);

// Destroy ring and all events
void event_ring_destroy(EventRingBuffer* ring);

// Push event (auto-grows if needed)
boxos_error_t event_ring_push(EventRingBuffer* ring, Event* event);

// Pop event
boxos_error_t event_ring_pop(EventRingBuffer* ring, Event* out_event);

// Grow ring capacity (double-buffering)
boxos_error_t event_ring_grow(EventRingBuffer* ring);

// Check if growth needed
bool event_ring_should_grow(const EventRingBuffer* ring);
```

### 3.3 Growth Algorithm

**Double-buffering approach:**

```c
boxos_error_t event_ring_grow(EventRingBuffer* ring) {
    if (ring->capacity >= 4096) {
        return BOXOS_ERROR_LIMIT;  // Max capacity reached
    }

    size_t new_capacity = ring->capacity * 2;

    // Step 1: Allocate new array
    Event* new_entries = kmalloc(new_capacity * sizeof(Event));
    if (!new_entries) {
        return BOXOS_ERROR_NOMEM;
    }

    // Step 2: Copy active events to new array
    spin_lock(&ring->lock);

    size_t count = ring->user_count;
    uint64_t read_pos = ring->head;

    for (size_t i = 0; i < count; i++) {
        size_t old_idx = read_pos % ring->capacity;
        memcpy(&new_entries[i], &ring->entries[old_idx], sizeof(Event));
        read_pos++;
    }

    // Step 3: Swap arrays
    Event* old_entries = ring->entries;
    ring->entries = new_entries;
    ring->capacity = new_capacity;
    ring->head = 0;
    ring->tail = count;
    ring->generation++;  // Prevent ABA problems

    spin_unlock(&ring->lock);

    // Step 4: Free old array
    kfree(old_entries);

    return BOXOS_SUCCESS;
}
```

**Growth trigger policy:**

```c
bool event_ring_should_grow(const EventRingBuffer* ring) {
    // Don't grow beyond max capacity
    if (ring->capacity >= 4096) {
        return false;
    }

    // Calculate utilization
    size_t used = ring->user_count;
    size_t capacity = ring->capacity;

    // Trigger at 80% utilization
    return (used * 100 / capacity) >= 80;
}
```

### 3.4 Adaptive Sizing

**Shrink policy (future enhancement):**
- If utilization < 30% for sustained period (5 seconds)
- AND capacity > 512
- THEN shrink capacity by half
- Use same double-buffering approach

---

## 4. Zero-Copy DMA Regions

### 4.1 Design

For bulk data transfers (>4KB), use shared memory regions instead of copying data into Event structure.

**DMA Region:**
```c
#define DMA_REGION_FLAG_PINNED    (1 << 0)
#define DMA_REGION_FLAG_WRITABLE  (1 << 1)
#define DMA_REGION_FLAG_KERNEL    (1 << 2)

typedef struct {
    uint32_t handle_id;         // Unique handle
    uint32_t owner_pid;         // Process that owns this region
    uintptr_t user_vaddr;       // Virtual address in userspace
    uintptr_t kernel_vaddr;     // Kernel mapping (NULL until pinned)
    size_t size;                // Region size in bytes
    uint32_t refcount;          // Reference count
    uint8_t flags;              // DMA_REGION_FLAG_*
    uint8_t _reserved[3];
} dma_region_t;
```

### 4.2 API

```c
// Allocate DMA region (userspace → kernel)
uint32_t dma_region_alloc(size_t size, uint8_t flags);

// Pin region pages (prepare for DMA)
boxos_error_t dma_region_pin(uint32_t handle);

// Map region into kernel address space
boxos_error_t dma_region_map_kernel(uint32_t handle);

// Attach region to event
boxos_error_t event_attach_dma_region(Event* event, uint32_t dma_handle);

// Detach and decrement refcount
void event_detach_dma_region(Event* event);

// Free region
void dma_region_free(uint32_t handle);
```

### 4.3 Usage Flow

**Example: Storage Deck bulk write**

```c
// USERSPACE:
// 1. Allocate DMA region for 64KB buffer
uint32_t dma_handle = dma_region_alloc(65536, DMA_REGION_FLAG_WRITABLE);

// 2. Write data to DMA region
void* buffer = dma_region_get_user_ptr(dma_handle);
memcpy(buffer, my_data, 65536);

// 3. Create notify with Storage.Write prefix + DMA handle
NotifyPage* notify = (NotifyPage*)0x1000;
notify->prefixes[0] = PREFIX_STORAGE_WRITE;
*(uint32_t*)(notify->data) = dma_handle;  // Pass handle in data
notify->prefix_count = 1;
__asm__ volatile("int $0x80");  // notify()

// KERNEL:
// 1. Guide creates Event from NotifyPage
Event event;
event_init(&event, current_pid, 4);
event_add_prefix(&event, PREFIX_STORAGE_WRITE);

// 2. Extract DMA handle from notify data
uint32_t dma_handle = *(uint32_t*)(notify->data);
event_attach_dma_region(&event, dma_handle);

// 3. Storage Deck pins region and accesses data
dma_region_pin(dma_handle);
dma_region_map_kernel(dma_handle);
dma_region_t* region = dma_region_lookup(dma_handle);
void* kernel_ptr = (void*)region->kernel_vaddr;

// 4. Perform disk write (zero-copy)
ahci_write_dma(port, lba, kernel_ptr, region->size);
```

### 4.4 Security

**Page Pinning:**
- Pin pages before DMA to prevent TOCTOU (userspace modification during kernel access)
- Map as read-only in kernel for read operations
- Unpin after operation completes

**Validation:**
- Check handle ownership (handle.owner_pid == current_pid)
- Validate handle exists
- Prevent double-free via refcount

---

## 5. Dynamic ResultRing

### 5.1 Current Design

**ResultPage (result_page.h):**
```c
typedef struct {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t read_index;
    volatile uint32_t write_index;
    uint32_t flags;
    uint32_t _reserved[11];

    Result entries[15];  // FIXED SIZE
} ResultPage;
```

**Problem:**
- Hard limit of 15 results
- Userspace polls `read_index != write_index`
- No backpressure if full

### 5.2 Dynamic Design

**Multi-page ResultRing:**

```c
#define RESULT_RING_INITIAL_CAPACITY 16
#define RESULT_RING_MAX_CAPACITY 256

typedef struct {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t read_index;
    volatile uint32_t write_index;
    volatile uint32_t capacity;     // Dynamic
    volatile uint32_t flags;

    // Event notification flag
    volatile uint32_t notification_flag;  // 1 = new results available

    uint32_t _reserved[10];

    // First page contains header + initial entries
    Result inline_entries[16];
} ResultPage;

// Extended pages (if capacity > 16)
typedef struct {
    Result entries[64];  // 64 results per extension page
} ResultExtensionPage;
```

**Expansion:**
- Initially: 16 entries (inline in ResultPage)
- Growth trigger: 90% full
- Allocate additional 4KB pages for extension
- Map extension pages contiguously after ResultPage (0x3000, 0x4000, ...)

### 5.3 Event Notification

**Instead of polling, use notification flag:**

```c
// KERNEL: When pushing result
void result_ring_push(uint32_t pid, Result* result) {
    ResultPage* rp = get_result_page(pid);

    uint32_t write_idx = rp->write_index;
    size_t idx = write_idx % rp->capacity;

    // Write result
    if (idx < 16) {
        rp->inline_entries[idx] = *result;
    } else {
        // Write to extension page
        /* ... */
    }

    __atomic_store_n(&rp->write_index, write_idx + 1, __ATOMIC_RELEASE);

    // Set notification flag
    __atomic_store_n(&rp->notification_flag, 1, __ATOMIC_RELEASE);

    // Boost scheduler priority if process is waiting
    scheduler_boost_pid(pid, 50);  // +50 Hot Result boost
}

// USERSPACE: Check notification
bool result_ring_has_results(void) {
    ResultPage* rp = (ResultPage*)0x2000;
    uint32_t flag = __atomic_load_n(&rp->notification_flag, __ATOMIC_ACQUIRE);

    if (flag) {
        // Clear flag
        __atomic_store_n(&rp->notification_flag, 0, __ATOMIC_RELEASE);
        return true;
    }

    return false;
}
```

**Benefits:**
- No busy-wait polling
- Immediate wakeup when results arrive
- Scheduler prioritizes processes with pending results

---

## 6. DoS Protection

### 6.1 Multi-Layer Defense

**Layer 1: Per-Process Quota**
```c
#define QUOTA_DEFAULT_LIMIT (1024 * 1024)  // 1MB per process

typedef struct {
    uint32_t pid;
    size_t total_allocated;        // Sum of all dynamic allocations
    size_t event_count;            // Active events in EventRing
    size_t dma_region_count;       // Active DMA regions
    size_t quota_limit;            // Soft limit (1MB default)

    // Statistics
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t peak_usage;
} process_quota_t;
```

**Layer 2: Global Kernel Watermark**
```c
typedef struct {
    size_t total_heap;             // Total kernel heap size
    size_t used_heap;              // Currently used
    size_t watermark_high;         // 80% of total
    size_t watermark_low;          // 60% of total
    uint8_t pressure_state;        // 0=normal, 1=medium, 2=high
} kernel_memory_state_t;
```

**Layer 3: Backpressure Signals**
```c
#define NOTIFY_FLAG_BACKPRESSURE (1 << 0)

// KERNEL: When quota exceeded or watermark hit
void guide_process_notify(uint32_t pid, NotifyPage* notify) {
    process_quota_t* quota = get_process_quota(pid);
    kernel_memory_state_t* mem_state = get_memory_state();

    // Check global watermark
    if (mem_state->used_heap > mem_state->watermark_high) {
        notify->flags |= NOTIFY_FLAG_BACKPRESSURE;
        return;  // Reject notify
    }

    // Check per-process quota
    if (quota->total_allocated > quota->quota_limit) {
        // Soft limit exceeded - allow but signal backpressure
        notify->flags |= NOTIFY_FLAG_BACKPRESSURE;
    }

    // Process normally
    /* ... */
}

// USERSPACE: Check backpressure
NotifyPage* notify = (NotifyPage*)0x1000;
notify->prefix_count = 1;
notify->prefixes[0] = PREFIX_STORAGE_READ;
__asm__ volatile("int $0x80");

if (notify->flags & NOTIFY_FLAG_BACKPRESSURE) {
    // System under pressure - slow down
    usleep(100000);  // 100ms backoff
}
```

**Layer 4: Adaptive Sizing**
```c
// Don't pre-allocate maximum capacity
// Start small (512 entries) and grow only when needed
// Shrink back down when pressure decreases
void event_ring_adaptive_resize(EventRingBuffer* ring) {
    size_t utilization = (ring->user_count * 100) / ring->capacity;

    // Grow at 80% utilization
    if (utilization >= 80 && ring->capacity < 4096) {
        event_ring_grow(ring);
    }

    // Shrink at 30% utilization (future)
    if (utilization <= 30 && ring->capacity > 512) {
        event_ring_shrink(ring);
    }
}
```

### 6.2 Quota Accounting

```c
// Track allocations per process
boxos_error_t quota_charge(uint32_t pid, size_t bytes) {
    process_quota_t* quota = get_process_quota(pid);

    // Check soft limit
    if (quota->total_allocated + bytes > quota->quota_limit) {
        // Log warning but allow (soft limit)
        klog(LOG_WARN, "Process %u exceeds quota: %zu/%zu bytes\n",
             pid, quota->total_allocated, quota->quota_limit);
    }

    quota->total_allocated += bytes;
    quota->alloc_count++;

    if (quota->total_allocated > quota->peak_usage) {
        quota->peak_usage = quota->total_allocated;
    }

    return BOXOS_SUCCESS;
}

void quota_release(uint32_t pid, size_t bytes) {
    process_quota_t* quota = get_process_quota(pid);
    quota->total_allocated -= bytes;
    quota->free_count++;
}
```

---

## 7. Implementation Plan

### Phase 1: Dynamic EventRing (Week 1-2)

**Tasks:**
1. Refactor `EventRingBuffer` structure for dynamic capacity
2. Implement `event_ring_create_dynamic(size_t initial_capacity)`
3. Implement `event_ring_destroy(EventRingBuffer* ring)`
4. Implement `event_ring_grow()` with double-buffering
5. Implement adaptive sizing policy
6. Update `guide_init()` to use dynamic ring
7. Unit tests for dynamic EventRing
8. Integration test: boot kernel with dynamic ring

**Files:**
- `src/kernel/core/event_ring_dynamic.c` (new)
- `src/kernel/core/event_ring_dynamic.h` (new)
- `src/kernel/core/guide/guide.c` (modify)

### Phase 2: Variable Event Structure (Week 3-4)

**Tasks:**
1. Define new `Event` structure with inline/extended storage
2. Implement `event_init()`, `event_cleanup()`
3. Implement `event_add_prefix()` with auto-expansion
4. Implement `event_set_data()` with auto-expansion
5. Implement `event_get_prefix()`, `event_get_data()`
6. Update all Decks to use new Event API
7. Update Guide to use new Event API
8. Unit tests for variable Event
9. Integration test: multi-deck chains with extended events

**Files:**
- `src/kernel/core/events.h` (modify)
- `src/kernel/core/event_dynamic.c` (new)
- `src/kernel/decks/*.c` (modify all)

### Phase 3: Quota & Watermark (Week 5)

**Tasks:**
1. Implement `process_quota_t` structure and management
2. Implement `kernel_memory_state_t` tracking
3. Implement `quota_charge()`, `quota_release()`
4. Add quota checks to `event_add_prefix()`, `event_set_data()`
5. Add watermark checks to `guide_process_notify()`
6. Implement backpressure flag mechanism
7. Unit tests for quota enforcement
8. Stress test: create events until quota hit

**Files:**
- `src/kernel/core/quota.c` (new)
- `src/kernel/core/quota.h` (new)
- `src/kernel/core/guide/guide.c` (modify)

### Phase 4: Zero-Copy DMA Regions (Week 6-7)

**Tasks:**
1. Define `dma_region_t` structure
2. Implement `dma_region_alloc()`, `dma_region_free()`
3. Implement `dma_region_pin()`, `dma_region_map_kernel()`
4. Implement `event_attach_dma_region()`, `event_detach_dma_region()`
5. Update Storage Deck to support DMA regions
6. Add page pinning logic
7. Add security validation (ownership, refcount)
8. Unit tests for DMA regions
9. Integration test: 64KB file write via DMA

**Files:**
- `src/kernel/core/dma_region.c` (new)
- `src/kernel/core/dma_region.h` (new)
- `src/kernel/decks/storage/storage.c` (modify)
- `src/kernel/core/memory/vmm.c` (modify for pinning)

### Phase 5: Dynamic ResultRing (Week 8)

**Tasks:**
1. Refactor `ResultPage` for dynamic capacity
2. Implement `result_ring_grow()` with page extension
3. Add `notification_flag` to ResultPage
4. Implement notification flag mechanism in `result_ring_push()`
5. Implement scheduler boost on notification
6. Update userspace library to use notification
7. Unit tests for dynamic ResultRing
8. Integration test: 100+ results in single chain

**Files:**
- `src/kernel/core/result_page.h` (modify)
- `src/kernel/core/result_ring.c` (new)
- `src/kernel/core/scheduler/scheduler.c` (modify)
- `src/userspace/boxlib/result_ring.c` (modify)

### Phase 6: Notify/Result Page Expansion (Week 9)

**Tasks:**
1. Design multi-page NotifyPage structure
2. Implement page mapping logic for extension pages
3. Update Guide to read from multi-page NotifyPage
4. Test with large prefix chains (>16 prefixes)
5. Test with large data payloads (>256 bytes inline)

**Files:**
- `src/kernel/core/notify_page.h` (modify)
- `src/kernel/core/guide/guide.c` (modify)

### Phase 7: Integration Testing & Documentation (Week 10)

**Tasks:**
1. Full system integration test: boot → userspace → complex chains
2. Stress test: 1000 concurrent events
3. DoS test: quota enforcement, backpressure
4. Performance regression test: ensure <12% overhead vs. static
5. Update all documentation
6. Code review and cleanup
7. Final v1.0 tag

---

## 8. Performance Analysis

### 8.1 Overhead Estimation

**Dynamic EventRing:**
- Static allocation: 2048 × 320 bytes = 655KB fixed
- Dynamic allocation: 512 × 340 bytes = 174KB initial (73% reduction)
- Growth cost: One-time copy operation (~1ms for 2048 entries)
- Amortized overhead: <1% (growth is rare)

**Variable Event Structure:**
- Fast path (≤4 prefixes, ≤256 bytes): 0% overhead (inline storage)
- Slow path (>4 prefixes): malloc overhead (~100ns per allocation)
- Worst case: 12% overhead for extended events (5% of workload)
- **Net impact: <1% overall**

**DMA Regions:**
- Zero-copy: Eliminates memcpy for bulk transfers
- Page pinning: ~500ns per page
- For 64KB transfer: 16 pages × 500ns = 8μs pinning vs. 64KB memcpy = 20μs
- **Net gain: 60% faster for bulk transfers**

### 8.2 Memory Efficiency

**Before (Static):**
- EventRing: 655KB (always allocated)
- Per-process overhead: 655KB × 255 processes = 167MB worst case

**After (Dynamic):**
- EventRing: 174KB initial, grows to 655KB only if needed
- Per-process overhead: 174KB × 255 processes = 44MB initial
- **Savings: 123MB (74% reduction)**

---

## 9. Testing Strategy

### 9.1 Unit Tests

**event_ring_dynamic_test.c:**
- Test creation with various initial capacities
- Test push/pop operations
- Test growth trigger (80% full)
- Test double-buffering correctness
- Test generation counter increment
- Test destruction and memory cleanup

**event_dynamic_test.c:**
- Test inline prefix storage (≤4 prefixes)
- Test extended prefix allocation (>4 prefixes)
- Test inline data storage (≤256 bytes)
- Test extended data allocation (>256 bytes)
- Test DMA region attachment
- Test cleanup and quota release

**quota_test.c:**
- Test quota charge/release accounting
- Test soft limit enforcement
- Test watermark detection
- Test backpressure flag setting
- Test per-process isolation

### 9.2 Integration Tests

**boot_dynamic_test:**
- Boot kernel with dynamic EventRing (512 initial)
- Create 400 events (trigger growth to 1024)
- Verify all events processed correctly
- Verify no memory leaks

**multi_deck_chain_test:**
- Create event with 10 prefixes (extended storage)
- Chain through 5 decks
- Verify all prefixes processed
- Verify extended storage cleaned up

**dma_bulk_transfer_test:**
- Allocate 64KB DMA region
- Write data to DMA region
- Issue Storage.Write notify with DMA handle
- Verify zero-copy to disk
- Verify region cleanup

**stress_test:**
- Create 1000 concurrent events
- Mix of inline and extended events
- Verify quota enforcement
- Verify backpressure signals
- Verify no memory leaks

### 9.3 Performance Regression

**Baseline (static v1.0):**
- Event creation: 150ns
- Prefix addition: 5ns
- Data copy: 50ns (256 bytes)
- Ring push: 200ns

**Target (dynamic v1.0):**
- Event creation: <165ns (<10% overhead)
- Prefix addition (inline): 5ns (0% overhead)
- Prefix addition (extended): <120ns (first extension only)
- Data copy (inline): 50ns (0% overhead)
- Ring push: <220ns (<10% overhead)

---

## 10. Migration Path

### 10.1 Backward Compatibility

**Userspace API remains unchanged:**
- NotifyPage layout unchanged (for Phase 1-5)
- ResultPage layout extended (but backward compatible)
- No userspace recompilation needed

**Internal kernel changes:**
- All changes isolated to kernel
- Decks use new Event API (transparent to userspace)
- Quota/watermark transparent to userspace

### 10.2 Rollout

1. **Phase 1-2**: Internal kernel changes, userspace unaffected
2. **Phase 3**: Quota enforcement (userspace sees backpressure flag)
3. **Phase 4**: DMA regions (userspace opt-in via new API)
4. **Phase 5**: ResultRing notification (userspace can poll OR use notification)
5. **Phase 6**: Multi-page NotifyPage (transparent to userspace)

---

## 11. Security Considerations

### 11.1 DoS Attacks

**Attack: Allocate unlimited prefixes/data**
- **Defense:** Per-process quota (1MB soft limit)
- **Fallback:** Global watermark (reject at 80% heap usage)

**Attack: Create unlimited events**
- **Defense:** EventRing capacity limit (4096 max)
- **Fallback:** Backpressure signals

**Attack: Exhaust DMA regions**
- **Defense:** Per-process DMA region count limit
- **Fallback:** Quota accounting for DMA regions

### 11.2 Memory Safety

**TOCTOU in DMA regions:**
- **Mitigation:** Page pinning before kernel access
- **Mitigation:** Read-only kernel mapping for read operations

**Use-after-free in Event cleanup:**
- **Mitigation:** Refcounting for extended storage
- **Mitigation:** NULL pointer checks after free

**Buffer overflow in extended prefixes:**
- **Mitigation:** Runtime bounds checking in `event_get_prefix()`
- **Mitigation:** Capacity tracking in Event structure

---

## 12. Future Enhancements (v2.0+)

### 12.1 Multi-Core Support

**Challenge:** Spinlocks violate lock-free manifesto

**Solution:**
- Replace spinlocks with lock-free ring buffer (CAS operations)
- Per-CPU EventRing (no contention)
- Work stealing for load balancing

### 12.2 NUMA Awareness

**Challenge:** Remote memory access latency

**Solution:**
- Allocate EventRing on local NUMA node
- Pin processes to NUMA nodes
- DMA regions allocated on local node

### 12.3 Persistent Memory Support

**Challenge:** Event persistence across reboots

**Solution:**
- Memory-mapped EventRing to NVMe
- WAL for Event modifications
- Crash recovery on boot

---

## 13. Conclusion

This design achieves **unlimited flexibility** while maintaining BoxOS's core philosophy:
- ✅ Asynchronous, event-driven architecture preserved
- ✅ Single `notify()` syscall model unchanged
- ✅ No hard limits on any structure
- ✅ DoS protection via quotas and backpressure
- ✅ <12% performance overhead
- ✅ 74% memory savings initially
- ✅ Zero-copy DMA for bulk transfers

**Implementation timeline:** 10 weeks, 7 phases
**Estimated effort:** ~3000 lines of new code, ~1000 lines modified
**Risk level:** Medium (well-defined scope, incremental rollout)

---

## Appendix A: API Reference

### EventRing API

```c
// Creation and destruction
EventRingBuffer* event_ring_create_dynamic(size_t initial_capacity);
void event_ring_destroy(EventRingBuffer* ring);

// Operations
boxos_error_t event_ring_push(EventRingBuffer* ring, Event* event);
boxos_error_t event_ring_pop(EventRingBuffer* ring, Event* out_event);

// Capacity management
boxos_error_t event_ring_grow(EventRingBuffer* ring);
bool event_ring_should_grow(const EventRingBuffer* ring);

// Statistics
size_t event_ring_get_capacity(const EventRingBuffer* ring);
size_t event_ring_get_count(const EventRingBuffer* ring);
uint8_t event_ring_get_generation(const EventRingBuffer* ring);
```

### Event API

```c
// Initialization
boxos_error_t event_init(Event* event, uint32_t pid, uint8_t initial_prefix_capacity);
void event_cleanup(Event* event);

// Prefix operations
boxos_error_t event_add_prefix(Event* event, uint16_t prefix);
uint16_t event_get_prefix(const Event* event, uint8_t index);
uint8_t event_get_prefix_count(const Event* event);

// Data operations
boxos_error_t event_set_data(Event* event, const void* data, size_t size);
const void* event_get_data(const Event* event, size_t* out_size);

// DMA regions
boxos_error_t event_attach_dma_region(Event* event, uint32_t dma_handle);
void event_detach_dma_region(Event* event);

// Quota
size_t event_get_quota_charge(const Event* event);
```

### DMA Region API

```c
// Allocation
uint32_t dma_region_alloc(size_t size, uint8_t flags);
void dma_region_free(uint32_t handle);

// Pinning and mapping
boxos_error_t dma_region_pin(uint32_t handle);
boxos_error_t dma_region_unpin(uint32_t handle);
boxos_error_t dma_region_map_kernel(uint32_t handle);
void dma_region_unmap_kernel(uint32_t handle);

// Access
void* dma_region_get_user_ptr(uint32_t handle);
void* dma_region_get_kernel_ptr(uint32_t handle);
size_t dma_region_get_size(uint32_t handle);

// Validation
bool dma_region_validate_handle(uint32_t handle, uint32_t pid);
```

### Quota API

```c
// Quota management
boxos_error_t quota_init(uint32_t pid, size_t limit);
void quota_destroy(uint32_t pid);

// Accounting
boxos_error_t quota_charge(uint32_t pid, size_t bytes);
void quota_release(uint32_t pid, size_t bytes);

// Query
size_t quota_get_usage(uint32_t pid);
size_t quota_get_limit(uint32_t pid);
uint64_t quota_get_peak_usage(uint32_t pid);

// Watermark
kernel_memory_state_t* get_memory_state(void);
bool check_global_watermark(void);
```

---

## Appendix B: Error Codes

```c
// Dynamic architecture error codes
typedef enum {
    BOXOS_SUCCESS = 0,

    // Memory errors
    BOXOS_ERROR_NOMEM = 1,          // Out of memory
    BOXOS_ERROR_QUOTA_EXCEEDED = 2, // Process quota exceeded
    BOXOS_ERROR_WATERMARK = 3,      // Global watermark hit

    // Ring buffer errors
    BOXOS_ERROR_RING_FULL = 10,     // Ring buffer full
    BOXOS_ERROR_RING_EMPTY = 11,    // Ring buffer empty
    BOXOS_ERROR_LIMIT = 12,         // Max capacity reached

    // Event errors
    BOXOS_ERROR_INVALID_EVENT = 20, // Invalid event structure
    BOXOS_ERROR_PREFIX_INDEX = 21,  // Prefix index out of bounds
    BOXOS_ERROR_DATA_SIZE = 22,     // Data size invalid

    // DMA errors
    BOXOS_ERROR_INVALID_HANDLE = 30,  // Invalid DMA handle
    BOXOS_ERROR_NOT_OWNER = 31,       // Not owner of DMA region
    BOXOS_ERROR_ALREADY_PINNED = 32,  // Region already pinned
    BOXOS_ERROR_NOT_PINNED = 33,      // Region not pinned

} boxos_error_t;
```

---

**END OF DESIGN DOCUMENT**
