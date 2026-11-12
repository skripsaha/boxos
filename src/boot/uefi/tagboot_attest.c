/*
 * TagBoot — Attest: the kernel a format-v2 volume's BOOT names, loaded, held
 * to the digests its book keeps of it before the jump (spec_v2 §16.7 пп.4, 6).
 *
 * The reading is the Lectern's (tagboot_lectern.c), which knows nothing of
 * UEFI; what is TagBoot's is its desk. The medium is read through the
 * loader's own reader of the volume (tagboot.c, ReadSectors), into pages the
 * firmware gives: Block IO reads into a buffer aligned to the medium's
 * io_align (UEFI 2.10 §13.9, EFI_BLOCK_IO_MEDIA.IoAlign — 0 or 1 any, else a
 * power of two), and a page is aligned to every io_align up to 4096, which is
 * as far as any medium BoxOS meets asks. Every page is handed back before
 * this returns, so the memory map the loader builds after it carries none.
 * Each line is said as TagBoot says one.
 */

#include "tagboot.h"
#include "tagboot_lectern.h"

typedef struct {
    EFI_BOOT_SERVICES *bs;
    AttestReader       read;
} AttestDesk;

static int AttestRead(void *ctx, uint64_t vlba, uint32_t sectors, uint8_t *into)
{
    return !EFI_ERROR(((AttestDesk *)ctx)->read(vlba, sectors, into));
}

static UINTN AttestPages(uint64_t bytes)
{
    return (UINTN)((bytes + VOLUME_BLOCK_BYTES - 1u) / VOLUME_BLOCK_BYTES);
}

static uint8_t *AttestTake(void *ctx, uint64_t bytes)
{
    EFI_PHYSICAL_ADDRESS at = 0;
    if (EFI_ERROR(((AttestDesk *)ctx)->bs->allocate_pages(AllocateAnyPages, EfiLoaderData,
                                                          AttestPages(bytes ? bytes : 1u), &at)))
        return NULL;
    return (uint8_t *)(uintptr_t)at;
}

static void AttestGive(void *ctx, uint8_t *room, uint64_t bytes)
{
    ((AttestDesk *)ctx)->bs->free_pages((EFI_PHYSICAL_ADDRESS)(uintptr_t)room, AttestPages(bytes ? bytes : 1u));
}

static void AttestSay(void *ctx, const char *line)
{
    (void)ctx;
    Print("TagBoot: ");
    Print(line);
    Print("\r\n");
}

const char *AttestBootKernel(EFI_BOOT_SERVICES *bs, AttestReader read, uint64_t first, uint64_t blocks,
                             const uint8_t *image)
{
    static AttestVerdict verdict;
    AttestDesk           ctx;
    LecternDesk          desk;

    ctx.bs         = bs;
    ctx.read       = read;
    desk.ctx       = &ctx;
    desk.read      = AttestRead;
    desk.take      = AttestTake;
    desk.give      = AttestGive;
    desk.say       = AttestSay;
    desk.uuid      = g_volume_uuid;
    MemCopy(&desk.frame, &g_frame, sizeof(desk.frame));
    desk.block_end = g_volume_end;
    LecternAttest(&desk, "the kernel the deed names", first, blocks, image, &verdict);
    return verdict.says == ATTEST_BROKEN ? verdict.line : NULL;
}
