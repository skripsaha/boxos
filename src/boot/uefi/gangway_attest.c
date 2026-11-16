/*
 * Gangway — the desk the Lectern chooses the volume's kernel at:
 * the kernel BOOT names, then the kernels its book names by role, each held to
 * the digests the book keeps of it before the jump (gangway_succession.c).
 *
 * The reading is the Lectern's, which knows nothing of UEFI; what is Gangway's
 * is its desk. The medium is read through the loader's own reader of the
 * volume (gangway_medium.c, ReadSectors), into pages the firmware gives: Block IO
 * reads into a buffer aligned to the medium's io_align (UEFI 2.10 §13.9,
 * EFI_BLOCK_IO_MEDIA.IoAlign — 0 or 1 any, else a power of two), and a page is
 * aligned to every io_align up to 4096, which is as far as any medium BoxOS
 * meets asks. A kernel is placed at KERNEL_LOAD_ADDR, in pages asked for at
 * that address and handed back when the kernel there is not the one — so the
 * next is placed where the first was. Every other page is handed back before
 * this returns — gangway.settings's too, once SettingsTake has read it — so
 * the memory map the loader builds after it carries none but the kernel's.
 * Each line is said as Gangway says one.
 */

#include "gangway.h"
#include "boxos_gangway_settings.h"
#include "settings_chosen.h"   /* SETTINGS_FILE_MAX */

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
    Print("Gangway: ");
    Print(line);
    Print("\r\n");
}

/*
 * The pages a kernel of `bytes` is placed in, asked for at KERNEL_LOAD_ADDR.
 * What this desk placed it hands back when the kernel there is not the one,
 * and only that.
 *
 * ‼ A refusal here is somebody else's range, and it is NOT freed.
 *
 * Nothing at KERNEL_LOAD_ADDR is this loader's when it is asked for —
 * ClaimHandoffPages (gangway_memory.c) asks for 0x0000, 0x1000 and 0xA000 and nothing
 * else, and a kernel this desk placed before is unplaced before the next is
 * asked for — so there is nothing here to re-claim. EDK2's
 * CoreInternalFreePages performs no ownership check whatsoever: it finds the
 * descriptor covering the address and calls CoreConvertPages(...,
 * EfiConventionalMemory), so a free would hand up to KERNEL_MAX_SIZE of memory
 * some firmware driver is holding back to the allocator and then write the
 * kernel image over it. AllocatePages' own EFI_NOT_FOUND (UEFI 2.10 §7.2.1,
 * "The requested pages could not be found") is exactly the answer EDK2 gives
 * when the range IS held — "the pages to allocate have been allocated" — so no
 * status code here distinguishes ours from theirs.
 *
 * Saying so and placing no kernel is the whole of the right answer: the
 * Succession stops with a sentence on the screen, which is a boot that did not
 * happen instead of a boot that corrupts firmware memory and fails somewhere
 * else entirely.
 */
static uint8_t *AttestPlace(void *ctx, uint64_t bytes)
{
    EFI_PHYSICAL_ADDRESS at     = KERNEL_LOAD_ADDR;
    UINTN                pages  = AttestPages(bytes);
    EFI_STATUS           status = ((AttestDesk *)ctx)->bs->allocate_pages(AllocateAddress, EfiLoaderData, pages, &at);
    if (!EFI_ERROR(status)) return (uint8_t *)(uintptr_t)at;
    Print("Gangway: cannot allocate ");
    PrintDec(pages);
    Print(" pages at 0x100000 (status=");
    PrintHex64((uint64_t)status);
    Print(") — something already holds that range and it is not ours to take back\r\n");
    return NULL;
}

static void AttestUnplace(void *ctx, uint8_t *room, uint64_t bytes)
{
    ((AttestDesk *)ctx)->bs->free_pages((EFI_PHYSICAL_ADDRESS)(uintptr_t)room, AttestPages(bytes));
}

/* The kernel the Succession chose, kept for the panel to show. */
static LecternKernel g_chosen;

const LecternKernel *KernelChosen(void)
{
    return &g_chosen;
}

const char *SuccessionTake(EFI_BOOT_SERVICES *bs, AttestReader read, uint64_t *bytes, uint64_t *alloc_end)
{
    LecternKernel       *chosen = &g_chosen;
    LecternCalled        called = { GANGWAY_SETTINGS_FILE, SETTINGS_FILE_MAX, CALLED_NONE, NULL, 0, 0 };
    AttestDesk           ctx;
    LecternDesk          desk;

    ctx.bs           = bs;
    ctx.read         = read;
    desk.ctx         = &ctx;
    desk.read        = AttestRead;
    desk.take        = AttestTake;
    desk.give        = AttestGive;
    desk.say         = AttestSay;
    desk.place       = AttestPlace;
    desk.unplace     = AttestUnplace;
    desk.uuid        = g_volume_uuid;
    MemCopy(&desk.frame, &g_frame, sizeof(desk.frame));
    desk.block_end   = g_volume_end;
    desk.atom_blocks = g_volume_atom_blocks;
    LecternSuccession(&desk, g_have_boot ? &g_boot : NULL, chosen, &called);
    if (chosen->says == ATTEST_BROKEN) return chosen->line;
    if (called.says != CALLED_NONE) SettingsTake(called.bytes, called.size);
    if (called.bytes) AttestGive(&ctx, called.bytes, called.room);
    *bytes     = chosen->bytes;
    *alloc_end = KERNEL_LOAD_ADDR + chosen->blocks * VOLUME_BLOCK_BYTES;
    Print("Gangway: kernel at volume block ");
    PrintDec(chosen->first);
    Print(" (");
    PrintDec(chosen->blocks);
    Print(" blocks in ");
    PrintDec(chosen->runs);
    Print(" run(s))\r\nGangway: kernel loaded (");
    PrintDec(chosen->bytes);
    Print(" bytes)\r\n");
    return NULL;
}
