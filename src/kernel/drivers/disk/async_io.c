#include "async_io.h"

static async_io_queue_t g_async_queue;

static const uint32_t lane_capacities[ASYNC_IO_LANE_COUNT] = {
    ASYNC_IO_LANE_META_CAPACITY,
    ASYNC_IO_LANE_DATA_CAPACITY,
    ASYNC_IO_LANE_BGND_CAPACITY,
};

static const char* lane_name(async_io_lane_t lane) {
    switch (lane) {
        case ASYNC_IO_LANE_META: return "META";
        case ASYNC_IO_LANE_DATA: return "DATA";
        case ASYNC_IO_LANE_BGND: return "BGND";
        default:                 return "?";
    }
}

void async_io_init(void) {
    memset(&g_async_queue, 0, sizeof(async_io_queue_t));
    spinlock_init(&g_async_queue.lock);

    for (int l = 0; l < ASYNC_IO_LANE_COUNT; l++) {
        async_io_lane_queue_t* lq = &g_async_queue.lanes[l];
        uint32_t cap = lane_capacities[l];

        lq->slots    = kmalloc(sizeof(async_io_request_t) * cap);
        if (!lq->slots) {
            kprintf("[AsyncIO] FATAL: cannot allocate lane %d queue\n", l);
            while (1) { __asm__ volatile("cli; hlt"); }
        }
        memset(lq->slots, 0, sizeof(async_io_request_t) * cap);
        lq->capacity = cap;
        lq->mask     = cap - 1;
        lq->head     = 0;
        lq->tail     = 0;
        atomic_init_u32(&lq->count, 0);
    }

    atomic_init_u32(&g_async_queue.total_submitted, 0);
    atomic_init_u32(&g_async_queue.total_completed, 0);
    atomic_init_u32(&g_async_queue.total_failed, 0);
    atomic_init_u32(&g_async_queue.queue_full_count, 0);
    atomic_init_u64(&g_async_queue.total_latency_cycles, 0);

    debug_printf("[AsyncIO] Initialized (META:%u DATA:%u BGND:%u)\n",
                 ASYNC_IO_LANE_META_CAPACITY,
                 ASYNC_IO_LANE_DATA_CAPACITY,
                 ASYNC_IO_LANE_BGND_CAPACITY);
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

    if (req->buffer_virt == NULL && req->op == ASYNC_IO_OP_WRITE) {
        debug_printf("[AsyncIO] ERROR: NULL buffer on WRITE\n");
        return ERR_NULL_POINTER;
    }

    if (req->op != ASYNC_IO_OP_READ && req->op != ASYNC_IO_OP_WRITE) {
        debug_printf("[AsyncIO] ERROR: Invalid operation=%u\n", req->op);
        return ERR_INVALID_ARGUMENT;
    }

    // Default lane if caller forgot to set it
    if (req->lane >= ASYNC_IO_LANE_COUNT) {
        req->lane = ASYNC_IO_LANE_DATA;
    }

    req->submit_tick = kernel_tick_get();

    async_io_lane_queue_t* lq = &g_async_queue.lanes[req->lane];

    if (atomic_load_u32(&lq->count) >= lq->capacity) {
        atomic_fetch_add_u32(&g_async_queue.queue_full_count, 1);
        debug_printf("[AsyncIO] Lane %s FULL (pid=%u, lba=%u)\n",
                     lane_name(req->lane), req->pid, req->lba);
        return ERR_IO_QUEUE_FULL;
    }

    spin_lock(&g_async_queue.lock);

    if (atomic_load_u32(&lq->count) >= lq->capacity) {
        spin_unlock(&g_async_queue.lock);
        atomic_fetch_add_u32(&g_async_queue.queue_full_count, 1);
        debug_printf("[AsyncIO] Lane %s FULL (pid=%u, lba=%u)\n",
                     lane_name(req->lane), req->pid, req->lba);
        return ERR_IO_QUEUE_FULL;
    }

    lq->slots[lq->tail] = *req;
    lq->tail = (lq->tail + 1) & lq->mask;
    atomic_fetch_add_u32(&lq->count, 1);

    spin_unlock(&g_async_queue.lock);

    atomic_fetch_add_u32(&g_async_queue.total_submitted, 1);

    debug_printf("[AsyncIO] Submitted (pid=%u, lane=%s, op=%s, lba=%u, sectors=%u)\n",
                 req->pid,
                 lane_name(req->lane),
                 req->op == ASYNC_IO_OP_READ ? "READ" : "WRITE",
                 req->lba,
                 req->sector_count);

    return OK;
}

bool async_io_dequeue(async_io_request_t* req) {
    if (req == NULL) {
        debug_printf("[AsyncIO] ERROR: NULL request output pointer\n");
        return false;
    }

    // Fast path: check total pending count without lock
    bool any = false;
    for (int l = 0; l < ASYNC_IO_LANE_COUNT; l++) {
        if (atomic_load_u32(&g_async_queue.lanes[l].count) > 0) {
            any = true;
            break;
        }
    }
    if (!any) return false;

    spin_lock(&g_async_queue.lock);

    // Drain in priority order: META > DATA > BGND
    for (int l = 0; l < ASYNC_IO_LANE_COUNT; l++) {
        async_io_lane_queue_t* lq = &g_async_queue.lanes[l];
        if (atomic_load_u32(&lq->count) == 0) continue;

        *req = lq->slots[lq->head];
        lq->head = (lq->head + 1) & lq->mask;
        atomic_fetch_sub_u32(&lq->count, 1);

        spin_unlock(&g_async_queue.lock);

        debug_printf("[AsyncIO] Dequeued (pid=%u, lane=%s, op=%s, lba=%u)\n",
                     req->pid,
                     lane_name((async_io_lane_t)l),
                     req->op == ASYNC_IO_OP_READ ? "READ" : "WRITE",
                     req->lba);
        return true;
    }

    spin_unlock(&g_async_queue.lock);
    return false;
}

uint32_t async_io_pending_count(void) {
    uint32_t total = 0;
    for (int l = 0; l < ASYNC_IO_LANE_COUNT; l++) {
        total += atomic_load_u32(&g_async_queue.lanes[l].count);
    }
    return total;
}

uint32_t async_io_cancel_by_pid(uint32_t pid) {
    spin_lock(&g_async_queue.lock);

    uint32_t cancelled = 0;

    for (int l = 0; l < ASYNC_IO_LANE_COUNT; l++) {
        async_io_lane_queue_t* lq = &g_async_queue.lanes[l];
        uint32_t cur = atomic_load_u32(&lq->count);
        if (cur == 0) continue;

        async_io_request_t* temp = kmalloc(sizeof(async_io_request_t) * cur);
        if (!temp) continue;

        uint32_t kept = 0;
        for (uint32_t i = 0; i < cur; i++) {
            uint32_t idx = (lq->head + i) & lq->mask;
            if (lq->slots[idx].pid == pid) {
                debug_printf("[AsyncIO] Cancelling pid=%u lane=%s lba=%u\n",
                             pid, lane_name((async_io_lane_t)l), lq->slots[idx].lba);
                cancelled++;
            } else {
                temp[kept++] = lq->slots[idx];
            }
        }

        lq->head = 0;
        for (uint32_t i = 0; i < kept; i++) {
            lq->slots[i] = temp[i];
        }
        lq->tail = kept;
        atomic_store_u32(&lq->count, kept);

        kfree(temp);
    }

    spin_unlock(&g_async_queue.lock);

    if (cancelled > 0) {
        debug_printf("[AsyncIO] Cancelled %u requests from pid=%u\n", cancelled, pid);
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

    debug_printf("[AsyncIO] Completed event_id=%u latency=%llu ticks (total=%u)\n",
                 event_id, latency, atomic_load_u32(&g_async_queue.total_completed));
}

void async_io_mark_failed(uint32_t event_id) {
    atomic_fetch_add_u32(&g_async_queue.total_failed, 1);
    debug_printf("[AsyncIO] Failed event_id=%u (total=%u)\n",
                 event_id, atomic_load_u32(&g_async_queue.total_failed));
}

uint32_t async_io_expire_stale(uint64_t timeout_ticks) {
    bool any = false;
    for (int l = 0; l < ASYNC_IO_LANE_COUNT; l++) {
        if (atomic_load_u32(&g_async_queue.lanes[l].count) > 0) { any = true; break; }
    }
    if (!any) return 0;

    spin_lock(&g_async_queue.lock);

    uint32_t expired = 0;
    uint64_t now = kernel_tick_get();

    for (int l = 0; l < ASYNC_IO_LANE_COUNT; l++) {
        async_io_lane_queue_t* lq = &g_async_queue.lanes[l];
        uint32_t cur = atomic_load_u32(&lq->count);
        if (cur == 0) continue;

        async_io_request_t* kept = kmalloc(sizeof(async_io_request_t) * cur);
        if (!kept) continue;

        uint32_t kept_count = 0;

        for (uint32_t i = 0; i < cur; i++) {
            uint32_t idx = (lq->head + i) & lq->mask;
            async_io_request_t* r = &lq->slots[idx];

            if (r->submit_tick > 0 && (now - r->submit_tick) > timeout_ticks) {
                async_io_mark_failed(r->event_id);
                debug_printf("[AsyncIO] Expired stale: event_id=%u pid=%u lane=%s lba=%u\n",
                             r->event_id, r->pid, lane_name((async_io_lane_t)l), r->lba);
                expired++;
            } else {
                kept[kept_count++] = *r;
            }
        }

        if (expired > 0) {
            lq->head = 0;
            for (uint32_t i = 0; i < kept_count; i++) {
                lq->slots[i] = kept[i];
            }
            lq->tail = kept_count;
            atomic_store_u32(&lq->count, kept_count);
        }

        kfree(kept);
    }

    spin_unlock(&g_async_queue.lock);

    return expired;
}
