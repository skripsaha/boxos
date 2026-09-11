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

    uint32_t target_pid = pocket->target_pid;

    if (target_pid != 0)
    {
        process_t *target = process_find_ref(target_pid);
        process_t *sender = proc;

        if (!target)
        {
            if (!sender) return 0;
            Result err_result;
            err_result.error_code  = ERR_PROCESS_NOT_FOUND;
            err_result.data_length = 0;
            err_result.data_addr   = 0;
            err_result.sender_pid  = 0;
            err_result.context     = KCTX_PACK24(KCTX_IPC, PocketCookie24(pocket));
            KResultPush(sender, &err_result);
            return -1;
        }

        Result ipc_result;
        ipc_result.error_code  = pocket->error_code;
        ipc_result.data_length = pocket->manifest_size;
        ipc_result.data_addr   = pocket->manifest_addr;
        ipc_result.sender_pid  = pocket->pid;
        ipc_result.context     = KCTX_IPC;
        KResultPush(target, &ipc_result);
        process_ref_dec(target);

        if (sender)
        {
            Result confirm;
            confirm.error_code  = pocket->error_code;
            confirm.data_length = 0;
            confirm.data_addr   = 0;
            confirm.sender_pid  = 0;
            confirm.context     = KCTX_PACK24(KCTX_IPC, PocketCookie24(pocket));
            KResultPush(sender, &confirm);
        }
        return 0;
    }

    process_t *target = proc;
    if (!target) return 0;

    Result result;
    result.error_code  = pocket->error_code;
    result.data_length = pocket->manifest_size;
    result.data_addr   = pocket->manifest_addr;
    result.sender_pid  = 0;
    result.context     = KCTX_PACK24(KCTX_GUIDE, PocketCookie24(pocket));

    if (!KResultPush(target, &result))
    {
        kprintf("[EXECUTION] DEFECT: the answer to pid %u's submit (token 0x%06x, "
                "rc=%u) was refused by its reply ring\n",
                (unsigned int)target->pid, (unsigned int)PocketCookie24(pocket),
                (unsigned int)pocket->error_code);
        return -1;
    }
    return 0;
}