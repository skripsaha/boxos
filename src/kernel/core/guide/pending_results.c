#include "pending_results.h"
#include "klib.h"
#include "vmm.h"
#include "process.h"
#include "atomics.h"
#include "scheduler.h"

typedef struct {
    volatile uint32_t total_enqueued;
    volatile uint32_t total_delivered;
    volatile uint32_t max_watermark;
    volatile uint32_t overflow_count;
} pending_results_stats_t;

static pending_results_stats_t stats;
static pending_queue_t pending_q;

void pending_results_init(void) {
    memset(&pending_q, 0, sizeof(pending_queue_t));
    pending_q.head = 0;
    pending_q.tail = 0;
    spinlock_init(&pending_q.lock);

    memset(&stats, 0, sizeof(pending_results_stats_t));

    debug_printf("[PENDING_RESULTS] Initialized (max_pending=%d, increased from 64)\n", PENDING_QUEUE_SIZE);
}

int pending_results_enqueue(uint32_t pid, const result_entry_t* entry) {
    if (!entry) {
        return -1;
    }

    spin_lock(&pending_q.lock);

    uint32_t head = pending_q.head;
    uint32_t tail = pending_q.tail;

    uint32_t count = (head >= tail) ? (head - tail) : (PENDING_QUEUE_SIZE - tail + head);
    uint32_t current_watermark = atomic_load_u32(&stats.max_watermark);
    if (count > current_watermark) {
        atomic_store_u32(&stats.max_watermark, count);
    }

    if (((head + 1) % PENDING_QUEUE_SIZE) == tail) {
        atomic_fetch_add_u32(&stats.overflow_count, 1);
        spin_unlock(&pending_q.lock);

        debug_printf("[PENDING_RESULTS] OVERFLOW: queue full (%u total overflows)\n",
                     atomic_load_u32(&stats.overflow_count));
        return -1;
    }

    pending_result_t* pending = &pending_q.entries[head];
    pending->pid = pid;
    memcpy(&pending->entry, entry, sizeof(result_entry_t));
    pending->timestamp = rdtsc();

    pending_q.head = (head + 1) % PENDING_QUEUE_SIZE;

    atomic_fetch_add_u32(&stats.total_enqueued, 1);

    spin_unlock(&pending_q.lock);

    return 0;
}

int pending_results_try_deliver(uint32_t pid) {
    spin_lock(&pending_q.lock);

    uint32_t head = pending_q.head;
    uint32_t tail = pending_q.tail;

    if (head == tail) {
        spin_unlock(&pending_q.lock);
        return 0;
    }

    uint32_t scan = tail;
    int delivered = 0;

    while (scan != head) {
        pending_result_t* pending = &pending_q.entries[scan];

        if (pending->pid == pid) {
            process_t* proc = process_find(pid);
            if (!proc) {
                scan = (scan + 1) % PENDING_QUEUE_SIZE;
                continue;
            }

            // validate magic before use to catch corrupted process structs
            if (proc->magic != PROCESS_MAGIC) {
                debug_printf("[PENDING_RESULTS] WARNING: Corrupted process PID %u (bad magic), skipping\n", pid);
                pending->pid = 0;
                scan = (scan + 1) % PENDING_QUEUE_SIZE;
                continue;
            }

            process_ref_inc(proc);

            uint64_t result_phys = proc->result_page_phys;
            if (result_phys == 0) {
                process_ref_dec(proc);
                scan = (scan + 1) % PENDING_QUEUE_SIZE;
                continue;
            }

            result_page_t* result_page = (result_page_t*)vmm_phys_to_virt(result_phys);
            if (!result_page) {
                process_ref_dec(proc);
                scan = (scan + 1) % PENDING_QUEUE_SIZE;
                continue;
            }

            uint32_t rhead = result_page->ring.head;
            __sync_synchronize();
            uint32_t rtail = result_page->ring.tail;

            if (((rtail + 1) % RESULT_RING_SIZE) != rhead) {
                uint32_t tail_idx = result_page->ring.tail;
                result_entry_t* entry = &result_page->ring.entries[tail_idx];

                memcpy(entry, &pending->entry, sizeof(result_entry_t));

                __sync_synchronize();
                result_page->ring.tail = (tail_idx + 1) % RESULT_RING_SIZE;
                __sync_synchronize();

                atomic_store_u8((volatile uint8_t*)&proc->result_there, 1);
                __sync_synchronize();

                // set state before clearing wait_reason to avoid ordering hazards
                if (proc->wait_reason == WAIT_OVERFLOW) {
                    scheduler_state_t* sched_state = scheduler_get_state();
                    spin_lock(&sched_state->scheduler_lock);

                    if (proc->wait_reason == WAIT_OVERFLOW) {
                        process_set_state(proc, PROC_WORKING);
                        __sync_synchronize();
                        proc->wait_start_time = 0;
                        proc->wait_reason = WAIT_NONE;
                    }

                    spin_unlock(&sched_state->scheduler_lock);
                }

                pending->pid = 0;
                delivered++;
            }

            process_ref_dec(proc);
        }

        scan = (scan + 1) % PENDING_QUEUE_SIZE;
    }

    if (delivered > 0) {
        atomic_fetch_add_u32(&stats.total_delivered, (uint32_t)delivered);

        while (pending_q.tail != head &&
               pending_q.entries[pending_q.tail].pid == 0) {
            pending_q.tail = (pending_q.tail + 1) % PENDING_QUEUE_SIZE;
        }
    }

    spin_unlock(&pending_q.lock);

    return delivered;
}

void pending_results_flush_all(void) {
    spin_lock(&pending_q.lock);

    uint32_t head = pending_q.head;
    uint32_t tail = pending_q.tail;

    if (head == tail) {
        spin_unlock(&pending_q.lock);
        return;
    }

    spin_unlock(&pending_q.lock);

    process_t* proc = process_get_first();
    while (proc) {
        pending_results_try_deliver(proc->pid);
        proc = proc->next;
    }
}
