/*
 * System Deck — helpers (Phase 12: post-prefix-chain).
 *
 * The legacy switch-style dispatcher and per-opcode helpers are gone. All
 * system ops now live in system_ops.c on the Manifest path. This file
 * retains the two helpers that survived the migration:
 *
 *   ipc_copy_to_heap                 — used by system.route / system.broadcast
 *   system_deck_cleanup_process_buffers — thin wrapper around BufferRegistry
 */

#include "system_deck.h"
#include "buffer_registry.h"
#include "klib.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"

void system_deck_cleanup_process_buffers(uint32_t pid)
{
    BufferRegistryCleanupProcess(pid);
}

uint64_t ipc_copy_to_heap(process_t *sender, process_t *target,
                          uint64_t src_addr, uint32_t length)
{
    if (!sender || !target || length == 0 || src_addr == 0) {
        return 0;
    }

    void *src = vmm_translate_user_addr(sender->cabin, src_addr, length);
    if (!src) return 0;

    uint32_t pages_needed = (length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t target_vaddr = target->buf_heap_next;

    for (uint32_t i = 0; i < pages_needed; i++) {
        void *page = pmm_alloc(1);
        if (!page) return 0;
        uint64_t vaddr = target_vaddr + (i * PMM_PAGE_SIZE);
        vmm_map_result_t ret = vmm_map_page(target->cabin, vaddr, (uint64_t)page,
                                            VMM_FLAGS_USER_RW);
        if (!ret.success) {
            pmm_free(page, 1);
            return 0;
        }
    }

    target->buf_heap_next += pages_needed * PMM_PAGE_SIZE;

    void *dst = vmm_translate_user_addr(target->cabin, target_vaddr, length);
    if (!dst) return 0;

    memcpy(dst, src, length);
    return target_vaddr;
}
