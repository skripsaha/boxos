#include "execution_deck.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "result_ring.h"
#include "kring.h"
#include "atomics.h"
#include "error.h"
#include "kresult.h"

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
        process_t *sender = proc;

        if (!target)
        {
            if (!sender) return 0;
            Result err_result;
            err_result.error_code  = ERR_PROCESS_NOT_FOUND;
            err_result.data_length = 0;
            err_result.data_addr   = 0;
            err_result.sender_pid  = 0;
            err_result.context     = KCTX_IPC;
            KResultPush(sender, &err_result);
            return -1;
        }

        // Deliver IPC Result to target
        Result ipc_result;
        ipc_result.error_code  = pocket->error_code;
        ipc_result.data_length = pocket->data_length;
        ipc_result.data_addr   = pocket->data_addr;
        ipc_result.sender_pid  = pocket->pid;
        ipc_result.context     = KCTX_IPC;
        KResultPush(target, &ipc_result);

        // Deliver confirmation Result to sender (sender_pid = 0 so result_pop_non_ipc finds it)
        if (sender)
        {
            Result confirm;
            confirm.error_code  = pocket->error_code;
            confirm.data_length = 0;
            confirm.data_addr   = 0;
            confirm.sender_pid  = 0;
            confirm.context     = KCTX_IPC;
            KResultPush(sender, &confirm);
        }
        return 0;
    }

    // Self: deliver Result to sender's own ResultRing
    process_t *target = proc;
    if (!target) return 0;

    Result result;
    result.error_code  = pocket->error_code;
    result.data_length = pocket->data_length;
    result.data_addr   = pocket->data_addr;
    result.sender_pid  = 0;
    result.context     = KCTX_GUIDE;

    if (!KResultPush(target, &result))
    {
        debug_printf("[EXECUTION] WARNING: PID %u ResultRing full, result dropped\n",
                     target->pid);
        return -1;
    }
    return 0;
}
