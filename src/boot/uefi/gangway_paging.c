/*
 * Gangway — the page tables the kernel is jumped into on (the low 4 GB
 * identity-mapped, the kernel's first 32 MB at the top of the address space)
 * with the boot stack beside them, and the check that the loader itself lies
 * inside that identity map before CR3 is switched under its feet.
 */

#include "gangway.h"

#define PAGE_TABLE_SIZE       0x8000ULL     /* 32 KB */
#define GUARD_PAGE_SIZE       0x1000ULL     /* 4 KB — guard between PT and stack */
#define BOOT_STACK_SIZE       0x10000ULL    /* 64 KB */

/* PTE flags */
#define PTE_PRESENT           (1ULL << 0)
#define PTE_WRITABLE          (1ULL << 1)
#define PTE_PAGE_SIZE         (1ULL << 7)   /* PS bit for 2MB pages */

/* =========================================================================
 * Page tables: identity + higher-half
 *
 * Layout (identical to stage2.asm's setup_paging):
 *   pt_base + 0x0000  PML4  (4 KB)
 *   pt_base + 0x1000  PDPT  (4 KB)   — identity, PML4[0]
 *   pt_base + 0x2000  PD0   (4 KB)   — 0 GB.. 1 GB
 *   pt_base + 0x3000  PD1   (4 KB)   — 1 GB.. 2 GB
 *   pt_base + 0x4000  PD2   (4 KB)   — 2 GB.. 3 GB
 *   pt_base + 0x5000  PD3   (4 KB)   — 3 GB.. 4 GB
 *   pt_base + 0x6000  PDPT_high      — higher-half, PML4[511]
 *   pt_base + 0x7000  PD_high        — first 32 MB at 0xFFFFFFFF80000000
 *
 * pt_base is placed at kernel_end (page-aligned) + 4 KB guard.
 * Stack base is pt_base + PAGE_TABLE_SIZE + GUARD_PAGE_SIZE + BOOT_STACK_SIZE.
 * ========================================================================= */
uint64_t g_pt_base   = 0;
uint64_t g_stack_base = 0;

/*
 * How many 2 MB identity pages to map: all of them, the whole low 4 GB.
 *
 * This used to be derived from the top of USABLE memory, which is the wrong
 * question by one word. What has to be reachable through these tables is not
 * where RAM ends — it is where anything the early kernel touches lives, and
 * on a UEFI machine the two are different: the GOP framebuffer, the local
 * APIC, ECAM and every runtime-services region sit ABOVE the last byte of
 * RAM, in the hole between it and 4 GB. A board with 2 GB of RAM and a
 * framebuffer at 0xE0000000 got an identity map that stopped at 2 GB, and
 * the kernel's first pixel was a page fault with no page tables of its own
 * yet to report it.
 *
 * The cost of asking the wide question is zero: PD0..PD3 are allocated
 * unconditionally either way (16 KB of the 32 KB block), and 2048 entries is
 * exactly what fills them.
 *
 * Mapping MMIO write-back here is harmless, but for a narrower reason than
 * "UC wins", which is what this note used to say under a section number that
 * names a table column rather than a section. The MTRR type does not simply
 * beat the page's: Intel SDM Vol 3A §11.5.2.2, "Selecting Memory Types for
 * Pentium III and More Recent Processor Families", Table 11-7, gives MTRR UC
 * an effective type of UC against PAT entries UC, UC-, WT, WB and WP — and
 * WC against a PAT entry of WC. A page that asked for write-combining would
 * override the firmware's UC.
 *
 * These entries ask for nothing. They carry neither PAT nor PCD nor PWT,
 * which selects PAT entry 0 ("Selecting a Memory Type from the PAT",
 * Table 11-11), whose type after power-up or reset is WB ("Programming the
 * PAT", Table 11-12) and which this loader never rewrites — it issues no
 * WRMSR at all, here or anywhere else. WB against a UC MTRR is UC, so the
 * firmware's MTRRs do decide these ranges.
 *
 * The one thing not established in this tree is the PAT the firmware hands
 * over. Every entry value but WC lands on UC anyway, and PAT0 is the type
 * ordinary RAM gets on a page with no attribute bits, so a firmware setting
 * it to WC would have mis-typed its own memory long before this line ran.
 * Nothing here may start setting PAT/PCD/PWT without revisiting that.
 *
 * These tables live only until vmm_init builds the kernel's own.
 */
static uint32_t CalculateIdentityMapPages(void)
{
    return 2048;   /* 2048 x 2 MB = 4 GB */
}
EFI_STATUS SetupPageTables(uint64_t kernel_phys_end)
{
    /* pt_base after kernel, page-aligned, plus guard. */
    uint64_t aligned_end = (kernel_phys_end + PAGE_4KB - 1) & ~(PAGE_4KB - 1);
    g_pt_base   = aligned_end + GUARD_PAGE_SIZE;
    g_stack_base = g_pt_base + PAGE_TABLE_SIZE + GUARD_PAGE_SIZE + BOOT_STACK_SIZE;

    /* Allocate contiguous region: page tables (32 KB) + guard (4 KB) + stack (64 KB).
     * All three must be reserved so UEFI firmware does not use them before
     * ExitBootServices.  The guard page is included in the allocation but left
     * unmapped — it exists only as a buffer between page tables and stack. */
    uint64_t total_alloc = PAGE_TABLE_SIZE + GUARD_PAGE_SIZE + BOOT_STACK_SIZE;
    EFI_PHYSICAL_ADDRESS pt_alloc = (EFI_PHYSICAL_ADDRESS)g_pt_base;
    UINTN alloc_pages = (UINTN)(total_alloc / PAGE_4KB);

    EFI_STATUS status = g_bs->allocate_pages(AllocateAddress,
                                             EfiLoaderData,
                                             alloc_pages,
                                             &pt_alloc);
    if (EFI_ERROR(status)) {
        /*
         * ‼ NOTHING IS FREED HERE, and there is no retry.
         *
         * This range begins one guard page past the kernel's physical end,
         * which is past everything this loader has ever asked for — the BSS
         * reservation stops at that end. So a refusal is somebody else's
         * range, and EDK2's CoreInternalFreePages would release it without
         * one ownership check, putting firmware memory into the pool this
         * loader is about to hand the kernel as free RAM.
         *
         * The guard this replaces tested for EFI_NOT_FOUND, reading
         * FreePages' meaning of that code (UEFI 2.10 §7.2.2) into a status
         * that came back from AllocatePages. §7.2.1 gives it the opposite
         * sense, and EDK2 returns it exactly when the pages ARE allocated to
         * someone. The guard fired only in the case it was written to stop.
         */
        Print("Gangway: cannot allocate page tables + stack at ");
        PrintHex64(g_pt_base);
        Print(" (status=");
        PrintHex64((uint64_t)status);
        Print(") — that range is held by something else\r\n");
        return status;
    }

    uint64_t *pt = (uint64_t *)(uintptr_t)g_pt_base;
    MemZero(pt, PAGE_TABLE_SIZE);

    uint64_t *pml4     = pt;              /* +0x0000 */
    uint64_t *pdpt     = pt + 512;        /* +0x1000 */
    uint64_t *pd_base  = pt + 512 * 2;   /* +0x2000 = PD0 */
    uint64_t *pdpt_hi  = pt + 512 * 6;   /* +0x6000 */
    uint64_t *pd_hi    = pt + 512 * 7;   /* +0x7000 */

    uint32_t total_2mb_pages = CalculateIdentityMapPages();
    uint32_t num_pds = (total_2mb_pages + 511) / 512;
    if (num_pds > 4) num_pds = 4;

    /* PML4[0] → PDPT */
    pml4[0] = (uint64_t)(uintptr_t)pdpt | PTE_PRESENT | PTE_WRITABLE;

    /* PDPT[0..num_pds-1] → PD0..PD3 */
    for (uint32_t i = 0; i < num_pds; i++) {
        uint64_t *pd = pd_base + i * 512;
        pdpt[i] = (uint64_t)(uintptr_t)pd | PTE_PRESENT | PTE_WRITABLE;
    }

    /* Fill PD entries: 2 MB identity pages. */
    for (uint32_t page = 0; page < total_2mb_pages; page++) {
        uint32_t pd_idx    = page / 512;
        uint32_t entry_idx = page % 512;
        if (pd_idx >= num_pds) break;
        uint64_t *pd = pd_base + pd_idx * 512;
        pd[entry_idx] = ((uint64_t)page << 21) |
                         PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE;
    }

    /* Higher-half: PML4[511] → PDPT_high → PD_high
     * Map first 32 MB (16 entries × 2 MB) at 0xFFFFFFFF80000000. */
    pml4[511] = (uint64_t)(uintptr_t)pdpt_hi | PTE_PRESENT | PTE_WRITABLE;
    pdpt_hi[510] = (uint64_t)(uintptr_t)pd_hi | PTE_PRESENT | PTE_WRITABLE;

    for (uint32_t i = 0; i < 16; i++) {
        pd_hi[i] = ((uint64_t)i << 21) |
                   PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE;
    }

    Print("Gangway: page tables at ");
    PrintHex64(g_pt_base);
    Print(", stack at ");
    PrintHex64(g_stack_base);
    Print("\r\n");

    return EFI_SUCCESS;
}

/*
 * CheckEfiLoadAddress — verify the EFI application is within our 4 GB
 * identity map.
 *
 * The JMP after `mov cr3` in gangway_jump.asm runs from the EFI .text
 * section, so the image must remain mapped through the CR3 switch. Our
 * identity map covers 4 GB. UEFI spec recommends loading EFI applications
 * below 4 GB and every implementation observed in the wild (OVMF, AMI,
 * Insyde, Phoenix UEFI, Mac, MS Surface) honours that. If a future
 * firmware violates this we panic with a clear message rather than
 * triple-faulting on the JMP.
 */
void CheckEfiLoadAddress(void)
{
    EFI_GUID img_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (EFI_ERROR(g_bs->handle_protocol(g_image_handle, &img_guid, (void **)&li)))
        return;
    if (!li || !li->image_base) return;

    uint64_t app_base = (uint64_t)(uintptr_t)li->image_base;
    uint64_t app_end  = app_base + li->image_size;

    Print("Gangway: EFI image at ");
    PrintHex64(app_base);
    Print(" size=");
    PrintDec(li->image_size);
    Print("\r\n");

    if (app_end > 0x100000000ULL) {
        Print("Gangway: FATAL — EFI app loaded above 4 GB (");
        PrintHex64(app_end);
        Print(")\r\n");
        Panic("EFI app above 4 GB — extend identity map or update firmware");
    }
}
