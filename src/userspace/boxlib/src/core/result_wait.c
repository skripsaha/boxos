#include "box/result.h"
#include "box/cpu.h"

extern bool result_wait_umwait(result_entry_t* out_entry, uint32_t timeout_ms);
extern bool result_wait_yield(result_entry_t* out_entry, uint32_t timeout_ms);

bool result_wait(result_entry_t* out_entry, uint32_t timeout_ms) {
    if (result_pop_non_ipc(out_entry)) {
        return true;
    }

    if (cpu_has_waitpkg()) {
        return result_wait_umwait(out_entry, timeout_ms);
    } else {
        return result_wait_yield(out_entry, timeout_ms);
    }
}
