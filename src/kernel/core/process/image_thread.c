#include "image_thread.h"
#include "pmm.h"
#include "klib.h"

/* The TCB: its own address at fs:0, and a word kept zero. */
#define IMAGE_THREAD_TCB_BYTES 16u

uint64_t ImageThreadBelow(const ImageThread *thread)
{
    return (thread->TotalBytes + thread->Align - 1) & ~(thread->Align - 1);
}

uint64_t ImageThreadPages(const ImageThread *thread)
{
    if (thread->TotalBytes == 0)
        return 0;
    return (ImageThreadBelow(thread) + IMAGE_THREAD_TCB_BYTES + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
}

/* What of `count` bytes meant for offset `at` of the block falls on the page
 * of the block that starts at offset `from`. */
static void ImageThreadSlice(uint8_t *page, uint64_t from, const void *bytes, uint64_t at,
                             uint64_t count)
{
    uint64_t start = at > from ? at : from;
    uint64_t end   = at + count < from + VMM_PAGE_SIZE ? at + count : from + VMM_PAGE_SIZE;
    if (start < end)
        memcpy(page + (start - from), (const uint8_t *)bytes + (start - at), end - start);
}

error_t ImageThreadPlace(vmm_context_t *vmm, const ImageThread *thread, uintptr_t at,
                         const char *name, uintptr_t *tp)
{
    uint64_t below  = ImageThreadBelow(thread);
    uint64_t tcb[2] = { at + below, 0 };

    for (uint64_t page = 0; page < ImageThreadPages(thread); page++) {
        void *frame = pmm_alloc_zero(1);
        if (!frame) {
            kprintf("[IMAGE] '%s' is not started: memory has no page left for its thread-local "
                    "storage\n", name);
            return ERR_NO_MEMORY;
        }
        uint8_t *bytes = (uint8_t *)vmm_phys_to_virt((uintptr_t)frame);
        uint64_t from  = page * VMM_PAGE_SIZE;
        if (thread->InitBytes > 0)
            ImageThreadSlice(bytes, from, thread->Init, 0, thread->InitBytes);
        ImageThreadSlice(bytes, from, tcb, below, sizeof(tcb));

        vmm_map_result_t mapped = vmm_map_page(vmm, at + from, (uintptr_t)frame,
                                               VMM_FLAG_PRESENT | VMM_FLAG_USER |
                                                   VMM_FLAG_WRITABLE | VMM_FLAG_NO_EXECUTE);
        if (!mapped.success) {
            pmm_free(frame, 1);
            kprintf("[IMAGE] '%s' is not started: its thread-local storage at 0x%llx was not "
                    "mapped: %s\n", name, (unsigned long long)(at + from), mapped.error_msg);
            return ERR_SPAWN_FAILED;
        }
    }
    *tp = at + below;
    return OK;
}

bool ImageThreadFitsStrand(const ImageThread *thread)
{
    return thread->TotalBytes == 0 || ImageThreadBelow(thread) <= VMM_PAGE_SIZE;
}

error_t ImageThreadKeep(const ImageThread *thread, const char *name, ImageThread *kept)
{
    *kept      = *thread;
    kept->Init = NULL;
    if (thread->InitBytes == 0 || !ImageThreadFitsStrand(thread))
        return OK;

    uint8_t *copy = kmalloc(thread->InitBytes);
    if (!copy) {
        kprintf("[IMAGE] '%s' is not started: the kernel has no room to keep its thread-local "
                "template for its strands\n", name);
        return ERR_NO_MEMORY;
    }
    memcpy(copy, thread->Init, thread->InitBytes);
    kept->Init = copy;
    return OK;
}

void ImageThreadLetGo(ImageThread *kept)
{
    if (kept->Init)
        kfree((void *)kept->Init);
    kept->Init = NULL;
}

void ImageThreadPour(const ImageThread *thread, uint8_t *page)
{
    if (thread->InitBytes > 0)
        memcpy(page + VMM_PAGE_SIZE - ImageThreadBelow(thread), thread->Init, thread->InitBytes);
}
