#include "image_load.h"
#include "box_load.h"
#include "elf_image.h"
#include "box_format.h"
#include "cabin_layout.h"
#include "kernel_config.h"
#include "nameplate.h"
#include "tagfs.h"
#include "pmm.h"
#include "klib.h"

/*
 * ‼ THE WHOLE OF WHAT IS ASKED, OR NONE OF IT
 *
 * The count is checked beside the answer. A read that stops early leaves the
 * rest of its buffer as it was — zeros, in pages allocated zeroed — and a page
 * of zeros is not an empty program: 0x00 0x00 decodes as `add [rax], al`, so
 * code that was never read starts, writes to address zero with every register
 * still clear and dies with a page fault at 0x0 — which is what a machine
 * whose flash drive stopped answering showed on the screen, having first
 * announced that it had started the program.
 */
error_t ImageSourceRead(const ImageSource *source, uint64_t offset, void *into, uint64_t count,
                        const char *name)
{
    if (offset > source->Bytes || count > source->Bytes - offset) {
        kprintf("[IMAGE] '%s' is not started: %llu bytes at %llu lie past its %llu\n", name,
                (unsigned long long)count, (unsigned long long)offset,
                (unsigned long long)source->Bytes);
        return ERR_INVALID_ELF;
    }
    if (count == 0)
        return OK;
    if (source->Memory) {
        memcpy(into, source->Memory + offset, count);
        return OK;
    }

    uint64_t got = 0;
    source->File->offset = offset;
    error_t rd = tagfs_read(source->File, into, count, &got);
    if (rd != OK || got != count) {
        kprintf("[IMAGE] '%s' is not started: %llu of its %llu bytes at %llu came back (error %u) — "
                "an image that is not all here is not started\n", name, (unsigned long long)got,
                (unsigned long long)count, (unsigned long long)offset, (unsigned)rd);
        return rd != OK ? rd : ERR_READ_FAILED;
    }
    return OK;
}

/* ── ELF ─────────────────────────────────────────────────────────────────── */

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

/* The ELF in `file`, read whole, put into `vmm`. */
static error_t ImageLoadElfFrom(vmm_context_t *vmm, const uint8_t *file, uint64_t bytes,
                                const char *name, ImageLoaded *loaded)
{
    ElfImage        elf;
    ElfImageVerdict verdict = ElfImageRead(file, bytes, CABIN_CODE_START_ADDR, CABIN_CODE_END_ADDR, &elf);
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

    /* The main strand's thread-local storage on the pages after the image. */
    ImageThread thread = { elf.Thread.Align, elf.Thread.InitBytes, elf.Thread.TotalBytes,
                           file + elf.Thread.FileOffset };
    uint64_t    pages  = ImageThreadPages(&thread);
    if (pages > (CABIN_CODE_END_ADDR - elf.End) / VMM_PAGE_SIZE) {
        kprintf("[IMAGE] '%s' is not started: its thread-local storage does not fit after its "
                "image in the room a program's image has\n", name);
        return ERR_BINARY_TOO_LARGE;
    }
    if (pages > 0) {
        error_t placed = ImageThreadPlace(vmm, &thread, (uintptr_t)elf.End, name,
                                          &loaded->ThreadPointer);
        if (placed != OK)
            return placed;
    }

    loaded->Entry = (uintptr_t)elf.Entry;
    (void)nameplate_locate(file, bytes, CABIN_CODE_START_ADDR, &loaded->NameplateVa,
                           &loaded->NameplateBytes);
    return ImageThreadKeep(&thread, name, &loaded->Thread);
}

/* An ELF is read whole: its headers say where everything is, and every byte
 * of it is checked before a page is given to it. A file is staged in pages of
 * its own for the length of the load; bytes already held are read in place. */
static error_t ImageLoadElf(vmm_context_t *vmm, const ImageSource *source, const char *name,
                            ImageLoaded *loaded)
{
    if (source->Memory)
        return ImageLoadElfFrom(vmm, source->Memory, source->Bytes, name, loaded);

    size_t pages = (size_t)((source->Bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE);
    void  *stage = pmm_alloc(pages);
    if (!stage) {
        kprintf("[IMAGE] '%s' is not started: no %zu pages to read it into\n", name, pages);
        return ERR_NO_MEMORY;
    }
    uint8_t *file = (uint8_t *)vmm_phys_to_virt((uintptr_t)stage);
    error_t  rc   = ImageSourceRead(source, 0, file, source->Bytes, name);
    if (rc == OK)
        rc = ImageLoadElfFrom(vmm, file, source->Bytes, name, loaded);
    pmm_free(stage, pages);
    return rc;
}

/* ── which kind ──────────────────────────────────────────────────────────── */

typedef enum { IMAGE_NO_PROGRAM, IMAGE_BOX, IMAGE_ELF } ImageKind;

/* What `look` bytes of an image's start, in `head`, say it is. */
static ImageKind ImageKindOf(const uint8_t *head, uint64_t look)
{
    static const uint8_t box_magic[8] = { BOX_MAGIC_0, BOX_MAGIC_1, BOX_MAGIC_2, BOX_MAGIC_3,
                                          BOX_MAGIC_4, BOX_MAGIC_5, BOX_MAGIC_6, BOX_MAGIC_7 };
    if (look == 8 && memcmp(head, box_magic, 8) == 0)
        return IMAGE_BOX;
    if (look >= 4 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F')
        return IMAGE_ELF;
    return IMAGE_NO_PROGRAM;
}

bool ImageIsProgram(const ImageSource *source, const char *name)
{
    uint8_t  head[8] = { 0 };
    uint64_t look    = source->Bytes < sizeof(head) ? source->Bytes : sizeof(head);
    return ImageSourceRead(source, 0, head, look, name) == OK &&
           ImageKindOf(head, look) != IMAGE_NO_PROGRAM;
}

error_t ImageLoad(vmm_context_t *vmm, const ImageSource *source, const char *name, ImageLoaded *loaded)
{
    uint8_t  head[8] = { 0 };
    uint64_t look    = source->Bytes < sizeof(head) ? source->Bytes : sizeof(head);

    loaded->Entry          = 0;
    loaded->NameplateVa    = 0;
    loaded->NameplateBytes = 0;
    loaded->ImageBase      = 0;
    loaded->PartsVa        = 0;
    loaded->Thread         = (ImageThread){ 0 };
    loaded->ThreadPointer  = 0;
    loaded->StackReserve   = CONFIG_USER_STACK_RESERVE;
    loaded->Rise           = false;
    loaded->Asks           = 0;
    loaded->Trusted        = false;
    loaded->Passport       = 0;

    error_t rc = ImageSourceRead(source, 0, head, look, name);
    if (rc != OK)
        return rc;

    ImageKind kind = ImageKindOf(head, look);
    if (kind == IMAGE_BOX)
        return BoxLoad(vmm, source, name, loaded);
    if (kind == IMAGE_ELF)
        return ImageLoadElf(vmm, source, name, loaded);

    kprintf("[IMAGE] '%s' is not started: it is no program this system knows — neither a box "
            "nor ELF\n", name);
    return ERR_INVALID_ELF;
}
