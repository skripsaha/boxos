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

    process_t* proc = process_find(pocket->pid);
    if (!proc) {
        return 0;
    }

    uint64_t rring_phys = proc->result_ring_phys;
    if (rring_phys == 0) {
        return -1;
    }

    ResultRing* rring = (ResultRing*)vmm_phys_to_virt(rring_phys);
    if (!rring) {
        return -1;
    }

    // Build the Result from the Pocket's final state
    Result result;
    result.error_code = pocket->error_code;
    result.data_length = pocket->data_length;
    result.data_addr = pocket->data_addr;
    result.sender_pid = pocket->target_pid;  // who sent this (for IPC)
    result._reserved = 0;

    if (!result_ring_push(rring, &result)) {
        // ResultRing full — drop the result.
        // Userspace must drain its ring; no kernel-side overflow queue.
        debug_printf("[EXECUTION] WARNING: PID %u ResultRing full, result dropped\n",
                     pocket->pid);
        return -1;
    }

    return 0;
}
