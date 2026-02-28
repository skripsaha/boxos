#include "system_deck.h"
#include "events.h"
#include "process.h"
#include "klib.h"
#include "atomics.h"

int system_deck_get_overflow_status(Event* event) {
    if (!event) {
        return -1;
    }

    process_t* proc = process_find(event->pid);
    if (!proc) {
        event->state = EVENT_STATE_ERROR;
        memset(event->data, 0, EVENT_DATA_SIZE);
        *((uint32_t*)event->data) = 0xFFFFFFFF;
        return -1;
    }
    process_ref_inc(proc);

    uint32_t overflow_count = atomic_load_u32(&proc->result_overflow_count);
    uint8_t overflow_flag = atomic_load_u8(&proc->result_overflow_flag);

    // Check if userspace wants to clear the flag
    bool clear_flag = (event->data[0] == 1);
    if (clear_flag) {
        atomic_store_u8(&proc->result_overflow_flag, 0);
    }

    memset(event->data, 0, EVENT_DATA_SIZE);
    *((uint32_t*)event->data) = overflow_count;
    *((uint8_t*)(event->data + 4)) = overflow_flag ? 1 : 0;

    process_ref_dec(proc);
    event->state = EVENT_STATE_COMPLETED;
    return 0;
}
