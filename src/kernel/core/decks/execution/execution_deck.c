#include "execution_deck.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "result_ring.h"
#include "atomics.h"
#include "error.h"

int execution_deck_handler(Pocket* pocket) {
    if (!pocket) {
        return -1;
    }

    // Determine target: if target_pid != 0, deliver to target process (IPC).
    // Otherwise deliver to the sender.
    uint32_t target_pid = pocket->target_pid;
    process_t* target;
    Result result;

    result.error_code = pocket->error_code;
    result.data_length = pocket->data_length;
    result.data_addr = pocket->data_addr;
    result._reserved = 0;

    if (target_pid != 0) {
        // IPC: deliver Result to target process's ResultRing
        target = process_find(target_pid);
        if (!target) {
            // Target gone — deliver error to sender instead
            target = process_find(pocket->pid);
            if (!target) return 0;
            result.error_code = ERR_PROCESS_NOT_FOUND;
            result.sender_pid = 0;
        } else {
            result.sender_pid = pocket->pid;  // sender's PID for IPC

            // Wake target if it's waiting
            if (process_get_state(target) == PROC_WAITING) {
                process_set_state(target, PROC_WORKING);
            }
        }
    } else {
        // Self: deliver Result to sender's own ResultRing
        target = process_find(pocket->pid);
        if (!target) return 0;
        result.sender_pid = 0;
    }

    uint64_t rring_phys = target->result_ring_phys;
    if (rring_phys == 0) {
        return -1;
    }

    ResultRing* rring = (ResultRing*)vmm_phys_to_virt(rring_phys);
    if (!rring) {
        return -1;
    }

    if (!result_ring_push(rring, &result)) {
        debug_printf("[EXECUTION] WARNING: PID %u ResultRing full, result dropped\n",
                     target->pid);
        return -1;
    }

    return 0;
}
