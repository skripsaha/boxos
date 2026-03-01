#include "execution_deck.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "result_page.h"
#include "notify_page.h"
#include "atomics.h"
#include "pending_results.h"
#include "error.h"
#include "cpu_caps_page.h"

int execution_deck_handler(Event* event) {
    if (!event) {
        return -1;
    }

    extern void process_list_validate(const char*);
    process_list_validate("exec_deck_before_find");

    process_t* proc = process_find(event->pid);
    if (!proc) {
        return 0;
    }
    process_ref_inc(proc);

    uint64_t result_phys = proc->result_page_phys;
    if (result_phys == 0) {
        process_ref_dec(proc);
        return -1;
    }

    result_page_t* result_page = (result_page_t*)vmm_phys_to_virt(result_phys);
    if (!result_page) {
        process_ref_dec(proc);
        return -1;
    }

    if (result_page->magic != RESULT_PAGE_MAGIC) {
        result_page->magic = RESULT_PAGE_MAGIC;
        result_page->ring.head = 0;
        result_page->ring.tail = 0;
        result_page->notification_flag = 0;
    }

    // Use memcpy to read head from packed struct (avoid alignment issues)
    uint32_t head;
    memcpy(&head, (const void*)&result_page->ring.head, sizeof(uint32_t));
    __sync_synchronize();  // acquire: ensure we see userspace's head update and associated data
    uint32_t tail = result_page->ring.tail;
    if (((tail + 1) % RESULT_RING_SIZE) == head) {
        // Result Ring full: set backpressure signal
        uint64_t notify_phys = proc->notify_page_phys;
        if (notify_phys != 0) {
            notify_page_t* notify_page = (notify_page_t*)vmm_phys_to_virt(notify_phys);
            if (notify_page) {
                atomic_store_u8(&notify_page->result_page_full, 1);
            }
        }

        if (event->state == EVENT_STATE_PROCESSING || event->state == EVENT_STATE_NEW) {
            event->state = EVENT_STATE_COMPLETED;
        }

        result_entry_t temp_entry;
        temp_entry.source = event->route_flags;
        temp_entry._reserved1 = 0;
        temp_entry.error_code = event->first_error;
        temp_entry.sender_pid = event->sender_pid;
        size_t copy_size = (sizeof(event->data) < sizeof(temp_entry.payload))
                           ? sizeof(event->data)
                           : sizeof(temp_entry.payload);
        temp_entry.size = (uint16_t)copy_size;
        memcpy(temp_entry.payload, event->data, copy_size);

        int enqueue_result = pending_results_enqueue(proc->pid, &temp_entry);

        if (enqueue_result == 0) {
            process_ref_dec(proc);
            return 0;
        }

        // Pending Queue also full: block process
        debug_printf("[EXECUTION] CRITICAL: PID %u ResultRing full, blocking process\n", proc->pid);

        atomic_store_u8(&proc->result_overflow_flag, 1);
        atomic_fetch_add_u32(&proc->result_overflow_count, 1);

        proc->wait_reason = WAIT_OVERFLOW;
        proc->wait_start_time = rdtsc();

        process_set_state(proc, PROC_WAITING);

        result_entry_t overflow_entry;
        overflow_entry.source = ROUTE_SOURCE_KERNEL;
        overflow_entry._reserved1 = 0;
        overflow_entry.error_code = ERR_RESULT_RING_FULL;
        overflow_entry.sender_pid = 0;
        overflow_entry.size = 8;
        *((uint32_t*)overflow_entry.payload) = RESULT_OVERFLOW_MARKER;
        *((uint32_t*)(overflow_entry.payload + 4)) = proc->result_overflow_count;

        if (pending_results_enqueue(proc->pid, &overflow_entry) != 0) {
            debug_printf("[EXECUTION] WARNING: PID %u overflow notification lost (pending queue full)\n",
                       proc->pid);
        }

        process_ref_dec(proc);
        return -1;  // Event blocked
    }

    // Result Page has space: clear backpressure signal
    uint64_t notify_phys = proc->notify_page_phys;
    if (notify_phys != 0) {
        notify_page_t* notify_page = (notify_page_t*)vmm_phys_to_virt(notify_phys);
        if (notify_page) {
            atomic_store_u8(&notify_page->result_page_full, 0);
        }
    }

    if (event->state == EVENT_STATE_PROCESSING || event->state == EVENT_STATE_NEW) {
        event->state = EVENT_STATE_COMPLETED;
    }

    uint32_t tail_idx = result_page->ring.tail;
    result_entry_t* entry = &result_page->ring.entries[tail_idx];

    entry->source = event->route_flags;
    entry->_reserved1 = 0;
    entry->error_code = event->first_error;
    entry->sender_pid = event->sender_pid;

    size_t copy_size = (sizeof(event->data) < sizeof(entry->payload))
                       ? sizeof(event->data)
                       : sizeof(entry->payload);
    entry->size = (uint16_t)copy_size;
    memcpy(entry->payload, event->data, copy_size);

    __sync_synchronize();  // ensure data written before tail update

    uint32_t new_tail = (tail_idx + 1) % RESULT_RING_SIZE;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
    atomic_store_u32(&result_page->ring.tail, new_tail);
#pragma GCC diagnostic pop

    __sync_synchronize();  // ensure tail written before notification_flag

    atomic_store_u8(&result_page->notification_flag, 1);

    process_state_t state = process_get_state(proc);
    if (state == PROC_WORKING || state == PROC_WAITING) {
        atomic_store_u8((volatile uint8_t*)&proc->result_there, 1);
        __sync_synchronize();
        if (state != PROC_WORKING) {
            process_set_state(proc, PROC_WORKING);
        }
    }

    process_ref_dec(proc);

    return 0;
}
