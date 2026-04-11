#include "execution_deck.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "result_ring.h"
#include "atomics.h"
#include "error.h"

int execution_deck_handler(Pocket *pocket, process_t *proc)
{
    if (!pocket)
    {
        return -1;
    }

    // Determine target: if target_pid != 0, deliver to target process (IPC).
    // Otherwise deliver to the sender.
    uint32_t target_pid = pocket->target_pid;

    if (target_pid != 0)
    {
        // IPC: deliver Result to target process's ResultRing,
        // then deliver confirmation Result to sender's ResultRing.
        process_t *target = process_find(target_pid);
        process_t *sender = proc; // source process already resolved by Guide

        if (!target)
        {
            // Target gone — deliver error to sender only
            if (!sender)
                return 0;

            Result err_result;
            err_result.error_code = ERR_PROCESS_NOT_FOUND;
            err_result.data_length = 0;
            err_result.data_addr = 0;
            err_result.sender_pid = 0;
            err_result._reserved = 0;

            if (sender->result_ring_phys)
            {
                ResultRing *rring = (ResultRing *)vmm_phys_to_virt(sender->result_ring_phys);
                if (rring)
                    result_ring_push(rring, &err_result);
            }
            return -1;
        }

        // Deliver IPC Result to target
        Result ipc_result;
        ipc_result.error_code = pocket->error_code;
        ipc_result.data_length = pocket->data_length;
        ipc_result.data_addr = pocket->data_addr;
        ipc_result.sender_pid = pocket->pid;
        ipc_result._reserved = 0;

        if (target->result_ring_phys)
        {
            ResultRing *rring = (ResultRing *)vmm_phys_to_virt(target->result_ring_phys);
            if (rring)
            {
                result_ring_push(rring, &ipc_result);
            }
        }

        // Wake target if it's waiting
        if (process_get_state(target) == PROC_WAITING)
        {
            process_set_state(target, PROC_WORKING);
        }

        // Deliver confirmation Result to sender (sender_pid = 0 so result_pop_non_ipc finds it)
        if (sender && sender->result_ring_phys)
        {
            Result confirm;
            confirm.error_code = pocket->error_code;
            confirm.data_length = 0;
            confirm.data_addr = 0;
            confirm.sender_pid = 0;
            confirm._reserved = 0;

            ResultRing *srring = (ResultRing *)vmm_phys_to_virt(sender->result_ring_phys);
            if (srring)
            {
                result_ring_push(srring, &confirm);
            }
        }

        return 0;
    }

    // Self: deliver Result to sender's own ResultRing
    process_t *target = proc; // source process already resolved by Guide
    if (!target)
        return 0;

    Result result;
    result.error_code = pocket->error_code;
    result.data_length = pocket->data_length;
    result.data_addr = pocket->data_addr;
    result.sender_pid = 0;
    result._reserved = 0;

    uint64_t rring_phys = target->result_ring_phys;
    if (rring_phys == 0)
    {
        return -1;
    }

    ResultRing *rring = (ResultRing *)vmm_phys_to_virt(rring_phys);
    if (!rring)
    {
        return -1;
    }

    if (!result_ring_push(rring, &result))
    {
        debug_printf("[EXECUTION] WARNING: PID %u ResultRing full, result dropped\n",
                     target->pid);
        return -1;
    }

    // FIX: Wake the process if it's waiting for the result.
    // Without this, the process spins in result_wait() for up to timeout_ms.
    if (process_get_state(target) == PROC_WAITING)
    {
        process_set_state(target, PROC_WORKING);
    }

    return 0;
}
