#include "box/result.h"
#include "box/system.h"

bool box_result_wait_yield(box_result_entry_t* out_entry, uint32_t timeout_ms) {
    box_result_page_t* rp = box_result_page();
    uint32_t iterations = 0;

    uint32_t max_iterations = timeout_ms / 10;

    while (1) {
        __sync_synchronize();

        if (rp->notification_flag != 0 || box_result_available()) {
            break;
        }

        if (timeout_ms > 0 && iterations++ >= max_iterations) {
            return false;
        }

        yield();
    }

    bool success = box_result_pop(out_entry);

    if (success) {
        rp->notification_flag = 0;
    }

    return success;
}
