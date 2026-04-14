#ifndef ASYNC_IO_H
#define ASYNC_IO_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
#include "atomics.h"
#include "kernel_clock.h"

typedef enum {
    ASYNC_IO_LANE_META  = 0,  // TagFS superblock, tag registry, bitmap writes — highest priority
    ASYNC_IO_LANE_DATA  = 1,  // file data block reads/writes
    ASYNC_IO_LANE_BGND  = 2,  // dedup GC, self-heal, BCDC background work
    ASYNC_IO_LANE_COUNT = 3
} async_io_lane_t;

#define ASYNC_IO_LANE_META_CAPACITY  64
#define ASYNC_IO_LANE_DATA_CAPACITY  256
#define ASYNC_IO_LANE_BGND_CAPACITY  64

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
    uint8_t  port_num;           // AHCI port index (0-based); 0 for ATA/single-drive
    async_io_operation_t op;

    /* For WRITE: buffer points to user heap data which is only valid until
     * ata_dma_start_async_transfer() returns; data must be copied before returning.
     * data_length is the actual valid bytes in buffer_virt (may be < sector_count * 512). */
    void*    buffer_virt;
    uint32_t data_length;
    uint64_t submit_tick;        // kernel_tick_get() at submission — use global tick, not TSC

    uint32_t file_id;
    uint64_t write_offset;
    uint64_t original_file_size;

    async_io_lane_t lane;        // which priority lane this request belongs to
} async_io_request_t;

typedef struct {
    async_io_request_t *slots;
    uint32_t            capacity;
    uint32_t            mask;
    uint32_t            head;
    uint32_t            tail;
    atomic_u32_t        count;
} async_io_lane_queue_t;

typedef struct {
    async_io_lane_queue_t lanes[ASYNC_IO_LANE_COUNT];
    spinlock_t            lock;

    atomic_u32_t total_submitted;
    atomic_u32_t total_completed;
    atomic_u32_t total_failed;
    atomic_u32_t queue_full_count;
    atomic_u64_t total_latency_cycles;
} async_io_queue_t;

void async_io_init(void);
error_t async_io_submit(async_io_request_t* req);
bool async_io_dequeue(async_io_request_t* req);
uint32_t async_io_pending_count(void);
uint32_t async_io_cancel_by_pid(uint32_t pid);
void async_io_mark_completed(uint32_t event_id);
void async_io_mark_completed_with_latency(uint32_t event_id, uint64_t submit_tick);
void async_io_mark_failed(uint32_t event_id);
// timeout_ticks: number of global PIT ticks (kernel_tick_get()) before a request is considered stale.
uint32_t async_io_expire_stale(uint64_t timeout_ticks);

#endif // ASYNC_IO_H
