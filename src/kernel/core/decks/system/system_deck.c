
#include "system_deck.h"
#include "klib.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"

uint64_t cabin_heap_deposit(process_t *target, const void *kbuf, uint32_t length)
{
    if (!target || !target->cabin || !kbuf || length == 0) {
        return 0;
    }

    uint32_t pages_needed = (length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t bytes_needed = (uint64_t)pages_needed * PMM_PAGE_SIZE;

    uint64_t target_vaddr = __atomic_fetch_add(&target->cabin->buf_heap_next,
                                               bytes_needed,
                                               __ATOMIC_ACQ_REL);

    for (uint32_t i = 0; i < pages_needed; i++) {
        void *page = pmm_alloc(1);
        if (!page) return 0;
        uint64_t vaddr = target_vaddr + (i * PMM_PAGE_SIZE);
        vmm_map_result_t ret = vmm_map_page(target->cabin->vmm, vaddr, (uint64_t)page,
                                            VMM_FLAGS_USER_RW);
        if (!ret.success) {
            pmm_free(page, 1);
            return 0;
        }
    }

    error_t crc = vmm_user_buf_commit_out(target->cabin->vmm, target_vaddr,
                                          kbuf, length);
    if (crc != OK) return 0;
    return target_vaddr;
}

uint64_t ipc_copy_to_heap(process_t *sender, process_t *target,
                          uint64_t src_addr, uint32_t length)
{
    if (!sender || !target || length == 0 || src_addr == 0) {
        return 0;
    }

    void *kbuf = vmm_user_buf_in(sender->cabin ? sender->cabin->vmm : NULL,
                                 src_addr, length);
    if (!kbuf) return 0;

    uint64_t target_vaddr = cabin_heap_deposit(target, kbuf, length);
    vmm_user_buf_free(kbuf);
    return target_vaddr;
}