/*
 * Gangway — the UEFI loader of BoxOS: the journey from the firmware's call to
 * the kernel's first instruction, step by step, each step in the file that
 * answers for it.
 *
 *   1. COM1 first, then the firmware's tables captured  (gangway_firmware.c)
 *   2. the pages the handoff is written into, asked for (gangway_memory.c)
 *   3. every medium asked for the ground its partition table claims for
 *      BoxOS and the Deed on it; the first that is a book this loader boots
 *      is kept                                           (gangway_medium.c,
 *                                                         gangway_deed.c)
 *   4. the Succession — the kernel BOOT names, then the kernels the book names
 *      by role, each placed at 0x100000 and held to the digests the book keeps
 *      of its blocks; the first attested is the one jumped into, one that does
 *      not match never is                                (gangway_attest.c,
 *                                                         gangway_succession.c,
 *                                                         gangway_lectern*.c)
 *   5. the screen: the GOP mode, the framebuffer         (gangway_video.c)
 *      — the crossings this medium keeps, the panel when
 *      the person holds Space or the last two starts did
 *      not land, and this start's crossing kept           (gangway_crossings.c,
 *                                                         gangway_panel*.c,
 *                                                         gangway_keys.c)
 *   6. the firmware's memory map as E820 at 0x500        (gangway_memory.c)
 *   7. the loader's own place checked against the 4 GB identity map, and the
 *      page tables and boot stack built after the kernel (gangway_paging.c)
 *   8. the ESRT kept where the kernel will not hand it away
 *                                                        (gangway_firmware.c)
 *   9. boot_info and the Boarding Pass at 0xA000/0xA600  (gangway_handoff.c)
 *  10. ExitBootServices, then the E820 rebuilt from the map whose key the
 *      firmware accepted — the only one that describes this loader's own late
 *      allocations                                       (gangway_memory.c)
 *  11. CR3 installed, the boot stack taken, the jump to 0x100000
 *                                                        (gangway_handoff.c,
 *                                                         gangway_jump.asm)
 *
 * What this loader does NOT do, and what said it did for a long time:
 * touch EFER. UEFI hands us a machine already in long mode (LME=1, PG=1,
 * PAE=1) and the kernel establishes every further EFER bit it needs — NXE
 * in CpuTakeUpNoExecute, SCE in per_core_setup_notify_msrs. Three comments
 * in this loader used to claim EFER.LME+NXE was set here while
 * gangway_jump.asm said the opposite and did nothing; the kernel's own NXE
 * write then landed after it had already mapped firmware pages with bit 63,
 * and two real boards panicked inside SetVirtualAddressMap with err=0x9.
 */

#include "gangway.h"

/* The firmware, as it called this loader: the one system table, its boot
 * services and this image's handle, for the whole of the journey. */
EFI_SYSTEM_TABLE  *g_st   = NULL;
EFI_BOOT_SERVICES *g_bs   = NULL;
EFI_HANDLE         g_image_handle = NULL;

/* =========================================================================
 * Kernel loading
 *
 * KERNEL_LOAD_ADDR is gangway.h's; KERNEL_MAX_SIZE is the Lectern's
 * (gangway_lectern.h), which holds every kernel to it before it is jumped
 * into.
 * ========================================================================= */

/* Where the pages the kernel was placed in END (gangway_attest.c, AttestPlace,
 * asked for them; SuccessionTake says where they end). Page-aligned by
 * construction, and the first address past it is the first one the loader has
 * NOT already reserved — which is where the BSS reservation has to begin, for
 * two reasons at once: AllocateAddress refuses an unaligned address outright
 * (EDK2 CoreInternalAllocatePages returns EFI_NOT_FOUND from its granularity
 * check before it looks at the map at all), and asking again for a page this
 * loader already holds is a range that is allocated, which is the other thing
 * that answer means. */
static uint64_t g_kernel_alloc_end = 0;

/*
 * The BSS the kernel's header says extends past the bytes loaded, set aside
 * so the firmware does not hand those pages to anybody before
 * ExitBootServices — said, with how it went.
 *
 * ‼ THE RESERVATION BEGINS WHERE THE IMAGE ALLOCATION ENDS, not at the
 * kernel's last loaded byte. It used to begin at KERNEL_LOAD_ADDR +
 * loaded_bytes, which is the kernel file's true size out of the Deed's
 * boot stamp and is therefore almost never a multiple of 4096 — and an
 * unaligned AllocateAddress is refused by EDK2 before the memory map is
 * consulted at all (CoreInternalAllocatePages: `if (Type ==
 * AllocateAddress) { if ((*Memory & (Alignment - 1)) != 0) { return
 * EFI_NOT_FOUND; } }`). Rounding down would not have helped either: the
 * page holding that byte is inside the range AttestPlace already
 * allocated, and asking for pages one already holds is the OTHER thing
 * EFI_NOT_FOUND means here. So the whole 2 MB window the kernel treats
 * as its BSS went unreserved on every UEFI boot, and the only trace was
 * a status in parentheses at the end of one line.
 *
 * Nothing here relies on what the pages contain: §7.2.1 says nothing
 * about the contents of a page AllocatePages hands back (EDK2 clears
 * pages only under DEBUG_CLEAR_MEMORY, a debug-build macro), and the
 * kernel's own entry zeroes __bss_start..__bss_end with rep stosb
 * before it runs a line of C. A comment here used to claim the spec
 * promised zeroed pages. It does not.
 *
 * A refusal is NOT followed by free_pages. Past the image allocation
 * this loader holds nothing, so a refusal is somebody else's range, and
 * EDK2 frees without one ownership check. The guard this replaces
 * tested AllocatePages' EFI_NOT_FOUND against FreePages' meaning of
 * the code (§7.2.2 vs §7.2.1) and so fired only when the pages WERE
 * held by someone. */
static void KernelBssSetAside(uint64_t kernel_end_phys)
{
    Print("Gangway: kernel phys end (BSS) ");
    PrintHex64(kernel_end_phys);

    if (kernel_end_phys > g_kernel_alloc_end) {
        EFI_PHYSICAL_ADDRESS bss_pa = (EFI_PHYSICAL_ADDRESS)g_kernel_alloc_end;
        UINTN bss_pages = (UINTN)((kernel_end_phys - g_kernel_alloc_end +
                                   PAGE_4KB - 1) / PAGE_4KB);
        EFI_STATUS bss_status = g_bs->allocate_pages(AllocateAddress,
                                                      EfiLoaderData,
                                                      bss_pages, &bss_pa);
        if (EFI_ERROR(bss_status)) {
            Print(" (BSS pages not set aside, status=");
            PrintHex64((uint64_t)bss_status);
            Print(")");
        } else {
            Print(" (");
            PrintDec((uint64_t)bss_pages);
            Print(" BSS pages set aside from ");
            PrintHex64(g_kernel_alloc_end);
            Print(")");
        }
    } else {
        /* The BSS ends inside the pages the image was loaded into, which
         * this loader already holds. Nothing left to ask for. */
        Print(" (BSS inside the kernel's own pages)");
    }
    Print("\r\n");
}

/*
 * The volume's kernel, chosen by the Succession: the kernel BOOT names held to
 * the book first, then the kernels the book names by role, each placed at
 * KERNEL_LOAD_ADDR and held to the digests the book keeps of its blocks — one
 * that does not match is never jumped into, and when none may be, the loader
 * stops with every kernel it tried and why (gangway_attest.c,
 * gangway_succession.c). Its header held, its BSS set aside; answers where the
 * kernel ends in physical memory, its BSS included.
 */
static uint64_t KernelTake(void)
{
    uint64_t loaded_bytes = 0;
    const char *stop = SuccessionTake(g_bs, ReadSectors, &loaded_bytes, &g_kernel_alloc_end);
    if (stop) Panic(stop);

    /* Validate kernel header magic — 'KERNEL' at byte offset 2.
     * Kernel header layout (kernel_entry.asm):
     *   +0  2 bytes  jmp short .past_header
     *   +2  6 bytes  "KERNEL"
     *   +8  4 bytes  _kernel_phys_end  (physical end INCLUDING BSS)
     */
    if (!MemEqual((const void *)(uintptr_t)(KERNEL_LOAD_ADDR + 2), "KERNEL", 6)) {
        Panic("kernel header magic invalid — wrong binary at load address");
    }
    Print("Gangway: kernel header OK\r\n");

    /* Read the true physical end (including BSS) from the kernel header.
     * This MUST be used for page table placement — if we place page tables
     * before BSS end, kernel_entry.asm's "rep stosb" will overwrite them. */
    uint32_t header_phys_end = 0;
    MemCopy(&header_phys_end,
            (const void *)(uintptr_t)(KERNEL_LOAD_ADDR + 8), 4);

    uint64_t kernel_end_phys = KERNEL_LOAD_ADDR + loaded_bytes;
    if (header_phys_end > (uint32_t)kernel_end_phys && header_phys_end < 0x10000000U) {
        kernel_end_phys = (uint64_t)header_phys_end;
        KernelBssSetAside(kernel_end_phys);
    }
    return kernel_end_phys;
}

/*
 * UEFI starts a 5-minute boot watchdog at LoadImage time. If the kernel
 * is slow to come up (large E820, AP wakeup, disk init), the firmware
 * will reset the box mid-boot. Disable the watchdog before handing off:
 * timeout=0 cancels it. Spec §7.5 — kernel never returns, so we never
 * want to be reset by it. Some firmware returns EFI_UNSUPPORTED (no
 * watchdog implementation) — harmless, we proceed regardless. Log the
 * status so an unexpected EFI_DEVICE_ERROR surfaces in the boot log.
 */
static void WatchdogStop(void)
{
    EFI_STATUS wd = g_bs->set_watchdog_timer(0, 0, 0, NULL);
    if (EFI_ERROR(wd)) {
        Print("Gangway: watchdog disable returned status=");
        PrintHex64((uint64_t)wd);
        Print(" (proceeding)\r\n");
    }
}

/*
 * GangwayMain — UEFI entry point.
 *
 * MUST be declared EFIAPI: UEFI firmware calls this with Microsoft x64 ABI
 * (image_handle in RCX, st in RDX).  Without EFIAPI the gcc-compiled function
 * reads System V registers RDI/RSI instead → garbage pointers → instant crash.
 */
EFI_STATUS EFIAPI GangwayMain(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    Moment("entry");

    /* Raw COM1 byte BEFORE touching any UEFI pointer — confirms we were called. */
    Com1Init();
    Com1Str("\r\n[GANGWAY] Entry reached\r\n");

    g_st           = st;
    g_bs           = st->boot_services;
    g_image_handle = image_handle;
    FirmwareTablesTake(st);

    /* The screen is left as the firmware left it — its logo, on a desk
     * machine — until the display program takes it; this is the journal's. */
    Print("Gangway — BoxOS UEFI Bootloader\r\n");
    Print("------------------------------------\r\n");
    FirmwareTablesSay(st);

    /* Before anything is written into them, and so before the firmware can be
     * asked for memory by any of the steps below and answer with these. */
    ClaimHandoffPages();

    /* The deed of every medium probed is taken up there and then, so a medium
     * whose volume is no book is passed over for the next (BlockIoProbeVolume);
     * the one found is a book, its Deed taken up. */
    Print("Gangway: locating disk...\r\n");
    if (EFI_ERROR(BlockIoFindDevice(image_handle)))
        Panic("no medium carries a book this loader can boot");
    Moment("medium");

    uint64_t kernel_end_phys = KernelTake();
    Moment("kernel");

    /* The screen chosen is the one a stop from here on is drawn on. */
    FbInfo fb = QueryGopFramebuffer();
    DrawUse(&fb);
    Moment("screen");

    /* The crossings this medium keeps; two starts in a row that did not land
     * open the panel by themselves, and a person holding Space opens it too —
     * what they change there may be the screen itself. This start's crossing
     * is kept once the panel is left: one restarted or turned off there never
     * crossed. */
    CrossingsTake();
    uint8_t panel = PanelIfAsked(&fb, CrossingsMisses());
    CrossingKeep(panel, &fb);
    SceneShow(&fb);

    Print("Gangway: building memory map...\r\n");
    uint32_t e820_count = 0;
    if (EFI_ERROR(BuildMemoryMap(&e820_count))) Panic("failed to get UEFI memory map");
    Moment("memory");

    /* Before CR3 is switched: the loader must lie inside the identity map. */
    CheckEfiLoadAddress();

    Print("Gangway: setting up page tables...\r\n");
    if (EFI_ERROR(SetupPageTables(kernel_end_phys))) Panic("page table setup failed");

    EsrtKeep();
    Moment("tables");

    /* boot_info carries the RSDP and the ESRT copy, so it is filled before EBS. */
    FillBootInfo(&fb, e820_count, kernel_end_phys);
    Print("Gangway: boot_info at 0xA000 (method=UEFI)\r\n");

    WatchdogStop();
    Print("Gangway: exiting boot services...\r\n");
    if (EFI_ERROR(DoExitBootServices(image_handle)))
        Panic("ExitBootServices failed after 10 attempts");

    /* From here: no UEFI services, and nothing more for the journal. Pure
     * bare metal. boot_info is patched with the FINAL EFI memory map — the
     * one whose key ExitBootServices accepted — and the Boarding Pass is
     * written, naming the journal now that it is whole. */
    PostEbsPatchBootInfo();
    Moment("leave");
    WriteBoardingPass();
    JumpToKernel(g_pt_base);
}
