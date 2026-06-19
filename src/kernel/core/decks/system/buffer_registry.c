/*
 * Buffer Registry — shared backing store for system.buf.* ops.
 *
 * Ownership: each entry is keyed by a 64-bit handle and owned by a PID.
 * Lifetime: explicit free, plus implicit cleanup when its owning process is
 *           torn down (proc_kill / process_destroy_safe).
 *
 * The registry is intentionally a small fixed-size open-addressed table — the
 * legacy implementation used 64 slots and we keep that envelope for now to
 * avoid rippling capacity changes through the rest of the kernel. The table
 * is removable later (Phase 11/12 territory) once the ring redesign lands.
 */

#include "buffer_registry.h"
#include "kernel_config.h"
#include "klib.h"
#include "atomics.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"

#define BUF_MAX_COUNT  64
#define BUF_MAX_SIZE   CONFIG_PROC_MAX_BUFFER_SIZE

typedef struct {
    uint64_t handle;
    uint64_t phys_addr;
    uint64_t virt_addr;
    uint64_t size;
    uint32_t owner_pid;
    bool     in_use;
} BufferEntry;

static BufferEntry g_buffers[BUF_MAX_COUNT];
static uint64_t    g_next_handle = 1;
static spinlock_t  g_buffers_lock = {0};

static int registry_find_free_slot(void)
{
    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (!g_buffers[i].in_use) return i;
    }
    return -1;
}

static int registry_find_by_handle(uint64_t handle)
{
    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (g_buffers[i].in_use && g_buffers[i].handle == handle) return i;
    }
    return -1;
}

static uint32_t registry_count_owned(uint32_t pid)
{
    uint32_t count = 0;
    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (g_buffers[i].in_use && g_buffers[i].owner_pid == pid) count++;
    }
    return count;
}

static uint64_t registry_random_handle(void)
{
    uint64_t tsc     = rdtsc();
    uint64_t counter = atomic_fetch_add_u64(&g_next_handle, 1);
    uint64_t result  = tsc ^ (counter << 32) ^ (counter >> 32);
    return result ? result : 1;
}

BufferAllocResult BufferRegistryAlloc(process_t *proc, uint64_t requested_size)
{
    BufferAllocResult out = {0};

    if (!proc || !proc->cabin) {
        out.err = ERR_PROCESS_NOT_FOUND;
        return out;
    }
    if (requested_size == 0) {
        out.err = ERR_INVALID_ARGUMENT;
        return out;
    }
    if (requested_size > BUF_MAX_SIZE) {
        out.err = ERR_BINARY_TOO_LARGE;
        return out;
    }

    spin_lock(&g_buffers_lock);

    if (registry_count_owned(proc->pid) >= BUF_MAX_COUNT / 4) {
        spin_unlock(&g_buffers_lock);
        out.err = ERR_BUFFER_LIMIT_EXCEEDED;
        return out;
    }

    int slot = registry_find_free_slot();
    if (slot < 0) {
        spin_unlock(&g_buffers_lock);
        out.err = ERR_BUFFER_LIMIT_EXCEEDED;
        return out;
    }

    uint64_t handle  = registry_random_handle();
    int      collide = 0;
    while (registry_find_by_handle(handle) >= 0) {
        handle = registry_random_handle();
        if (++collide > BUF_MAX_COUNT) {
            spin_unlock(&g_buffers_lock);
            out.err = ERR_BUFFER_LIMIT_EXCEEDED;
            return out;
        }
    }

    spin_unlock(&g_buffers_lock);

    size_t pages       = (requested_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void  *phys        = pmm_alloc_zero(pages);
    __sync_synchronize();
    if (!phys) {
        out.err = ERR_NO_MEMORY;
        return out;
    }

    spin_lock(&g_buffers_lock);
    if (g_buffers[slot].in_use) {
        spin_unlock(&g_buffers_lock);
        pmm_free(phys, pages);
        out.err = ERR_BUFFER_LIMIT_EXCEEDED;
        return out;
    }
    g_buffers[slot].handle    = handle;
    g_buffers[slot].phys_addr = (uint64_t)phys;
    g_buffers[slot].size      = pages * PMM_PAGE_SIZE;
    g_buffers[slot].owner_pid = proc->pid;
    g_buffers[slot].virt_addr = 0;
    g_buffers[slot].in_use    = true;
    spin_unlock(&g_buffers_lock);

    /* Atomically reserve a unique VA range BEFORE mapping. A plain
     * `virt = buf_heap_next; … buf_heap_next += size` races the atomic-FAA
     * reservers (touch.c, system_deck.c) and sibling strands of the same
     * cabin, handing two allocations the same VA. Mirrors system_deck.c. */
    uint64_t virt = __atomic_fetch_add(&proc->cabin->buf_heap_next,
                                       (uint64_t)pages * PMM_PAGE_SIZE,
                                       __ATOMIC_ACQ_REL);
    vmm_map_result_t mr = vmm_map_pages(proc->cabin->vmm, virt, (uintptr_t)phys,
                                        pages, VMM_FLAGS_USER_RW);
    if (mr.success) {
        spin_lock(&g_buffers_lock);
        g_buffers[slot].virt_addr = virt;
        spin_unlock(&g_buffers_lock);
    } else {
        virt = 0;   /* VA range abandoned (monotonic window, as the FAA sites) */
    }

    out.err         = OK;
    out.handle      = handle;
    out.phys_addr   = (uint64_t)phys;
    out.virt_addr   = virt;
    out.actual_size = pages * PMM_PAGE_SIZE;
    return out;
}

error_t BufferRegistryFree(uint32_t owner_pid, uint64_t handle)
{
    if (handle == 0) return ERR_INVALID_ARGUMENT;

    spin_lock(&g_buffers_lock);
    int slot = registry_find_by_handle(handle);
    if (slot < 0) {
        spin_unlock(&g_buffers_lock);
        return ERR_INVALID_BUFFER_ID;
    }
    if (g_buffers[slot].owner_pid != owner_pid) {
        spin_unlock(&g_buffers_lock);
        return ERR_ACCESS_DENIED;
    }

    void    *phys      = (void *)g_buffers[slot].phys_addr;
    uint64_t virt      = g_buffers[slot].virt_addr;
    size_t   pages     = g_buffers[slot].size / PMM_PAGE_SIZE;
    uint32_t pid       = g_buffers[slot].owner_pid;

    g_buffers[slot].in_use    = false;
    g_buffers[slot].handle    = 0;
    g_buffers[slot].phys_addr = 0;
    g_buffers[slot].virt_addr = 0;
    g_buffers[slot].size      = 0;
    g_buffers[slot].owner_pid = 0;
    spin_unlock(&g_buffers_lock);

    if (virt != 0) {
        process_t *p = process_find(pid);
        if (p && p->cabin) {
            vmm_unmap_pages(p->cabin->vmm, virt, pages);
        }
    }
    pmm_free(phys, pages);
    __sync_synchronize();
    return OK;
}

error_t BufferRegistryResize(process_t *proc, uint64_t handle,
                             uint64_t new_size,
                             uint64_t *out_actual,
                             uint64_t *out_virt)
{
    if (!proc || !proc->cabin)         return ERR_PROCESS_NOT_FOUND;
    if (handle == 0 || new_size == 0)  return ERR_INVALID_ARGUMENT;
    if (new_size > BUF_MAX_SIZE)       return ERR_BINARY_TOO_LARGE;

    spin_lock(&g_buffers_lock);
    int slot = registry_find_by_handle(handle);
    if (slot < 0) {
        spin_unlock(&g_buffers_lock);
        return ERR_INVALID_BUFFER_ID;
    }
    if (g_buffers[slot].owner_pid != proc->pid) {
        spin_unlock(&g_buffers_lock);
        return ERR_ACCESS_DENIED;
    }

    uint64_t old_phys  = g_buffers[slot].phys_addr;
    uint64_t old_virt  = g_buffers[slot].virt_addr;
    uint64_t old_size  = g_buffers[slot].size;
    size_t   old_pages = old_size / PMM_PAGE_SIZE;
    size_t   new_pages = (new_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t new_act   = new_pages * PMM_PAGE_SIZE;

    if (new_pages == old_pages) {
        spin_unlock(&g_buffers_lock);
        if (out_actual) *out_actual = new_act;
        if (out_virt)   *out_virt   = old_virt;
        return OK;
    }
    spin_unlock(&g_buffers_lock);

    void *new_phys = pmm_alloc_zero(new_pages);
    if (!new_phys) return ERR_NO_MEMORY;

    void *src_kv = vmm_phys_to_virt(old_phys);
    void *dst_kv = vmm_phys_to_virt((uintptr_t)new_phys);
    size_t copy  = old_size < new_act ? old_size : new_act;
    if (src_kv && dst_kv) {
        memcpy(dst_kv, src_kv, copy);
        __sync_synchronize();
    }

    uint64_t new_virt = old_virt;
    if (old_virt != 0) {
        vmm_unmap_pages(proc->cabin->vmm, old_virt, old_pages);
        if (new_pages <= old_pages) {
            vmm_map_pages(proc->cabin->vmm, old_virt, (uintptr_t)new_phys,
                          new_pages, VMM_FLAGS_USER_RW);
        } else {
            new_virt = __atomic_fetch_add(&proc->cabin->buf_heap_next,
                                          (uint64_t)new_pages * PMM_PAGE_SIZE,
                                          __ATOMIC_ACQ_REL);
            vmm_map_result_t mr = vmm_map_pages(proc->cabin->vmm, new_virt,
                                                (uintptr_t)new_phys, new_pages,
                                                VMM_FLAGS_USER_RW);
            if (!mr.success) {
                new_virt = 0;
            }
        }
    }

    spin_lock(&g_buffers_lock);
    if (!g_buffers[slot].in_use || g_buffers[slot].handle != handle) {
        spin_unlock(&g_buffers_lock);
        pmm_free(new_phys, new_pages);
        return ERR_INVALID_BUFFER_ID;
    }
    g_buffers[slot].phys_addr = (uint64_t)new_phys;
    g_buffers[slot].virt_addr = new_virt;
    g_buffers[slot].size      = new_act;
    spin_unlock(&g_buffers_lock);

    pmm_free((void *)old_phys, old_pages);

    if (out_actual) *out_actual = new_act;
    if (out_virt)   *out_virt   = new_virt;
    return OK;
}

void BufferRegistryCleanupProcess(uint32_t pid)
{
    process_t *proc = process_find(pid);

    spin_lock(&g_buffers_lock);
    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (!g_buffers[i].in_use || g_buffers[i].owner_pid != pid) continue;

        void    *phys  = (void *)g_buffers[i].phys_addr;
        uint64_t virt  = g_buffers[i].virt_addr;
        size_t   pages = g_buffers[i].size / PMM_PAGE_SIZE;

        if (virt != 0 && proc && proc->cabin) {
            vmm_unmap_pages(proc->cabin->vmm, virt, pages);
        }
        pmm_free(phys, pages);
        __sync_synchronize();

        g_buffers[i].in_use    = false;
        g_buffers[i].handle    = 0;
        g_buffers[i].phys_addr = 0;
        g_buffers[i].virt_addr = 0;
        g_buffers[i].size      = 0;
        g_buffers[i].owner_pid = 0;
    }
    spin_unlock(&g_buffers_lock);
}
