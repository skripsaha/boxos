#include "async_io.h"

static async_io_queue_t g_async_queue;

void async_io_init(void) {
    memset(&g_async_queue, 0, sizeof(async_io_queue_t));
    spinlock_init(&g_async_queue.lock);

    atomic_init_u8(&g_async_queue.count, 0);
    atomic_init_u32(&g_async_queue.total_submitted, 0);
    atomic_init_u32(&g_async_queue.total_completed, 0);
    atomic_init_u32(&g_async_queue.total_failed, 0);
    atomic_init_u32(&g_async_queue.queue_full_count, 0);
    atomic_init_u64(&g_async_queue.total_latency_cycles, 0);

    debug_printf("[AsyncIO] Initialized (queue size: %d)\n", ASYNC_IO_QUEUE_SIZE);
}

error_t async_io_submit(async_io_request_t* req) {
    if (req == NULL) {
        debug_printf("[AsyncIO] ERROR: NULL request pointer\n");
        return ERR_NULL_POINTER;
    }

    if (req->sector_count == 0) {
        debug_printf("[AsyncIO] ERROR: Invalid sector_count=0\n");
        return ERR_INVALID_ARGUMENT;
    }

    if (req->buffer_virt == NULL) {
        debug_printf("[AsyncIO] ERROR: NULL buffer pointer\n");
        return ERR_NULL_POINTER;
    }

    if (req->op != ASYNC_IO_OP_READ && req->op != ASYNC_IO_OP_WRITE) {
        debug_printf("[AsyncIO] ERROR: Invalid operation=%u\n", req->op);
        return ERR_INVALID_ARGUMENT;
    }

    req->submit_time = rdtsc();

    if (atomic_load_u8(&g_async_queue.count) >= ASYNC_IO_QUEUE_SIZE) {
        atomic_fetch_add_u32(&g_async_queue.queue_full_count, 1);
        debug_printf("[AsyncIO] Queue FULL (pid=%u, lba=%u)\n", req->pid, req->lba);
        return ERR_IO_QUEUE_FULL;
    }

    spin_lock(&g_async_queue.lock);

    if (atomic_load_u8(&g_async_queue.count) >= ASYNC_IO_QUEUE_SIZE) {
        spin_unlock(&g_async_queue.lock);
        atomic_fetch_add_u32(&g_async_queue.queue_full_count, 1);
        debug_printf("[AsyncIO] Queue FULL (pid=%u, lba=%u)\n", req->pid, req->lba);
        return ERR_IO_QUEUE_FULL;
    }

    g_async_queue.queue[g_async_queue.tail] = *req;
    g_async_queue.tail = (g_async_queue.tail + 1) & ASYNC_IO_QUEUE_MASK;
    atomic_fetch_add_u8(&g_async_queue.count, 1);

    spin_unlock(&g_async_queue.lock);

    atomic_fetch_add_u32(&g_async_queue.total_submitted, 1);

    debug_printf("[AsyncIO] Submitted (pid=%u, op=%s, lba=%u, sectors=%u, queue=%u/%u)\n",
                 req->pid,
                 req->op == ASYNC_IO_OP_READ ? "READ" : "WRITE",
                 req->lba,
                 req->sector_count,
                 atomic_load_u8(&g_async_queue.count),
                 ASYNC_IO_QUEUE_SIZE);

    return OK;
}

bool async_io_dequeue(async_io_request_t* req) {
    if (req == NULL) {
        debug_printf("[AsyncIO] ERROR: NULL request output pointer\n");
        return false;
    }

    if (atomic_load_u8(&g_async_queue.count) == 0) {
        return false;
    }

    spin_lock(&g_async_queue.lock);

    if (atomic_load_u8(&g_async_queue.count) == 0) {
        spin_unlock(&g_async_queue.lock);
        return false;
    }

    *req = g_async_queue.queue[g_async_queue.head];
    g_async_queue.head = (g_async_queue.head + 1) & ASYNC_IO_QUEUE_MASK;
    atomic_fetch_sub_u8(&g_async_queue.count, 1);

    spin_unlock(&g_async_queue.lock);

    debug_printf("[AsyncIO] Dequeued (pid=%u, op=%s, lba=%u, queue=%u/%u)\n",
                 req->pid,
                 req->op == ASYNC_IO_OP_READ ? "READ" : "WRITE",
                 req->lba,
                 atomic_load_u8(&g_async_queue.count),
                 ASYNC_IO_QUEUE_SIZE);

    return true;
}

uint8_t async_io_pending_count(void) {
    return atomic_load_u8(&g_async_queue.count);
}

uint32_t async_io_cancel_by_pid(uint32_t pid) {
    spin_lock(&g_async_queue.lock);

    uint32_t cancelled = 0;
    async_io_request_t temp_queue[ASYNC_IO_QUEUE_SIZE];
    uint8_t temp_count = 0;

    uint8_t current_count = atomic_load_u8(&g_async_queue.count);
    for (uint8_t i = 0; i < current_count; i++) {
        uint8_t idx = (g_async_queue.head + i) & ASYNC_IO_QUEUE_MASK;

        if (g_async_queue.queue[idx].pid == pid) {
            cancelled++;
            debug_printf("[AsyncIO] Cancelling pid=%u, lba=%u\n",
                         pid, g_async_queue.queue[idx].lba);
        } else {
            temp_queue[temp_count++] = g_async_queue.queue[idx];
        }
    }

    g_async_queue.head = 0;
    for (uint8_t i = 0; i < temp_count; i++) {
        g_async_queue.queue[i] = temp_queue[i];
    }
    g_async_queue.tail = temp_count;
    atomic_store_u8(&g_async_queue.count, temp_count);

    spin_unlock(&g_async_queue.lock);

    if (cancelled > 0) {
        debug_printf("[AsyncIO] Cancelled %u requests from pid=%u (remaining=%u)\n",
                     cancelled, pid, temp_count);
    }

    return cancelled;
}

void async_io_mark_completed(uint32_t event_id) {
    atomic_fetch_add_u32(&g_async_queue.total_completed, 1);
    debug_printf("[AsyncIO] Completed event_id=%u (total=%u)\n",
                 event_id, atomic_load_u32(&g_async_queue.total_completed));
}

void async_io_mark_completed_with_latency(uint32_t event_id, uint64_t submit_time) {
    uint64_t now = rdtsc();
    uint64_t latency = now - submit_time;

    atomic_fetch_add_u64(&g_async_queue.total_latency_cycles, latency);
    atomic_fetch_add_u32(&g_async_queue.total_completed, 1);

    debug_printf("[AsyncIO] Completed event_id=%u latency=%llu cycles (total=%u)\n",
                 event_id, latency, atomic_load_u32(&g_async_queue.total_completed));
}

void async_io_mark_failed(uint32_t event_id) {
    atomic_fetch_add_u32(&g_async_queue.total_failed, 1);
    debug_printf("[AsyncIO] Failed event_id=%u (total=%u)\n",
                 event_id, atomic_load_u32(&g_async_queue.total_failed));
}

void async_io_get_stats(uint32_t* submitted, uint32_t* completed,
                        uint32_t* failed, uint32_t* queue_full,
                        uint64_t* avg_latency_cycles) {
    if (submitted) *submitted = atomic_load_u32(&g_async_queue.total_submitted);
    if (completed) *completed = atomic_load_u32(&g_async_queue.total_completed);
    if (failed) *failed = atomic_load_u32(&g_async_queue.total_failed);
    if (queue_full) *queue_full = atomic_load_u32(&g_async_queue.queue_full_count);

    if (avg_latency_cycles) {
        uint32_t comp = atomic_load_u32(&g_async_queue.total_completed);
        if (comp > 0) {
            uint64_t total_lat = atomic_load_u64(&g_async_queue.total_latency_cycles);
            *avg_latency_cycles = total_lat / comp;
        } else {
            *avg_latency_cycles = 0;
        }
    }
}
