#include "image_load.h"
#include "elf_image.h"
#include "cabin_layout.h"
#include "pmm.h"
#include "klib.h"

static bool ImageIsElf(const uint8_t *image, uint64_t bytes)
{
    return bytes >= 4 && image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F';
}

/* One segment of an accepted ELF on pages of its own, its bytes copied in and
 * the rest left zero, mapped with exactly its rights. */
static error_t ImagePlaceSegment(vmm_context_t *vmm, const ElfImage *elf,
                                 const ElfImageSegment *segment, const char *name)
{
    uintptr_t first = (uintptr_t)(segment->Virt & VMM_PAGE_MASK);
    uint64_t  lead  = segment->Virt - first;
    size_t    pages = (size_t)((lead + segment->MemoryBytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE);

    void *frames = pmm_alloc_zero(pages);
    if (!frames) {
        kprintf("[IMAGE] '%s' is not started: no %zu pages for its segment at 0x%llx\n",
                name, pages, (unsigned long long)segment->Virt);
        return ERR_NO_MEMORY;
    }
    memcpy((uint8_t *)vmm_phys_to_virt((uintptr_t)frames) + lead,
           elf->File + segment->FileOffset, segment->FileBytes);

    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (segment->Writable)
        flags |= VMM_FLAG_WRITABLE;
    if (!segment->Executable)
        flags |= VMM_FLAG_NO_EXECUTE;

    vmm_map_result_t mapped = vmm_map_pages(vmm, first, (uintptr_t)frames, pages, flags);
    if (!mapped.success) {
        pmm_free(frames, pages);
        kprintf("[IMAGE] '%s' is not started: its segment at 0x%llx was not mapped: %s\n",
                name, (unsigned long long)segment->Virt, mapped.error_msg);
        return ERR_SPAWN_FAILED;
    }

    debug_printf("[IMAGE] '%s': 0x%llx-0x%llx %s, %llu bytes from the file, %llu in memory\n",
                 name, (unsigned long long)first,
                 (unsigned long long)(first + pages * VMM_PAGE_SIZE - 1),
                 segment->Writable ? "R+W" : segment->Executable ? "R+X" : "R",
                 (unsigned long long)segment->FileBytes, (unsigned long long)segment->MemoryBytes);
    return OK;
}

static error_t ImageLoadElf(vmm_context_t *vmm, const void *image, uint64_t bytes,
                            const char *name, uintptr_t *entry)
{
    ElfImage        elf;
    ElfImageVerdict verdict = ElfImageRead(image, bytes, CABIN_CODE_START_ADDR,
                                           CABIN_CODE_END_ADDR, &elf);
    if (verdict != ELF_IMAGE_OK) {
        if (elf.Header != ELF_IMAGE_NO_HEADER)
            kprintf("[IMAGE] '%s' is not started: %s (program header %u)\n",
                    name, ElfImageVerdictText(verdict), (unsigned)elf.Header);
        else
            kprintf("[IMAGE] '%s' is not started: %s\n", name, ElfImageVerdictText(verdict));
        return ERR_INVALID_ELF;
    }

    ElfImageSegment segment;
    uint16_t        at = 0;
    while (ElfImageNextSegment(&elf, &at, &segment)) {
        error_t placed = ImagePlaceSegment(vmm, &elf, &segment, name);
        if (placed != OK)
            return placed;
    }

    *entry = (uintptr_t)elf.Entry;
    return OK;
}

static error_t ImageLoadRaw(vmm_context_t *vmm, const uint8_t *code, uint64_t bytes,
                            const char *name, uintptr_t *entry)
{
    if (bytes > CABIN_CODE_END_ADDR - CABIN_CODE_START_ADDR) {
        kprintf("[IMAGE] '%s' is not started: %llu bytes of raw code do not fit the room a "
                "program's image has\n", name, (unsigned long long)bytes);
        return ERR_BINARY_TOO_LARGE;
    }

    /*
     * ‼ RAW CODE THAT BEGINS WITH ZEROS IS A READ THAT DID NOT HAPPEN.
     *
     * Every caller reads the whole file and checks the count, and this is the
     * check that does not depend on any of them being right: 0x00 0x00 is
     * `add [rax], al`, so raw code of zeros does not fail to start — it runs,
     * writes to address zero with every register clear and dies with a page
     * fault at 0x0 that names the program, not the medium that would not read.
     * Sixteen zero bytes at the front is no instruction anybody wrote.
     */
    uint64_t look = bytes < 16 ? bytes : 16;
    bool     anything = false;
    for (uint64_t i = 0; i < look && !anything; i++)
        anything = code[i] != 0;
    if (!anything) {
        kprintf("[IMAGE] '%s' is not started: its first %llu bytes are all zero — this is a "
                "read that did not happen, not a program\n", name, (unsigned long long)look);
        return ERR_INVALID_ELF;
    }

    size_t pages  = (size_t)((bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE);
    void  *frames = pmm_alloc_zero(pages);
    if (!frames) {
        kprintf("[IMAGE] '%s' is not started: no %zu pages for its raw code\n", name, pages);
        return ERR_NO_MEMORY;
    }
    memcpy(vmm_phys_to_virt((uintptr_t)frames), code, bytes);

    vmm_map_result_t mapped = vmm_map_pages(vmm, CABIN_CODE_START_ADDR, (uintptr_t)frames, pages,
                                            VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    if (!mapped.success) {
        pmm_free(frames, pages);
        kprintf("[IMAGE] '%s' is not started: its raw code was not mapped: %s\n",
                name, mapped.error_msg);
        return ERR_SPAWN_FAILED;
    }

    kprintf("[IMAGE] '%s' runs as raw code: its %zu pages at 0x%llx are writable and "
            "executable at once\n", name, pages, (unsigned long long)CABIN_CODE_START_ADDR);
    *entry = (uintptr_t)CABIN_CODE_START_ADDR;
    return OK;
}

error_t ImageLoad(vmm_context_t *vmm, const void *image, uint64_t bytes, const char *name,
                  bool tagged_raw, uintptr_t *entry)
{
    const uint8_t *head = (const uint8_t *)image;

    *entry = 0;
    if (ImageIsElf(head, bytes)) {
        if (tagged_raw) {
            kprintf("[IMAGE] '%s' is not started: it is tagged raw, and it is an ELF "
                    "program — one of the two is wrong\n", name);
            return ERR_INVALID_ELF;
        }
        return ImageLoadElf(vmm, image, bytes, name, entry);
    }
    if (tagged_raw)
        return ImageLoadRaw(vmm, head, bytes, name, entry);

    kprintf("[IMAGE] '%s' is not started: it is no program this system knows — not ELF, "
            "and not tagged raw\n", name);
    return ERR_INVALID_ELF;
}
