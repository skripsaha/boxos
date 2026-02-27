#include "box/result.h"
#include "box/cpu.h"

extern bool box_result_wait_umwait(box_result_entry_t* out_entry, uint32_t timeout_ms);
extern bool box_result_wait_yield(box_result_entry_t* out_entry, uint32_t timeout_ms);

bool box_result_wait_smart(box_result_entry_t* out_entry, uint32_t timeout_ms) {
    if (box_result_available()) {
        return box_result_pop(out_entry);
    }

    if (box_cpu_has_waitpkg()) {
        return box_result_wait_umwait(out_entry, timeout_ms);
    } else {
        return box_result_wait_yield(out_entry, timeout_ms);
    }
}
