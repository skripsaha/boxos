#ifndef ASYNC_IO_H
#define ASYNC_IO_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
#include "atomics.h"

#define ASYNC_IO_QUEUE_SIZE 16
#define ASYNC_IO_QUEUE_MASK (ASYNC_IO_QUEUE_SIZE - 1)

typedef enum {
    ASYNC_IO_OP_READ = 0,
    ASYNC_IO_OP_WRITE = 1
} async_io_operation_t;

typedef struct {
    uint32_t event_id;
    uint32_t pid;
    uint32_t lba;
    uint16_t sector_count;
    uint8_t  is_master;
    async_io_operation_t op;

    /**
     * buffer_virt - Buffer pointer
     *
     * For READ: Destination buffer (user-space or kernel buffer)
     * For WRITE: Source buffer (EPHEMERAL - must be copied in ata_dma_start_async_transfer)
     *            WARNING: WRITE buffers point to Event.data which is only valid until
     *            ata_dma_start_async_transfer() returns. Data MUST be copied to DMA
     *            buffer synchronously before returning.
     */
    void*    buffer_virt;
    uint64_t submit_time;

    // Write-specific metadata (used when op == ASYNC_IO_OP_WRITE)
    uint32_t file_id;
    uint64_t write_offset;
    uint64_t original_file_size;
} async_io_request_t;

typedef struct {
    async_io_request_t queue[ASYNC_IO_QUEUE_SIZE];
    uint8_t head;
    uint8_t tail;
    atomic_u8_t count;
    spinlock_t lock;

    atomic_u32_t total_submitted;
    atomic_u32_t total_completed;
    atomic_u32_t total_failed;
    atomic_u32_t queue_full_count;
    atomic_u64_t total_latency_cycles;
} async_io_queue_t;

/**
 * async_io_init - Initialize async I/O queue
 *
 * Must be called once during kernel initialization before any other async_io_* calls.
 * Initializes queue state, spinlock, and atomic counters.
 */
void async_io_init(void);

/**
 * async_io_submit - Submit async I/O request to pending queue
 * @req: Request structure to submit (must not be NULL)
 *
 * Validates request fields and enqueues if space available.
 * Request is copied, caller can reuse req structure after return.
 *
 * Returns:
 *   BOXOS_OK - Request queued successfully, caller should poll for completion
 *   BOXOS_ERR_IO_QUEUE_FULL - Queue saturated, caller should retry or use synchronous I/O
 *   BOXOS_ERR_NULL_POINTER - req is NULL or req->buffer_virt is NULL
 *   BOXOS_ERR_INVALID_ARGUMENT - Invalid sector_count (0) or operation type
 */
boxos_error_t async_io_submit(async_io_request_t* req);

/**
 * async_io_dequeue - Dequeue next pending request (FIFO order)
 * @req: Output buffer for dequeued request (must not be NULL)
 *
 * Used by I/O dispatch thread/IRQ handler to get next work item.
 * Thread-safe with concurrent submit/cancel operations.
 *
 * Returns:
 *   true - Request dequeued successfully, req filled with data
 *   false - Queue empty or req is NULL
 */
bool async_io_dequeue(async_io_request_t* req);

/**
 * async_io_pending_count - Get current queue size
 *
 * Lock-free atomic read, suitable for polling loops.
 *
 * Returns: Number of pending requests (0 to ASYNC_IO_QUEUE_SIZE)
 */
uint8_t async_io_pending_count(void);

/**
 * async_io_cancel_by_pid - Cancel all pending requests from process
 * @pid: Process ID to filter
 *
 * Removes matching requests from queue, preserving FIFO order for others.
 * Used during process termination to prevent orphaned I/O.
 *
 * Returns: Number of cancelled requests
 */
uint32_t async_io_cancel_by_pid(uint32_t pid);

/**
 * async_io_mark_completed - Mark I/O request as successfully completed
 * @event_id: Event ID from request structure (for logging)
 *
 * Increments total_completed counter. Does NOT track latency.
 * For latency tracking use async_io_mark_completed_with_latency().
 */
void async_io_mark_completed(uint32_t event_id);

/**
 * async_io_mark_completed_with_latency - Mark completed with latency measurement
 * @event_id: Event ID from request
 * @submit_time: TSC timestamp from request->submit_time
 *
 * Calculates latency (rdtsc() - submit_time) and updates average.
 * Preferred over async_io_mark_completed() for performance monitoring.
 */
void async_io_mark_completed_with_latency(uint32_t event_id, uint64_t submit_time);

/**
 * async_io_mark_failed - Mark I/O request as failed
 * @event_id: Event ID from request
 *
 * Increments total_failed counter.
 */
void async_io_mark_failed(uint32_t event_id);

/**
 * async_io_get_stats - Get queue statistics
 * @submitted: Output - total submitted requests (NULL to skip)
 * @completed: Output - total completed requests (NULL to skip)
 * @failed: Output - total failed requests (NULL to skip)
 * @queue_full: Output - queue saturation events (NULL to skip)
 * @avg_latency_cycles: Output - average completion latency in TSC cycles (NULL to skip)
 *
 * All outputs are optional (can pass NULL).
 * Latency is 0 if no completions yet.
 */
void async_io_get_stats(uint32_t* submitted, uint32_t* completed,
                        uint32_t* failed, uint32_t* queue_full,
                        uint64_t* avg_latency_cycles);

#endif // ASYNC_IO_H
