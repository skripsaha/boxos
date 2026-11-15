/*
 * Gangway — the handoff: boot_info (the machine), the Boarding Pass (the
 * journey) and the jump into the kernel on the loader's own page tables.
 */

#include "gangway.h"
#include "boot_info.h"
#include "boarding_pass.h"

/* =========================================================================
 * The Boarding Pass — what this loader tells the kernel about its own arrival,
 * written by the structures the kernel reads it by (boarding_pass.h).
 *
 * The stamp that matters is the volume. A kernel in long mode cannot ask the
 * firmware which device it was booted from, so without this the Boardroom
 * picks by a rule — "the removable medium wins" — which is wrong on any
 * machine carrying BoxOS on an internal disk with a flash drive in a socket.
 * This loader knows the answer: it has the volume's Deed in hand.
 * ========================================================================= */

/* =========================================================================
 * The journey's moments (boarding_pass.h, BoardingMoment)
 *
 * A step marked as it is reached, by the time-stamp counter — the clock every
 * x86-64 core has from power-on, read with no firmware call, so the first
 * moment also says how long the firmware took before this loader ran. The
 * kernel counts on in the same ticks, which is what lets one line be drawn
 * from power-on to a shell.
 * ========================================================================= */

/* The loader marks seven steps; the pass has room for some twenty-five. */
#define MOMENTS_MAX 16u

static BoardingMoment g_moments[MOMENTS_MAX];
static uint16_t       g_moment_count = 0;

static uint64_t Tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void Moment(const char *what)
{
    if (g_moment_count >= MOMENTS_MAX) return;
    BoardingMoment *m = &g_moments[g_moment_count++];
    MemZero(m->what, sizeof(m->what));
    for (unsigned i = 0; i < BOARDING_MOMENT_WORD && what[i]; i++) m->what[i] = what[i];
    m->tsc = Tsc();
}

/* Append one stamp. The kind and the length go down first, then the payload,
 * then the write cursor moves to the next four-byte boundary — which is the
 * whole of what lets a kernel walk a pass carrying stamps it has never been
 * taught about. */
static uint16_t BoardingStamp(uint8_t *base, uint16_t at, uint16_t kind,
                              const void *payload, uint16_t bytes)
{
    uint16_t k = kind, b = bytes;
    MemCopy(base + at,     &k, 2);
    MemCopy(base + at + 2, &b, 2);
    if (bytes) {
        MemCopy(base + at + 4, payload, bytes);
    }
    uint16_t stride = (uint16_t)((4U + bytes + 3U) & ~3U);
    return (uint16_t)(at + stride);
}

void WriteBoardingPass(void)
{
    uint8_t *base = (uint8_t *)(uintptr_t)BOARDING_PASS_ADDR;
    MemZero(base, BOARDING_PASS_BYTES);

    uint16_t at = sizeof(BoardingPassHeader);
    uint16_t count = 0;

    /* The volume this kernel was read out of, straight out of the Deed
     * this loader verified on the way in. */
    at = BoardingStamp(base, at, BOARDING_STAMP_VOLUME,
                       g_volume_uuid, sizeof(BoardingVolume));
    count++;

    /* How it was reached. UEFI has no BIOS drive number, and saying 0xFF is
     * saying so rather than leaving a zero that reads as drive zero. */
    BoardingMedium medium = { BOARDING_FIRMWARE_UEFI, 0xFF, 0 };
    at = BoardingStamp(base, at, BOARDING_STAMP_MEDIUM, &medium, sizeof(medium));
    count++;

    /* Who wrote it. On a board that boots through CSM one week and UEFI the
     * next, that is a real question with nothing else on the screen to answer
     * it. */
    BoardingLoader loader = { { 'G','a','n','g','w','a','y',0,0,0,0,0 } };
    at = BoardingStamp(base, at, BOARDING_STAMP_LOADER, &loader, sizeof(loader));
    count++;

    /* What it said, kept rather than printed (gangway_console.c) — written
     * after ExitBootServices, so the journal is whole: nothing is said past
     * this but the one COM1 line a cut memory map earns. */
    uint32_t said = 0, lost = 0;
    const char *text = JournalText(&said, &lost);
    BoardingJournal journal = { (uint64_t)(uintptr_t)text, said, lost };
    at = BoardingStamp(base, at, BOARDING_STAMP_JOURNAL, &journal, sizeof(journal));
    count++;

    /* And when each step of the journey was reached. */
    at = BoardingStamp(base, at, BOARDING_STAMP_MOMENTS, g_moments,
                       (uint16_t)(g_moment_count * sizeof(BoardingMoment)));
    count++;

    /*
     * The seal, and it goes on last because it covers everything ahead of it.
     *
     * Its four-byte kind/length pair is written here; the sum that fills its
     * payload cannot be taken until the HEADER is final, because the header is
     * part of what it covers. So: reserve the stamp, finish the header with
     * the seal counted in, then sum.
     */
    BoardingSeal seal = { 0 };
    uint16_t seal_at = at;
    at = BoardingStamp(base, at, BOARDING_STAMP_SEAL, &seal, sizeof(seal));
    count++;

    BoardingPassHeader hdr;
    hdr.magic        = BOARDING_PASS_MAGIC;
    hdr.header_bytes = sizeof(BoardingPassHeader);
    hdr.used_bytes   = at;
    hdr.capacity     = BOARDING_PASS_BYTES;
    hdr.count        = count;
    MemCopy(base, &hdr, sizeof(hdr));

    /* Every byte ahead of the seal's payload: the header, the stamps, and the
     * seal's own kind/length pair. Nothing sums itself. */
    uint32_t covered = (uint32_t)seal_at + sizeof(BoardingStampHeader);   /* past its kind/length */
    seal.crc32 = Crc32(base, covered);
    MemCopy(base + covered, &seal, sizeof(seal));
}

/* =========================================================================
 * boot_info_t population
 * ========================================================================= */

void FillBootInfo(const FbInfo *fb, uint32_t e820_count,
                         uint64_t kernel_end_phys)
{
    boot_info_t *bi = (boot_info_t *)(uintptr_t)BOOT_INFO_ADDR;
    MemZero(bi, sizeof(boot_info_t));

    bi->magic          = BOOT_INFO_MAGIC;
    bi->boot_method    = BOOT_METHOD_UEFI;
    bi->e820_map_addr  = (uint32_t)E820_MAP_ADDR;
    bi->e820_count     = (uint16_t)e820_count;
    bi->reserved1      = 0;
    bi->kernel_start   = (uint32_t)KERNEL_LOAD_ADDR;

    uint64_t kend_aligned = (kernel_end_phys + PAGE_4KB - 1) & ~(PAGE_4KB - 1);
    bi->kernel_end     = (uint32_t)kend_aligned;
    bi->boot_drive     = 0xFF;   /* UEFI has no BIOS drive number */
    bi->reserved2      = 0;
    bi->reserved3      = 0;
    bi->page_table_base = (uint32_t)g_pt_base;
    bi->stack_base     = (uint32_t)g_stack_base;

    bi->fb_addr        = fb->addr;
    bi->fb_width       = fb->width;
    bi->fb_height      = fb->height;
    bi->fb_stride      = fb->stride;
    bi->fb_format      = fb->format;
    bi->rsdp_addr      = FindAcpiRsdp();

    /* The EFI runtime handoff. g_efi_mmap_* are populated inside
     * DoExitBootServices — FillBootInfo runs BEFORE EBS so we publish
     * placeholders here and the EBS retry-loop updates them in-place via the
     * same fixed BOOT_INFO_ADDR. After EBS returns the final values are
     * patched in (PostEbsPatchBootInfo, gangway_memory.c). */
    bi->efi_rt_services_phys = g_efi_rt_services_phys;
    bi->efi_mmap_phys        = 0;   /* set by post-EBS patch */
    bi->efi_mmap_size        = 0;
    bi->efi_mmap_desc_size   = 0;
    bi->efi_mmap_desc_ver    = 0;
    bi->efi_fw_revision      = g_efi_fw_revision;

    /* The system and configuration tables, captured at entry
     * (FirmwareTablesTake, gangway_firmware.c) — they survive EBS because both live in EfiRuntimeServicesData.
     * efi_esrt_copy_* are populated by the pre-EBS ESRT capture path; zero
     * if the firmware did not publish an ESRT. */
    bi->efi_system_table_phys = g_efi_system_table_phys;
    bi->efi_cfg_table_phys    = g_efi_cfg_table_phys;
    bi->efi_cfg_table_count   = g_efi_cfg_table_count;
    bi->efi_esrt_copy_phys    = g_efi_esrt_copy_phys;
    bi->efi_esrt_copy_size    = g_efi_esrt_copy_size;
}

/* =========================================================================
 * Kernel handoff: install our tables and jump
 *
 * After ExitBootServices we have no UEFI services. We are ALREADY in
 * 64-bit long mode — UEFI spec §2.3.4 mandates LME=1, PG=1, PAE=1 for an
 * x64 firmware — so there is no mode switch to perform. All that is left:
 *   1. Enforce CR4.PAE (already set; cheap insurance)
 *   2. Load our page table base into CR3
 *   3. Switch RSP to the boot stack and jump
 *
 * EFER is deliberately NOT touched here. Every bit the kernel needs beyond
 * what firmware left is the kernel's own business, and it takes them up
 * itself before it uses them — NXE first of all, in CpuTakeUpNoExecute,
 * before vmm_init builds anything.
 * ========================================================================= */

/*
 * Implemented in gangway_jump.asm — pure NASM, no inline asm ambiguity.
 * Installs our CR3, switches RSP, jumps to entry. Does not write EFER.
 * Never returns.
 *
 * IMPORTANT: the NASM code uses System V AMD64 ABI registers (rdi, rsi, rdx).
 * When compiled with clang -target x86_64-unknown-windows, the default calling
 * convention is MS ABI (rcx, rdx, r8), which would put arguments in the wrong
 * registers and cause a triple fault.  The sysv_abi attribute forces System V
 * calling convention regardless of the compilation target.
 */
extern void __attribute__((sysv_abi)) GangwayJump(uint64_t pt_base,
                                                   uint64_t stack,
                                                   uint64_t entry);

void __attribute__((noreturn)) JumpToKernel(uint64_t pt_base)
{
    GangwayJump(pt_base, g_stack_base, (uint64_t)KERNEL_LOAD_ADDR);
    __builtin_unreachable();
}
