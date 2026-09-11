
#include "crate_stage.h"
#include "ktypes.h"
#include "klib.h"
#include "vmm.h"

error_t crate_stage_in(vmm_context_t        *cabin,
                        uint64_t              user_uaddr,
                        uint16_t              count,
                        Crate               **out_crates)
{
    if (!cabin || !out_crates) return ERR_NULL_POINTER;
    *out_crates = NULL;
    if (count == 0) return OK;

    size_t bytes = (size_t)count * sizeof(Crate);
    Crate *kbuf = (Crate *)kmalloc(bytes);
    if (!kbuf) return ERR_NO_MEMORY;

    error_t rc = vmm_user_buf_in_into(cabin, (uintptr_t)user_uaddr, bytes, kbuf);
    if (rc != OK) {
        kfree(kbuf);
        return rc;
    }
    *out_crates = kbuf;
    return OK;
}

error_t crate_stage_commit_and_release(Crate                *crates,
                                        uint16_t              count,
                                        vmm_context_t        *cabin,
                                        uint64_t              user_uaddr)
{
    if (!crates) return OK;
    error_t rc = OK;
    if (count > 0 && cabin && user_uaddr != 0) {
        size_t bytes = (size_t)count * sizeof(Crate);
        rc = vmm_user_buf_commit_out(cabin, (uintptr_t)user_uaddr, crates, bytes);
        if (rc != OK) {
            debug_printf("[CrateStage] commit_out failed: rc=%d uaddr=0x%lx bytes=%lu\n",
                         rc, (unsigned long)user_uaddr, (unsigned long)bytes);
        }
    }
    kfree(crates);
    return rc;
}

void crate_stage_release_no_commit(Crate *crates)
{
    if (crates) kfree(crates);
}