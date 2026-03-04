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

    /* For WRITE: buffer points to Event.data which is only valid until
     * ata_dma_start_async_transfer() returns; data must be copied before returning. */
    void*    buffer_virt;
    uint64_t submit_time;

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

void async_io_init(void);
error_t async_io_submit(async_io_request_t* req);
bool async_io_dequeue(async_io_request_t* req);
uint8_t async_io_pending_count(void);
uint32_t async_io_cancel_by_pid(uint32_t pid);
void async_io_mark_completed(uint32_t event_id);
void async_io_mark_completed_with_latency(uint32_t event_id, uint64_t submit_time);
void async_io_mark_failed(uint32_t event_id);
uint32_t async_io_expire_stale(uint64_t timeout_tsc);
void async_io_get_stats(uint32_t* submitted, uint32_t* completed,
                        uint32_t* failed, uint32_t* queue_full,
                        uint64_t* avg_latency_cycles);

#endif // ASYNC_IO_H
