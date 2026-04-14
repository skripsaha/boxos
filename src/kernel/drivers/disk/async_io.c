#include "async_io.h"

static async_io_queue_t g_async_queue;

void async_io_init(void) {
    memset(&g_async_queue, 0, sizeof(async_io_queue_t));
    spinlock_init(&g_async_queue.lock);

    g_async_queue.capacity = ASYNC_IO_DEFAULT_CAPACITY;
    g_async_queue.mask     = ASYNC_IO_DEFAULT_CAPACITY - 1;
    g_async_queue.queue    = kmalloc(sizeof(async_io_request_t) * ASYNC_IO_DEFAULT_CAPACITY);
    if (!g_async_queue.queue) {
        kprintf("[AsyncIO] FATAL: cannot allocate queue\n");
        while (1) { __asm__ volatile("cli; hlt"); }
    }
    memset(g_async_queue.queue, 0, sizeof(async_io_request_t) * ASYNC_IO_DEFAULT_CAPACITY);

    atomic_init_u32(&g_async_queue.count, 0);
    atomic_init_u32(&g_async_queue.total_submitted, 0);
    atomic_init_u32(&g_async_queue.total_completed, 0);
    atomic_init_u32(&g_async_queue.total_failed, 0);
    atomic_init_u32(&g_async_queue.queue_full_count, 0);
    atomic_init_u64(&g_async_queue.total_latency_cycles, 0);

    debug_printf("[AsyncIO] Initialized (capacity: %u)\n", ASYNC_IO_DEFAULT_CAPACITY);
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

    req->submit_tick = kernel_tick_get();

    if (atomic_load_u32(&g_async_queue.count) >= g_async_queue.capacity) {
        atomic_fetch_add_u32(&g_async_queue.queue_full_count, 1);
        debug_printf("[AsyncIO] Queue FULL (pid=%u, lba=%u)\n", req->pid, req->lba);
        return ERR_IO_QUEUE_FULL;
    }

    spin_lock(&g_async_queue.lock);

    if (atomic_load_u32(&g_async_queue.count) >= g_async_queue.capacity) {
        spin_unlock(&g_async_queue.lock);
        atomic_fetch_add_u32(&g_async_queue.queue_full_count, 1);
        debug_printf("[AsyncIO] Queue FULL (pid=%u, lba=%u)\n", req->pid, req->lba);
        return ERR_IO_QUEUE_FULL;
    }

    g_async_queue.queue[g_async_queue.tail] = *req;
    g_async_queue.tail = (g_async_queue.tail + 1) & g_async_queue.mask;
    atomic_fetch_add_u32(&g_async_queue.count, 1);

    spin_unlock(&g_async_queue.lock);

    atomic_fetch_add_u32(&g_async_queue.total_submitted, 1);

    debug_printf("[AsyncIO] Submitted (pid=%u, op=%s, lba=%u, sectors=%u, queue=%u/%u)\n",
                 req->pid,
                 req->op == ASYNC_IO_OP_READ ? "READ" : "WRITE",
                 req->lba,
                 req->sector_count,
                 atomic_load_u32(&g_async_queue.count),
                 g_async_queue.capacity);

    return OK;
}

bool async_io_dequeue(async_io_request_t* req) {
    if (req == NULL) {
        debug_printf("[AsyncIO] ERROR: NULL request output pointer\n");
        return false;
    }

    if (atomic_load_u32(&g_async_queue.count) == 0) {
        return false;
    }

    spin_lock(&g_async_queue.lock);

    if (atomic_load_u32(&g_async_queue.count) == 0) {
        spin_unlock(&g_async_queue.lock);
        return false;
    }

    *req = g_async_queue.queue[g_async_queue.head];
    g_async_queue.head = (g_async_queue.head + 1) & g_async_queue.mask;
    atomic_fetch_sub_u32(&g_async_queue.count, 1);

    spin_unlock(&g_async_queue.lock);

    debug_printf("[AsyncIO] Dequeued (pid=%u, op=%s, lba=%u, queue=%u/%u)\n",
                 req->pid,
                 req->op == ASYNC_IO_OP_READ ? "READ" : "WRITE",
                 req->lba,
                 atomic_load_u32(&g_async_queue.count),
                 g_async_queue.capacity);

    return true;
}

uint32_t async_io_pending_count(void) {
    return atomic_load_u32(&g_async_queue.count);
}

uint32_t async_io_cancel_by_pid(uint32_t pid) {
    spin_lock(&g_async_queue.lock);

    uint32_t current_count = atomic_load_u32(&g_async_queue.count);

    async_io_request_t *temp = kmalloc(sizeof(async_io_request_t) * current_count);
    if (!temp) {
        spin_unlock(&g_async_queue.lock);
        return 0;
    }

    uint32_t cancelled = 0;
    uint32_t temp_count = 0;

    for (uint32_t i = 0; i < current_count; i++) {
        uint32_t idx = (g_async_queue.head + i) & g_async_queue.mask;

        if (g_async_queue.queue[idx].pid == pid) {
            cancelled++;
            debug_printf("[AsyncIO] Cancelling pid=%u, lba=%u\n",
                         pid, g_async_queue.queue[idx].lba);
        } else {
            temp[temp_count++] = g_async_queue.queue[idx];
        }
    }

    g_async_queue.head = 0;
    for (uint32_t i = 0; i < temp_count; i++) {
        g_async_queue.queue[i] = temp[i];
    }
    g_async_queue.tail = temp_count;
    atomic_store_u32(&g_async_queue.count, temp_count);

    spin_unlock(&g_async_queue.lock);

    kfree(temp);

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

void async_io_mark_completed_with_latency(uint32_t event_id, uint64_t submit_tick) {
    uint64_t now     = kernel_tick_get();
    uint64_t latency = now - submit_tick;

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

uint32_t async_io_expire_stale(uint64_t timeout_tsc) {
    if (atomic_load_u32(&g_async_queue.count) == 0) {
        return 0;
    }

    spin_lock(&g_async_queue.lock);

    uint32_t current_count = atomic_load_u32(&g_async_queue.count);

    async_io_request_t *kept = kmalloc(sizeof(async_io_request_t) * current_count);
    if (!kept) {
        spin_unlock(&g_async_queue.lock);
        return 0;
    }

    uint32_t expired = 0;
    uint64_t now = kernel_tick_get();   // global PIT tick — consistent across all cores
    uint32_t kept_count = 0;

    for (uint32_t i = 0; i < current_count; i++) {
        uint32_t idx = (g_async_queue.head + i) & g_async_queue.mask;
        async_io_request_t *req = &g_async_queue.queue[idx];

        if (req->submit_tick > 0 && (now - req->submit_tick) > timeout_tsc) {
            async_io_mark_failed(req->event_id);
            debug_printf("[AsyncIO] Expired stale request: event_id=%u pid=%u lba=%u\n",
                         req->event_id, req->pid, req->lba);
            expired++;
        } else {
            kept[kept_count++] = *req;
        }
    }

    if (expired > 0) {
        g_async_queue.head = 0;
        for (uint32_t i = 0; i < kept_count; i++) {
            g_async_queue.queue[i] = kept[i];
        }
        g_async_queue.tail = kept_count;
        atomic_store_u32(&g_async_queue.count, kept_count);
    }

    spin_unlock(&g_async_queue.lock);

    kfree(kept);
    return expired;
}
